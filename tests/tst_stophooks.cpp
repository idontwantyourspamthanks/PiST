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
    void breakAtAddressOnce(quint32 address) override { Q_UNUSED(address); }

    void writeMemoryByte(quint32 address, quint8 value) override
    {
        events << QStringLiteral("write %1=%2").arg(address).arg(value);
        memory.insert(address, value);
        complete();
    }

    void writeRegister(const QString &name, quint32 value) override
    {
        events << QStringLiteral("write %1=$%2").arg(name).arg(value, 0, 16);
        if (name == QLatin1String("d0"))
            state.regs.d[0] = value;
        complete();
    }

    void readDisassembly() override {}
    void readDisassemblyAt(quint32 address) override { Q_UNUSED(address); }
    void readHistory(int count) override { Q_UNUSED(count); }
    BackendKind kind() const override { return BackendKind::Native; }

private:
    /// The command's completion, one turn later — the transport's prompt.
    void complete()
    {
        if (holdCommands)
            return;
        QTimer::singleShot(0, this, [this] {
            events << QStringLiteral("ack");
            emit commandFinished(QStringLiteral("(fake)"), QString());
        });
    }
};

/// A script file in a temporary directory, and its path.
QString writeScript(QTemporaryDir &dir, const QString &source)
{
    const QString path = dir.filePath(QStringLiteral("hook.lua"));
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
    void raisesAnApiRefusalToTheScript();
    void refusesBadArgumentsByName();
    void stopsARunawayScript();
    void reportsAMissingFile();
    void offersContinueAndResume();
    void reportsTheLineAnErrorCameFrom();

    // --- the runner ------------------------------------------------------

    void writesCompleteBeforeTheContinueItAsksFor();
    void holdsTheConsoleForTheRun();
    void doesNotContinueWhenAutoContinueIsOff();
    void answersRegistersFromTheStopAndRefreshesAfterAWrite();
    void refusesToRunWhileTheMachineIsRunning();
    void doesNotContinueWhenTheDebuggerDoesNotAnswer();
    void doesNothingWhenTheHookIsOff();
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

QTEST_MAIN(TstStopHooks)
#include "tst_stophooks.moc"
