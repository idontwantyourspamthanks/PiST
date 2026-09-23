// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Integration test for HrdbBackend against the HRDB fork of Hatari
// (tattlemuss/hatari, branch hrdb-main).
//
// The fork is not a bundled or system tool, so the suite is gated on
// $PIST_HRDB_HATARI naming the fork's binary, plus a resolvable
// vasmm68k_mot and a TOS ROM — the same prerequisites as the upstream
// emulator suites; without any one of them every test skips. CI builds the
// fork itself (ci.yml's "Build Hatari (hrdb fork)" leg) and runs this suite
// with PIST_HRDB_HATARI set, so it is only developer machines that skip it.
// PIST_REQUIRE_EMULATOR deliberately does not apply here: that contract is
// about upstream Hatari.
//
// The transport is framed (NUL-terminated messages, 0x01 field separators,
// `!`-prefixed async notifications) rather than prompt-scraped, but it has
// its own hazard: notifications interleave with replies on the one stream, so
// a mis-demultiplexed reply would be attributed to the wrong command. That is
// what these tests exist to catch.

#include "emu/DebugBackend.h"
#include "emu/HrdbBackend.h"
#include "emu/MemoryDump.h"
#include "emu/SessionConfig.h"
#include "emu/TosRom.h"

#include "emu/EmulatorHost.h"
#include "emu/HatariProbe.h"
#include "emu/EmbedSocket.h"
#include <QLocalSocket>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

namespace {

QString findTos()
{
    const QList<TosRom> roms = findTosRoms();
    const TosRom chosen = selectPreferredRom(roms, Machine::St);
    if (chosen.path.isEmpty() || !chosen.supportsAutostart())
        return {};
    return chosen.path;
}

/// A stand-in for the fork *process*: it only has to start and stay alive,
/// because the protocol is the socket, and the test plays the fork's end of it
/// with a QTcpServer. Once the trigger file exists it writes the disassembly a
/// `console d` answers with — on stderr, where the fork's console output goes —
/// and then says so on stdout, which the backend logs: that is the test's signal
/// that the bytes are in the pipe. It stays silent afterwards, so a later
/// command's stderr tail is empty, which is what a `dspbreak` ack looks like.
///
/// The jsr sits at the address the register reply reports for PC, so the
/// snapshot marks it as the current instruction and stepOver() can read the
/// fall-through address from the line after it.
QString writeFakeFork(const QString &dir, const QString &triggerPath)
{
    const QString trigger = QStringLiteral("while [ ! -f '%1' ]; do sleep 0.02; done\n")
                                .arg(triggerPath);
    QFile f(dir + QStringLiteral("/fake-fork.sh"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    f.write("#!/bin/sh\n");
    f.write(trigger.toUtf8());
    f.write("printf '$00010000 4e71 nop\\n' >&2\n"
            "printf '$00010002 6104 bsr.s $10008\\n' >&2\n"
            "printf '$00010008 4e71 nop\\n' >&2\n"
            "printf 'fake-fork-disassembly-sent\\n'\n"
            "sleep 60\n");
    f.close();
    QFile::setPermissions(f.fileName(),
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return f.fileName();
}

/// Wait for the next NUL-terminated command the backend writes, i.e. the next
/// command it dispatches. One command is outstanding at a time on this
/// transport, so the order the test reads here is the order it was sent in.
QByteArray nextCommand(QTcpSocket *fork, int timeoutMs = 5000)
{
    QByteArray buf;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        // Pump the event loop rather than blocking on this socket: the backend
        // runs in the same thread, so its outgoing bytes are only flushed (and
        // our replies only read) while its event loop runs.
        QTest::qWait(5);
        buf += fork->readAll();
        const int nul = buf.indexOf('\0');
        if (nul >= 0)
            return buf.left(nul);
    }
    return {};
}

/// Answer the command just read, the way the fork does: a 0x01-separated
/// message, NUL-terminated.
void reply(QTcpSocket *fork, const QByteArray &fields)
{
    fork->write(fields + '\0');
    fork->flush();
    fork->waitForBytesWritten(1000);
}

bool commandAnswered(QSignalSpy *finished, const QString &command, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        for (const auto &args : *finished)
            if (args.at(0).toString() == command)
                return true;
        finished->wait(100);
    }
    return false;
}

bool sawLogLine(const QSignalSpy &logged, const QString &text)
{
    for (const auto &args : logged)
        if (args.at(0).toString() == text)
            return true;
    return false;
}

} // namespace

class TstHrdb : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void entryStopArrivesOverHrdb();
    void registersAndBasesFillState();
    void memoryDumpReturnsProgramBytes();
    void stepAdvancesPc();
    void stepOverSkipsSubroutine();
    void breakpointFiresAtArmedAddress();
    /// Pause must break into a running emulation over HRDB's `break`, which is
    /// the capability this backend exists to provide (the Windows pause gap).
    void pauseStopsARunningProgram();
    /// resume() must keep the `profile` control commands queued in the same
    /// stack frame (finding CRIT-6): profileToCursor arms its breakpoint, sends
    /// `profile on` and resumes synchronously, and the HRDB prune used to drop
    /// the profile command — so the guided run collected nothing while the UI
    /// reported "Collecting".
    void profileOnSurvivesResume();
    /// Sidebar Change while stopped must use `console setopt`, not
    /// hatari-option (SDL does not pump the control socket in the break loop).
    void floppyInsertsWhileStopped();

    /// The path the shipping build uses for memory editing: a `w b` write is
    /// translated to the fork's 3-arity `memset`, and the byte must read back
    /// changed. A regression here fails silently as an NG on the release path.
    void memoryWriteReadsBack();

    /// The shared control-socket helper parses a "<w>x<h>" report and ignores
    /// noise — the parse that sizes the embedded display.
    void embedSocketParsesSizeReports();

    /// The seam that was silently broken twice: a fork session with a control
    /// socket must deliver the video-size report as embeddedSizeChanged.
    void embedSizeReportArrivesOnForkSession();

    /// MAJ-12, HRDB side: the response type is carried by the request, never
    /// inferred from the command text. `db` is a dspbreak, and a free-text
    /// command's stderr tail used to be parsed as a disassembly when its text
    /// started with `d`: that cleared the cached snapshot stepOver() reads the
    /// fall-through address from — which silently degraded step-over to a plain
    /// step. The process and the socket protocol are supplied by the test, so
    /// this runs without the fork binary.
    void dspbreakCommandKeepsTheDisassemblyStepOverReads();

    /// Free text (the console and the remote `cmd` verb) reaches the fork's
    /// console as written — the prefix table that rewrote `s`/`c`/`b …`/`w b …`
    /// is gone — and a command that cannot run yet is reported instead of being
    /// held silently until the next stop (findings CRIT-6/MAJ-12/MAJ-21). Runs
    /// without the fork binary, like the test above.
    void freeTextIsPassedThroughAndItsDeferralIsAnnounced();

private:
    /// Start a session and wait for the entry stop. Fills `state` with the
    /// last snapshot (valid on success).
    bool startToEntry(HrdbBackend *host, MachineState *state);

    QString m_hatari;
    QString m_vasm;
    QString m_tos;
    QString m_program;
    QString m_sourceDir;
    QTemporaryDir *m_work = nullptr;
    int m_sessions = 0;
    QStringList m_log;
};

/// The work directory outlives the test functions by design, so it must be
/// released while QApplication is still alive: a QTemporaryDir destroyed after
/// its QCoreApplication can call into a platform plugin that is already gone
/// (MIN-67). cleanupTestCase runs before the app object is destroyed.
void TstHrdb::cleanupTestCase()
{
    delete m_work;
    m_work = nullptr;
}

void TstHrdb::initTestCase()
{
    m_hatari = qEnvironmentVariable("PIST_HRDB_HATARI");
    m_vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    m_tos = findTos();

    if (m_hatari.isEmpty() || !QFileInfo::exists(m_hatari) || m_vasm.isEmpty()
        || m_tos.isEmpty()) {
        QStringList missing;
        if (m_hatari.isEmpty() || !QFileInfo::exists(m_hatari))
            missing << QStringLiteral("the hrdb-main Hatari fork ($PIST_HRDB_HATARI)");
        if (m_vasm.isEmpty())
            missing << QStringLiteral("vasmm68k_mot");
        if (m_tos.isEmpty())
            missing << QStringLiteral("a TOS/EmuTOS ROM 1.04+");

        // Same contract as the upstream emulator suite (ci.yml): on an
        // emulator leg the fork is CI-provided, so a missing prerequisite is a
        // failure, not a skip — otherwise a broken fork path silently skips
        // the entire shipping HRDB surface with a green run.
        if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR")) {
            QFAIL(qPrintable(QStringLiteral(
                "PIST_REQUIRE_EMULATOR is set but the HRDB integration cannot run: "
                "missing %1").arg(missing.join(QStringLiteral(", ")))));
        }
        QSKIP(qPrintable(QStringLiteral("needs %1").arg(missing.join(QStringLiteral(", ")))));
    }

    m_work = new QTemporaryDir;
    QVERIFY(m_work->isValid());
    m_sourceDir = m_work->path();

    // One subroutine call, so step-over has something to skip.
    const QString source = m_sourceDir + QStringLiteral("/prog.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmove.l\t#1,d0\n"
              "\tbsr.s\tsub\n"
              "\tmove.l\t#3,d2\n"
              "loop:\tbra.s\tloop\n"
              "sub:\tmove.l\t#2,d1\n"
              "\trts\n"
              "\teven\n"
              "msg:\tdc.b\t\"HI\",0\n"
              "\teven\n"
              "\tend\n");
    src.close();

    m_program = m_sourceDir + QStringLiteral("/prog.prg");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"), QStringLiteral("-o"),
                        m_program, source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_program));
}

void TstHrdb::cleanup()
{
    if (QTest::currentTestFailed() && !m_log.isEmpty()) {
        qWarning().noquote() << "--- emulator dialogue ---";
        const int start = qMax(0, m_log.size() - 60);
        for (int i = start; i < m_log.size(); ++i)
            qWarning().noquote() << "   " << m_log.at(i);
    }
    m_log.clear();
}

bool TstHrdb::startToEntry(HrdbBackend *host, MachineState *state)
{
    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.machine = QStringLiteral("st");
    config.fastForward = true;
    config.sessionDir = m_work->path() + QStringLiteral("/s%1").arg(++m_sessions);
    config.gemdosDir = m_sourceDir;

    connect(host, &IDebugBackend::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    connect(host, &IDebugBackend::errorOccurred, this,
            [this](const QString &l) { m_log.append(QStringLiteral("[err] ") + l); });
    connect(host, &IDebugBackend::stateUpdated, this,
            [state](const MachineState &s) { *state = s; });

    // Same bootstrap the app writes, created BEFORE start: the fork runs
    // --parse at launch and its entry breakpoint fires into the remote break
    // loop, which then waits for our connect. Arming over the socket instead
    // races TOS boot.
    const HatariCapabilities caps = probeHatari(m_hatari);
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);
    if (config.bootstrapScriptPath.isEmpty())
        return false;

    QSignalSpy stoppedSpy(host, &IDebugBackend::stoppedChanged);
    if (!host->start(config, nullptr))
        return false;

    if (!stoppedSpy.wait(20000))
        return false;
    QSignalSpy stateSpy(host, &IDebugBackend::stateUpdated);
    host->refresh();
    if (!stateSpy.wait(10000))
        return false;
    return true;
}

/// Wait for a stopped(true) transition, tolerating the synchronous `false`
/// that resume()/stepOver() emit first.
static bool waitForStop(QSignalSpy *spy, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        spy->wait(250);
        for (const auto &args : *spy)
            if (args.at(0).toBool())
                return true;
    }
    return false;
}

void TstHrdb::entryStopArrivesOverHrdb()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    QVERIFY(host.isStopped());
    host.stop();
}

void TstHrdb::registersAndBasesFillState()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");

    QVERIFY2(state.regs.valid, "registers never became valid");
    // The entry stop is at the program's first instruction, which is also the
    // live text base — and the bases arrive as register keys, not by parsing
    // `info basepage` output.
    QVERIFY2(state.hasBases(), "no section bases in the regs reply");
    QCOMPARE(state.pc, state.textBase);
    QVERIFY2(!state.disassembly.isEmpty(), "no disassembly in the snapshot");
    host.stop();
}

void TstHrdb::memoryDumpReturnsProgramBytes()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    QVERIFY(state.hasBases());

    // The dump arrives parsed (MAJ-45): the backend decodes the fork's payload
    // into rows, so nothing here re-reads a transcript.
    QSignalSpy dumpSpy(&host, &IDebugBackend::memoryDumpReady);
    host.requestMemoryDump(state.textBase, 32, 0);
    QVERIFY2(dumpSpy.wait(10000), "no memory dump");
    const QList<MemoryRow> rows = dumpSpy.first().at(1).value<QList<MemoryRow>>();

    // vasm optimizes `move.l #1,d0` to `moveq` (verified in the test dump):
    // the program's first bytes are 70 01 61 04 (moveq #1,d0; bsr.b sub).
    // If the uuencoded payload were misread, these bytes would not match.
    QVERIFY2(rows.size() >= 2, qPrintable(QStringLiteral("rows: %1").arg(rows.size())));
    QCOMPARE(rows.first().address, state.textBase);
    const QVector<quint8> &b = rows.first().bytes;
    QCOMPARE(b.at(0), quint8(0x70));
    QCOMPARE(b.at(1), quint8(0x01));
    QCOMPARE(b.at(2), quint8(0x61));
    host.stop();
}

void TstHrdb::stepAdvancesPc()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    const quint32 pc0 = state.pc;

    host.step();
    host.refresh();
    QSignalSpy stateSpy(&host, &IDebugBackend::stateUpdated);
    QVERIFY2(stateSpy.wait(10000), "no state after step");
    QCOMPARE(state.pc, pc0 + 2);  // moveq #imm,d0 is 2 bytes
    host.stop();
}

void TstHrdb::stepOverSkipsSubroutine()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    const quint32 entry = state.pc;

    // Step once: past moveq #1,d0 (2 bytes) onto the bsr.b.
    {
        QSignalSpy freshSpy(&host, &IDebugBackend::stateUpdated);
        host.step();
        host.refresh();
        QVERIFY(freshSpy.wait(10000));
    }
    QCOMPARE(state.pc, entry + 2);

    // Step over the call: the backend must break at the fall-through, having
    // run the subroutine.
    QSignalSpy stopSpy(&host, &IDebugBackend::stoppedChanged);
    host.stepOver();
    QVERIFY2(waitForStop(&stopSpy, 10000), "step-over never stopped");
    QSignalSpy freshSpy2(&host, &IDebugBackend::stateUpdated);
    host.refresh();
    QVERIFY(freshSpy2.wait(10000));

    QCOMPARE(state.pc, entry + 2 + 2);  // bsr.b is 2 bytes; moveq #3,d2 next
    QCOMPARE(state.regs.d[1], quint32(2));  // the subroutine ran
    host.stop();
}

void TstHrdb::breakpointFiresAtArmedAddress()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    const quint32 entry = state.pc;

    // `loop:` is at entry+6 (2+2+2: moveq, bsr.b, moveq). The planning form
    // `b pc = $addr` maps to the fork's `bp` unchanged.
    const quint32 loop = entry + 6;
    host.armBreakpoint(QStringLiteral("b pc = $%1").arg(loop, 0, 16));

    QSignalSpy stopSpy(&host, &IDebugBackend::stoppedChanged);
    host.resume();
    QVERIFY2(waitForStop(&stopSpy, 10000), "breakpoint never fired");

    host.refresh();
    QSignalSpy stateSpy(&host, &IDebugBackend::stateUpdated);
    QVERIFY(stateSpy.wait(10000));
    QCOMPARE(state.pc, loop);
    host.stop();
}

void TstHrdb::profileOnSurvivesResume()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");

    QStringList errors;
    connect(&host, &IDebugBackend::errorOccurred, this,
            [&errors](const QString &m) { errors.append(m); });

    // Same address as breakpointFiresAtArmedAddress: `loop:` is at entry+6
    // (2+2+2: moveq, bsr.b, moveq).
    const quint32 loop = state.pc + 6;

    // MainWindow::profileToCursor's sequence exactly, in one synchronous stack
    // frame: the breakpoint at the cursor, collection on, continue. `profile on`
    // is already queued behind the arm's own round trip when resume() prunes,
    // which is where the HRDB predicate used to drop it.
    QSignalSpy finished(&host, &IDebugBackend::commandFinished);
    host.armBreakpoint(QStringLiteral("b pc = $%1").arg(loop, 0, 16));
    host.profileOn();
    host.resume();

    QSignalSpy stopSpy(&host, &IDebugBackend::stoppedChanged);
    QVERIFY2(waitForStop(&stopSpy, 10000), "breakpoint never fired");

    // A `console` command is answered with the fork's OK on the socket, which
    // surfaces as the command's completion — so a completion for `profile on`
    // is the fork having accepted it. A pruned command is never written, and no
    // completion for it can ever arrive.
    bool sawProfileOn = false;
    QElapsedTimer timer;
    timer.start();
    while (!sawProfileOn && timer.elapsed() < 10000) {
        finished.wait(250);
        for (const auto &args : finished)
            if (args.at(0).toString() == QLatin1String("profile on"))
                sawProfileOn = true;
    }
    QVERIFY2(sawProfileOn,
             qPrintable(QStringLiteral("profile on never reached the fork after resume(): %1")
                            .arg(errors.join(QStringLiteral(" | ")))));
    for (const QString &e : errors)
        QVERIFY2(!e.contains(QLatin1String("profile on")), qPrintable(e));

    host.stop();
}

void TstHrdb::pauseStopsARunningProgram()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");

    // Resume, then pause: the machine must end up stopped again with its PC
    // still inside the program (the loop at entry+6).
    host.resume();
    host.pause();
    QSignalSpy stopSpy(&host, &IDebugBackend::stoppedChanged);
    QVERIFY2(waitForStop(&stopSpy, 10000), "pause never stopped");
    host.refresh();
    QSignalSpy stateSpy(&host, &IDebugBackend::stateUpdated);
    QVERIFY(stateSpy.wait(10000));
    QVERIFY2(state.pc >= state.textBase && state.pc < state.textBase + 0x20,
             qPrintable(QStringLiteral("PC %1 outside program text")
                            .arg(state.pc, 0, 16)));
    host.stop();
}

void TstHrdb::floppyInsertsWhileStopped()
{
    const QString image = m_sourceDir
        + QStringLiteral("/ST Format Magazine Issue 08 (1990-03)(Future Publishing).st");
    QFile img(image);
    QVERIFY(img.open(QIODevice::WriteOnly));
    QByteArray data(737280, '\0');
    data[0] = static_cast<char>(0x60);
    data[1] = static_cast<char>(0x1c);
    img.write(data);
    img.close();

    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    QVERIFY(host.isStopped());

    QSignalSpy finished(&host, &IDebugBackend::commandFinished);
    host.setFloppyImage(0, image);
    QVERIFY2(finished.wait(15000), "console setopt --disk-a did not complete");

    bool inserted = false;
    for (const QString &line : m_log) {
        if (line.contains(QLatin1String("Inserted disk")))
            inserted = true;
    }
    QVERIFY2(inserted, qPrintable("live insert did not reach Hatari:\n" + m_log.join(QLatin1Char('\n'))));
    host.stop();
}

void TstHrdb::embedSocketParsesSizeReports()
{
    EmbedSocket server;
    QString error;
    const QString path = m_work->path() + QStringLiteral("/embed.sock");
    QVERIFY2(server.listen(path, &error), qPrintable(error));

    QSignalSpy spy(&server, &EmbedSocket::sizeReported);
    QLocalSocket client;
    client.connectToServer(path);
    QVERIFY(client.waitForConnected(5000));
    QTRY_VERIFY_WITH_TIMEOUT(server.connected(), 5000);

    client.write("640x436");
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.first().at(0).toInt(), 640);
    QCOMPARE(spy.first().at(1).toInt(), 436);


    // Noise is not a size.
    client.write("not-a-size");
    QTest::qWait(200);
    QCOMPARE(spy.size(), 1);
}

void TstHrdb::embedSizeReportArrivesOnForkSession()
{
    // The embed-size reports travel over the upstream control socket, which a
    // Windows build of the fork does not have — so there is nothing to test
    // there, and passing the option would fail the launch.
    const HatariCapabilities probe = probeHatari(m_hatari);
    if (!probe.hasControlSocket)
        QSKIP("embed-size reports need the control socket");

    SessionConfig config;
    config.hatariPath = m_hatari;
    config.programPath = m_program;
    config.tosPath = m_tos;
    config.machine = QStringLiteral("st");
    config.fastForward = true;
    config.sessionDir = m_work->path() + QStringLiteral("/s-embed");
    config.gemdosDir = m_sourceDir;
    config.controlSocketPath = config.sessionDir + QStringLiteral("/ctl.sock");

    HrdbBackend host;
    connect(&host, &IDebugBackend::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    const HatariCapabilities caps = probeHatari(m_hatari);
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(config.sessionDir, caps, nullptr);
    QVERIFY(!config.bootstrapScriptPath.isEmpty());

    QSignalSpy embedSpy(&host, &IDebugBackend::embeddedSizeChanged);
    QVERIFY(host.start(config, nullptr));

    // The fork answers hatari-embed-info while running — no display needed.
    QVERIFY2(embedSpy.wait(20000), "no video-size report on the control socket");
    QVERIFY(embedSpy.first().at(0).toInt() > 0);
    QVERIFY(embedSpy.first().at(1).toInt() > 0);
    host.stop();
}

void TstHrdb::memoryWriteReadsBack()
{
    HrdbBackend host;
    MachineState state;
    QVERIFY2(startToEntry(&host, &state), "no entry stop over HRDB");
    QVERIFY(state.hasBases());

    // Write through the same request the UI's memory editing makes. The first
    // instruction byte at TEXT is 0x70 (moveq); write 0x5a.
    host.writeMemoryByte(state.textBase, 0x5a);
    QSignalSpy finished(&host, &IDebugBackend::commandFinished);
    QVERIFY2(finished.wait(10000), "write never answered");

    // The read-back arrives parsed (MAJ-45), the same rows the memory pane
    // would show.
    QSignalSpy dumpSpy(&host, &IDebugBackend::memoryDumpReady);
    host.requestMemoryDump(state.textBase, 16, 0);
    QVERIFY2(dumpSpy.wait(10000), "no memory dump after write");
    const QList<MemoryRow> rows = dumpSpy.first().at(1).value<QList<MemoryRow>>();

    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows.first().bytes.at(0), quint8(0x5a));
    host.stop();
}

// MAJ-12 on the HRDB transport. `completeCurrent` decided what a response was
// from the command text: anything starting with `d` was parsed as a
// disassembly. A `db pc = $X :once` (a dspbreak) text therefore had its (empty)
// stderr tail parsed with parseDisassembly(), which starts by clearing the
// cached snapshot — the snapshot stepOver() reads the fall-through address from
// (it looks for the line marked as the current PC and takes the address after
// it). The result was a silent degradation to a plain step, and a stateUpdated
// publishing an empty disassembly. The text now never decides: only a typed
// disassembly read is parsed.
void TstHrdb::dspbreakCommandKeepsTheDisassemblyStepOverReads()
{
#ifdef Q_OS_WIN
    QSKIP("the fake fork is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake fork needs /bin/sh");

    // The fork binary is not needed for this one: the transport is the socket,
    // and the test answers its end. The fake process only supplies what the
    // socket cannot — the console output that travels on stderr — and it does
    // that on demand, so the response window of each command is exact.
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 56001))
        QSKIP("port 56001 is already held — another HRDB client or PiST session");

    const QString trigger = m_work->path() + QStringLiteral("/fake-fork-go");
    const QString script = writeFakeFork(m_work->path(), trigger);
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/s-fake-fork");
    config.machine = QStringLiteral("st");

    HrdbBackend host;
    connect(&host, &IDebugBackend::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy logged(&host, &IDebugBackend::logLine);
    QSignalSpy stateSpy(&host, &IDebugBackend::stateUpdated);
    QSignalSpy finished(&host, &IDebugBackend::commandFinished);
    MachineState state;
    connect(&host, &IDebugBackend::stateUpdated, this,
            [&state](const MachineState &s) { state = s; });

    QVERIFY(host.start(config, nullptr));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
    QTcpSocket *fork = server.nextPendingConnection();
    QVERIFY(fork);

    // Handshake, then the stop: field 1 of `!status` is 0 while the fork sits in
    // its remote break loop (the inverse of what the comment over
    // RemoteDebug_status suggests, verified live — docs/PLAN.md §9).
    reply(fork, QByteArray("!connected") + QByteArray(1, '\x01') + "100A");
    reply(fork, QByteArray("!status") + QByteArray(1, '\x01') + "0" + QByteArray(1, '\x01')
                    + "00010002");
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 5000);

    // refresh(): the register read is typed, the disassembly is the fork's
    // console text on stderr. The register reply puts PC on the jsr so the
    // snapshot marks the line the step-over contract needs.
    host.refresh();
    QCOMPARE(nextCommand(fork), QByteArray("regs"));
    QByteArray regs("OK");
    const char *const pairs[] = {"PC",   "00010002", "D0",   "00000001", "USP",  "00001000",
                                 "ISP",  "00002000", "SR",   "0000",     "TEXT", "00010000",
                                 "DATA", "00011000", "BSS",  "00012000"};
    for (const char *const pair : pairs)
        regs += QByteArray(1, '\x01') + pair;
    reply(fork, regs);
    QCOMPARE(nextCommand(fork), QByteArray("console d"));

    // Release the console output only now: it has to arrive after the command
    // was dispatched to be that command's response at all.
    {
        QFile go(trigger);
        QVERIFY(go.open(QIODevice::WriteOnly));
        go.write("go\n");
    }
    QTRY_VERIFY_WITH_TIMEOUT(sawLogLine(logged, QStringLiteral("fake-fork-disassembly-sent")),
                             5000);
    reply(fork, QByteArray("OK"));

    QTRY_COMPARE_WITH_TIMEOUT(stateSpy.count(), 1, 5000);
    QCOMPARE(int(state.disassembly.size()), 3);
    QVERIFY2(state.disassembly.at(1).isCurrentPc,
             "the current instruction was not identified, so step-over has no fall-through");
    QCOMPARE(state.disassembly.at(2).address, quint32(0x00010008));

    // A free-text command that looks like a disassembly read. The guided
    // profile's teardown no longer sends one (its `db` was a dspbreak that
    // deleted nothing — see DebugSessionController::onDebuggerStopped), but the user can
    // type one, and console text is passed through as written: no state update
    // may follow it, and the cached disassembly has to survive it.
    host.consoleCommand(QStringLiteral("db pc = $1234 :once"));
    QCOMPARE(nextCommand(fork), QByteArray("console db pc = $1234 :once"));
    reply(fork, QByteArray("OK"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("db pc = $1234 :once")),
             "the dspbreak command was never answered");
    QCOMPARE(stateSpy.count(), 1);

    // The observable that matters: step-over still has its fall-through address,
    // so it arms the one-shot at the instruction after the jsr and runs. A wiped
    // snapshot falls through to a plain `step` instead.
    host.stepOver();
    QCOMPARE(nextCommand(fork), QByteArray("bp pc = $10008 :once"));
    reply(fork, QByteArray("OK"));
    QCOMPARE(nextCommand(fork), QByteArray("run"));
    reply(fork, QByteArray("OK"));

    host.stop();
}

// Free text is the user's own debugger language (the console, and the remote
// `cmd` verb), so it reaches the fork's console as written: the prefix table
// that used to rewrite `s` → `step`, `c` → `run`, `b …` → `bp`, `w b …` →
// `memset` and `r` → `regs` made a command mean whatever its spelling looked
// like (CRIT-6, MAJ-12), and its fallback held everything else in the queue
// silently until the next stop. A free-text command can still not run against a
// running machine — the fork's console handler lives in its remote break loop —
// so it waits, and now says so.
void TstHrdb::freeTextIsPassedThroughAndItsDeferralIsAnnounced()
{
#ifdef Q_OS_WIN
    QSKIP("the fake fork is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake fork needs /bin/sh");

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, 56001))
        QSKIP("port 56001 is already held — another HRDB client or PiST session");

    const QString trigger = m_work->path() + QStringLiteral("/fake-fork-free");
    const QString script = writeFakeFork(m_work->path(), trigger);
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/s-fake-free");
    config.machine = QStringLiteral("st");

    HrdbBackend host;
    connect(&host, &IDebugBackend::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy logged(&host, &IDebugBackend::logLine);
    QSignalSpy finished(&host, &IDebugBackend::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 5000);
    QTcpSocket *fork = server.nextPendingConnection();
    QVERIFY(fork);

    // Handshake, then a *running* emulation: !status field 1 is 1 while the fork
    // is emulating (0 while it sits in the remote break loop).
    reply(fork, QByteArray("!connected") + QByteArray(1, '\x01') + "100A");
    reply(fork, QByteArray("!status") + QByteArray(1, '\x01') + "1" + QByteArray(1, '\x01')
                    + "00010002");
    QTRY_VERIFY_WITH_TIMEOUT(host.isRunning() && !host.isStopped(), 5000);

    // Typed at the console while it runs: nothing is dispatched, and the user is
    // told why rather than left waiting on a command that was quietly held.
    host.consoleCommand(QStringLiteral("s"));
    const auto sawDeferral = [&logged] {
        for (const auto &args : logged) {
            const QString line = args.at(0).toString();
            if (line.contains(QLatin1String("'s'")) && line.contains(QLatin1String("stop")))
                return true;
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawDeferral(), 5000);
    QVERIFY2(nextCommand(fork, 300).isEmpty(),
             "a debugger command was dispatched while the emulator was running");

    // The stop releases it, at the fork's own console — the text is not rewritten
    // into a typed `step`.
    reply(fork, QByteArray("!status") + QByteArray(1, '\x01') + "0" + QByteArray(1, '\x01')
                    + "00010002");
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 5000);
    QCOMPARE(nextCommand(fork), QByteArray("console s"));
    reply(fork, QByteArray("OK"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("s")), "the command was never answered");

    host.stop();
}

QTEST_GUILESS_MAIN(TstHrdb)
#include "tst_hrdb.moc"