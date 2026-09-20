// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Integration test for EmulatorHost against a real Hatari.
//
// This covers the part of the design with the least margin for error: the
// command queue is framed on the debugger prompt, which is the only reliable
// end-of-command signal available (the socket is starved while stopped, and
// stdout/stderr have no ordering guarantee). If that framing is wrong, commands
// are silently attributed to the wrong response.
//
// Skips when Hatari, a TOS ROM, or vasm is unavailable, so the suite stays
// runnable on a bare machine.

#include "build/LineMap.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "emu/EmulatorHost.h"
#include "emu/EmbedSocket.h"
#include "emu/HrdbBackend.h"
#include "emu/HatariProbe.h"
#include "emu/SessionConfig.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

namespace {

/// Choose a ROM that can actually autostart. Using the alphabetically first
/// image would pick TOS 1.02, which silently cannot run a program from the HD
/// directory (docs/PLAN.md §5 rule 3).
QString findTos()
{
    // Use exactly the resolver the application uses. Hard-coding a directory
    // here would make the only end-to-end emulator test skip on macOS and
    // Windows, which are two of the three target platforms.
    const QList<TosRom> roms = findTosRoms();
    const TosRom chosen = selectPreferredRom(roms, Machine::St);
    if (chosen.path.isEmpty() || !chosen.supportsAutostart())
        return {};
    return chosen.path;
}

QString liveInsertImage(const QString &fallbackDir)
{
    // The path that failed in the wild: spaces (and parentheses) that Hatari's
    // setopt strtok / unescaped hatari-option split apart.
    const QString magazine = QStringLiteral(
        "/home/ryan/Code/AtariST/ST Format Magazine Issue 08 (1990-03)(Future Publishing).st");
    if (QFileInfo::exists(magazine))
        return magazine;

    const QString dest = fallbackDir + QStringLiteral(
        "/ST Format Magazine Issue 08 (1990-03)(Future Publishing).st");
    QFile img(dest);
    if (!img.open(QIODevice::WriteOnly))
        return {};
    QByteArray data(737280, '\0');
    data[0] = static_cast<char>(0x60);
    data[1] = static_cast<char>(0x1c);
    img.write(data);
    return dest;
}

} // namespace

class TstEmulatorHost : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void debuggerStopsAtProgramEntry();
    void registersRoundTrip();
    void stackDumpRoutesSeparatelyFromMemoryDump();
    void basepageReportsProgramSections();
    void disassemblyIsLabelled();
    void steppingAdvancesPc();
    void runsWithoutControlSocket();
    void breaksInOnIllegalInstruction();
    void doesNotBreakInOnNormalRun();
    void sourceLineBreakpointFiresAndResolvesBack();
    /// Continue at the entry stop used to discard unsent `b pc=` commands.
    void resumeFlushesPendingBreakpointCommands();
    /// A second session on the same host must re-frame cleanly: resetTransport()
    /// exists for this and no other test exercises it, as each builds a fresh host.
    void secondSessionOnOneHostReframesCleanly();
    /// Both backends reject a refresh with no session with exactly one error
    /// (native used to emit three — C3 drift 2). Emulator-free.
    void refreshWithoutSessionEmitsOneError();
    /// A split "<w>x<h>" report must complete, not emit the implausible partial
    /// (finding C9).
    void embedSocketCompletesSplitSizeReport();
    void watchpointFiresOnChangeAndNotOnSameValue();
    void floppyIsMountedInTheEmulator();
    /// Sidebar Change while stopped at entry must reach Hatari via `setopt`,
    /// not `hatari-option` (the control socket is unread then).
    void floppyInsertsWhileStopped();
    /// The same Change while the program is running goes over hatari-option,
    /// which must keep spaces in the image path (`\ `).
    void floppyInsertsWhileRunning();
    /// Pause must enter the debugger on a running program (a hatari-debug
    /// one-shot breakpoint over the control socket), not merely halt the VBL
    /// loop — hatari-stop alone would wedge the session with no prompt.
    void pauseStopsARunningProgram();

private:
    QString m_hatari;
    QString m_vasm;
    QString m_tos;
    QString m_program;
    QString m_sourceDir;
    QTemporaryDir *m_work = nullptr;
    QStringList m_log;
};

void TstEmulatorHost::initTestCase()
{
    m_hatari = QStandardPaths::findExecutable(QStringLiteral("hatari"));
    m_vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    m_tos = findTos();

    if (m_hatari.isEmpty() || m_vasm.isEmpty() || m_tos.isEmpty()) {
        QStringList missing;
        if (m_hatari.isEmpty())
            missing << QStringLiteral("hatari");
        if (m_vasm.isEmpty())
            missing << QStringLiteral("vasmm68k_mot");
        if (m_tos.isEmpty())
            missing << QStringLiteral("a TOS/EmuTOS ROM 1.04+");
        const QString what = missing.join(QStringLiteral(", "));

        // Skipping is the right default on a developer's machine, which may
        // legitimately have none of this. But a skip in CI is a *lie*: every test
        // in this suite would report as skipped and the run would look green while
        // the emulator integration — the most fragile part of the project, and the
        // part that has never been verified automatically — went untested. CI sets
        // this variable so that a missing prerequisite fails loudly instead.
        if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR")) {
            QFAIL(qPrintable(QStringLiteral(
                "PIST_REQUIRE_EMULATOR is set but the emulator integration cannot run: "
                "missing %1").arg(what)));
        }
        QSKIP(qPrintable(QStringLiteral("needs %1 (set $PIST_TOS_DIR if needed)").arg(what)));
    }

    m_work = new QTemporaryDir;
    QVERIFY(m_work->isValid());
    m_sourceDir = m_work->path();

    // A program with a loop and a string, so there is something to step through
    // and a label to resolve.
    const QString source = m_sourceDir + QStringLiteral("/hello.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmove.l\t#1,d0\n"
              "\tmove.l\t#2,d1\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "msg:\tdc.b\t\"HI\",0\n"
              "\teven\n"
              "\tend\n");
    src.close();

    m_program = m_sourceDir + QStringLiteral("/hello.prg");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"), QStringLiteral("-o"),
                        m_program, source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_program));
}

// QtTest calls this after every test, before the next one. Dumping the captured
// dialogue only on failure keeps successful runs quiet while making a failure
// explain itself — which matters here because the transport is a conversation
// with another program, and an assertion alone says nothing about what it said.
void TstEmulatorHost::cleanup()
{
    if (QTest::currentTestFailed() && !m_log.isEmpty()) {
        qWarning().noquote() << "--- emulator dialogue ---";
        // Bounded, so a pathological run cannot flood the log.
        const int start = qMax(0, m_log.size() - 60);
        for (int i = start; i < m_log.size(); ++i)
            qWarning().noquote() << "   " << m_log.at(i);
    }
    m_log.clear();
}

/// The control socket path for a session, or empty when this Hatari build has
/// no socket (stock Windows builds): MainWindow gates the option on exactly
/// this probe, and passing it to a socketless build fails the launch.
static QString controlSocketFor(const HatariCapabilities &caps, const QString &sessionDir)
{
    return caps.hasControlSocket ? sessionDir + QStringLiteral("/ctl.sock") : QString();
}

void TstEmulatorHost::debuggerStopsAtProgramEntry()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY(caps.valid);

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s1");
    config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
    config.gemdosDir = m_sourceDir;

    QString error;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, &error);
    QVERIFY2(!config.bootstrapScriptPath.isEmpty(), qPrintable(error));

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QStringList log;
    connect(&host, &EmulatorHost::logLine, this,
            [&log](const QString &l) { log.append(l); });
    connect(&host, &EmulatorHost::errorOccurred, this,
            [&log](const QString &l) { log.append(QStringLiteral("[err] ") + l); });

    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QVERIFY2(host.start(config, &error), qPrintable(error));

    // The bootstrap breakpoint is armed for program entry, so the debugger must
    // stop without any further interaction.
    const bool stopped = stoppedSpy.wait(15000);
    if (!stopped) {
        // Surface the emulator's own output; without it a failure here looks
        // like a hung test rather than a specific emulator error.
        qWarning().noquote() << "emulator log:\n" << log.join(QLatin1Char('\n'));
    }
    QVERIFY2(stopped, "debugger never stopped at program entry");
    QVERIFY(host.isStopped());

    host.stop();
}

void TstEmulatorHost::registersRoundTrip()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s2");
    config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(15000), "debugger never stopped");

    host.command(QStringLiteral("r"));
    QVERIFY2(finished.wait(15000), "no response to 'r'");

    const QList<QVariant> args = finished.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("r"));
    const QString response = args.at(1).toString();

    // The response must contain a real register dump, proving the command's
    // output was attributed to the right command.
    QVERIFY2(response.contains(QLatin1String("SR=")),
             qPrintable("register dump missing SR: " + response.left(300)));
    QVERIFY(response.contains(QLatin1String("D0")));

    host.stop();
}

void TstEmulatorHost::stackDumpRoutesSeparatelyFromMemoryDump()
{
    // The stack view and the memory view both refresh on a stop, so their dumps
    // must be routed to different signals — a stack dump landing in
    // memoryDumpReady would make the memory view show the stack. The routing
    // travels with the queued command (src/emu/EmulatorHost.h Pending), so a
    // memory dump and a stack dump back-to-back each reach the right listener.
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/stackroute");
    config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy stackSpy(&host, &EmulatorHost::stackDumpReady);
    QSignalSpy memSpy(&host, &EmulatorHost::memoryDumpReady);
    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(15000), "debugger never stopped");

    host.requestStackDump(0x10000, 64);
    QVERIFY2(stackSpy.wait(15000), "no stackDumpReady");

    const QList<QVariant> args = stackSpy.first();
    QCOMPARE(args.at(0).toUInt(), 0x10000u);
    // The memory view's channel must not have fired for a stack dump.
    QVERIFY2(memSpy.isEmpty(), "stack dump leaked into memoryDumpReady");

    host.stop();
}

void TstEmulatorHost::basepageReportsProgramSections()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s3");
    config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(15000), "debugger never stopped");

    // `symbols prg` relocates against the live basepage; it must find symbols.
    host.command(QStringLiteral("symbols prg"));
    QVERIFY2(finished.wait(15000), "no response to 'symbols prg'");
    const QString symResponse = finished.takeFirst().at(1).toString();
    QVERIFY2(symResponse.contains(QLatin1String("symbols")),
             qPrintable("no symbol load reported: " + symResponse.left(300)));

    host.command(QStringLiteral("info basepage"));
    QVERIFY2(finished.wait(15000), "no response to 'info basepage'");
    const QString bp = finished.takeFirst().at(1).toString();
    QVERIFY2(bp.contains(QLatin1String("Text segment")), qPrintable("no basepage: " + bp.left(300)));

    // The parsed state must now know the program's load address, which is what
    // source-line mapping depends on.
    QTRY_VERIFY_WITH_TIMEOUT(last.hasBases(), 5000);
    QVERIFY(last.textBase != 0);

    host.stop();
}

void TstEmulatorHost::disassemblyIsLabelled()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s4");
    config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(15000), "debugger never stopped");

    host.command(QStringLiteral("symbols prg"));
    QVERIFY(finished.wait(15000));
    finished.clear();

    host.command(QStringLiteral("d"));
    QVERIFY2(finished.wait(15000), "no response to 'd'");

    QTRY_VERIFY_WITH_TIMEOUT(!last.disassembly.isEmpty(), 5000);
    QVERIFY(last.disassembly.size() > 1);

    // vasm's `move.l #1,d0` / `move.l #2,d1` must appear as real instructions.
    bool sawMove = false;
    for (const DisasmLine &dl : last.disassembly) {
        if (dl.instruction.startsWith(QLatin1String("move")))
            sawMove = true;
    }
    QVERIFY2(sawMove, "no move instructions in disassembly");

    host.stop();
}

void TstEmulatorHost::steppingAdvancesPc()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s5");
    config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(15000), "debugger never stopped");

    host.command(QStringLiteral("r"));
    QVERIFY(finished.wait(15000));
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    const quint32 firstPc = last.pc;
    QVERIFY(firstPc != 0);

    // Single-step once; the PC must move forward.
    host.step();
    host.command(QStringLiteral("r"));
    QVERIFY(finished.wait(15000));

    QTRY_VERIFY_WITH_TIMEOUT(last.pc != firstPc, 5000);
    QVERIFY2(last.pc > firstPc, "PC did not advance after a single step");

    host.stop();
}

// Windows parity: Hatari's control socket is compiled only under
// HAVE_UNIX_DOMAIN_SOCKETS, so on Windows the IDE must run a full session with
// no socket at all. The integration suite runs on Linux, so the socket is
// simply left unset here, which exercises the same code path.
void TstEmulatorHost::runsWithoutControlSocket()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/nosock");
    config.gemdosDir = m_sourceDir;
    // Deliberately empty: this is what a Windows session looks like.
    config.controlSocketPath.clear();
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    QVERIFY(!config.toArgv().contains(QStringLiteral("--control-socket")));

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY2(host.start(config, nullptr), "a session must start without a control socket");
    QVERIFY2(stoppedSpy.wait(15000), "debugger never stopped without a control socket");

    // The debug transport is stdin/stderr, so it must work identically.
    host.command(QStringLiteral("r"));
    QVERIFY2(finished.wait(15000), "no response to 'r' without a control socket");
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    QVERIFY(last.pc != 0);

    host.stop();
}

// A program that executes an illegal instruction must break into the debugger
// on its own, with no breakpoint set. This is the portable equivalent of
// "break in when something goes wrong", and it works on Windows too because it
// uses --debug-except rather than the control socket.
void TstEmulatorHost::breaksInOnIllegalInstruction()
{
    // Assemble a program that faults, with no entry breakpoint so it runs free
    // until the fault.
    const QString source = m_sourceDir + QStringLiteral("/fault.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "\tillegal\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();
    const QString prg = m_sourceDir + QStringLiteral("/fault.prg");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"), QStringLiteral("-o"),
                        prg, source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);

    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY2(caps.hasDebugExcept, "this Hatari does not support --debug-except");

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = prg;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/fault");
    config.gemdosDir = m_sourceDir;
    config.controlSocketPath.clear(); // must work without a socket, as on Windows
    config.debugExceptions = QStringLiteral("autostart,illegal");
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);
    QVERIFY(config.toArgv().contains(QStringLiteral("--debug-except")));

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(20000), "illegal instruction did not break in");

    // The break-in must land on the faulting instruction, not somewhere in TOS:
    // no breakpoint was set, so this stop can only have come from the exception.
    host.command(QStringLiteral("r"));
    QVERIFY2(finished.wait(15000), "no response to 'r' after the break-in");
    const QString response = finished.takeFirst().at(1).toString();
    // The exception is reported on the faulting instruction, which appears in
    // the Prefetch line ("4afc (ILLEGAL)") and as the address after "Next PC".
    // Matching is case-insensitive because Hatari upper-cases the mnemonic in
    // the prefetch summary while the disassembly line uses lower case.
    QVERIFY2(response.contains(QLatin1String("illegal"), Qt::CaseInsensitive),
             qPrintable("the stop was not at an illegal instruction: " + response.left(400)));
    QVERIFY2(response.contains(QLatin1String("Next PC")),
             qPrintable("no next-PC line in the break-in dump: " + response.left(400)));

    host.stop();
}

// The default exception set must not break in on a healthy program, or the
// debugger would be unusable: arming `linea`/`linef`/`trace` would trip during
// ordinary TOS and VDI calls.
void TstEmulatorHost::doesNotBreakInOnNormalRun()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program; // prints and then spins in a loop
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/clean");
    config.gemdosDir = m_sourceDir;
    config.controlSocketPath.clear();
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);

    // Replace the bootstrap's entry breakpoint with nothing, so the program runs
    // from autostart through to its idle loop unattended.
    QFile boot(config.bootstrapScriptPath);
    QVERIFY(boot.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    boot.write("symbols autoload on\n");
    boot.close();

    QVERIFY(host.start(config, nullptr));

    // Give it well past the time a spurious break-in would take.
    QTest::qWait(6000);
    QVERIFY2(!host.isStopped(),
             "a healthy program must run without break-in under the default exception set");

    host.stop();
}

// The end-to-end claim: a breakpoint set on a *source line* is resolved to an
// address, armed, and then actually fires — and the resulting PC maps back to
// that same line. This is the whole debug loop, and until it is asserted the
// breakpoint and highlight behaviour are only partly proven: resolution is
// covered by tst_debug, but not that the resulting command hits.
void TstEmulatorHost::sourceLineBreakpointFiresAndResolvesBack()
{
    // A program with a distinctive loop to break on.
    const QString source = m_sourceDir + QStringLiteral("/line.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "\tmoveq\t#2,d1\n"
              "\tmoveq\t#3,d2\n"
              "loop:\tnop\n"
              "\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    const QString prg = m_sourceDir + QStringLiteral("/line.prg");
    const QString listing = m_sourceDir + QStringLiteral("/line.lst");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"),
                        QStringLiteral("-L"), listing, QStringLiteral("-o"), prg, source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);

    // Parse with the source path exactly as the build passed it, which is what
    // the real application does.
    ProgramLineMap map;
    QString mapError;
    QVERIFY2(map.addModule(source, prg, listing, &mapError), qPrintable(mapError));

    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = prg;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/line");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(20000), "no entry stop");

    // Phase two of the attach: load symbols and read the live section bases.
    host.command(QStringLiteral("symbols prg"));
    QVERIFY(finished.wait(15000));
    finished.clear();
    host.command(QStringLiteral("info basepage"));
    QVERIFY(finished.wait(15000));
    QTRY_VERIFY_WITH_TIMEOUT(last.hasBases(), 5000);
    finished.clear();

    LineMap::SectionBases bases;
    bases.text = last.textBase;
    bases.data = last.dataBase;
    bases.bss = last.bssBase;
    map.setLiveBases(bases);

    // `loop:` is line 5. Resolve it exactly as the gutter click does.
    QList<Breakpoint> bps;
    bps.append(Breakpoint{QFileInfo(source).fileName(), 5, QString(), true, 0, false});
    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 1);
    QVERIFY2(plan.unresolved.isEmpty(), "line 5 should have an address");
    const quint32 breakAddress = plan.armed.first().address;

    // Arm it, then let the program run: it must stop on its own.
    host.command(QStringLiteral("b all"));
    QVERIFY(finished.wait(15000));
    finished.clear();
    host.command(plan.commands.first());
    QVERIFY(finished.wait(15000));
    const QString armResponse = finished.takeFirst().at(1).toString();
    QVERIFY2(armResponse.contains(QLatin1String("breakpoint"), Qt::CaseInsensitive),
             qPrintable("breakpoint was not accepted: " + armResponse.left(300)));

    host.resume();

    // Wait on state rather than on the signal: subsequent debugger entries are
    // detected from the prompt, and the spy would already hold earlier entries.
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    host.command(QStringLiteral("r"));
    QVERIFY(finished.wait(15000));
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);

    // The PC must be exactly where the breakpoint was placed.
    QCOMPARE(last.pc, breakAddress);

    // ...and mapping that PC back through the line map must land on line 5,
    // which is what drives the editor highlight.
    LineMap::Address back;
    QVERIFY(map.lineFor(last.pc, &back));
    QCOMPARE(back.line, 5);
    QVERIFY2(LineMap::sameSource(back.file, QFileInfo(source).fileName()),
             qPrintable("highlight would not match the open file: " + back.file));

    host.stop();
}

void TstEmulatorHost::resumeFlushesPendingBreakpointCommands()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY(caps.valid);

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s-resume-arm");
    if (caps.hasControlSocket)
        config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY(host.start(config, nullptr));
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    host.command(QStringLiteral("r"));
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    const quint32 entry = last.pc;
    QVERIFY(entry != 0);

    // Queue the arm and a dump; do not wait for either. Resume must still send
    // the `b` — dropping the queue used to let Continue at entry miss it.
    host.command(QStringLiteral("b pc > $%1").arg(entry, 0, 16));
    host.requestMemoryDump(entry, 64);
    host.resume();

    // Without the `b`, this loops forever. Stopping again is the proof the arm
    // survived resume's queue flush.
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    host.stop();
}

void TstEmulatorHost::pauseStopsARunningProgram()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY(caps.valid);
    if (!caps.hasControlSocket)
        QSKIP("pause needs the control socket");

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s-pause");
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps,
                                                                    nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });

    QVERIFY(host.start(config, nullptr));
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    // The program loops forever (loop: bra.s loop). Resume, then pause: the
    // machine must end up stopped in the debugger again, with a readable PC.
    host.resume();
    QVERIFY(!host.isStopped());
    host.pause();
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 10000);

    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });
    host.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 10000);
    QVERIFY(last.pc != 0);
    host.stop();
}

// The project's only data-watch mechanism is the self-inequality condition
// `($addr).w ! ($addr).w` (debug/Watchpoint.h). The debugger substitutes the
// right side with the value it reads when the command is parsed and then fires
// when the two differ — breakcond.c marks an identical-sides inequality as
// "track value changes" (BreakCond_CheckTracking/BreakCond_UpdateTracked), so it
// fires once per change, not once per instruction. Both halves of that contract
// are asserted here against the real emulator, because until they are the
// premise is only ever string-tested (tst_remotecontrol pins the input
// validation) and no emulator test arms a watchpoint at all.
void TstEmulatorHost::watchpointFiresOnChangeAndNotOnSameValue()
{
    // Layout, by source line (the assertions below name these lines):
    //   3  lea    val(pc),a0
    //   4  move.w #$1111,(a0)   different value: 0 -> $1111, must stop
    //   5  move.w #$1111,(a0)   same value: must NOT stop
    //   6  lea    val2(pc),a1
    //   7  move.w #$2222,(a1)   a second watch's change, which bounds the run
    //   8  spin: bra.s spin
    //  10  val:  dc.w 0
    //  11  val2: dc.w 0
    // Both words are loaded as 0 from the .prg, so the store on line 4 is a
    // genuine change and the one on line 5 is not, deterministically.
    const QString source = m_sourceDir + QStringLiteral("/watch.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\n"
              "\tlea\tval(pc),a0\n"
              "\tmove.w\t#$1111,(a0)\n"
              "\tmove.w\t#$1111,(a0)\n"
              "\tlea\tval2(pc),a1\n"
              "\tmove.w\t#$2222,(a1)\n"
              "spin:\tbra.s\tspin\n"
              "\teven\n"
              "val:\tdc.w\t0\n"
              "val2:\tdc.w\t0\n"
              "\teven\n"
              "\tend\n");
    src.close();

    const QString prg = m_sourceDir + QStringLiteral("/watch.prg");
    const QString listing = m_sourceDir + QStringLiteral("/watch.lst");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"),
                        QStringLiteral("-L"), listing, QStringLiteral("-o"), prg, source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);

    ProgramLineMap map;
    QString mapError;
    QVERIFY2(map.addModule(source, prg, listing, &mapError), qPrintable(mapError));

    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = prg;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/watch");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState last;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&last](const MachineState &s) { last = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(20000), "no entry stop");

    // Phase two of the attach: load symbols and read the live section bases. The
    // watched words live in the program's text section, so the listing's offsets
    // plus the live text base give the addresses the watches need.
    host.command(QStringLiteral("symbols prg"));
    QVERIFY(finished.wait(15000));
    finished.clear();
    host.command(QStringLiteral("info basepage"));
    QVERIFY(finished.wait(15000));
    QTRY_VERIFY_WITH_TIMEOUT(last.hasBases(), 5000);
    finished.clear();

    LineMap::SectionBases bases;
    bases.text = last.textBase;
    bases.data = last.dataBase;
    bases.bss = last.bssBase;
    map.setLiveBases(bases);

    quint32 addrA = 0;    // line 4: changes val
    quint32 addrB = 0;    // line 5: writes the same value
    quint32 addrC = 0;    // line 7: changes val2
    quint32 addrSpin = 0; // line 8
    quint32 valAddr = 0;  // line 10
    quint32 val2Addr = 0; // line 11
    QVERIFY2(map.addressFor(source, 4, &addrA), "no address for the first store");
    QVERIFY2(map.addressFor(source, 5, &addrB), "no address for the same-value store");
    QVERIFY2(map.addressFor(source, 7, &addrC), "no address for the val2 store");
    QVERIFY2(map.addressFor(source, 8, &addrSpin), "no address for the idle loop");
    QVERIFY2(map.addressFor(source, 10, &valAddr), "no address for val");
    QVERIFY2(map.addressFor(source, 11, &val2Addr), "no address for val2");
    // A guard on the fixture's shape: if the program drifts, the stops below
    // could be attributed to the wrong instruction and pass by accident.
    QVERIFY2(valAddr % 2 == 0, "a .w watch needs an even address");
    QVERIFY2(addrA < addrB && addrB < addrC && addrC < addrSpin && addrSpin < valAddr,
             "the fixture's layout is not what the assertions assume");

    // Arm exactly as the IDE does: a Watchpoint rendered into its `b` command.
    Watchpoint valWatch;
    valWatch.address = valAddr;
    host.armBreakpoint(valWatch.command());
    QVERIFY2(finished.wait(15000), "no response to the val watchpoint");
    const QString valArm = finished.takeFirst().at(1).toString();
    QVERIFY2(valArm.contains(QLatin1String("breakpoint"), Qt::CaseInsensitive),
             qPrintable("the watchpoint was not accepted: " + valArm.left(300)));

    // A second watch, on the word the program changes next. It exists so the
    // same-value case is bounded by a real stop rather than a timeout: the
    // second run must reach it.
    Watchpoint val2Watch;
    val2Watch.address = val2Addr;
    host.armBreakpoint(val2Watch.command());
    QVERIFY2(finished.wait(15000), "no response to the val2 watchpoint");
    const QString val2Arm = finished.takeFirst().at(1).toString();
    QVERIFY2(val2Arm.contains(QLatin1String("breakpoint"), Qt::CaseInsensitive),
             qPrintable("the second watchpoint was not accepted: " + val2Arm.left(300)));
    finished.clear();

    // First run: the store on line 4 changes val, so the machine must stop.
    // Hatari evaluates conditional breakpoints after each instruction
    // (debugcpu.c DebugCpu_Check), so the stop reports the PC of the *next*
    // instruction — line 5, immediately after the storing one.
    host.resume();
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);
    last = MachineState();
    host.command(QStringLiteral("r"));
    QVERIFY2(finished.wait(15000), "no response to 'r' after the watchpoint hit");
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    QVERIFY2(last.pc == addrB,
             qPrintable(QStringLiteral("the changed value did not stop the machine at the "
                                       "instruction after the store: pc=$%1, expected $%2 "
                                       "(the store is at $%3)")
                            .arg(last.pc, 0, 16).arg(addrB, 0, 16).arg(addrA, 0, 16)));

    // Second run: the store on line 5 writes the same value, so it must not stop
    // — the next stop has to be the val2 watch' change on line 7, reported at the
    // loop on line 8. A stop at addrB would mean the same value re-fired; one at
    // addrC would mean the same-value write itself fired.
    host.resume();
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);
    last = MachineState();
    host.command(QStringLiteral("r"));
    QVERIFY2(finished.wait(15000), "no response to 'r' after the second stop");
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    QVERIFY2(last.pc == addrSpin,
             qPrintable(QStringLiteral("a write of the same value stopped the machine: pc=$%1, "
                                       "expected $%2 (the same-value store is at $%3)")
                            .arg(last.pc, 0, 16).arg(addrSpin, 0, 16).arg(addrB, 0, 16)));

    host.stop();
}

// The command line carrying --disk-a is not proof the disk mounted: Hatari
// validates images and can reject one. Assert on its own log line instead.
void TstEmulatorHost::floppyIsMountedInTheEmulator()
{
    // A blank 720K DOS-format image, written here rather than committed as a
    // binary fixture.
    const QString image = m_sourceDir + QStringLiteral("/blank.st");
    QFile img(image);
    QVERIFY(img.open(QIODevice::WriteOnly));
    QByteArray data(737280, '\0');
    data[0] = static_cast<char>(0x60);
    data[1] = static_cast<char>(0x1c);
    img.write(data);
    img.close();
    QVERIFY(QFileInfo::exists(image));

    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/floppy");
    config.gemdosDir = m_sourceDir;
    config.floppyImages = {image};
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QStringList log;
    connect(&host, &EmulatorHost::logLine, this,
            [&log](const QString &line) { log.append(line); });

    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(20000), "no entry stop");

    // Hatari reports each inserted image; a rejected one produces a WARN instead.
    bool inserted = false;
    for (const QString &line : log) {
        if (line.contains(QLatin1String("Inserted disk")))
            inserted = true;
    }
    QVERIFY2(inserted, qPrintable("the floppy was not mounted:\n" + log.join(QLatin1Char('\n'))));

    host.stop();
}

void TstEmulatorHost::floppyInsertsWhileStopped()
{
    const QString image = liveInsertImage(m_sourceDir);
    QVERIFY2(QFileInfo::exists(image), qPrintable(image));

    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/floppy-live");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QStringList log;
    connect(&host, &EmulatorHost::logLine, this,
            [&log](const QString &line) { log.append(line); });

    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(20000), "no entry stop");
    QVERIFY(host.isStopped());

    const QString staged = floppyImageForDebugger(config.sessionDir, 0, image);
    QVERIFY2(!staged.contains(QLatin1Char(' ')), qPrintable(staged));

    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    host.setFloppyImage(0, image);
    QVERIFY2(finished.wait(15000), "setopt --disk-a did not complete");

    bool inserted = false;
    for (const QString &line : log) {
        if (line.contains(QLatin1String("Inserted disk")))
            inserted = true;
    }
    QVERIFY2(inserted, qPrintable("live insert did not reach Hatari:\n" + log.join(QLatin1Char('\n'))));

    host.stop();
}

void TstEmulatorHost::floppyInsertsWhileRunning()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY(caps.valid);
    if (!caps.hasControlSocket)
        QSKIP("running insert needs the control socket");

    const QString image = liveInsertImage(m_sourceDir);
    QVERIFY2(QFileInfo::exists(image), qPrintable(image));

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/floppy-run");
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QStringList log;
    connect(&host, &EmulatorHost::logLine, this,
            [&log](const QString &line) { log.append(line); });

    QVERIFY(host.start(config, nullptr));
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    host.resume();
    QVERIFY(!host.isStopped());

    host.setFloppyImage(0, image);

    auto inserted = [&log] {
        for (const QString &line : log) {
            if (line.contains(QLatin1String("Inserted disk")))
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(inserted(), 15000);
    QVERIFY2(inserted(), qPrintable("running insert did not reach Hatari:\n" + log.join(QLatin1Char('\n'))));

    host.stop();
}

void TstEmulatorHost::secondSessionOnOneHostReframesCleanly()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY(caps.valid);

    const auto configFor = [this, &caps](const QString &name) {
        SessionConfig config;
        config.hatariPath = m_hatari;
        config.programPath = m_program;
        config.tosPath = m_tos;
        // Short leaf names: macOS's sun_path is 104 bytes, and QTemporaryDir
        // lives under the deep /var/folders $TMPDIR there — "second-session-1"
        // overflowed it and QLocalServer::listen failed (CI, macOS leg).
        config.sessionDir = m_work->path() + QLatin1Char('/') + name;
        config.controlSocketPath = controlSocketFor(caps, config.sessionDir);
        config.gemdosDir = m_sourceDir;
        config.bootstrapScriptPath
            = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);
        return config;
    };

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });

    // Session 1, left dirty on purpose, in the shape this class was actually
    // broken by once: arms queued, Continue pressed at the entry stop before
    // they flushed, then Stop. That leaves m_continueWhenIdle set — the one
    // piece of per-session framing that stop() does not clear and
    // resetTransport() does. Ending the session cleanly instead would leave
    // nothing the reset owns, and the test would pass with the reset deleted.
    QSignalSpy entry1(&host, &EmulatorHost::stoppedChanged);
    QVERIFY2(host.start(configFor(QStringLiteral("ss1")), nullptr),
             "session 1 failed to start");
    QVERIFY2(entry1.wait(15000), "session 1 never stopped at program entry");
    host.armBreakpoint(QStringLiteral("b pc = $100 && pc < $e00000 :once"));
    host.armBreakpoint(QStringLiteral("b pc = $108 && pc < $e00000 :once"));
    host.resume();
    host.stop();

    // Session 2 on the same object: a surviving m_continueWhenIdle makes the
    // queue-empty dispatch write `c`, so the new session resumes itself instead
    // of waiting stopped at the entry stop, and its first command never returns.
    QSignalSpy entry2(&host, &EmulatorHost::stoppedChanged);
    QVERIFY2(host.start(configFor(QStringLiteral("ss2")), nullptr),
             "session 2 failed to start");
    QVERIFY2(entry2.wait(15000), "session 2 never stopped at program entry");
    QVERIFY2(host.isStopped(),
             "session 2 resumed itself: session 1's continue request survived");

    QSignalSpy answered(&host, &EmulatorHost::commandFinished);
    host.command(QStringLiteral("r"));
    QVERIFY2(answered.wait(15000),
             "session 2's first command was never answered — session 1's "
             "transport state survived into it");

    // And the reply is session 2's own register dump rather than something
    // swallowed or attributed from session 1.
    const QList<QVariant> args = answered.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("r"));
    QVERIFY2(args.at(1).toString().contains(QStringLiteral("D0"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("unexpected reply: %1").arg(args.at(1).toString())));

    // A stale m_continueWhenIdle fires `c` the moment this command's reply
    // drains the queue — so the resume has to be caught after the answer and
    // not at the entry stop, which is why the check lives down here.
    QTest::qWait(1500);
    QVERIFY2(host.isStopped(),
             "session 2 resumed itself after its first command: session 1's "
             "continue request survived the transport reset");

    host.stop();
}

void TstEmulatorHost::refreshWithoutSessionEmitsOneError()
{
    // Both backends must reject a refresh with no session with exactly one error.
    // Native used to enqueue three snapshot commands, each emitting its own
    // "No emulator session is running." (C3 drift 2); the guard makes it one,
    // matching HrdbBackend. No process is ever started, so this needs no emulator.
    EmulatorHost native;
    QSignalSpy nativeErrors(&native, &IDebugBackend::errorOccurred);
    native.refresh();
    QCOMPARE(nativeErrors.count(), 1);

    HrdbBackend hrdb;
    QSignalSpy hrdbErrors(&hrdb, &IDebugBackend::errorOccurred);
    hrdb.refresh();
    QCOMPARE(hrdbErrors.count(), 1);
}

void TstEmulatorHost::embedSocketCompletesSplitSizeReport()
{
    const QString path = m_work->path() + QStringLiteral("/embed-split.sock");
    EmbedSocket server;
    QString error;
    QVERIFY2(server.listen(path, &error), qPrintable(error));

    QSignalSpy sizes(&server, &EmbedSocket::sizeReported);
    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected(2000));
    QTest::qWait(150); // let the server accept the connection

    // Feed the report split across two writes, giving the server a chance to read
    // the first before the second lands. "320x2" is an implausible height; the
    // old parser accepted it as a real 320x2 and cleared the buffer, so the
    // trailing "00" was lost and the reported size was wrong (finding C9).
    client.write("320x2");
    QVERIFY(client.waitForBytesWritten(2000));
    QTest::qWait(150);
    QCOMPARE(sizes.count(), 0); // held as a partial, not emitted

    client.write("00");
    QVERIFY(client.waitForBytesWritten(2000));
    QTest::qWait(150);
    QCOMPARE(sizes.count(), 1); // completed to 320x200
    if (sizes.count() == 1) {
        QCOMPARE(sizes.at(0).at(0).toInt(), 320);
        QCOMPARE(sizes.at(0).at(1).toInt(), 200);
    }

    // The unbounded-buffer half: a blob longer than any single report that never
    // parses must be dropped, so the next clean report still gets through.
    client.write(QByteArray(200, '1')); // no 'x' — never a size
    QVERIFY(client.waitForBytesWritten(2000));
    QTest::qWait(150);
    client.write("640x480");
    QVERIFY(client.waitForBytesWritten(2000));
    QTest::qWait(150);
    // Without the cap the 200 stray bytes stay and pollute "640x480" (its width
    // field becomes an unparseable 200-digit number); with it the blob is dropped
    // and the report parses, so a second size is reported.
    QCOMPARE(sizes.count(), 2);
    if (sizes.count() == 2) {
        QCOMPARE(sizes.at(1).at(0).toInt(), 640);
        QCOMPARE(sizes.at(1).at(1).toInt(), 480);
    }
}

QTEST_MAIN(TstEmulatorHost)
#include "tst_emulatorhost.moc"
