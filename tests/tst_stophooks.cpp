// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The stop hook, with no emulator and no window (docs/stop-hooks-plan.md §12).
//
// Two halves, because the feature has two halves. The engine is driven through a
// fake HookApi, and the runner through a fake IDebugBackend whose commands
// complete asynchronously the way the real transport's do. The bugs worth
// catching are the silent ones the plan names: a register read answered from a
// copy a scripted write made stale, and a write pruned by the resume a script
// asked for — neither of which throws, fails a build, or looks wrong on screen.

#include "emu/DebugBackend.h"
#include "emu/MemoryDump.h"
#include "hooks/StopHookEngine.h"
#include "hooks/StopHookRunner.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

using namespace pist;

namespace {

/// A hook API that answers from a byte map and records what it was asked.
/// Deliberately dumb: the engine's job is to bind and translate, not to be the
/// debugger.
class FakeApi : public hooks::HookApi
{
public:
    QHash<quint32, quint8> memory;
    QHash<QString, quint32> registers;
    QStringList logs;
    int reads = 0;
    int writes = 0;
    int continues = 0;
    /// When set, writeMemory fails with this sentence — a refusal to test the
    /// path where the debugger, not the script, says no.
    QString refuseWrite;

    bool readMemory(quint32 address, int length, QByteArray *bytes, QString *error) override
    {
        Q_UNUSED(error);
        ++reads;
        QByteArray out(length, '\0');
        for (int i = 0; i < length; ++i)
            out[i] = char(memory.value(address + quint32(i), 0));
        *bytes = out;
        return true;
    }

    bool writeMemory(quint32 address, quint8 value, QString *error) override
    {
        if (!refuseWrite.isEmpty()) {
            *error = refuseWrite;
            return false;
        }
        ++writes;
        memory.insert(address, value);
        return true;
    }

    bool readRegister(const QString &name, quint32 *value, QString *error) override
    {
        if (!registers.contains(name)) {
            *error = QStringLiteral("readreg: '%1' is not a register").arg(name);
            return false;
        }
        *value = registers.value(name);
        return true;
    }

    bool writeRegister(const QString &name, quint32 value, QString *error) override
    {
        if (name == QLatin1String("usp") || name == QLatin1String("isp")) {
            *error = QStringLiteral("writereg: Hatari cannot set %1 by name").arg(name);
            return false;
        }
        ++writes;
        registers.insert(name, value);
        return true;
    }

    void log(const QString &message) override { logs << message; }
    void requestContinue() override { ++continues; }

    /// What the engine reads back for `stophook.reason` / `.at`.
    QString reason = QStringLiteral("unknown");
    QString at;
    /// `sym()` answers from here — the model, not the debugger, is what resolves
    /// a label, so the fake only has to be a lookup.
    QHash<QString, quint32> symbols;

    QString stopReason() const override { return reason; }
    QString stopAt() const override { return at; }
    bool resolveSymbol(const QString &name, quint32 *address, QString *error) override
    {
        if (!symbols.contains(name)) {
            *error = QStringLiteral("sym: no symbol named '%1'").arg(name);
            return false;
        }
        *address = symbols.value(name);
        return true;
    }
};

/// A backend that records an ordered event log and completes each command one
/// event-loop turn later, as the transport does — the ordering assertions are
/// the point of most of the runner tests, so the log is the assertion surface.
class FakeBackend : public IDebugBackend
{
public:
    QStringList events;
    QHash<quint32, quint8> memory;
    MachineState state;
    bool running = true;
    bool stopped = true;
    /// When set, commands are issued but never complete — the answer that never
    /// comes, which is what the runner's backstop exists for.
    bool holdCommands = false;
    int dumpCount = 0;
    int registerReads = 0;

    bool start(const SessionConfig &config, QString *error) override
    {
        Q_UNUSED(config);
        Q_UNUSED(error);
        return true;
    }
    void stop() override {}
    bool isRunning() const override { return running; }
    bool isStopped() const override { return stopped; }

    void step() override { events << QStringLiteral("step"); }
    void stepOver() override { events << QStringLiteral("stepOver"); }
    void resume() override { events << QStringLiteral("resume"); }
    void pause() override {}
    void refresh() override { events << QStringLiteral("refresh"); }
    void command(const QString &text) override { events << QStringLiteral("command %1").arg(text); }
    void clearBreakpoints() override {}
    void armBreakpoint(const QString &condition) override { events << condition; }

    void requestMemoryDump(quint32 address, int length, int tag) override
    {
        events << QStringLiteral("dump %1").arg(address);
        ++dumpCount;
        QList<MemoryRow> rows;
        MemoryRow row;
        row.address = address;
        for (int i = 0; i < length; ++i)
            row.bytes.append(memory.value(address + quint32(i), 0));
        rows << row;
        QTimer::singleShot(0, this, [this, address, rows, tag] {
            emit memoryDumpReady(address, rows, tag);
        });
    }

    void requestStackDump(quint32 address, int length) override
    {
        Q_UNUSED(address);
        Q_UNUSED(length);
    }

    void dumpRegisters() override
    {
        events << QStringLiteral("registers");
        ++registerReads;
        QTimer::singleShot(0, this, [this] { emit stateUpdated(state); });
    }

    void loadSymbols() override {}
    void infoSubject(const QString &subject) override { Q_UNUSED(subject); }
    void readBasepage() override {}
    void setDisasmEngine(DisasmEngine engine) override { Q_UNUSED(engine); }
    void dropBootTurbo() override {}
    void profileOn() override {}
    void profileOff() override {}
    void profileSave(const QString &path) override { Q_UNUSED(path); }
    void coverageOn() override {}
    void coverageSave(const QString &path) override { Q_UNUSED(path); }
    void breakAtAddressOnce(quint32 address) override { Q_UNUSED(address); }

    void writeMemoryByte(quint32 address, quint8 value) override
    {
        events << QStringLiteral("write %1=%2").arg(address).arg(value);
        memory.insert(address, value);
        complete(QStringLiteral("w b $%1 $%2").arg(address, 0, 16).arg(value, 0, 16));
    }

    void writeRegister(const QString &name, quint32 value) override
    {
        events << QStringLiteral("write %1=$%2").arg(name).arg(value, 0, 16);
        if (name == QLatin1String("d0"))
            state.regs.d[0] = value;
        complete(QStringLiteral("r %1=$%2").arg(name).arg(value, 0, 16));
    }

    /// Complete a command *someone else* issued, as a toolbar Step or the remote
    /// channel can while a hook is waiting.
    void completeForeign(const QString &command)
    {
        emit commandFinished(command, QString());
    }

    void readDisassembly() override {}
    void readDisassemblyAt(quint32 address) override { Q_UNUSED(address); }
    void readHistory(int count) override { Q_UNUSED(count); }
    BackendKind kind() const override { return BackendKind::Native; }

private:
    /// The command's completion, one turn later — the transport's prompt. The
    /// text is the one the backend was asked with, which is what the runner
    /// correlates on.
    void complete(const QString &command)
    {
        if (holdCommands)
            return;
        QTimer::singleShot(0, this, [this, command] {
            events << QStringLiteral("ack");
            emit commandFinished(command, QString());
        });
    }
};

/// A script file in a temporary directory, and its path.
QString writeScript(QTemporaryDir &dir, const QString &source,
                    const QString &name = QStringLiteral("hook.lua"))
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    file.write(source.toUtf8());
    file.close();
    return path;
}

} // namespace

class TstStopHooks : public QObject
{
    Q_OBJECT

private slots:
    // --- the engine ------------------------------------------------------

    void readsMemoryAsByteValues();
    void symReturnsTheAddressAndRaisesOnARefusal();
    void raisesAnApiRefusalToTheScript();
    void refusesBadArgumentsByName();
    void stopsARunawayScript();
    void reportsAMissingFile();
    void offersContinueAndResume();
    void printJoinsItsArgumentsOnTheConsole();
    void reportsTheLineAnErrorCameFrom();
    /// The sandbox cannot be widened from inside a script, and a binary chunk
    /// is refused rather than trusted.
    void refusesToCompileMoreCodeAndBinaryChunks();
    /// The budget is not defeatable by catching its error in a `pcall`.
    void aBudgetCannotBeSwallowedByPcall();
    /// `xpcall` runs the body, not the handler, and still reports errors.
    void guardedXpcallRunsTheBodyAndItsHandler();
    /// A handler that consumes or replaces the timeout token cannot swallow it.
    void aBudgetCannotBeSwallowedByXpcall();

    // --- the runner ------------------------------------------------------

    void writesCompleteBeforeTheContinueItAsksFor();
    void holdsTheConsoleForTheRun();
    void doesNotContinueWhenAutoContinueIsOff();
    void answersRegistersFromTheStopAndRefreshesAfterAWrite();
    void refusesToRunWhileTheMachineIsRunning();
    void doesNotContinueWhenTheDebuggerDoesNotAnswer();
    void doesNothingWhenTheHookIsOff();
    /// `sym()`, and the two values that describe a stop.
    void symResolvesThroughTheHost();
    void theStopReportsItsCauseToScripts();
    /// What a run reports to a caller that is a program: the fields the `hook`
    /// verb, its MCP tool and the `hook` event are built from.
    void theRunIsReportedInFieldsAndToWatchers();
    /// ...including the difference between a script that raised and one that
    /// could not be read at all.
    void aFailedRunAndAnUnreadableScriptAreToldApart();
    /// A script attached to a breakpoint wins over the project's, and is enabled
    /// by being attached rather than by the project's switch.
    void aBreakpointScriptOverridesTheProjectHook();
    /// Someone else's command completing while a hook waits is not its answer.
    void anotherCommandsCompletionIsNotTheHooks();
};

// ---------------------------------------------------------------------------
// The engine
// ---------------------------------------------------------------------------

void TstStopHooks::readsMemoryAsByteValues()
{
    QTemporaryDir dir;
    FakeApi api;
    api.memory[0x100] = 0x01;
    api.memory[0x101] = 0xfe;
    api.memory[0x102] = 0x00;

    // A Lua string is its bytes, so readmem returns the machine's memory
    // exactly — no encoding in the way.
    const QString path = writeScript(
        dir,
        QStringLiteral("local b = readmem(0x100, 3)\n"
                       "log(string.format(\"%d,%d,%d\", string.byte(b, 1, 3)))"));
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(api.reads, 1);
    QCOMPARE(api.logs, QStringList{QStringLiteral("1,254,0")});
}

/// `sym()` is the one call that answers with a value rather than touching the
/// machine — and 0 is a legal address, so a name that resolves to nothing must
/// raise rather than return 0, which a script would happily read from.
void TstStopHooks::symReturnsTheAddressAndRaisesOnARefusal()
{
    QTemporaryDir dir;
    FakeApi api;
    api.symbols.insert(QStringLiteral("start"), 0x12596u);
    api.symbols.insert(QStringLiteral("msg"), 0x125acu);

    const QString path = writeScript(
        dir,
        QStringLiteral("log(string.format(\"%08x %s\", sym(\"msg\"), "
                       "sym(\"start\") == 0x12596 and \"yes\" or \"no\"))\n"
                       "local ok, err = pcall(sym, \"nope\")\n"
                       "log(tostring(ok) .. \" \" .. tostring(err))"));
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(api.logs.size(), 2);
    QCOMPARE(api.logs.at(0), QStringLiteral("000125ac yes"));
    QVERIFY2(api.logs.at(1).startsWith(QStringLiteral("false sym: no symbol named 'nope'")),
             qPrintable(api.logs.at(1)));
}

void TstStopHooks::raisesAnApiRefusalToTheScript()
{
    QTemporaryDir dir;
    FakeApi api;

    // The script catches it: the console keeps what the debugger said, and the
    // run itself is not a failure.
    const QString caught = writeScript(
        dir,
        QStringLiteral("local ok, err = pcall(function() writereg(\"usp\", 1) end)\n"
                       "log(\"caught: \" .. err)"));
    const hooks::StopHookResult first = hooks::StopHookEngine::run(caught, api);
    QVERIFY2(first.ok, qPrintable(first.error));
    QCOMPARE(api.logs.size(), 1);
    QVERIFY(api.logs.first().contains(QStringLiteral("cannot set usp")));

    // Uncaught, the same refusal fails the run and says so.
    api.logs.clear();
    const QString uncaught = writeScript(dir, QStringLiteral("writereg(\"usp\", 1);"));
    const hooks::StopHookResult second = hooks::StopHookEngine::run(uncaught, api);
    QVERIFY(!second.ok);
    QVERIFY(second.error.contains(QStringLiteral("cannot set usp")));
    QVERIFY(api.logs.isEmpty());
    QCOMPARE(api.writes, 0);
}

void TstStopHooks::refusesBadArgumentsByName()
{
    QTemporaryDir dir;
    FakeApi api;

    const QString notANumber = writeScript(dir, QStringLiteral("readmem(\"nope\", 4)"));
    const hooks::StopHookResult first = hooks::StopHookEngine::run(notANumber, api);
    QVERIFY(!first.ok);
    // Lua's own check names the function and the argument; that refusal is the
    // one the user reads, so it is the one asserted.
    QVERIFY2(first.error.contains(QStringLiteral("bad argument")), qPrintable(first.error));
    QCOMPARE(api.reads, 0);

    // Past the cap, the refusal names the numbers rather than asking the
    // debugger for a gigabyte of text.
    const QString tooLong = writeScript(dir, QStringLiteral("readmem(0x100, 5000)"));
    const hooks::StopHookResult second = hooks::StopHookEngine::run(tooLong, api);
    QVERIFY(!second.ok);
    QVERIFY2(second.error.contains(QStringLiteral("4096")), qPrintable(second.error));
    QCOMPARE(api.reads, 0);
}

void TstStopHooks::stopsARunawayScript()
{
    QTemporaryDir dir;
    FakeApi api;
    const QString path = writeScript(dir, QStringLiteral("while true do end"));

    QElapsedTimer clock;
    clock.start();
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api, 80);
    const qint64 elapsed = clock.elapsed();

    QVERIFY2(result.timedOut, qPrintable(result.error));
    QVERIFY(!result.ok);
    // The budget is the promise: it must have stopped near it, not hung. The
    // count hook is what enforces it (PLAN §9 measured it), and this asserts the
    // wiring did not lose it.
    QVERIFY2(elapsed < 3000, qPrintable(QStringLiteral("took %1 ms").arg(elapsed)));
}

void TstStopHooks::reportsAMissingFile()
{
    FakeApi api;
    const hooks::StopHookResult result =
        hooks::StopHookEngine::run(QStringLiteral("/nonexistent/nope.lua"), api);

    QVERIFY(!result.ran);
    QVERIFY(!result.ok);
    QVERIFY2(result.error.contains(QStringLiteral("cannot read")), qPrintable(result.error));
    QVERIFY(result.error.contains(QStringLiteral("nope.lua")));
}

void TstStopHooks::offersContinueAndResume()
{
    QTemporaryDir dir;
    FakeApi api;

    // Lua has no reserved-word problem with `continue`, so the sheet's spelling
    // works as written; `resume` is the same function under the other name.
    const QString continued = writeScript(dir, QStringLiteral("continue()"));
    QVERIFY(hooks::StopHookEngine::run(continued, api).ok);
    QCOMPARE(api.continues, 1);

    const QString resumed = writeScript(dir, QStringLiteral("resume()"));
    QVERIFY(hooks::StopHookEngine::run(resumed, api).ok);
    QCOMPARE(api.continues, 2);
}

void TstStopHooks::printJoinsItsArgumentsOnTheConsole()
{
    QTemporaryDir dir;
    FakeApi api;

    // `print` is rebound to the console (a windowed application may have no
    // stdout), and it keeps Lua's own behaviour: arguments joined with tabs.
    const QString path =
        writeScript(dir, QStringLiteral("print(\"lives\", 3, true)"));
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(api.logs, QStringList{QStringLiteral("lives\t3\ttrue")});
}

void TstStopHooks::reportsTheLineAnErrorCameFrom()
{
    QTemporaryDir dir;
    FakeApi api;
    const QString path = writeScript(
        dir, QStringLiteral("log(\"first\")\nnotAFunction()\nlog(\"never\")"));

    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api);
    QVERIFY(!result.ok);
    QCOMPARE(result.line, 2);
    QCOMPARE(api.logs, QStringList{QStringLiteral("first")});
}

void TstStopHooks::refusesToCompileMoreCodeAndBinaryChunks()
{
    QTemporaryDir dir;
    FakeApi api;

    // `load`, `loadfile` and `dofile` are gone from the script's world: any of
    // them would compile a chunk from a string, and a prefixed one is a binary
    // chunk the interpreter does not verify. `string.dump` would produce one.
    const QString path = writeScript(
        dir,
        QStringLiteral("local refused, why = pcall(load, \"return 1\")\n"
                       "log(tostring(refused) .. \" \" .. tostring(why))\n"
                       "local lf, e1 = pcall(loadfile, \"x.lua\")\n"
                       "local df, e2 = pcall(dofile, \"x.lua\")\n"
                       "log(tostring(lf) .. \" \" .. tostring(e1))\n"
                       "log(tostring(df) .. \" \" .. tostring(e2))\n"
                       "local fn, e3 = pcall(load, function() return \"return 1\" end)\n"
                       "log(tostring(fn) .. \" \" .. tostring(e3))\n"
                       "log(tostring(string.dump))"));
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(api.logs.size(), 5);
    // Every loader refuses rather than compiling, and names itself. The name is
    // the loader's, not the chunk's: `load("return 1")` must not answer with
    // "return 1: ...", and `load(fn)` must reach the refusal at all.
    QVERIFY2(api.logs.at(0).startsWith(QStringLiteral("false load: compiling")),
             qPrintable(api.logs.at(0)));
    QVERIFY2(api.logs.at(1).startsWith(QStringLiteral("false loadfile: compiling")),
             qPrintable(api.logs.at(1)));
    QVERIFY2(api.logs.at(2).startsWith(QStringLiteral("false dofile: compiling")),
             qPrintable(api.logs.at(2)));
    QVERIFY2(api.logs.at(3).startsWith(QStringLiteral("false load: compiling")),
             qPrintable(api.logs.at(3)));
    // And the producer of a binary chunk is gone.
    QCOMPARE(api.logs.at(4), QStringLiteral("nil"));

    // A bytecode chunk is refused by the loader itself, before it runs. The
    // signature is Lua's own (`\x1bLua`); "t" is the mode it is loaded in, so
    // the error names the mode rather than executing the bytes.
    QFile binary(dir.filePath(QStringLiteral("binary.lua")));
    QVERIFY(binary.open(QIODevice::WriteOnly));
    binary.write(QByteArray::fromHex("1b4c7561") + QByteArrayLiteral("\x54\x00\x19\x93\r\n\x1a\n")
                 + QByteArray(8, '\0'));
    binary.close();
    const hooks::StopHookResult precompiled =
        hooks::StopHookEngine::run(binary.fileName(), api);
    QVERIFY(!precompiled.ok);
    QVERIFY2(precompiled.error.contains(QStringLiteral("binary")),
             qPrintable(precompiled.error));
}

void TstStopHooks::aBudgetCannotBeSwallowedByPcall()
{
    QTemporaryDir dir;
    FakeApi api;

    // The error the deadline raises is an ordinary Lua error, so a script can
    // catch it — and one that loops around a `pcall` would then never stop. The
    // deadline is latched: every later instruction raises again, and the engine
    // refuses to call a run that swallowed the last one finished.
    const QString path = writeScript(
        dir,
        QStringLiteral("while true do pcall(function() while true do end end) end"));

    QElapsedTimer clock;
    clock.start();
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api, 80);
    const qint64 elapsed = clock.elapsed();

    QVERIFY2(result.timedOut, qPrintable(result.error));
    QVERIFY(!result.ok);
    QVERIFY2(elapsed < 5000, qPrintable(QStringLiteral("took %1 ms").arg(elapsed)));
}

void TstStopHooks::guardedXpcallRunsTheBodyAndItsHandler()
{
    QTemporaryDir dir;
    FakeApi api;

    // The body must run, the handler must not run on success, and an ordinary
    // error must come back through the handler. The old insert(1) layout ran
    // the handler as the body and left the body on the stack.
    const QString path = writeScript(
        dir,
        QStringLiteral("local calls = 0\n"
                       "function body(x) calls = calls + 1; return 'body:' .. x end\n"
                       "function handler(e) return 'handled:' .. tostring(e) end\n"
                       "local ok, value = xpcall(body, handler, 42)\n"
                       "log(tostring(ok) .. '|' .. tostring(value) .. '|' .. tostring(calls))\n"
                       "local ok2, value2 = xpcall(function(x) error('boom:' .. x) end, handler, 7)\n"
                       "log(tostring(ok2) .. '|' .. tostring(value2))"));
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(api.logs.size(), 2);
    QCOMPARE(api.logs.at(0), QStringLiteral("true|body:42|1"));
    QVERIFY2(api.logs.at(1).startsWith(QStringLiteral("false|handled:")),
             qPrintable(api.logs.at(1)));
    QVERIFY2(api.logs.at(1).contains(QStringLiteral("boom:7")), qPrintable(api.logs.at(1)));
}

void TstStopHooks::aBudgetCannotBeSwallowedByXpcall()
{
    QTemporaryDir dir;
    FakeApi api;

    // A handler that returns a normal value is the essential case: with a
    // correct errfunc it receives the token, so `isTimeout` on the result is
    // false and only the latched `timedOut` flag can stop the loop.
    const QString returns = writeScript(
        dir,
        QStringLiteral("while true do xpcall(function() while true do end end, "
                       "function(e) return 1 end) end"),
        QStringLiteral("returns.lua"));
    QElapsedTimer clock;
    clock.start();
    const hooks::StopHookResult normal = hooks::StopHookEngine::run(returns, api, 80);
    QVERIFY2(normal.timedOut, qPrintable(normal.error));
    QVERIFY(!normal.ok);
    QVERIFY2(clock.elapsed() < 5000,
             qPrintable(QStringLiteral("took %1 ms").arg(clock.elapsed())));

    // A handler that raises a different error takes the non-LUA_OK branch; the
    // latched check has to run before that branch too.
    const QString throws = writeScript(
        dir,
        QStringLiteral("while true do xpcall(function() while true do end end, "
                       "function(e) error('swallowed') end) end"),
        QStringLiteral("throws.lua"));
    clock.restart();
    const hooks::StopHookResult replaced = hooks::StopHookEngine::run(throws, api, 80);
    QVERIFY2(replaced.timedOut, qPrintable(replaced.error));
    QVERIFY(!replaced.ok);
    QVERIFY2(clock.elapsed() < 5000,
             qPrintable(QStringLiteral("took %1 ms").arg(clock.elapsed())));
}

// ---------------------------------------------------------------------------
// The runner
// ---------------------------------------------------------------------------

void TstStopHooks::writesCompleteBeforeTheContinueItAsksFor()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    QStringList events;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 7)\ncontinue()"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return true; };
    host.resume = [&events] { events << QStringLiteral("resume"); };
    host.refresh = [&events] { events << QStringLiteral("refresh"); };
    host.holdConsole = [&events](bool hold) {
        events << (hold ? QStringLiteral("hold") : QStringLiteral("release"));
    };
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    state.pc = 0x1234;
    runner.onStopPublished(state);
    QTest::qWait(150);

    // The write was acknowledged before the resume was issued. Without the
    // wait, resume() would prune it out of the queue and the byte would never
    // reach the machine — silently (docs/stop-hooks-plan.md R5).
    QVERIFY(backend.events.contains(QStringLiteral("write 512=7")));
    const int write = backend.events.indexOf(QStringLiteral("write 512=7"));
    const int ack = backend.events.indexOf(QStringLiteral("ack"), write);
    QVERIFY2(ack > write, qPrintable(backend.events.join(QLatin1Char('|'))));
    const int resume = events.indexOf(QStringLiteral("resume"));
    QVERIFY(resume >= 0);
    QVERIFY2(console.join(QLatin1Char('|')).contains(QStringLiteral("asked to continue")),
             qPrintable(console.join(QLatin1Char('|'))));
    QCOMPARE(backend.memory.value(0x200), quint8(7));
}

void TstStopHooks::holdsTheConsoleForTheRun()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    QStringList events;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 1)"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [&events] { events << QStringLiteral("resume"); };
    host.refresh = [&events] { events << QStringLiteral("refresh"); };
    host.holdConsole = [&events](bool hold) {
        events << (hold ? QStringLiteral("hold") : QStringLiteral("release"));
    };
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(150);

    // Held across the whole run and released after it, whatever the script did:
    // a command the user typed must not complete first and be mistaken for one
    // of ours.
    QVERIFY(!events.isEmpty());
    QCOMPARE(events.first(), QStringLiteral("hold"));
    QCOMPARE(events.last(), QStringLiteral("release"));
    QVERIFY(events.contains(QStringLiteral("refresh")));
}

void TstStopHooks::doesNotContinueWhenAutoContinueIsOff()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    QStringList events;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 3)\ncontinue()"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [&events] { events << QStringLiteral("resume"); };
    host.refresh = [&events] { events << QStringLiteral("refresh"); };
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(150);

    QVERIFY(!events.contains(QStringLiteral("resume")));
    QVERIFY2(console.join(QLatin1Char('|')).contains(QStringLiteral("autoContinue is off")),
             qPrintable(console.join(QLatin1Char('|'))));
    // The write still happened: only the resume was withheld.
    QCOMPARE(backend.memory.value(0x200), quint8(3));
}

void TstStopHooks::answersRegistersFromTheStopAndRefreshesAfterAWrite()
{
    QTemporaryDir dir;
    FakeBackend backend;
    backend.state.regs.valid = true;
    backend.state.regs.d[0] = 0x11;
    QStringList console;
    const QString path = writeScript(
        dir,
        QStringLiteral("log(\"before=\" .. readreg(\"d0\"))\n"
                       "writereg(\"d0\", 0x99)\n"
                       "log(\"after=\" .. readreg(\"d0\"))"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    state.regs.d[0] = 0x11;
    runner.onStopPublished(state);
    QTest::qWait(150);

    // The first read answered from the stop's own registers (no round trip);
    // the second one had to ask, because a scripted write made the copy stale.
    QCOMPARE(backend.registerReads, 1);
    QVERIFY2(console.contains(QStringLiteral("[hook] before=17")),
             qPrintable(console.join(QLatin1Char('|'))));
    QVERIFY2(console.contains(QStringLiteral("[hook] after=153")),
             qPrintable(console.join(QLatin1Char('|'))));
}

void TstStopHooks::refusesToRunWhileTheMachineIsRunning()
{
    QTemporaryDir dir;
    FakeBackend backend;
    backend.stopped = false; // a manual Continue won the race with the queued run
    QStringList console;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 9)"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(80);

    QVERIFY2(console.join(QLatin1Char('|')).contains(QStringLiteral("skipped")),
             qPrintable(console.join(QLatin1Char('|'))));
    QVERIFY(backend.events.isEmpty());
}

void TstStopHooks::doesNotContinueWhenTheDebuggerDoesNotAnswer()
{
    QTemporaryDir dir;
    FakeBackend backend;
    backend.holdCommands = true; // the command is written; its answer never comes
    QStringList console;
    QStringList events;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 4)\ncontinue()"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return true; };
    host.resume = [&events] { events << QStringLiteral("resume"); };
    host.refresh = [&events] { events << QStringLiteral("refresh"); };
    host.holdConsole = [](bool) {};
    host.operationTimeoutMs = [] { return 100; };
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(400);

    // Giving up leaves the machine stopped: continuing here would resume a
    // machine whose state the script only half-changed (docs/stop-hooks-plan.md
    // R4).
    QVERIFY(!events.contains(QStringLiteral("resume")));
    QVERIFY2(console.join(QLatin1Char('|')).contains(QStringLiteral("did not acknowledge")),
             qPrintable(console.join(QLatin1Char('|'))));
}

void TstStopHooks::doesNothingWhenTheHookIsOff()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 5)"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return false; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(80);

    QVERIFY(backend.events.isEmpty());
    QVERIFY(console.isEmpty());
}

void TstStopHooks::symResolvesThroughTheHost()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    const QHash<QString, quint32> labels{{QStringLiteral("counter"), 0x1234}};
    const QString path = writeScript(
        dir,
        QStringLiteral("local ok, err = pcall(function() return sym(\"nope\") end)\n"
                       "log(\"refused: \" .. tostring(err ~= nil))\n"
                       "log(string.format(\"counter=$%06x\", sym(\"counter\")))"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    host.symbol = [&labels](const QString &name, quint32 *address, QString *error) {
        if (!labels.contains(name)) {
            *error = QStringLiteral("sym: no symbol named '%1'").arg(name);
            return false;
        }
        *address = labels.value(name);
        return true;
    };
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(150);

    // A refusal raises — it never comes back as 0, which is a real address.
    QVERIFY2(console.contains(QStringLiteral("[hook] refused: true")),
             qPrintable(console.join(QLatin1Char('|'))));
    QVERIFY2(console.contains(QStringLiteral("[hook] counter=$001234")),
             qPrintable(console.join(QLatin1Char('|'))));
}

void TstStopHooks::theStopReportsItsCauseToScripts()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    const QString path = writeScript(dir, QStringLiteral("log(reason .. \" at \" .. tostring(at))"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;

    StopTrigger hit;
    hit.reason = StopReason::Breakpoint;
    hit.at = QStringLiteral("hello.s:5");
    runner.onStopPublished(state, hit);
    QTest::qWait(150);
    QVERIFY2(console.contains(QStringLiteral("[hook] breakpoint at hello.s:5")),
             qPrintable(console.join(QLatin1Char('|'))));

    // A step says so and has no breakpoint to name — `at` stays nil rather than
    // becoming an empty string a script would have to special-case.
    StopTrigger step;
    step.reason = StopReason::Step;
    runner.onStopPublished(state, step);
    QTest::qWait(150);
    QVERIFY2(console.contains(QStringLiteral("[hook] step at nil")),
             qPrintable(console.join(QLatin1Char('|'))));
}

/// What a run reports: the fields an agent reads (`hook` verb, `pist_hook` tool,
/// the `hook` event) and the sentence the dialog shows, rendered from the same
/// record. A field that drifts from the others is a wrong answer given
/// confidently, so the numbers here are the script's own effects.
void TstStopHooks::theRunIsReportedInFieldsAndToWatchers()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    const QString path = writeScript(dir, QStringLiteral("log(\"poking\")\n"
                                                        "writemem(0x100, 1)\n"
                                                        "writemem(0x101, 2)"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    QList<StopHookRunner::LastRun> runs;
    connect(&runner, &StopHookRunner::runFinished, this,
            [&runs](const StopHookRunner::LastRun &run) { runs << run; });

    MachineState state;
    state.regs.valid = true;
    state.pc = 0x12596;
    runner.onStopPublished(state);
    QTest::qWait(150);

    QCOMPARE(runs.size(), 1);
    const StopHookRunner::LastRun run = runs.first();
    QVERIFY(run.ran);
    QVERIFY(run.ok);
    QVERIFY(!run.timedOut);
    QCOMPARE(run.pc, 0x12596u);
    QCOMPARE(run.file, QStringLiteral("hook.lua"));
    QCOMPARE(run.logs, 1);
    QCOMPARE(run.writes, 2);
    QVERIFY2(run.message.isEmpty(), qPrintable(run.message));

    QCOMPARE(runner.lastRun().ran, true);
    QCOMPARE(runner.lastRun().writes, 2);
    QVERIFY2(runner.lastRunSummary().contains(QStringLiteral("2 write(s)")),
             qPrintable(runner.lastRunSummary()));

    // The event's line, pinned whole: this is what an agent parses, and a field
    // that quietly changes shape here breaks it without breaking anything that
    // compiles.
    QCOMPARE(hookEventDetail(run),
             QStringLiteral("ok file=hook.lua line=-1 writes=2 logs=1 pc=0x00012596"));
}

/// A script that raises carries the line and its own words; a script that could
/// not be read ran *nothing*, which `ran` distinguishes from a failed run. An
/// agent that could not tell those apart would report "your script raised" for a
/// typo in a path.
void TstStopHooks::aFailedRunAndAnUnreadableScriptAreToldApart()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    const QString broken =
        writeScript(dir, QStringLiteral("local x = nil\nlog(x.field)"),
                    QStringLiteral("broken.lua"));
    const QString absent = dir.filePath(QStringLiteral("absent.lua"));

    QString path = broken;
    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    QList<StopHookRunner::LastRun> runs;
    connect(&runner, &StopHookRunner::runFinished, this,
            [&runs](const StopHookRunner::LastRun &run) { runs << run; });

    MachineState state;
    state.regs.valid = true;

    runner.onStopPublished(state);
    QTest::qWait(150);
    QCOMPARE(runs.size(), 1);
    StopHookRunner::LastRun run = runs.first();
    QVERIFY(run.ran);
    QVERIFY(!run.ok);
    QCOMPARE(run.line, 2);
    QCOMPARE(run.file, QStringLiteral("broken.lua"));
    QVERIFY2(!run.message.isEmpty(), "the script's own words");
    QVERIFY2(runner.lastRunSummary().contains(QStringLiteral("line 2")),
             qPrintable(runner.lastRunSummary()));

    // The script's own words are the engine's, so this pins the shape and the
    // field that answers "where" rather than the message Lua happens to print.
    const QString detail = hookEventDetail(run);
    QVERIFY2(detail.startsWith(QStringLiteral("error file=broken.lua line=2 ")),
             qPrintable(detail));
    QVERIFY2(detail.contains(QStringLiteral(" message=")), qPrintable(detail));

    path = absent;
    runs.clear();
    runner.onStopPublished(state);
    QTest::qWait(150);
    QCOMPARE(runs.size(), 1);
    run = runs.first();
    QVERIFY(!run.ran);
    QVERIFY(!run.ok);
    QCOMPARE(run.file, QStringLiteral("absent.lua"));
    QVERIFY2(!run.message.isEmpty(), "why it could not run");

    // `norun` is what keeps an unreadable file from looking like a raising
    // script: the status token says nothing ran.
    const QString missingDetail = hookEventDetail(run);
    QVERIFY2(missingDetail.startsWith(QStringLiteral("norun file=absent.lua ")),
             qPrintable(missingDetail));
    QVERIFY2(missingDetail.contains(QStringLiteral(" message=")), qPrintable(missingDetail));
}

void TstStopHooks::aBreakpointScriptOverridesTheProjectHook()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    const QString project = writeScript(dir, QStringLiteral("log(\"project\")"));
    const QString breakpoint =
        writeScript(dir, QStringLiteral("log(\"breakpoint\")"), QStringLiteral("bp.lua"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&project] { return project; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [] {};
    host.refresh = [] {};
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    MachineState state;
    state.regs.valid = true;

    StopTrigger hit;
    hit.reason = StopReason::Breakpoint;
    hit.at = QStringLiteral("hello.s:5");
    hit.script = breakpoint;
    runner.onStopPublished(state, hit);
    QTest::qWait(150);
    // Exactly one script ran, and it was the breakpoint's.
    QCOMPARE(console, QStringList{QStringLiteral("[hook] breakpoint")});

    // With no script on the breakpoint, the project's hook is what runs.
    console.clear();
    StopTrigger plain;
    plain.reason = StopReason::Breakpoint;
    runner.onStopPublished(state, plain);
    QTest::qWait(150);
    QCOMPARE(console, QStringList{QStringLiteral("[hook] project")});

    // And the project's switch does not gate a breakpoint's own script: it is
    // enabled by being attached, which is what the dialog's checkbox says it is
    // for.
    console.clear();
    host.enabled = [] { return false; };
    StopHookRunner offRunner(host);
    offRunner.onStopPublished(state, hit);
    QTest::qWait(150);
    QCOMPARE(console, QStringList{QStringLiteral("[hook] breakpoint")});

    // ...while the project's own hook is off, as before.
    console.clear();
    offRunner.onStopPublished(state, plain);
    QTest::qWait(150);
    QVERIFY(console.isEmpty());
}

void TstStopHooks::anotherCommandsCompletionIsNotTheHooks()
{
    QTemporaryDir dir;
    FakeBackend backend;
    QStringList console;
    QStringList events;
    const QString path = writeScript(dir, QStringLiteral("writemem(0x200, 9)"));

    StopHookRunner::Host host;
    host.backend = [&backend] { return &backend; };
    host.log = [&console](const QString &line) { console << line; };
    host.scriptPath = [&path] { return path; };
    host.enabled = [] { return true; };
    host.autoContinue = [] { return false; };
    host.resume = [&events] { events << QStringLiteral("resume"); };
    host.refresh = [&events] { events << QStringLiteral("refresh"); };
    host.holdConsole = [](bool) {};
    StopHookRunner runner(host);

    // The hook's write is issued but never acknowledged by the backend; while it
    // waits, a toolbar Step completes. Counting that as the hook's answer would
    // report a write the debugger never performed.
    backend.holdCommands = true;
    MachineState state;
    state.regs.valid = true;
    runner.onStopPublished(state);
    QTest::qWait(50);

    backend.completeForeign(QStringLiteral("s"));
    QTest::qWait(50);
    QVERIFY2(console.join(QLatin1Char('|')).contains(QStringLiteral("did not acknowledge")),
             qPrintable(console.join(QLatin1Char('|'))));

    // The hook's own completion still counts: the correlation is by text, not a
    // count of completions.
    console.clear();
    backend.events.clear();
    backend.holdCommands = false;
    StopHookRunner second(host);
    second.onStopPublished(state);
    QTest::qWait(150);
    QVERIFY(!console.join(QLatin1Char('|')).contains(QStringLiteral("did not acknowledge")));
    QCOMPARE(backend.memory.value(0x200), quint8(9));
}

QTEST_MAIN(TstStopHooks)
#include "tst_stophooks.moc"
