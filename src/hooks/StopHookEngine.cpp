// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "hooks/StopHookEngine.h"

#include <QByteArray>
#include <QFile>

// Lua's headers declare C linkage for none of their functions: unlike most C
// libraries they carry no `extern "C"` guards, so the consumer supplies them.
// Without this the references are mangled and nothing links.
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <chrono>
#include <exception>

namespace pist {
namespace hooks {

namespace {

/// How often the interpreter calls back to check the clock. The hook is a count
/// hook — Lua has no wall-clock hook — so the budget is honoured to within this
/// many VM instructions, which Lua 5.4.8 runs in well under a millisecond
/// (measured: PLAN §9). A tight loop therefore pays one hook call per 10,000
/// iterations, and a long C call inside a scripted read is not interrupted at
/// all: those are bounded by the transport, not by the script.
constexpr int kHookInstructionCount = 10000;

/// The registry key the run's state lives under, so the C functions below need no
/// globals: one run at a time, one interpreter at a time.
char kRunKey = 0;

/// The error object the deadline raises: a light userdata, not a string, so it
/// is an identity the guarded `pcall` can recognise. A string could be produced
/// by a script, and then a script could make its own `pcall` refuse to continue.
char kTimeoutToken = 0;

/// What one run needs to reach: the API it is bound to, and when it must stop.
struct Run
{
    HookApi *api = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool timedOut = false;
    bool hasDeadline = false;
};

Run *currentRun(lua_State *L)
{
    lua_pushlightuserdata(L, &kRunKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    Run *run = static_cast<Run *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return run;
}

/// The budget, checked between bytecodes: the only code of ours that runs while a
/// script runs, and therefore the only place a runaway one can be stopped.
///
/// The error carries `kTimeoutToken`, and the `pcall`/`xpcall` the script sees
/// are guarded: they re-raise that token instead of returning it, so a loop
/// around a `pcall` cannot swallow the deadline and run forever. The token is a
/// light userdata, which a script cannot forge.
void budgetHook(lua_State *L, lua_Debug *)
{
    Run *run = currentRun(L);
    if (!run || !run->hasDeadline)
        return;
    if (std::chrono::steady_clock::now() >= run->deadline) {
        run->timedOut = true;
        lua_pushlightuserdata(L, &kTimeoutToken);
        lua_error(L);
    }
}

/// Replaces Lua's `load` / `loadfile` / `dofile`. Each of them can compile a
/// chunk, and `load` accepts one from a string — which would let a script
/// compile a precompiled (binary) chunk that never went through the text-only
/// gate below, and undo the sandbox one call at a time. Refused by name, so a
/// script that used to work says why it no longer does.
int refuseLoading(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    return luaL_error(L, "%s: compiling more code from a script is disabled", name);
}

/// The three loaders, removed from `_G` and replaced by a function that refuses.
const luaL_Reg kRefused[] = {
    {"load", refuseLoading},
    {"loadfile", refuseLoading},
    {"dofile", refuseLoading},
    {nullptr, nullptr},
};

/// Whether the value on the top of the stack is the deadline's error.
bool isTimeout(lua_State *L)
{
    return lua_touserdata(L, -1) == &kTimeoutToken;
}

/// `pcall`, guarded: an ordinary error is returned as `false, err` exactly as
/// Lua's own does, but the deadline's token is *re-raised* rather than reported.
/// Re-raising from inside the guarded call unwinds to the next guarded one, and
/// the engine's protected call is the last of them — so no amount of nesting
/// around `pcall` can keep a runaway script alive past its budget.
int guardedPcall(lua_State *L)
{
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);  /* first result if no errors */
    lua_insert(L, 1);
    const int status = lua_pcall(L, lua_gettop(L) - 2, LUA_MULTRET, 0);
    if (status == LUA_OK)
        return lua_gettop(L);
    if (isTimeout(L))
        return lua_error(L);
    lua_pushboolean(L, 0);
    lua_pushvalue(L, -2);
    return 2;
}

/// `xpcall`, guarded the same way. The handler still runs for ordinary errors;
/// if it transforms the deadline's token into something else, the hook raises
/// again at the next instruction, because the deadline stays passed.
int guardedXpcall(lua_State *L)
{
    const int args = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const int handler = 2;
    lua_pushboolean(L, 1);  /* first result if no errors */
    lua_insert(L, 1);
    const int status = lua_pcall(L, args - 2, LUA_MULTRET, handler);
    if (status == LUA_OK)
        return lua_gettop(L);
    if (isTimeout(L))
        return lua_error(L);
    lua_pushboolean(L, 0);
    lua_pushvalue(L, -2);
    return 2;
}

/// A number argument with a range check. Lua's `luaL_checkinteger` already refuses
/// a non-number by naming the function and the argument, so only the range is
/// ours to state — and `return luaL_error(...)` is the idiom Lua itself uses.
lua_Integer checkInteger(lua_State *L, int index, const char *what, lua_Integer minimum,
                         lua_Integer maximum)
{
    const lua_Integer value = luaL_checkinteger(L, index);
    if (value < minimum || value > maximum)
        return luaL_error(L, "%s must be in %I..%I", what, minimum, maximum);
    return value;
}

/// Raise `sentence` as a Lua error, once every C++ object in the caller's frame
/// has gone.
///
/// Lua here is upstream C, so its error handling is `longjmp` (ldo.c: `LUAI_THROW`
/// is `longjmp` unless Lua is compiled as C++). A `luaL_error` therefore jumps
/// straight out of a C function that owns a `QString` or a `QByteArray`, skipping
/// the destructor — undefined behaviour, and a leak on every script error. The
/// helper is the one place a message crosses into the interpreter, and it must be
/// called only after the frame's destructors have run: `QString`/`QByteArray`
/// temporaries live in a `{}` block and the sentence is copied into a `QByteArray`
/// first, which is trivially destructible.
[[noreturn]] void raiseLuaError(lua_State *L, const QByteArray &sentence)
{
    lua_pushlstring(L, sentence.constData(), size_t(sentence.size()));
    // Longjmp: it does not return, but the compiler cannot know that from C's
    // declaration, so the attribute has to be stated here as well.
    lua_error(L);
    std::terminate();
}

int apiReadmem(lua_State *L)
{
    Run *run = currentRun(L);
    const lua_Integer address = checkInteger(L, 1, "readmem: address", 0, 0xFFFFFFFF);
    const lua_Integer length = checkInteger(L, 2, "readmem: length", 1, HookApi::kMaxReadBytes);

    QByteArray bytes;
    QByteArray sentence;
    {
        QString error;
        if (!run->api->readMemory(quint32(address), int(length), &bytes, &error))
            sentence = error.toUtf8();
    }
    if (!sentence.isEmpty())
        raiseLuaError(L, sentence);

    // A Lua string is its bytes, so a byte above 0x7F survives this — which is
    // exactly what a debugger read has to promise.
    lua_pushlstring(L, bytes.constData(), size_t(bytes.size()));
    return 1;
}

int apiWritemem(lua_State *L)
{
    Run *run = currentRun(L);
    const lua_Integer address = checkInteger(L, 1, "writemem: address", 0, 0xFFFFFFFF);
    const lua_Integer value = checkInteger(L, 2, "writemem: value", 0, 0xFF);

    QByteArray sentence;
    {
        QString error;
        if (!run->api->writeMemory(quint32(address), quint8(value), &error))
            sentence = error.toUtf8();
    }
    if (!sentence.isEmpty())
        raiseLuaError(L, sentence);
    return 0;
}

int apiReadreg(lua_State *L)
{
    Run *run = currentRun(L);
    const char *name = luaL_checkstring(L, 1);
    quint32 value = 0;
    QByteArray sentence;
    {
        QString error;
        // `fromUtf8` allocates a QString; the read either answers before the
        // block ends or the error is copied out of it, so nothing it owns is
        // alive when the error unwinds.
        if (!run->api->readRegister(QString::fromUtf8(name), &value, &error))
            sentence = error.toUtf8();
    }
    if (!sentence.isEmpty())
        raiseLuaError(L, sentence);
    lua_pushinteger(L, lua_Integer(value));
    return 1;
}

int apiWritereg(lua_State *L)
{
    Run *run = currentRun(L);
    const char *name = luaL_checkstring(L, 1);
    const lua_Integer value = checkInteger(L, 2, "writereg: value", 0, 0xFFFFFFFF);
    QByteArray sentence;
    {
        QString error;
        if (!run->api->writeRegister(QString::fromUtf8(name), quint32(value), &error))
            sentence = error.toUtf8();
    }
    if (!sentence.isEmpty())
        raiseLuaError(L, sentence);
    return 0;
}

/// `log(value)` — one line on the console. Any value, like a print.
int apiLog(lua_State *L)
{
    Run *run = currentRun(L);
    luaL_tolstring(L, 1, nullptr);
    const QString message = QString::fromUtf8(lua_tostring(L, -1));
    lua_pop(L, 1);
    run->api->log(message);
    return 0;
}

/// `print(...)` — the base library's version writes to stdout, which a windowed
/// application may not even have, so it is rebound to the same console line as
/// `log`, joined with tabs as Lua's print does.
int apiPrint(lua_State *L)
{
    Run *run = currentRun(L);
    const int count = lua_gettop(L);
    QByteArray line;
    for (int i = 1; i <= count; ++i) {
        luaL_tolstring(L, i, nullptr);
        if (i > 1)
            line += '\t';
        line += lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    run->api->log(QString::fromUtf8(line));
    return 0;
}

/// `sym(name)` — a label's address in this run. Refuses by name; never 0, which
/// is a valid address and so cannot stand for "not found".
int apiSym(lua_State *L)
{
    Run *run = currentRun(L);
    const char *name = luaL_checkstring(L, 1);
    quint32 address = 0;
    QByteArray sentence;
    {
        QString error;
        if (!run->api->resolveSymbol(QString::fromUtf8(name), &address, &error))
            sentence = error.toUtf8();
    }
    if (!sentence.isEmpty())
        raiseLuaError(L, sentence);
    lua_pushinteger(L, lua_Integer(address));
    return 1;
}

int apiContinue(lua_State *L)
{
    currentRun(L)->api->requestContinue();
    return 0;
}

/// The API of docs/stop-hooks-plan.md §7 — flat globals, as documented.
const luaL_Reg kApi[] = {
    {"readmem", apiReadmem},
    {"writemem", apiWritemem},
    {"readreg", apiReadreg},
    {"writereg", apiWritereg},
    {"sym", apiSym},
    {"log", apiLog},
    // `continue` is not a keyword in Lua, so the sheet's spelling works as
    // written; `resume` is kept as the same function under the name the plan's
    // prose uses for it.
    {"continue", apiContinue},
    {"resume", apiContinue},
    {nullptr, nullptr},
};

/// Lua prefixes an error with `chunk:line: `; pull the line out and hand back the
/// message without it, so the console does not repeat the file the runner has
/// already named. Everything stays as it was when the shape is not recognised —
/// a message is never worth rewriting into something it did not say.
int lineOfLuaMessage(const QString &message, QString *withoutPrefix)
{
    *withoutPrefix = message;
    const int firstColon = message.indexOf(QLatin1Char(':'));
    if (firstColon <= 0)
        return -1;
    const QString rest = message.mid(firstColon + 1);
    const int secondColon = rest.indexOf(QLatin1Char(':'));
    if (secondColon <= 0)
        return -1;
    bool ok = false;
    const int line = rest.left(secondColon).trimmed().toInt(&ok);
    if (!ok || line <= 0)
        return -1;
    *withoutPrefix = rest.mid(secondColon + 1).trimmed();
    return line;
}

} // namespace

StopHookResult StopHookEngine::run(const QString &scriptPath, HookApi &api, int budgetMs)
{
    StopHookResult result;
    result.file = scriptPath;

    QFile file(scriptPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.ok = false;
        result.error = QStringLiteral("cannot read the stop hook '%1': %2")
                           .arg(scriptPath, file.errorString());
        return result;
    }
    const QByteArray source = file.readAll();
    result.ran = true;

    lua_State *L = luaL_newstate();
    if (!L) {
        result.ok = false;
        result.error = QStringLiteral("could not start a Lua interpreter");
        return result;
    }

    Run run;
    run.api = &api;
    run.hasDeadline = budgetMs > 0;
    run.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);

    lua_pushlightuserdata(L, &kRunKey);
    lua_pushlightuserdata(L, &run);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Deliberately not the whole standard library: base, table, string and math
    // are what a hook needs to look at a machine and decide something. No `io`,
    // no `os`, no `package`/`require`, no `debug` — so the documented API is the
    // whole surface a script can reach (docs/stop-hooks-plan.md D6), and no
    // loader, so the sandbox cannot be widened from inside a script.
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);

    // `load` is in the base library and takes a *string*, so leaving it in place
    // would let a script compile a precompiled chunk and skip the text-only gate
    // the load below applies. Replaced rather than only removed: a name that
    // answers with a reason is a script that tells its author what happened.
    for (const luaL_Reg *entry = kRefused; entry->name != nullptr; ++entry) {
        lua_pushcfunction(L, entry->func);
        lua_setglobal(L, entry->name);
    }
    // The base library's `pcall`/`xpcall` make the budget catchable; the guarded
    // pair re-raises the deadline instead (see guardedPcall).
    lua_pushcfunction(L, guardedPcall);
    lua_setglobal(L, "pcall");
    lua_pushcfunction(L, guardedXpcall);
    lua_setglobal(L, "xpcall");
    // `string.dump` is the other half: it compiles a function into the binary
    // form `load` would have accepted, so a script could produce one even though
    // it cannot read one back.
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    for (const luaL_Reg *entry = kApi; entry->name != nullptr; ++entry) {
        lua_pushcfunction(L, entry->func);
        lua_setglobal(L, entry->name);
    }
    lua_pushcfunction(L, apiPrint);
    lua_setglobal(L, "print");

    // `reason` and `at` are values, not calls: they describe the stop the script
    // is running at, and testing `reason == "breakpoint"` should not need a call
    // in the way. `at` is only set when there is something to name, so a script
    // checks `if at then` rather than comparing against an empty string.
    lua_pushstring(L, api.stopReason().toUtf8().constData());
    lua_setglobal(L, "reason");
    const QString at = api.stopAt();
    if (!at.isEmpty()) {
        lua_pushstring(L, at.toUtf8().constData());
        lua_setglobal(L, "at");
    }

    // "@path" makes Lua name the file in every message it raises. "t" restricts
    // the chunk to text: a hook is source a person writes, and a precompiled one
    // is not checked by the interpreter's own verifier (Lua 5.4 documents binary
    // chunks as unsafe to load from untrusted sources). The gate is here, the
    // one place a chunk is compiled.
    const QByteArray chunkName = QByteArrayLiteral("@") + scriptPath.toUtf8();
    int status = luaL_loadbufferx(L, source.constData(), size_t(source.size()),
                                  chunkName.constData(), "t");
    if (status == LUA_OK) {
        lua_sethook(L, budgetHook, LUA_MASKCOUNT, kHookInstructionCount);
        status = lua_pcall(L, 0, 0, 0);
    }

    // A `pcall` inside the script catches the deadline's error and the script
    // then returns normally. A passed deadline is final: reporting the script as
    // finished would be a lie the console repeats.
    if (status == LUA_OK && run.hasDeadline && !run.timedOut
        && std::chrono::steady_clock::now() >= run.deadline) {
        run.timedOut = true;
    }

    if (run.timedOut) {
        result.ok = false;
        result.timedOut = true;
        result.error = QStringLiteral("the script did not finish within %1 ms").arg(budgetMs);
    } else if (status != LUA_OK) {
        result.ok = false;
        const char *raw = lua_tostring(L, -1);
        const QString message = raw ? QString::fromUtf8(raw) : QStringLiteral("unknown error");
        QString withoutPrefix;
        const int line = lineOfLuaMessage(message, &withoutPrefix);
        if (line > 0) {
            result.line = line;
            result.error = withoutPrefix;
        } else {
            result.error = message;
        }
    }

    lua_close(L);
    return result;
}

} // namespace hooks
} // namespace pist
