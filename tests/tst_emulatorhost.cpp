// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development
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

#include "emu/EmulatorHost.h"
#include "emu/HatariProbe.h"
#include "emu/SessionConfig.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
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
    const TosRom chosen = selectPreferredRom(roms);
    if (chosen.path.isEmpty() || !chosen.supportsAutostart())
        return {};
    return chosen.path;
}

} // namespace

class TstEmulatorHost : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void debuggerStopsAtProgramEntry();
    void registersRoundTrip();
    void basepageReportsProgramSections();
    void disassemblyIsLabelled();
    void steppingAdvancesPc();
    void runsWithoutControlSocket();
    void breaksInOnIllegalInstruction();
    void doesNotBreakInOnNormalRun();

private:
    QString m_hatari;
    QString m_vasm;
    QString m_tos;
    QString m_program;
    QString m_sourceDir;
    QTemporaryDir *m_work = nullptr;
};

void TstEmulatorHost::initTestCase()
{
    m_hatari = QStandardPaths::findExecutable(QStringLiteral("hatari"));
    m_vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    m_tos = findTos();

    if (m_hatari.isEmpty() || m_vasm.isEmpty() || m_tos.isEmpty()) {
        QSKIP("needs hatari, vasmm68k_mot, and a TOS ROM 1.04+ (set $PIST_TOS_DIR if needed)");
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

void TstEmulatorHost::debuggerStopsAtProgramEntry()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    QVERIFY(caps.valid);

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s1");
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;

    QString error;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, &error);
    QVERIFY2(!config.bootstrapScriptPath.isEmpty(), qPrintable(error));

    EmulatorHost host;
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
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
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

void TstEmulatorHost::basepageReportsProgramSections()
{
    HatariCapabilities caps = probeHatari(m_hatari);
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.sessionDir = m_work->path() + QStringLiteral("/s3");
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
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
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
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
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");
    config.gemdosDir = m_sourceDir;
    config.bootstrapScriptPath = EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);

    EmulatorHost host;
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

QTEST_MAIN(TstEmulatorHost)
#include "tst_emulatorhost.moc"
