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
void budgetHook(lua_State *L, lua_Debug *)
{
    Run *run = currentRun(L);
    if (!run || !run->hasDeadline)
        return;
    if (std::chrono::steady_clock::now() >= run->deadline) {
        run->timedOut = true;
        // Raised as an ordinary Lua error, so a script's own pcall sees it and
        // the unwinding is the interpreter's usual path.
        luaL_error(L, "the script did not finish in time");
    }
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

int apiReadmem(lua_State *L)
{
    Run *run = currentRun(L);
    const lua_Integer address = checkInteger(L, 1, "readmem: address", 0, 0xFFFFFFFF);
    const lua_Integer length = checkInteger(L, 2, "readmem: length", 1, HookApi::kMaxReadBytes);

    QByteArray bytes;
    QString error;
    if (!run->api->readMemory(quint32(address), int(length), &bytes, &error))
        return luaL_error(L, "%s", error.toUtf8().constData());

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

    QString error;
    if (!run->api->writeMemory(quint32(address), quint8(value), &error))
        return luaL_error(L, "%s", error.toUtf8().constData());
    return 0;
}

int apiReadreg(lua_State *L)
{
    Run *run = currentRun(L);
    const char *name = luaL_checkstring(L, 1);
    quint32 value = 0;
    QString error;
    if (!run->api->readRegister(QString::fromUtf8(name), &value, &error))
        return luaL_error(L, "%s", error.toUtf8().constData());
    lua_pushinteger(L, lua_Integer(value));
    return 1;
}

int apiWritereg(lua_State *L)
{
    Run *run = currentRun(L);
    const char *name = luaL_checkstring(L, 1);
    const lua_Integer value = checkInteger(L, 2, "writereg: value", 0, 0xFFFFFFFF);
    QString error;
    if (!run->api->writeRegister(QString::fromUtf8(name), quint32(value), &error))
        return luaL_error(L, "%s", error.toUtf8().constData());
    return 0;
}

/// `log(value)` — one line on the console. Any value, like a print.
int apiLog(lua_State *L)
{
    Run *run = currentRun(L);
    luaL_tolstring(L, 1, nullptr);
    run->api->log(QString::fromUtf8(lua_tostring(L, -1)));
    lua_pop(L, 1);
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
    // whole surface a script can reach (docs/stop-hooks-plan.md D6).
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);

    for (const luaL_Reg *entry = kApi; entry->name != nullptr; ++entry) {
        lua_pushcfunction(L, entry->func);
        lua_setglobal(L, entry->name);
    }
    lua_pushcfunction(L, apiPrint);
    lua_setglobal(L, "print");

    // "@path" makes Lua name the file in every message it raises.
    const QByteArray chunkName = QByteArrayLiteral("@") + scriptPath.toUtf8();
    int status = luaL_loadbuffer(L, source.constData(), size_t(source.size()), chunkName.constData());
    if (status == LUA_OK) {
        lua_sethook(L, budgetHook, LUA_MASKCOUNT, kHookInstructionCount);
        status = lua_pcall(L, 0, 0, 0);
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
