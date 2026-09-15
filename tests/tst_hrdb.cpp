// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Integration test for HrdbBackend against the HRDB fork of Hatari
// (tattlemuss/hatari, branch hrdb-main).
//
// The fork is not a bundled or system tool, so the suite is gated on
// $PIST_HRDB_HATARI naming the fork's binary; without it every test skips.
// That mirrors the other emulator suites, but note the difference: there is
// no CI runner for this one yet — the fork is built by hand (cmake, same
// dependencies as upstream) and the suite run locally.
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
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
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

} // namespace

class TstHrdb : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
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

    QList<MemoryRow> rows;
    connect(&host, &IDebugBackend::memoryDumpReady, this,
            [&rows](quint32, const QString &response, int) {
                rows = parseMemoryDump(response);
            });
    QSignalSpy dumpSpy(&host, &IDebugBackend::memoryDumpReady);
    host.requestMemoryDump(state.textBase, 32, 0);
    QVERIFY2(dumpSpy.wait(10000), "no memory dump");

    // vasm optimizes `move.l #1,d0` to `moveq` (verified in the test dump):
    // the program's first bytes are 70 01 61 04 (moveq #1,d0; bsr.b sub).
    // If the uuencoded payload were misread, these bytes would not match.
    QVERIFY2(rows.size() >= 2, qPrintable(QStringLiteral("rows: %1").arg(rows.size())));
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

    // Write through the same command the UI's memory editing sends. The first
    // instruction byte at TEXT is 0x70 (moveq); write 0x5a.
    host.command(QStringLiteral("w b $%1 $5a").arg(state.textBase, 0, 16));
    QSignalSpy finished(&host, &IDebugBackend::commandFinished);
    QVERIFY2(finished.wait(10000), "write never answered");

    QList<MemoryRow> rows;
    connect(&host, &IDebugBackend::memoryDumpReady, this,
            [&rows](quint32, const QString &response, int) {
                rows = parseMemoryDump(response);
            });
    QSignalSpy dumpSpy(&host, &IDebugBackend::memoryDumpReady);
    host.requestMemoryDump(state.textBase, 16, 0);
    QVERIFY2(dumpSpy.wait(10000), "no memory dump after write");

    QVERIFY(!rows.isEmpty());
    QCOMPARE(rows.first().bytes.at(0), quint8(0x5a));
    host.stop();
}

QTEST_GUILESS_MAIN(TstHrdb)
#include "tst_hrdb.moc"
