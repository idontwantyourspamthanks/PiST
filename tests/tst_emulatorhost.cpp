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
#include "emu/HatariTextParse.h"
#include "emu/MachineState.h"
#include "emu/SessionConfig.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"

#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
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
    // A real image is wanted only if the developer says so; the default is to
    // fabricate, because a hard-coded personal path made this fixture
    // machine-dependent (MIN-61).
    const QString override = qEnvironmentVariable("PIST_TEST_FLOPPY_IMAGE");
    if (!override.isEmpty() && QFileInfo::exists(override))
        return override;

    // The name keeps the hazard the test is about: a space and parentheses that
    // Hatari's setopt strtok / unescaped hatari-option split apart.
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

/// Write an executable stand-in emulator. The caller must have skipped unless
/// /bin/sh exists (Q_OS_WIN).
QString writeScript(const QString &dir, const QString &name, const QByteArray &body)
{
    const QString script = dir + QLatin1Char('/') + name;
    QFile f(script);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    f.write(body);
    f.close();
    QFile::setPermissions(script,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return script;
}

/// Write a stand-in for Hatari that speaks the native transport's framing
/// without an emulator: it announces debugger entry, prints the prompt on
/// **stderr** (the no-readline build), and answers every command line with two
/// stderr bursts separated by more than the settle period, the `> ` prompt
/// appearing only in the second. That is the one hazard a real Hatari cannot be
/// asked to reproduce on demand — a command whose own output pauses longer than
/// the quiet period — so the transport tests that need it drive this script.
QString writeFakeHatari(const QString &dir)
{
    return writeScript(dir, QStringLiteral("fake-hatari.sh"),
                       "#!/bin/sh\n"
                       "printf 'You have entered debug mode\\n' >&2\n"
                       "printf '> ' >&2\n"
                       "while IFS= read -r line; do\n"
                       "  printf 'burst-one-line\\n' >&2\n"
                       "  sleep 0.3\n"
                       "  printf 'burst-two-line\\n' >&2\n"
                       "  printf '> ' >&2\n"
                       "done\n");
}

/// A stand-in modelling the stdout-prompt (readline) shape the production
/// transport sees: command output and `> <cmd>` echoes on stderr, the prompt on
/// stdout — and because a continue prints nothing on stdout, every prompt after
/// the entry one follows the previous prompt's `> ` with no newline between
/// them. Each prompt is its own write while the loop blocks in `read`, so they
/// arrive as separate chunks: the split that used to swallow the second prompt.
QString writeFakeHatariAdjacent(const QString &dir)
{
    return writeScript(dir, QStringLiteral("fake-hatari-adjacent.sh"),
                       "#!/bin/sh\n"
                       "printf 'You have entered debug mode\\n' >&2\n"
                       "printf '\\n> '\n"
                       "while IFS= read -r line; do\n"
                       "  printf '> %s\\n' \"$line\" >&2\n"
                       "  printf 'reply-to %s\\n' \"$line\" >&2\n"
                       "  sleep 0.1\n"
                       "  printf '> '\n"
                       "done\n");
}

/// A stand-in debugger whose answers are worth parsing: `d` and `d $addr`
/// return a disassembly the shared parser recognises (three lines, with a `jsr`
/// at the PC the register reply reports, so HRDB's step-over contract can be
/// driven too), everything else a plain ack. The prompt is written to
/// **stdout** — the readline build — which is the stream the incremental
/// prompt tally counts (finding MAJ-15).
QString writeFakeDebugger(const QString &dir)
{
    return writeScript(dir, QStringLiteral("fake-debugger.sh"),
                       "#!/bin/sh\n"
                       "printf 'You have entered debug mode\\n' >&2\n"
                       "printf '\n> '\n"
                       "while IFS= read -r line; do\n"
                       "  case \"$line\" in\n"
                       "    d|d\\ *) printf '$00010000 4e71 nop\\n$00010002 4eb9 00010100 jsr $10100\\n"
                       "$00010008 4e71 nop\\n' >&2 ;;\n"
                       // A real memdump row, so a typed dump request's parsed
                       // rows (MAJ-45) can be asserted without an emulator.
                       "    m\\ *|memdump\\ *) printf '00010000: 4e 71 4e 71 4e 71 4e 71 4e 71 4e 71 4e 71 4e 71  "
                       "NqNqNqNqNqNqNqNq\\n' >&2 ;;\n"
                       // The one info subject whose report carries fields, so
                       // the parsed summary (MAJ-45) can be asserted against
                       // the debugger's own text.
                       "    info\\ video*) printf 'Video base : 0x00012596\\nVBL counter : 1\\n"
                       "V-overscan : none\\nRefresh rate : 50 Hz\\n' >&2 ;;\n"
                       "    *) printf 'ack %s\\n' \"$line\" >&2 ;;\n"
                       "  esac\n"
                       "  printf '\n> '\n"
                       "done\n");
}

/// A stand-in whose entry is the shape the real debugger has: the banner, then
/// its multi-KB session dump, and only then the first prompt. The 0.3 s pause
/// is what makes the ordering observable — it guarantees a command written in
/// response to the banner's stop signal is already in flight while the dump is
/// still being printed (finding MAJ-14). A `c` is answered with a prompt and no
/// output: the next stop of a resumed session, with nothing in flight.
QString writeFakeHatariEntryDump(const QString &dir)
{
    return writeScript(dir, QStringLiteral("fake-hatari-entry.sh"),
                       "#!/bin/sh\n"
                       "printf 'You have entered debug mode\\n' >&2\n"
                       "sleep 0.3\n"
                       "printf 'entry-dump-symbols-prg\\n' >&2\n"
                       "printf 'entry-dump-D0-00000000\\n' >&2\n"
                       "printf 'entry-dump-00012596 7001 moveq\\n' >&2\n"
                       "printf '\n> '\n"
                       "while IFS= read -r line; do\n"
                       "  if [ \"$line\" = \"c\" ]; then\n"
                       "    sleep 0.1\n"
                       "    printf '\n> '\n"
                       "    continue\n"
                       "  fi\n"
                       "  printf 'ack %s\\n' \"$line\" >&2\n"
                       "  printf '\n> '\n"
                       "done\n");
}

/// A stand-in that floods stdout with exactly 65536 bytes — prompts included, so
/// the tally has something to lose — and then answers a resume with the next
/// stop's prompt on its own. The byte count is the point: 65536 is exactly the
/// bound below which the buffer is not truncated, so the two bytes of that stop
/// prompt are what pushes it over, and the truncation happens in the same append
/// that carries the prompt. The pre-fix fold-forward (count the whole buffer
/// before and after, take the difference) then subtracts the prompts the
/// truncation dropped and loses the stop prompt entirely: the session has
/// nothing in flight, every following command is re-prepended and no timeout is
/// armed, so it looks alive and answers nothing (finding MAJ-15). The marker
/// line is the last filler line, so a test that waits for it knows all 64 KiB
/// have been read out of the pipe.
QString writeFakeHatariFlood(const QString &dir)
{
    return writeScript(dir, QStringLiteral("fake-hatari-flood.sh"),
                       "#!/bin/sh\n"
                       "printf 'You have entered debug mode\\n' >&2\n"
                       "printf '\n> '\n"
                       // Three prompts among the bytes a later truncation
                       // drops, then whole lines and the marker, sized so the
                       // stream is exactly 65536 bytes: 3 (the entry prompt)
                       // + 10 + 2113 * 31 + 20. The next append — the stop
                       // prompt — is therefore the one that crosses the bound.
                       "printf '\\n> \\n> \\n> \\n'\n"
                       "i=0\n"
                       "while [ $i -lt 2113 ]; do\n"
                       "  printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\n'\n"
                       "  i=$((i+1))\n"
                       "done\n"
                       "printf 'flood-marker-end-ok\\n'\n"
                       "while IFS= read -r line; do\n"
                       "  if [ \"$line\" = \"c\" ]; then\n"
                       "    printf '\n> '\n"
                       "    continue\n"
                       "  fi\n"
                       "  printf 'flood-reply-%s\\n' \"$line\" >&2\n"
                       "  printf '\n> '\n"
                       "done\n");
}

/// A stand-in that models the one hazard a real Hatari cannot be asked to
/// produce on demand: a command that times out and then *still* prints its
/// response. The transport gave up on `slow` after 10 s with the emulator still
/// running it, so the prompt closing it is owed — and the rest of its output can
/// arrive *after* that prompt. Here it does, 50 ms after, on stderr while the
/// prompt is on stdout. Unless the owed branch drains with a window wider than
/// that gap, the tail is read while the next command is current and is appended
/// to its response (MIN-1).
///
/// The gap is what gives the case its teeth, and the drain's owed window
/// (kOwedTailDrainWaitMs) is what closes it: a tail written *before* the prompt
/// is usually already in Qt's hands when the prompt is processed, so removing
/// the drain then changes nothing and the case proves nothing. Keep the gap well
/// inside that window — the ratio between the two is this assertion's margin.
/// Anything else gets an immediate reply.
QString writeFakeHatariLateTail(const QString &dir)
{
    return writeScript(dir, QStringLiteral("fake-hatari-late-tail.sh"),
                       "#!/bin/sh\n"
                       "printf 'You have entered debug mode\\n' >&2\n"
                       "printf '\\n> '\n"
                       "while IFS= read -r line; do\n"
                       "  if [ \"$line\" = \"slow\" ]; then\n"
                       // Past the transport's 10 s command timeout: the command
                       // is failed, one prompt becomes owed.
                       "    sleep 11\n"
                       "    printf '\\n> '\n"
                       // The tail of the command the transport gave up on, still
                       // arriving after its own prompt: inside the owed drain's
                       // window, and far enough outside the short one the
                       // completion path uses that only the window explains this
                       // passing.
                       "    sleep 0.05\n"
                       "    printf 'slow-tail-line\\n' >&2\n"
                       "    continue\n"
                       "  fi\n"
                       "  printf 'reply-to %s\\n' \"$line\" >&2\n"
                       "  printf '\\n> '\n"
                       "done\n");
}

/// A stand-in for the hrdb fork's remote-debug listener on TCP 56001: the
/// handshake, a `!status` stop (which is what releases deferred commands), and
/// the commands it receives. The frame's reply-owed guard is exactly what a
/// real fork cannot be asked to misbehave on demand (MIN-2).
class FakeHrdb : public QObject
{
public:
    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost, 56001))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            m_client = m_server.nextPendingConnection();
            connect(m_client, &QTcpSocket::readyRead, this, [this] {
                m_buffer += m_client->readAll();
                int nul;
                while ((nul = m_buffer.indexOf('\0')) >= 0) {
                    const QByteArray message = m_buffer.left(nul);
                    m_buffer.remove(0, nul + 1);
                    m_received.append(QString::fromUtf8(message));
                }
            });
            // `!connected` sets the backend ready; `!status 0` reports the
            // stopped remote break loop, where console commands may run.
            sendRaw(QByteArray("!connected\x01" "100A"));
            sendRaw(QByteArray("!status\x01" "0\x01" "00010000"));
        });
        return true;
    }
    void sendRaw(const QByteArray &message) { m_client->write(message + '\0'); }
    bool hasClient() const { return m_client != nullptr; }
    QString nextLine(int timeoutMs = 10000)
    {
        if (!QTest::qWaitFor([this] { return !m_received.isEmpty(); }, timeoutMs))
            return QString();
        return m_received.takeFirst();
    }

private:
    QTcpServer m_server;
    QTcpSocket *m_client = nullptr;
    QByteArray m_buffer;
    QStringList m_received;
};

/// Whether a command has been answered, and with what. The transports answer
/// asynchronously, and these tests care about which response belongs to which
/// command, so a matching commandFinished is the only sufficient condition.
bool commandAnswered(QSignalSpy *finished, const QString &command, QString *response = nullptr,
                     int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        for (const auto &args : *finished) {
            if (args.at(0).toString() != command)
                continue;
            if (response)
                *response = args.at(1).toString();
            return true;
        }
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

class TstEmulatorHost : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void cleanupTestCase();

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
    /// A command whose stderr output pauses longer than the settle period must
    /// still wait for its prompt (finding CRIT-1). Emulator-free: a fake Hatari
    /// script drives the framing.
    void settleTimerWaitsForThePrompt();
    /// Two prompts adjacent in the stdout stream (`> ` following a previous
    /// prompt's space with no newline between — what a silent continue followed
    /// by a stop produces) must each complete their own command. The old
    /// `(?:^|\n)> ` pattern plus the carry skip lost the second one, timing out
    /// every command after a silent continue ("did not respond to 'r' in
    /// time"). Emulator-free, adjacent-prompt fake.
    void adjacentPromptsEachCompleteTheirCommand();
    /// resume() must keep a `profile` control command queued in the same stack
    /// frame (finding CRIT-6, native side). Emulator-free, same fake.
    void resumeKeepsQueuedProfileCommand();
    /// A second session on the same host must re-frame cleanly: resetTransport()
    /// exists for this and no other test exercises it, as each builds a fresh host.
    void secondSessionOnOneHostReframesCleanly();
    /// The response type is carried by the request, never inferred from the
    /// command text: a `db` (dspbreak) prefix-matched a disassembly read, so a
    /// `db pc = $X :once` wiped the cached disassembly and published an empty
    /// snapshot; a text `memdump`/`m $a n` was published as a memory pane's
    /// dump with address 0 (finding MAJ-12). Emulator-free.
    void nonDisassemblyDCommandKeepsTheSnapshot();
    /// The hardware pane's `info <subject>` report comes back named by the
    /// subject it asked for, rather than matched out of a command's text
    /// (finding MAJ-21). Emulator-free.
    void infoSubjectReportsToItsAsker();
    /// A profile save reports its own completion, with the file it wrote — the
    /// caller parses that file, so it must not have to recognise a command's
    /// text to know the save landed (finding MAJ-21). Emulator-free.
    void profileSaveCompletionCarriesThePath();
    /// A disassembly read at a given address reports the text it read (the
    /// remote `disasm` verb's answer) as well as filling the snapshot, while
    /// the PC read reports nothing (finding MAJ-21). Emulator-free.
    void disassemblyAtAnAddressReportsItsText();
    /// The disassembly byte column ends where the alignment gap begins, so a
    /// hex-only mnemonic in the text column (`dbcc`, `dbf`, `abcd`) is never
    /// swallowed into it (finding MAJ-57). Emulator-free: pure text.
    void disassemblyKeepsHexOnlyMnemonicsWhole();
    /// The entry dump must not be attributed to the first command: the guard has
    /// to exist before the stop is published, because stoppedChanged's handlers
    /// queue that command synchronously (finding MAJ-14). Emulator-free.
    void entryDumpIsNotAttributedToTheFirstCommand();
    /// A prompt arriving in the append that truncates the stdout buffer must
    /// still be counted; losing it wedges the session with no timeout armed
    /// (finding MAJ-15). Emulator-free.
    void promptTallySurvivesTheStdoutTruncation();
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

    /// A timed-out command's response tail must not land in the *next*
    /// command's response: the owed-prompt branch drains stderr before it
    /// dispatches (MIN-1). Emulator-free, late-tail fake.
    void aTimedOutCommandsTailDoesNotContaminateTheNext();
    /// A reply owed by a timed-out HRDB command holds the queue: the next
    /// command must not be dispatched into the reply that is still owed, or a
    /// missing reply makes every command answer its predecessor (MIN-2).
    /// Emulator-free: a fake listener stands in for the fork.
    void hrdbHoldsTheQueueWhileAReplyIsOwed();

    /// The bootstrap script must be safe on Hatari 2.6.1 (`echo` aborts it) and
    /// version-gated on the `symbols autoload` form (MAJ-48 seam).
    void bootstrapScriptIsSafeAndVersionGated();
    /// Capabilities come from the probe by *option name*, never inferred from the
    /// version (MAJ-48 seam). Emulator-free: a script answers `--version`/`-h`.
    void capabilitiesKeyOnOptionNamesNotVersion();

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

// m_work is heap-allocated because initTestCase may skip before it is needed;
// without this the whole tree leaks into /tmp on every run (MIN-67).
void TstEmulatorHost::cleanupTestCase()
{
    delete m_work;
    m_work = nullptr;
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

    host.dumpRegisters();
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
    // The signal carries the dump parsed (MAJ-45): rows at the requested
    // address, with bytes. The stack view renders these, and the step-out path
    // read its return address from the same rows rather than parsing the
    // transcript a second time.
    const QList<MemoryRow> rows = args.at(1).value<QList<MemoryRow>>();
    QVERIFY2(!rows.isEmpty(), "the stack dump carried no parsed rows");
    QCOMPARE(rows.first().address, 0x10000u);
    QVERIFY2(rows.first().bytes.size() >= 4,
             qPrintable(QStringLiteral("first row has %1 bytes").arg(rows.first().bytes.size())));
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
    host.loadSymbols();
    QVERIFY2(finished.wait(15000), "no response to 'symbols prg'");
    const QString symResponse = finished.takeFirst().at(1).toString();
    QVERIFY2(symResponse.contains(QLatin1String("symbols")),
             qPrintable("no symbol load reported: " + symResponse.left(300)));

    host.readBasepage();
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

    host.loadSymbols();
    QVERIFY(finished.wait(15000));
    finished.clear();

    host.readDisassembly();
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

    host.dumpRegisters();
    QVERIFY(finished.wait(15000));
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    const quint32 firstPc = last.pc;
    QVERIFY(firstPc != 0);

    // Single-step once; the PC must move forward.
    host.step();
    host.dumpRegisters();
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
    host.dumpRegisters();
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
    host.dumpRegisters();
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
    host.loadSymbols();
    QVERIFY(finished.wait(15000));
    finished.clear();
    host.readBasepage();
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
    host.clearBreakpoints();
    QVERIFY(finished.wait(15000));
    finished.clear();
    host.armBreakpoint(plan.commands.first());
    QVERIFY(finished.wait(15000));
    const QString armResponse = finished.takeFirst().at(1).toString();
    QVERIFY2(armResponse.contains(QLatin1String("breakpoint"), Qt::CaseInsensitive),
             qPrintable("breakpoint was not accepted: " + armResponse.left(300)));

    host.resume();

    // Wait on state rather than on the signal: subsequent debugger entries are
    // detected from the prompt, and the spy would already hold earlier entries.
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    host.dumpRegisters();
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

    host.dumpRegisters();
    QTRY_VERIFY_WITH_TIMEOUT(last.regs.valid, 5000);
    const quint32 entry = last.pc;
    QVERIFY(entry != 0);

    // Queue the arm and a dump; do not wait for either. Resume must still send
    // the `b` — dropping the queue used to let Continue at entry miss it.
    host.armBreakpoint(QStringLiteral("b pc > $%1").arg(entry, 0, 16));
    host.requestMemoryDump(entry, 64);
    host.resume();

    // Without the `b`, this loops forever. Stopping again is the proof the arm
    // survived resume's queue flush.
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 20000);

    host.stop();
}

void TstEmulatorHost::settleTimerWaitsForThePrompt()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeHatari(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-settle");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy logged(&host, &EmulatorHost::logLine);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    host.dumpRegisters();

    // The settle timer counts the prompt *at dispatch* as "arrived"; with the
    // pre-fix predicate every line of a command's own output re-arms it, so the
    // 300 ms gap this script leaves inside one response completes the command
    // on its first burst. Wait for that burst, then give the timer several
    // multiples of its 40 ms quiet period to fire: nothing may complete yet,
    // because no prompt has arrived since the command was written.
    auto sawFirstBurst = [&logged] {
        for (const auto &args : logged)
            if (args.at(0).toString().contains(QLatin1String("burst-one-line")))
                return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawFirstBurst(), 5000);
    QTest::qWait(200);
    QVERIFY2(finished.isEmpty(),
             "the command completed while only the first of its two stderr bursts had arrived");

    QVERIFY2(finished.wait(5000), "the command never completed after its prompt");
    QCOMPARE(finished.size(), 1);
    const QString response = finished.first().at(1).toString();
    QVERIFY2(response.contains(QLatin1String("burst-two-line")),
             qPrintable("the response was settled before the prompt arrived: " + response));

    host.stop();
}

// Adjacent prompts must each complete their own command. The entry prompt is
// followed — with nothing on stdout in between — by every later command's
// prompt, because a continue prints nothing and the prompt carries no newline.
// The old `(?:^|\n)> ` pattern merged such a pair into one match and the carry
// skip then suppressed the chunk-split case, so a command after a silent
// continue could lose its completion signal and time out. Pre-fix this fails at
// the first `commandAnswered` (its `> ` prompt is the second of an adjacent
// pair and is never counted).
void TstEmulatorHost::adjacentPromptsEachCompleteTheirCommand()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeHatariAdjacent(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-adjacent");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    // The entry prompt is consumed as the entry stop, so this command's own
    // `> ` is already the second of an adjacent pair (its write lands in its
    // own chunk after the entry prompt's).
    host.command(QStringLiteral("first"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("first"), nullptr, 5000),
             "the prompt after the entry prompt did not complete the command");

    host.command(QStringLiteral("second"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("second"), nullptr, 5000),
             "an adjacent prompt did not complete the second command");

    host.stop();
}

void TstEmulatorHost::resumeKeepsQueuedProfileCommand()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeHatari(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-profile");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    // profileToCursor's synchronous sequence: an earlier command is still in
    // flight (its round trip is outstanding), the profile control command is
    // queued behind it, and then resume prunes the queue. Dropping the profile
    // command here loses it for good — the UI reports "Collecting" while the
    // debugger is never told to collect.
    host.dumpRegisters();
    host.profileOn();
    host.resume();

    auto sawProfileOn = [&finished] {
        for (const auto &args : finished)
            if (args.at(0).toString() == QLatin1String("profile on"))
                return true;
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(sawProfileOn(), 5000);
}

// A command the transport timed out is not necessarily finished: it is still
// running inside the emulator, so the tail of its response can arrive *after*
// the prompt that closes it — on stderr, while that prompt came on stdout, and
// the two pipes have no ordering between them. The owed-prompt branch must
// therefore drain stderr before it dispatches the next command, with a window
// wide enough for a tail that is still on its way; otherwise the timed-out
// command's tail is appended to the *next* command's response.
void TstEmulatorHost::aTimedOutCommandsTailDoesNotContaminateTheNext()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeHatariLateTail(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-late-tail");

    EmulatorHost host;
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    // The first command's answer arrives past the 10 s command timeout; the
    // second is queued behind it and can only go out once the owed reply (here,
    // the late tail) has been consumed.
    host.command(QStringLiteral("slow"));
    host.command(QStringLiteral("next"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("slow"), nullptr, 15000),
             "the slow command was never timed out");

    QString response;
    QVERIFY2(commandAnswered(&finished, QStringLiteral("next"), &response, 10000),
             "the next command was never answered");
    QVERIFY2(!response.contains(QLatin1String("slow-tail-line")),
             qPrintable(QStringLiteral("the timed-out command's tail contaminated the next "
                                       "response: %1").arg(response)));
    QVERIFY2(response.contains(QLatin1String("reply-to next")), qPrintable(response));

    host.stop();
}

// The HRDB watchdog fails a command the fork never answers, but the reply may
// still be on its way. Dispatching the next command into it lets the late reply
// be swallowed as if it were *that* command's answer, so a reply that never
// comes at all leaves every later command answered by its predecessor. The
// guard holds the queue until the swallow consumes the owed reply (MIN-2).
void TstEmulatorHost::hrdbHoldsTheQueueWhileAReplyIsOwed()
{
#ifdef Q_OS_WIN
    QSKIP("the fake fork needs /bin/sh and a loopback listener");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake fork needs /bin/sh");

    FakeHrdb fork;
    if (!fork.start())
        QSKIP("port 56001 is already in use");

    // HrdbBackend starts an emulator process of its own and speaks to the
    // listener separately; a script that stays alive stands in for it.
    const QString script = writeScript(m_work->path(), QStringLiteral("fake-hrdb.sh"),
                                       "#!/bin/sh\nexec sleep 600\n");
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-hrdb");

    HrdbBackend host;
    QSignalSpy finished(&host, &HrdbBackend::commandFinished);
    QSignalSpy errors(&host, &HrdbBackend::errorOccurred);
    QSignalSpy logs(&host, &HrdbBackend::logLine);
    QVERIFY(host.start(config, nullptr));

    // The handshake has to land before any command can be dispatched.
    if (!QTest::qWaitFor([&fork] { return fork.hasClient(); }, 3000)) {
        QStringList lines;
        for (const auto &args : logs)
            lines.append(args.at(0).toString());
        QStringList failures;
        for (const auto &args : errors)
            failures.append(args.at(0).toString());
        QFAIL(qPrintable(QStringLiteral("the fake fork never got a connection; log: %1; errors: %2")
                             .arg(lines.join(QLatin1String(" | ")),
                                  failures.join(QLatin1String(" | ")))));
    }

    host.command(QStringLiteral("first"));
    {
        const QString first = fork.nextLine(5000);
        if (first.isEmpty()) {
            QStringList lines;
            for (const auto &args : logs)
                lines.append(args.at(0).toString());
            QStringList failures;
            for (const auto &args : errors)
                failures.append(args.at(0).toString());
            QFAIL(qPrintable(QStringLiteral("`first` never reached the fork; log: %1; errors: %2")
                                 .arg(lines.join(QLatin1String(" | ")),
                                      failures.join(QLatin1String(" | ")))));
        }
        QCOMPARE(first, QStringLiteral("console first"));
    }

    // Queued behind `first` (one command outstanding at a time).
    host.command(QStringLiteral("second"));

    // The fork withholds its reply to `first`, so the watchdog fails it.
    QVERIFY2(commandAnswered(&finished, QStringLiteral("first"), nullptr, 15000),
             "the first command was never timed out");

    // With a reply owed, the queue must not advance into it.
    QTest::qWait(500);
    QVERIFY2(fork.nextLine(1000).isEmpty(),
             "a command was dispatched while a reply was still owed");

    // The late reply is swallowed and releases the queue; `second` goes out and
    // is answered by its own reply.
    fork.sendRaw(QByteArray("OK"));
    QCOMPARE(fork.nextLine(), QStringLiteral("console second"));
    fork.sendRaw(QByteArray("OK"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("second"), nullptr, 5000),
             "the second command was never answered");

    host.stop();
}

// The bootstrap script is what puts the debugger at the program's entry; it is
// written before any session exists, so its content is testable on its own. Two
// things must hold whatever the build: 2.6.1 aborts the emulator on `echo` (a
// failed assertion in Str_UnEscape), and the `symbols autoload` line must match
// the build's form — boolean on 2.6.1, three-mode on git main (MAJ-48 seam).
void TstEmulatorHost::bootstrapScriptIsSafeAndVersionGated()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const auto read = [](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        return QString::fromUtf8(f.readAll());
    };

    HatariCapabilities legacy; // 2.6.1: boolean `symbols autoload on`
    QString error;
    const QString legacyPath = EmulatorHost::writeBootstrapScript(dir.path(), legacy, &error);
    QVERIFY2(!legacyPath.isEmpty(), qPrintable(error));
    const QString legacyText = read(legacyPath);
    QVERIFY2(legacyText.contains(QLatin1String("symbols autoload on")), qPrintable(legacyText));
    QVERIFY2(!legacyText.contains(QLatin1String("echo")), qPrintable(legacyText));
    QVERIFY2(legacyText.contains(QLatin1String("history cpu")), qPrintable(legacyText));
    // The entry stop must be the upper-guarded `TEXT` form, not a bare symbol.
    QVERIFY2(legacyText.contains(QLatin1String("b pc = TEXT && pc < $e00000 :once")),
             qPrintable(legacyText));

    HatariCapabilities modern; // git main: three-mode form
    modern.hasSymbolAutoloadOption = true;
    const QString modernPath = EmulatorHost::writeBootstrapScript(dir.path(), modern, &error);
    QVERIFY2(!modernPath.isEmpty(), qPrintable(error));
    const QString modernText = read(modernPath);
    QVERIFY2(modernText.contains(QLatin1String("symbols autoload debugger")),
             qPrintable(modernText));
    QVERIFY2(!modernText.contains(QLatin1String("symbols autoload on")), qPrintable(modernText));
    QVERIFY2(!modernText.contains(QLatin1String("echo")), qPrintable(modernText));
}

// Capabilities are probed from the *output* (`-h`), not inferred from the
// version banner: a distro build or a fork need not track upstream's numbering,
// and getting this wrong silently turns a supported option into "absent"
// (MAJ-48 seam). Emulator-free: a script answers both info runs.
void TstEmulatorHost::capabilitiesKeyOnOptionNamesNotVersion()
{
#ifdef Q_OS_WIN
    QSKIP("the probe stand-in is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the probe stand-in needs /bin/sh");

    // Reports a modern-looking version but lists only --debug-except: the flags
    // must follow the help text, never the number.
    const QString script = writeScript(m_work->path(), QStringLiteral("fake-probe.sh"),
                                       "#!/bin/sh\n"
                                       "case \"$1\" in\n"
                                       "  --version) printf 'Hatari v2.6.1 (test build)\\n' ;;\n"
                                       "  -h) printf 'Usage: hatari [options]\\n"
                                       "  --debug-except <list>   break on exceptions\\n' ;;\n"
                                       "esac\n");
    QVERIFY(!script.isEmpty());

    const HatariCapabilities caps = probeHatari(script);
    QVERIFY2(caps.valid, "the version probe did not recognise the build");
    QCOMPARE(caps.version, QStringLiteral("2.6.1"));
    QCOMPARE(caps.versionMajor, 2);
    QCOMPARE(caps.versionMinor, 6);
    QCOMPARE(caps.versionPatch, 1);
    QVERIFY2(caps.hasDebugExcept, "an option the help lists was not detected");
    QVERIFY2(!caps.hasControlSocket, "--control-socket is not in this build's help");
    QVERIFY2(!caps.hasSymbolAutoloadOption, "--symload is not in this build's help");
    QVERIFY2(!caps.hasHrdb, "this stand-in is not the fork");
}

// The response type is what the *request* was, not what the command text starts
// with. Hatari has many non-disassembly `d*` commands (`db` is dspbreak, and
// there are `dm`, `ds`, `dspbreak`), and `parseDisassembly()` begins by clearing
// the cached disassembly — so the `db pc = $X :once` that ends every guided
// profile run wiped the snapshot the Disassembly pane renders, published a
// stateUpdated carrying an empty one, and left HRDB's step-over without the
// fall-through address it reads from that snapshot. The mirror case is text that
// merely looks like a dump: a console `memdump $100 16` was published as
// memoryDumpReady with address 0, to a pane that never asked for a dump.
void TstEmulatorHost::nonDisassemblyDCommandKeepsTheSnapshot()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeDebugger(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-kind");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy stateSpy(&host, &EmulatorHost::stateUpdated);
    QSignalSpy dumpSpy(&host, &EmulatorHost::memoryDumpReady);
    QSignalSpy stackSpy(&host, &EmulatorHost::stackDumpReady);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState snapshot;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&snapshot](const MachineState &s) { snapshot = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    // The disassembly read the entry attach sends: it is parsed, and fills the
    // cache a `d`-looking command used to wipe.
    host.readDisassembly();
    QVERIFY2(commandAnswered(&finished, QStringLiteral("d")), "`d` was never answered");
    QTRY_COMPARE_WITH_TIMEOUT(stateSpy.count(), 1, 5000);
    const int lines = snapshot.disassembly.size();
    QVERIFY2(lines == 3, qPrintable(QStringLiteral("`d` was not parsed: %1 lines").arg(lines)));

    // A free-text command that looks like a disassembly read. `db` is a
    // dspbreak (a DSP breakpoint), so its response is not one: no state update
    // may follow it, and the cached snapshot must survive (a text command can
    // never ask for a state read — the typed intents do).
    host.command(QStringLiteral("db pc = $1234 :once"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("db pc = $1234 :once")),
             "`db` was never answered");
    QCOMPARE(stateSpy.count(), 1);

    // And the cache really survived, not merely stopped being published: the next
    // state read still carries the three lines `d` produced.
    host.dumpRegisters();
    QVERIFY2(commandAnswered(&finished, QStringLiteral("r")), "`r` was never answered");
    QTRY_COMPARE_WITH_TIMEOUT(stateSpy.count(), 2, 5000);
    QCOMPARE(snapshot.disassembly.size(), lines);

    // The mirror defect, through both doors: a console command, and a text
    // command that looks like the dump request. Both are answered with a
    // memdump-shaped transcript (the fake's `m` case), so this is the routing
    // being kind-based rather than the text being unrecognisable.
    host.consoleCommand(QStringLiteral("memdump $100 16"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("memdump $100 16")),
             "the console command was never answered");
    host.command(QStringLiteral("m $100 16"));
    QVERIFY2(commandAnswered(&finished, QStringLiteral("m $100 16")),
             "the text dump command was never answered");
    QCOMPARE(dumpSpy.count(), 0);
    QCOMPARE(stackSpy.count(), 0);

    // ... while a request that does carry the pane's tag still routes, so the fix
    // cannot be satisfied by disabling dump routing. The rows are the debugger's
    // dump, parsed where it was read (MAJ-45): the pane receives bytes.
    host.requestMemoryDump(0x10000, 16, 7);
    QTRY_COMPARE_WITH_TIMEOUT(dumpSpy.count(), 1, 5000);
    QCOMPARE(dumpSpy.at(0).at(0).toUInt(), 0x10000u);
    QCOMPARE(dumpSpy.at(0).at(2).toInt(), 7);
    const QList<MemoryRow> rows = dumpSpy.at(0).at(1).value<QList<MemoryRow>>();
    QVERIFY2(rows.size() == 1, qPrintable(QStringLiteral("rows: %1").arg(rows.size())));
    QCOMPARE(rows.first().address, 0x10000u);
    QCOMPARE(rows.first().bytes.size(), 16);
    QCOMPARE(rows.first().bytes.at(0), quint8(0x4e));
    host.requestStackDump(0x10000, 16);
    QTRY_COMPARE_WITH_TIMEOUT(stackSpy.count(), 1, 5000);
    QCOMPARE(stackSpy.at(0).at(1).value<QList<MemoryRow>>().size(), 1);

    host.stop();
}

// The typed requests answer their own caller. The hardware pane asked for one
// subject, so the report comes back named by that subject — the routing that
// used to happen by comparing a finished command's text against
// `"info " + subject()`, which a caller's own text could also match.
void TstEmulatorHost::infoSubjectReportsToItsAsker()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeDebugger(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-info");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy infoSpy(&host, &EmulatorHost::hardwareInfoReady);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    host.infoSubject(QStringLiteral("mfp"));
    QVERIFY2(infoSpy.wait(5000), "no info report arrived");
    QCOMPARE(infoSpy.first().at(0).toString(), QStringLiteral("mfp"));
    // The debugger's own answer for that subject, not a re-rendered command,
    // carried as the transcript of the summary the pane displays (MAJ-45).
    const HardwareSummary report = infoSpy.first().at(1).value<HardwareSummary>();
    QVERIFY2(report.transcript.contains(QLatin1String("ack info mfp")),
             qPrintable(report.transcript));
    // The MFP report states none of the video fields, so the summary claims
    // none — the pane shows the transcript alone rather than inventing a header.
    QVERIFY(!report.hasScreenBase);
    QCOMPARE(report.refreshHz, 0);
    QVERIFY(report.overscan.isEmpty());

    // An info report is not part of MachineState: no state update may follow it.
    QSignalSpy stateSpy(&host, &EmulatorHost::stateUpdated);
    host.infoSubject(QStringLiteral("video"));
    QVERIFY2(infoSpy.wait(5000), "the second report never arrived");
    QCOMPARE(infoSpy.count(), 2);
    QCOMPARE(stateSpy.count(), 0);

    // ... and the video report's own fields are parsed where the report was
    // read (MAJ-45): the pane is handed this summary, so no view (and no other
    // consumer) reads the transcript for meaning. A `$`-prefixed base, the
    // overscan token and the rate are exactly what `info video` prints.
    const QList<QVariant> video = infoSpy.at(1);
    QCOMPARE(video.at(0).toString(), QStringLiteral("video"));
    const HardwareSummary summary = video.at(1).value<HardwareSummary>();
    QVERIFY(summary.transcript.contains(QLatin1String("VBL counter")));
    QVERIFY(summary.hasScreenBase);
    QCOMPARE(summary.screenBase, 0x00012596u);
    QCOMPARE(summary.refreshHz, 50);
    QCOMPARE(summary.overscan, QStringLiteral("none"));

    host.stop();
}

// A profile save is complete when its command completes, whatever the debugger
// said: the caller reads the file it named. Making it recognisable by command
// text meant a rename or a reworded label silently left the Profiler on
// "Saving…".
void TstEmulatorHost::profileSaveCompletionCarriesThePath()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeDebugger(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-profsave");
    const QString path = config.sessionDir + QStringLiteral("/profile.txt");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy saveSpy(&host, &EmulatorHost::profileSaveFinished);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced stopped");

    host.profileSave(path);
    QVERIFY2(saveSpy.wait(5000), "the save never reported completion");
    QCOMPARE(saveSpy.first().at(0).toString(), path);
    // The request is still named for the log, and the save is one command.
    QVERIFY2(commandAnswered(&finished, QStringLiteral("profile save ") + path),
             "the save command was never answered");
    QCOMPARE(saveSpy.count(), 1);

    host.stop();
}

// A read at a given address reports the text it read, which is what the remote
// `disasm` verb answers with; the PC read (the snapshot's) reports no text,
// because nothing is waiting on it.
void TstEmulatorHost::disassemblyAtAnAddressReportsItsText()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeDebugger(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-disasm-at");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy disasmSpy(&host, &EmulatorHost::disassemblyReady);
    QSignalSpy stateSpy(&host, &EmulatorHost::stateUpdated);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);
    MachineState snapshot;
    connect(&host, &EmulatorHost::stateUpdated, this,
            [&snapshot](const MachineState &s) { snapshot = s; });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    host.readDisassemblyAt(0x00010000);
    QVERIFY2(disasmSpy.wait(5000), "no disassembly text was reported");
    // The address it was asked about, so the waiter can tell its own answer
    // from a stale one.
    QCOMPARE(disasmSpy.first().at(0).toUInt(), 0x00010000u);
    const QString text = disasmSpy.first().at(1).toString();
    QVERIFY2(text.contains(QLatin1String("$00010002")), qPrintable(text));
    // ... and it fills the snapshot like any disassembly read.
    QTRY_COMPARE_WITH_TIMEOUT(stateSpy.count(), 1, 5000);
    QCOMPARE(snapshot.disassembly.size(), 3);

    // The PC read answers, and reports no text.
    host.readDisassembly();
    QVERIFY2(commandAnswered(&finished, QStringLiteral("d")), "`d` was never answered");
    QCOMPARE(disasmSpy.count(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(stateSpy.count(), 2, 5000);

    host.stop();
}

// The byte column of `d` output is a run of hex words separated by a *single*
// space and then padded out to the fixed text column. A group that allowed any
// amount of whitespace between words walked across that padding and swallowed a
// hex-only mnemonic standing in the text column — `dbcc`, `dbf` (the canonical
// 68k loop idiom), `abcd` — so dl.instruction was left holding nothing but the
// operands: the pane rendered the truncated line, and HRDB's step-over, which
// reads the mnemonic out of that field, silently degraded to a plain step
// (finding MAJ-57). The other half of the same boundary is Hatari's own cut
// token for a ten-byte instruction, which belongs to the byte column for the
// same reason. The parser is the root fix; DisassemblyView renders the field
// verbatim and needs no change. Emulator-free: the parser is pure text.
void TstEmulatorHost::disassemblyKeepsHexOnlyMnemonicsWhole()
{
    // The first block is real output captured from Hatari 2.6.1 — the
    // debugger's `w w` writes into ST RAM, then `d`: a nine-wide address
    // column, hex words from column 10, the text column at 45.
    MachineState state;
    hataritext::parseDisassembly(
        QStringLiteral(
            "$00010000 51c8 fffa                          dbra      d0,$fffc\n"
            "$00010014 54c8 000a                          dbcc      d0,$10020\n"
            // The same shape without the size suffix the UAE engine adds, so
            // the mnemonic itself is all hex — the Capstone spelling.
            "$00010048 c10c                               abcd d0,d1\n"
            "$00010020 4eb9 0001 260a                     jsr       $1260a.l\n"
            "$00010030 b07c 0064                          cmp.w     #$64,d0\n"
            "$00010040 7001                               moveq     #$1,d0\n"
            "$00010016 000a                               dc.w      $000a                     ; unknown opcode\n"
            "$00010050 23fc 1234 5678 0001 23+            move.l    #$12345678,$12345.l\n"
            // The '$' prefix is optional and the gap width follows the column
            // settings; this is the quoting from the review.
            "000125a0 51c8 fffa                dbf d0,$125a0\n"
            // The compact shape the suite's fake debuggers emit: one space
            // between the byte group and the text, with no alignment gap.
            "$00010060 4e71 nop\n"),
        &state);
    QCOMPARE(state.disassembly.size(), 10);

    const auto lineAt = [&state](quint32 address) -> DisasmLine {
        for (const DisasmLine &dl : state.disassembly) {
            if (dl.address == address)
                return dl;
        }
        return {};
    };

    // A hex-only mnemonic stays whole and its operands stay intact. Before the
    // fix these three parsed as `d0,$10020`, `d0,d1` and `d0,$125a0`, with the
    // mnemonic — and the alignment padding — absorbed into the byte column.
    QCOMPARE(lineAt(0x00010014).bytes, QStringLiteral("54c8 000a"));
    QCOMPARE(lineAt(0x00010014).instruction, QStringLiteral("dbcc      d0,$10020"));
    QCOMPARE(lineAt(0x00010048).instruction, QStringLiteral("abcd d0,d1"));
    QCOMPARE(lineAt(0x000125a0).bytes, QStringLiteral("51c8 fffa"));
    QCOMPARE(lineAt(0x000125a0).instruction, QStringLiteral("dbf d0,$125a0"));

    // A ten-byte instruction (five words) overruns the byte column, so Hatari
    // cuts the last word short with a '+'. That cut token is part of the byte
    // column — before the fix it led the instruction text (`23+            move.l
    // ...`) — and pinning it is also what keeps a remedy that caps the *number*
    // of byte groups honest: three groups would cut `0001 23+` off here and push
    // it in front of the mnemonic, where a fixed column split is immune by
    // construction.
    QCOMPARE(lineAt(0x00010050).bytes, QStringLiteral("23fc 1234 5678 0001 23+"));
    QCOMPARE(lineAt(0x00010050).instruction, QStringLiteral("move.l    #$12345678,$12345.l"));

    // Guards: the shapes that already split correctly keep splitting correctly.
    // A six-byte operand (three words) and a mnemonic that is not all hex never
    // depended on the split, and the compact fake shape has no gap to find.
    QCOMPARE(lineAt(0x00010000).instruction, QStringLiteral("dbra      d0,$fffc"));
    QCOMPARE(lineAt(0x00010020).instruction, QStringLiteral("jsr       $1260a.l"));
    QCOMPARE(lineAt(0x00010030).instruction, QStringLiteral("cmp.w     #$64,d0"));
    QCOMPARE(lineAt(0x00010040).instruction, QStringLiteral("moveq     #$1,d0"));
    QCOMPARE(lineAt(0x00010016).instruction,
             QStringLiteral("dc.w      $000a                     ; unknown opcode"));
    QCOMPARE(lineAt(0x00010060).instruction, QStringLiteral("nop"));
}

// The guard that keeps the debugger's entry dump out of the first command's
// response has to exist *before* the stop is published: stoppedChanged(true)
// runs its handlers synchronously, MainWindow's queues the entry attach there
// (`symbols prg`, `info basepage`, `r`, `d`), and with the guard armed afterwards
// the first of those was already written to stdin while the dump was still being
// printed — so the rest of the dump was appended to that command's response.
void TstEmulatorHost::entryDumpIsNotAttributedToTheFirstCommand()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeHatariEntryDump(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-entry");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy logged(&host, &EmulatorHost::logLine);
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    // DebugSessionController::onDebuggerStopped's shape: the first command is queued from
    // inside the stopped handler, i.e. before handleStderrLine has returned.
    bool queued = false;
    connect(&host, &EmulatorHost::stoppedChanged, this, [&host, &queued](bool stopped) {
        if (!stopped || queued)
            return;
        queued = true;
        host.readBasepage();
    });

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    // The dump was genuinely printed before the first prompt: without this the
    // assertion below would pass on a debugger that never wrote a dump.
    QTRY_VERIFY_WITH_TIMEOUT(sawLogLine(logged, QStringLiteral("entry-dump-symbols-prg")), 5000);

    QString response;
    QVERIFY2(commandAnswered(&finished, QStringLiteral("info basepage"), &response),
             "the entry command was never answered");
    QVERIFY2(!response.contains(QLatin1String("entry-dump")),
             qPrintable(QStringLiteral("the entry dump was attributed to the first "
                                       "command: %1").arg(response)));

    // The guard must also be gone once the entry is over: a stale one would
    // swallow the next stop's prompt and the session would never look stopped
    // again. The fake answers a `c` with the next stop's prompt and no output.
    host.resume();
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 5000);
    host.dumpRegisters();
    QVERIFY2(commandAnswered(&finished, QStringLiteral("r")),
             "the next stop's prompt did not let a command complete");

    host.stop();
}

// The prompt tally must not go through the whole stdout buffer: the buffer is
// truncated above 64 KiB, and a count that spans the truncation subtracts the
// prompts it dropped from the prompts that just arrived, losing a stop prompt.
// Nothing in flight, so the lost prompt means the session never records the stop
// — every command is then re-prepended by dispatchNext and no timeout is armed,
// which is a silent wedge rather than a failure.
void TstEmulatorHost::promptTallySurvivesTheStdoutTruncation()
{
#ifdef Q_OS_WIN
    QSKIP("the fake debugger is a /bin/sh script");
#endif
    if (!QFileInfo::exists(QStringLiteral("/bin/sh")))
        QSKIP("the fake debugger needs /bin/sh");

    const QString script = writeFakeHatariFlood(m_work->path());
    QVERIFY(!script.isEmpty());

    SessionConfig config;
    config.hatariPath = script;
    config.sessionDir = m_work->path() + QStringLiteral("/fake-flood");

    EmulatorHost host;
    connect(&host, &EmulatorHost::logLine, this,
            [this](const QString &l) { m_log.append(l); });
    QSignalSpy logged(&host, &EmulatorHost::logLine);
    QSignalSpy stoppedSpy(&host, &EmulatorHost::stoppedChanged);
    QSignalSpy finished(&host, &EmulatorHost::commandFinished);

    QVERIFY(host.start(config, nullptr));
    QVERIFY2(stoppedSpy.wait(5000), "the fake debugger never announced entry");

    // The flood is fully read out once its last line has been logged: every one
    // of the 65536 stdout bytes is in the buffer, and the two bytes of the stop
    // prompt are what takes it over the bound.
    QTRY_VERIFY_WITH_TIMEOUT(sawLogLine(logged, QStringLiteral("flood-marker-end-ok")), 10000);

    // Resume: the fake answers with the next stop's prompt, and nothing else.
    host.resume();
    QTRY_VERIFY_WITH_TIMEOUT(host.isStopped(), 5000);

    // The wedge's own symptom: the session must still answer commands.
    host.dumpRegisters();
    QVERIFY2(commandAnswered(&finished, QStringLiteral("r")), "the session wedged: no answer");

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
    host.loadSymbols();
    QVERIFY(finished.wait(15000));
    finished.clear();
    host.readBasepage();
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
    host.dumpRegisters();
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
    host.dumpRegisters();
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
    host.dumpRegisters();
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
    // A positive wait for the acceptance, not a fixed sleep (MIN-60).
    QTRY_VERIFY_WITH_TIMEOUT(server.connected(), 2000);

    // Feed the report split across two writes, giving the server a chance to read
    // the first before the second lands. "320x2" is an implausible height; the
    // old parser accepted it as a real 320x2 and cleared the buffer, so the
    // trailing "00" was lost and the reported size was wrong (finding C9).
    client.write("320x2");
    QVERIFY(client.waitForBytesWritten(2000));
    // Negative: the partial must not be emitted. A fixed window is the only
    // assertion available for "nothing happened".
    QTest::qWait(150);
    QCOMPARE(sizes.count(), 0); // held as a partial, not emitted

    client.write("00");
    QVERIFY(client.waitForBytesWritten(2000));
    QTRY_COMPARE_WITH_TIMEOUT(sizes.count(), 1, 2000); // completed to 320x200
    QCOMPARE(sizes.at(0).at(0).toInt(), 320);
    QCOMPARE(sizes.at(0).at(1).toInt(), 200);

    // The unbounded-buffer half: a blob longer than any single report that never
    // parses must be dropped, so the next clean report still gets through.
    client.write(QByteArray(200, '1')); // no 'x' — never a size
    QVERIFY(client.waitForBytesWritten(2000));
    QTest::qWait(150);
    client.write("640x480");
    QVERIFY(client.waitForBytesWritten(2000));
    // Without the cap the 200 stray bytes stay and pollute "640x480" (its width
    // field becomes an unparseable 200-digit number); with it the blob is dropped
    // and the report parses, so a second size is reported.
    QTRY_COMPARE_WITH_TIMEOUT(sizes.count(), 2, 2000);
    QCOMPARE(sizes.at(1).at(0).toInt(), 640);
    QCOMPARE(sizes.at(1).at(1).toInt(), 480);
}

QTEST_MAIN(TstEmulatorHost)
#include "tst_emulatorhost.moc"
