// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/EmulatorHost.h"

#include "build/ProcessUtil.h"
#include "emu/HatariProbe.h"
#include "emu/HatariTextParse.h"
#include "emu/Paths.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

namespace pist {

namespace {

/// How long to wait for a prompt before giving up on the current command. The
/// prompt is normally immediate; this only guards against a hung or wedged
/// emulator so the queue cannot stall forever.
constexpr int kCommandTimeoutMs = 10000;

/// Quiet period on stderr that marks a response as complete. The prompt arrives
/// on stdout and the response on stderr, and the two pipes are independent, so
/// completion is driven by stderr going quiet rather than by the prompt alone.
constexpr int kSettleMs = 40;

/// Wait for stderr inside a drain. A completing command's output is already
/// written — Hatari flushes the response before it prints the prompt that closes
/// it — so this only has to collect bytes that are in the pipe but unread, and a
/// short wait keeps the queue moving.
constexpr int kStderrDrainWaitMs = 20;

/// The same wait, widened for the one path whose tail may not have been written
/// yet: a command that outlived kCommandTimeoutMs is still running inside the
/// emulator, so the rest of its output can arrive *after* the prompt we swallow
/// for it. The drain is what keeps that tail out of the next command's response
/// (MIN-1), and nothing is waiting on this path — the command was already
/// reported as failed — so the window is wide enough to outlast a loaded
/// machine's scheduling of the tail. A window the tail can miss is a coin flip:
/// it lost on the macOS runner, where 20 ms did not cover a shell's `sleep` plus
/// its output.
constexpr int kOwedTailDrainWaitMs = 200;

/// `  D0 00000000   D1 00000019   D2 00002304   D3 00000000`
const QRegularExpression &dataRegRe()
{
    static const QRegularExpression re(QStringLiteral(R"(\b([DA][0-7])\s+([0-9A-Fa-f]{8}))"));
    return re;
}

/// `CPU=$fdd174, VBL=750, ...` — the debugger's *entry* header. Note that the
/// `r` command's own response does NOT contain this line, so it is only a
/// fallback source for the PC.
const QRegularExpression &cpuRe()
{
    static const QRegularExpression re(QStringLiteral(R"(CPU=\$?([0-9A-Fa-f]+))"));
    return re;
}

/// The register dump ends with the instruction at the PC, which is the reliable
/// source of the current PC:
///
///     Prefetch 7001 (MOVE) 7202 (MOVE) Chip latch 00000000
///     00012596 7001                     moveq #$01,d0
///     Next PC: 00012598
///
/// `Next PC` is the *following* address, so the instruction line is preferred.
const QRegularExpression &pcLineRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^([0-9A-Fa-f]{8})\s+[0-9A-Fa-f]{2,4}\s+\S)"),
        QRegularExpression::MultilineOption);
    return re;
}

/// `USP  000061A0 ISP  00007B2E`
const QRegularExpression &stackRegRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(USP\s+([0-9A-Fa-f]{8})\s+ISP\s+([0-9A-Fa-f]{8}))"));
    return re;
}

const QRegularExpression &statusRe()
{
    static const QRegularExpression re(QStringLiteral(R"(SR=([0-9A-Fa-f]{4}))"));
    return re;
}

const QRegularExpression &flagRe()
{
    static const QRegularExpression re(QStringLiteral(R"(\b([XNZVC])=([01]))"));
    return re;
}

/// `- Text segment   : 0x012596`
/// Anchored per line: responses are complete multi-line dumps.
const QRegularExpression &basepageRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\s*-\s*(TPA start|Text segment|Data segment|BSS segment)\s*:\s*0x([0-9A-Fa-f]+))"),
        QRegularExpression::MultilineOption);
    return re;
}


/// On stderr, `> <cmd>` is an *echo* of a command the debugger is about to run
/// (DebugUI_ParseLine and DebugUI_ParseFile both print it), not a prompt. Only a
/// trailing `> ` with no newline after it is a prompt, which is how builds
/// without readline write it (`fprintf(stderr, "> ")`).
bool stderrEndsWithPrompt(const QByteArray &buffer)
{
    return buffer.endsWith("> ");
}

} // namespace

EmulatorHost::EmulatorHost(QObject *parent)
    : IDebugBackend(parent)
{
    // The dump rows and the hardware summary ride on this backend's signals
    // (MAJ-45): registered here, before any connection can be made, so a
    // queued connection or a QSignalSpy can carry them.
    qRegisterMetaType<QList<MemoryRow>>("QList<MemoryRow>");
    qRegisterMetaType<HardwareSummary>("HardwareSummary");

    m_commandTimeout = new QTimer(this);
    m_commandTimeout->setSingleShot(true);
    connect(m_commandTimeout, &QTimer::timeout, this, &EmulatorHost::onCommandTimeout);

    // Completes the current command once its stderr output has stopped arriving.
    m_settleTimer = new QTimer(this);
    m_settleTimer->setSingleShot(true);
    connect(m_settleTimer, &QTimer::timeout, this, [this] {
        if (m_haveCurrent)
            completeCurrent();
    });

    // The embed socket forwards: size reports become embeddedSizeChanged for
    // MainWindow's display fit, and its log lines join ours.
    connect(&m_embedSocket, &EmbedSocket::sizeReported, this,
            [this](int width, int height) {
                emit embeddedSizeChanged(width, height);
            });
    connect(&m_embedSocket, &EmbedSocket::logLine, this,
            [this](const QString &line) { emit logLine(line); });
}

EmulatorHost::~EmulatorHost()
{
    stop();
}

bool EmulatorHost::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

QString EmulatorHost::writeBootstrapScript(const QString &directory,
                                           const HatariCapabilities &caps,
                                           QString *error)
{
    // Create the directory here rather than relying on the caller: this is
    // called before start() creates the session directory, and a failure to
    // write the script is otherwise reported as "the debugger never stopped".
    if (!paths::ensureDirectory(directory, error))
        return {};

    const QString path = QDir(directory).filePath(QStringLiteral("boot.ini"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot write bootstrap script '%1': %2")
                         .arg(path, file.errorString());
        return {};
    }

    QTextStream out(&file);

    // Version-gate: 2.6.1 has the boolean form, git main has three modes and a
    // CLI equivalent. Never assume either (docs/PLAN.md §5 rule 9).
    if (caps.hasSymbolAutoloadOption)
        out << "symbols autoload debugger\n";
    else
        out << "symbols autoload on\n";

    // Track the execution path so the PC-history view has something to show.
    // `history cpu` starts recording the recent PCs from here on.
    out << "history cpu\n";

    // Stop at program entry. `TEXT` is a Hatari variable rather than a symbol,
    // so this works before any symbol table is loaded; the upper guard keeps it
    // from matching ROM addresses.
    out << "b pc = TEXT && pc < $e00000 :once\n";

    // `echo` must never appear here: on 2.6.1 it aborts the emulator through a
    // failed assertion in Str_UnEscape (docs/PLAN.md §2.4).

    out.flush();
    file.close();
    return path;
}

bool EmulatorHost::openSocketServer(QString *error)
{
    // No socket requested: the emulator build has no support for one (it is
    // compiled only under HAVE_UNIX_DOMAIN_SOCKETS), so nothing will connect.
    // This is not an error — the session runs with stdin/stderr only.
    if (m_config.controlSocketPath.isEmpty())
        return true;

    // Hatari is the *client*: it calls connect() and never binds, so the IDE
    // has to be listening before the process starts. However, the socket is
    // only serviced from the SDL event pump while emulation is running, so it
    // carries control commands only, never debugger commands (docs/PLAN.md
    // §3.3). The server itself is shared with the HRDB backend.
    m_embedSocket.setRequestOnConnect(!m_config.parentWindowId.isEmpty());
    return m_embedSocket.listen(m_config.controlSocketPath, error);
}


// Every per-session framing field is reset here rather than in start(), so the
// next session cannot inherit the previous one's state. The fields that were
// missed before each corrupt framing in its own way: a stale m_owedPrompts
// swallows that many real prompts of the new session, a stale
// m_stderrAtDispatch larger than the fresh stderr buffer hides the prompt on
// builds that write it to stderr (Windows), a partial m_socketBuffer is
// concatenated with the first new size report, and a pending settle fires into
// the new session (docs/code-review-glm-001.md §P2).
void EmulatorHost::resetTransport()
{
    m_queue.clear();
    m_haveCurrent = false;
    m_current = Pending();
    m_promptCount = 0;
    m_promptTarget = 0;
    m_awaitingEntryPrompt = false;
    m_stdoutLoggedChars = 0;
    m_stdoutText.clear();
    m_promptCarry.clear();
    m_owedPrompts = 0;
    m_stderrBuffer.clear();
    m_stderrAtDispatch = 0;
    m_continueWhenIdle = false;
    m_embedSocket.close();
    m_commandTimeout->stop();
    m_settleTimer->stop();
}

bool EmulatorHost::start(const SessionConfig &config, QString *error)
{
    stop();
    m_config = config;
    m_state = MachineState();
    m_stopped = false;
    resetTransport();

    if (!paths::ensureDirectory(config.sessionDir, error))
        return false;

    // A socket is optional: it is unavailable on Windows, and the session works
    // over stdin/stderr without it.
    if (!openSocketServer(error))
        return false;

    m_process = new QProcess(this);

    // Config isolation + embedded-display X11 wiring, shared with the HRDB
    // backend (see makeSessionEnvironment).
    m_process->setProcessEnvironment(makeSessionEnvironment(config));

    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    // Every *read* here is channel-specific (readAllStandardOutput /
    // readAllStandardError, each with its own readyRead signal connected
    // below), but `bytesAvailable()` and `waitForReadyRead()` act on the
    // *current* read channel, which defaults to stdout. drainStderr() uses both
    // of those to pull the response tail out of the pipe, so without this they
    // inspected the stdout buffer and blocked their full 20 ms timeout at every
    // completion, with the response tail arriving only incidentally through a
    // readyReadStandardError emitted inside the wait (MAJ-13).
    m_process->setReadChannel(QProcess::StandardError);

    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        processStderrData();
    });

    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        handleStdoutData(m_process->readAllStandardOutput());
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit errorOccurred(tr("Failed to start Hatari. Check the executable path."));
    });

    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                m_stopped = false;
                m_haveCurrent = false;
                m_commandTimeout->stop();
                m_queue.clear();
                emit stoppedChanged(false);
                emit runningChanged(false);
                emit logLine(tr("Hatari exited (code %1).").arg(code));
                m_embedSocket.close();
            });

    const QStringList argv = config.toArgv();
    emit logLine(QStringLiteral("$ %1").arg(argv.join(QLatin1Char(' '))));
    m_process->start(argv.first(), argv.mid(1));

    if (!m_process->waitForStarted(5000)) {
        if (error)
            *error = tr("Hatari did not start: %1").arg(m_process->errorString());
        stop();
        return false;
    }

    emit runningChanged(true);
    return true;
}

void EmulatorHost::stop()
{
    // Captured before the teardown, and from our own state rather than from the
    // process: the process may already be dead, and it is exactly that case
    // (plus the finished handler having been disconnected below) in which the
    // session end would otherwise go unreported.
    const bool hadSession = m_process != nullptr || m_stopped;

    m_queue.clear();
    m_haveCurrent = false;
    m_stopped = false;
    if (m_commandTimeout)
        m_commandTimeout->stop();
    // A pending settle must not fire into the next session. Harmless today only
    // because completeCurrent checks m_haveCurrent.
    if (m_settleTimer)
        m_settleTimer->stop();

    if (m_process) {
        killAndRelease(m_process, this);
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_embedSocket.close();

    // `finished` is the only other emitter, and it is disconnected above, so a
    // manual stop would otherwise leave the UI showing "Stopped in debugger"
    // with step/continue still enabled, and never tell remote-control clients
    // the session ended (MainWindow's runningChanged handler). Both are state
    // signals, so repeating them for an already-dead process is harmless.
    if (hadSession) {
        emit stoppedChanged(false);
        emit runningChanged(false);
    }
}

void EmulatorHost::handleStdoutData(const QByteArray &data)
{
    if (data.isEmpty())
        return;

    const QString appended = QString::fromUtf8(data);
    m_stdoutText += appended;

    // Bound memory while preserving the not-yet-logged tail. This is a logging
    // concern only: the prompt tally below never re-reads the buffer, so
    // dropping its leading bytes cannot drop a prompt with them (MAJ-15).
    if (m_stdoutText.size() > 65536) {
        const int drop = m_stdoutText.size() - 32768;
        m_stdoutText.remove(0, drop);
        m_stdoutLoggedChars = qMax(0, m_stdoutLoggedChars - drop);
    }

    // Count the prompts in the newly arrived text only, seeded with the
    // previous chunk's last two characters so a prompt split across two chunks
    // ("\n>" then " ") is still seen.
    //
    // A prompt is `> ` at a line start **or immediately after another prompt**:
    // a continue prints nothing on stdout, so the next stop's prompt follows
    // the previous prompt's space with no newline between it. The old
    // `(?:^|\n)> ` pattern could not see that second form — two adjacent
    // prompts merged into one match — and the carry heuristic then suppressed
    // the chunk-boundary case as "already counted". Every stop after a silent
    // continue could lose its completion signal ("did not respond to 'r' in
    // time"), a blind spot masked until CRIT-1 stopped the settle timer from
    // firing early on every stderr line.
    const int carried = m_promptCarry.size();
    const QString scanned = m_promptCarry + appended;
    // Whether the previous chunk ended exactly on a prompt is derivable from
    // the carry: such a chunk's last two characters are the prompt's own "> ".
    const bool chunkStartedAfterPrompt = carried == 2 && m_promptCarry == QLatin1String("> ");
    static const QRegularExpression promptToken(QStringLiteral("> "));
    int promptEnd = -1; // end of the last prompt-shaped token, scanned coords
    auto it = promptToken.globalMatch(scanned);
    while (it.hasNext()) {
        const auto match = it.next();
        const int start = match.capturedStart();
        const int end = match.capturedEnd();
        const bool lineStart = start > 0 && scanned.at(start - 1) == QLatin1Char('\n');
        const bool chainStart = start == 0 ? chunkStartedAfterPrompt : (start == promptEnd);
        const bool streamStart = start == 0 && carried == 0;
        if (!(lineStart || chainStart || streamStart))
            continue;
        promptEnd = end;
        // A match wholly inside the carry was counted when it arrived: without
        // this, a prompt at the end of one chunk ("\n> ") would be counted
        // again on the next.
        if (end <= carried)
            continue;
        onPrompt();
    }
    m_promptCarry = scanned.right(2);

    // Emit only newly arrived text; re-emitting the whole buffer on every chunk
    // would repeat the entire log.
    if (m_stdoutLoggedChars < m_stdoutText.size()) {
        const QString fresh = m_stdoutText.mid(m_stdoutLoggedChars);
        m_stdoutLoggedChars = m_stdoutText.size();
        for (const QString &line : fresh.split(QLatin1Char('\n'))) {
            const QString trimmed = line.trimmed();
            if (!trimmed.isEmpty() && trimmed != m_current.text)
                emit logLine(trimmed);
        }
    }
}

void EmulatorHost::enqueue(Pending pending)
{
    if (!isRunning()) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }
    m_queue.enqueue(pending);
    dispatchNext();
}

void EmulatorHost::command(const QString &commandText)
{
    // Free text only — the user's console and the remote `cmd` verb. Its
    // response is text for the log, never a state read: nothing a caller passes
    // here can rewrite MachineState or be published to a pane (MAJ-12). The
    // entry attach's reads and the panes' dumps are typed intents now, so the
    // exact-match table that used to classify this path has no callers left.
    //
    // The one thing read out of the text is whether it must survive a resume(),
    // which is the debugger's own language and the user's to write: see
    // survivesResume.
    enqueueIntent(commandText, ResponseKind::None, survivesResume(commandText));
}

void EmulatorHost::enqueueIntent(const QString &commandText, ResponseKind kind, bool keepOnResume)
{
    Pending p;
    p.text = commandText;
    p.kind = kind;
    p.keepOnResume = keepOnResume;
    enqueue(p);
}

void EmulatorHost::enqueueSnapshotCommand(const QString &command, ResponseKind kind, bool last)
{
    Pending p;
    p.text = command;
    p.kind = kind;
    p.batchMember = true;
    p.batchEnd = last;
    enqueue(p);
}

void EmulatorHost::dispatchNext()
{
    // Single entry. The pre-drain in the body can pull a prompt whose handlers
    // (stoppedChanged consumers queueing the entry attach) call back into
    // dispatchNext()/processStderrData() while this frame is still on the
    // stack; a nested body would interleave the queue and the stderr buffer.
    // Defer instead: the outermost frame re-runs the attempt until no request
    // is pending and no command is in flight.
    if (m_inStderr || m_inDispatch) {
        m_dispatchPending = true;
        return;
    }
    m_inDispatch = true;
    do {
        m_dispatchPending = false;
        dispatchNextOnce();
    } while (m_dispatchPending && !m_haveCurrent);
    m_inDispatch = false;
}

void EmulatorHost::dispatchNextOnce()
{
    // Anything the debugger has already written belongs to the state *before*
    // the command we are about to send, never to its response — and the two
    // pipes have no ordering between them, so the entry prompt (2 bytes on
    // stdout) can be read before the multi-KB session dump written before it on
    // stderr. The prompt then dispatches the first command, and the dump, still
    // sitting unread in the stderr pipe, lands in that command's response:
    // pulling it out first is what actually keeps the entry dump out of the
    // first command (MAJ-14). Non-blocking, and it may re-enter dispatchNext
    // (a drained prompt calls onPrompt), so every check below is re-evaluated
    // after it.
    if (m_process && m_process->bytesAvailable() == 0)
        m_process->waitForReadyRead(0);
    if (m_process && m_process->bytesAvailable() > 0)
        processStderrData();

    if (m_haveCurrent || !m_process
        || m_process->state() != QProcess::Running) {
        return;
    }

    // The debugger's entry dump is still being written; its prompt will
    // re-trigger dispatch once it is genuinely idle. Sending now would mix that
    // dump into the first command's response.
    if (m_awaitingEntryPrompt)
        return;

    // A timed-out command may still be executing; wait for its prompt to come
    // back before sending another, or the late prompt would complete the next
    // command with the previous command's output.
    if (m_owedPrompts > 0)
        return;

    if (m_queue.isEmpty()) {
        if (m_continueWhenIdle)
            finishContinue();
        return;
    }

    m_current = m_queue.dequeue();
    m_haveCurrent = true;

    if (!m_stopped) {
        // Nothing reads stdin while emulation is running, so a written command
        // would sit in the pipe and fire at the next debugger entry, corrupting
        // the framing of whatever command is current then. Wait for the debugger
        // instead.
        m_queue.prepend(m_current);
        m_haveCurrent = false;
        return;
    }

    // The debugger is currently blocked at a prompt, which has already been
    // counted. Our command completes when the *next* prompt appears, so record
    // the count as it stands now.
    m_promptTarget = m_promptCount;
    m_current.raw.clear();
    m_current.response.clear();
    // A trailing prompt already in the buffer belongs to the idle state the
    // debugger is sitting at, not to this command — and it must not survive
    // into the completion check: on builds whose prompt lives on stderr
    // (macOS readline puts it there, not on stdout), every command leaves the
    // buffer ending in "> " again, so a size-only comparison can never tell
    // the new prompt from the stale one. Chopping it makes any later trailing
    // prompt provably new.
    if (stderrEndsWithPrompt(m_stderrBuffer))
        m_stderrBuffer.chop(2);
    m_stderrAtDispatch = m_stderrBuffer.size();

    m_process->write(m_current.text.toUtf8() + "\n");
    m_commandTimeout->start(kCommandTimeoutMs);
}

void EmulatorHost::processStderrData()
{
    // Single entry. The line loop below runs handleStderrLine, whose handlers
    // can dispatch (and a nested dispatch pre-drains back into here) while the
    // buffer still holds un-consumed lines; a nested pass would consume the
    // same buffer under the outer loop's feet. Defer to the outer pass — its
    // loop keeps consuming after the nested frame unwinds — and run any
    // dispatch request once the buffer is fully processed, so a command is
    // never written while output that predates it is still unread.
    if (m_inStderr || !m_process)
        return;
    m_inStderr = true;

    m_stderrBuffer += m_process->readAllStandardError();

    int nl;
    while ((nl = m_stderrBuffer.indexOf('\n')) >= 0) {
        const QByteArray raw = m_stderrBuffer.left(nl);
        m_stderrBuffer.remove(0, nl + 1);
        // Consuming a line moves the dispatch watermark with it: handlers run
        // by the line loop (the entry banner arms queued commands) can dispatch
        // while the buffer still holds un-consumed lines, and without this the
        // recorded offset would point past bytes that no longer exist — which
        // is how every stop went undetected on stderr-prompt builds (macOS CI).
        m_stderrAtDispatch = qMax(0, m_stderrAtDispatch - (nl + 1));
        handleStderrLine(QString::fromUtf8(raw).remove(QLatin1Char('\r')));
    }

    // On builds without readline the debugger prompt is written to stderr rather
    // than stdout, with no trailing newline. Checked only after every complete
    // line is consumed, or a trailing prompt would be counted once per line still
    // in the buffer. The `> cmd` echoes from DebugUI_ParseLine/ParseFile cannot be
    // confused with it, because those always end in a newline.
    //
    // A prompt already sitting in the buffer is stale: it is the one from
    // *before* the command we are waiting on, so only a prompt that has arrived
    // since is a completion signal. Tracked by remembering the length the buffer
    // had when the command was dispatched — an offset the line loop above keeps
    // valid as it consumes — and by consuming the prompt on fire, so unrelated
    // stderr noise cannot re-fire the same prompt once the watermark has shifted.
    if (stderrEndsWithPrompt(m_stderrBuffer) && m_stderrBuffer.size() > m_stderrAtDispatch) {
        m_stderrBuffer.chop(2);
        onPrompt();
    }

    m_inStderr = false;
    if (m_dispatchPending) {
        m_dispatchPending = false;
        dispatchNext();
    }
}

void EmulatorHost::drainStderr(int firstWaitMs)
{
    if (!m_process)
        return;

    // The response travels on stderr and the completion prompt on stdout, and the
    // two pipes are independent. Hatari flushes the response *before* printing the
    // next prompt, so by the time the prompt is readable the response is already
    // in its pipe — but Qt may not have read it out yet.
    //
    // Both of these act on the process's *current* read channel, which start()
    // points at StandardError: `bytesAvailable()` reports Qt's buffer for that
    // channel, and `waitForReadyRead` waits on that channel's pipe. On the
    // default (stdout) channel this loop inspected the stdout buffer and its wait
    // ran its full timeout on every completion, because nothing is ever pending
    // on stdout at completion time (MAJ-13). For stderr it returns as soon as
    // anything arrives, which for data already written is immediate — this is not
    // a sleep, it is a read that can block only if there is genuinely nothing
    // there.
    //
    // Only the first wait is the caller's to choose: a widened window is for a
    // tail that may not have been written yet, and once anything has arrived what
    // follows is bytes already in the pipe, which the short wait collects.
    int guard = 0;
    while (guard < 8) {
        const int waitMs = guard == 0 ? firstWaitMs : kStderrDrainWaitMs;
        ++guard;
        if (m_process->bytesAvailable() > 0) {
            processStderrData();
            continue;
        }
        if (!m_process->waitForReadyRead(waitMs))
            break;
        processStderrData();
    }
}

void EmulatorHost::completeCurrent()
{
    if (!m_haveCurrent)
        return;

    // Take anything still queued before deciding the response is complete.
    drainStderr(kStderrDrainWaitMs);

    m_commandTimeout->stop();
    m_settleTimer->stop();

    const QString commandText = m_current.text;
    const QString response = m_current.raw.trimmed();
    m_current.response = response;

    // What the response *is* came in with the request (see Pending::kind); the
    // command text is not consulted anywhere. A `db pc = $X :once` used to
    // match a `d` prefix and be parsed as a disassembly, which clears the cached
    // snapshot and publishes an empty state update (MAJ-12).
    bool parsedState = false;
    switch (m_current.kind) {
    case ResponseKind::Registers:
        parseRegisters(response);
        parsedState = true;
        break;
    case ResponseKind::Basepage:
        parseBasepage(response);
        parsedState = true;
        break;
    case ResponseKind::Disassembly:
        parseDisassembly(response);
        parsedState = true;
        // A caller that asked about one address wants the text back as well as
        // the snapshot (the remote `disasm` verb); the PC read reports neither,
        // because nothing is waiting on its text.
        if (m_current.reportDisassembly)
            emit disassemblyReady(m_current.disassemblyAddress, response);
        break;
    case ResponseKind::MemoryDump: {
        // Only a requestMemoryDump()/requestStackDump() pending can get here,
        // so the dump carries the address and the pane tag of the pane that
        // asked: a text `m`/`memdump` reaches its caller through
        // commandFinished instead of being published as someone's dump.
        //
        // Parsed once, here (MAJ-45): the rows are what the panes and the
        // step-out path read. Parsing it per consumer meant a stack dump was
        // read twice from the same text, and an `info <subject>` report's
        // summary was re-derived by a regex in the pane.
        const QList<MemoryRow> rows = parseMemoryDump(response);
        if (m_current.stackDump)
            emit stackDumpReady(m_current.dumpAddress, rows);
        else
            emit memoryDumpReady(m_current.dumpAddress, rows, m_current.dumpTag);
        break;
    }
    case ResponseKind::None:
        break;
    }

    // The parse functions only fill m_state. Emission is decided here because
    // it is a property of the batch, not of one response: a snapshot queued by
    // refresh() is incomplete until its last command has been parsed, while a
    // standalone state command still reports an update of its own.
    if (parsedState && (!m_current.batchMember || m_current.batchEnd))
        emit stateUpdated(m_state);

    // A profile save is complete whatever the debugger answered: the file is
    // what the caller reads, and a save that failed produces a parse error
    // there rather than a silent "Saving…".
    if (!m_current.profilePath.isEmpty())
        emit profileSaveFinished(m_current.profilePath);

    // A typed `info <subject>` report is not part of MachineState: it is
    // parsed into its summary here and goes to the caller that asked, named by
    // its subject (MAJ-45).
    if (!m_current.infoSubject.isEmpty())
        emit hardwareInfoReady(m_current.infoSubject, hataritext::parseHardwareInfo(response));

    // The execution-path read's text, for the pane that asked (a typed request
    // reports its own answer rather than a caller matching command text).
    if (m_current.reportHistory)
        emit historyReady(response);

    m_haveCurrent = false;
    const QString text = m_current.text;
    m_current = Pending();
    emit commandFinished(text, response);

    dispatchNext();
}

void EmulatorHost::step()
{
    // An internal control command, not free text: it goes straight to the
    // queue with no caller-supplied text (MAJ-21).
    enqueueIntent(QStringLiteral("s"));
}

void EmulatorHost::stepOver()
{
    enqueueIntent(QStringLiteral("n"));
}

void EmulatorHost::finishContinue()
{
    m_continueWhenIdle = false;
    m_stopped = false;
    emit stoppedChanged(false);
    m_process->write("c\n");
}

void EmulatorHost::resume()
{
    if (!m_process || m_process->state() != QProcess::Running)
        return;
    if (m_continueWhenIdle)
        return;
    if (!m_stopped)
        return;

    // Keep the arms and the profile control commands queued in the same stack
    // frame as this resume; drop dumps and reads. Continue is enabled at the
    // entry stop, which is before armBreakpoints() has been flushed, and
    // writing `c` immediately used to discard those unsent arms so a pre-Run
    // breakpoint never fired. Each request states its own keep rule (see
    // Pending::keepOnResume); a free-text command's comes from its own text.
    QQueue<Pending> arms;
    while (!m_queue.isEmpty()) {
        const Pending p = m_queue.dequeue();
        if (p.keepOnResume)
            arms.enqueue(p);
    }
    m_queue = arms;

    if (!m_haveCurrent && m_queue.isEmpty()) {
        finishContinue();
        return;
    }

    m_continueWhenIdle = true;
    if (!m_haveCurrent)
        dispatchNext();
}

void EmulatorHost::refresh()
{
    // Match HrdbBackend::refresh: without a session each queued command would
    // dispatch and emit its own "No emulator session is running." — three
    // identical errors from one refresh. Guard once instead.
    if (!isRunning()) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }

    // The three responses each fill a different part of MachineState, so they
    // are queued as one batch: stateUpdated fires once, after the disassembly —
    // the last of the three — has been parsed, and carries the complete state.
    // One emission per response made every listener react three times and queue
    // three copies of the memory, stack and hardware refresh.
    enqueueSnapshotCommand(QStringLiteral("r"), ResponseKind::Registers, false);
    enqueueSnapshotCommand(QStringLiteral("info basepage"), ResponseKind::Basepage, false);
    enqueueSnapshotCommand(QStringLiteral("d"), ResponseKind::Disassembly, true);
}

void EmulatorHost::clearBreakpoints()
{
    // `b all` removes every conditional breakpoint. Note this also removes the
    // bootstrap entry breakpoint if it somehow still exists, which is fine
    // because it is set with `:once` and has already fired by this point.
    //
    // Kept across a resume(): a clear-then-arm happens in one stack frame with
    // the continue (see survivesResume).
    enqueueIntent(QStringLiteral("b all"), ResponseKind::None, true);
}

void EmulatorHost::pause()
{
    if (m_stopped)
        return;
    if (!m_embedSocket.connected()) {
        emit errorOccurred(tr("Pause needs the control socket, which this Hatari "
                              "build does not have."));
        return;
    }
    // `hatari-stop` only clears the VBL loop's active flag and never enters
    // the debugger — no prompt would arrive and the session would wedge. So
    // arm an always-true one-shot breakpoint instead: `hatari-debug` lines run
    // immediately (the socket is serviced from the SDL event pump while
    // emulation runs, docs/PLAN.md §3.3), the condition fires at the next
    // instruction, and the debugger enters normally, producing the standard
    // prompt/stop flow. BreakCond operators are the single characters
    // = ! < > (no <> form), so `pc ! 0` is the always-true condition; PC is
    // never 0 after boot (the reset vectors are only read, not executed).
    m_embedSocket.writeLine("hatari-debug b pc ! 0 :once\n");
}

void EmulatorHost::setFloppyImage(int drive, const QString &path)
{
    if (drive < 0 || drive > 1)
        return;
    const QChar letter(QLatin1Char('A' + drive));
    if (!isRunning()) {
        emit logLine(tr("Floppy %1: %2 (applies when the emulator starts)")
                         .arg(letter, path.isEmpty() ? tr("empty") : path));
        return;
    }
    // Default Run stops at entry: Hatari is in the debugger and does not
    // service the control socket. `setopt` is the debugger command that
    // applies --disk-a/--disk-b (and `none` to eject). Paths with spaces are
    // staged first because Hatari tokenizes with strtok. While emulation is
    // running stdin is unread, so that case uses hatari-debug (the same
    // socket path as pause) rather than hatari-option.
    const QString staged = floppyImageForDebugger(m_config.sessionDir, drive, path);
    const QString cmd = floppySetoptCommand(drive, staged);
    if (cmd.isEmpty())
        return;
    emit logLine(tr("Floppy %1: %2").arg(letter, path.isEmpty() ? tr("ejected") : path));
    if (isStopped()) {
        // Hatari's `setopt` over stdin — the only debugger channel it reads
        // while stopped, so the running case below takes the control socket.
        enqueueIntent(cmd);
        return;
    }
    if (!m_embedSocket.connected()) {
        emit logLine(tr("Pause the program (or stop at a breakpoint) to change floppy %1.")
                         .arg(letter));
        return;
    }
    m_embedSocket.writeLine(QByteArray("hatari-debug ") + cmd.toUtf8() + '\n');
}

void EmulatorHost::armBreakpoint(const QString &condition)
{
    // The caller's condition is already the debugger's own `b` command, and
    // stdin carries it verbatim. Kept across a resume() (see survivesResume):
    // Continue is enabled at the entry stop, before the arms have flushed.
    enqueueIntent(condition, ResponseKind::None, true);
}

void EmulatorHost::breakAtAddressOnce(quint32 address)
{
    // Hatari's one-shot form; `:once` makes it remove itself on the hit, which
    // is why nothing has to delete it afterwards in the common case.
    enqueueIntent(QStringLiteral("b pc = $%1 :once").arg(address, 0, 16), ResponseKind::None,
                  true);
}

void EmulatorHost::requestMemoryDump(quint32 address, int length, int tag)
{
    // The address carries a `$` prefix so it is read as hex, but the count is a
    // bare number and the debugger reads that as **decimal**. Verified against
    // Hatari 2.6.1: `m $12596 100` returns 112 bytes (7 rows of 16), i.e. 100
    // bytes rounded up to a row; a hex reading would have returned 256.
    Pending p;
    p.text = QStringLiteral("m $%1 %2").arg(address, 0, 16).arg(length);
    p.kind = ResponseKind::MemoryDump;
    p.dumpAddress = address;
    p.dumpTag = tag;
    enqueue(p);
}

void EmulatorHost::requestStackDump(quint32 address, int length)
{
    Pending p;
    p.text = QStringLiteral("m $%1 %2").arg(address, 0, 16).arg(length);
    p.kind = ResponseKind::MemoryDump;
    p.dumpAddress = address;
    p.stackDump = true;
    enqueue(p);
}

void EmulatorHost::dumpRegisters()
{
    Pending p;
    p.text = QStringLiteral("r");
    p.kind = ResponseKind::Registers;
    enqueue(p);
}

void EmulatorHost::loadSymbols()
{
    // `symbols prg` relocates the program's symbols against the live basepage.
    enqueueIntent(QStringLiteral("symbols prg"));
}

void EmulatorHost::infoSubject(const QString &subject)
{
    Pending p;
    p.text = QStringLiteral("info ") + subject;
    // The report is not part of MachineState: it is answered to the caller that
    // asked, named by its subject.
    p.infoSubject = subject;
    enqueue(p);
}

void EmulatorHost::readBasepage()
{
    // Parsed into the section bases; the response text is the debugger's
    // basepage dump.
    enqueueIntent(QStringLiteral("info basepage"), ResponseKind::Basepage);
}

void EmulatorHost::setDisasmEngine(DisasmEngine engine)
{
    enqueueIntent(engine == DisasmEngine::Ext ? QStringLiteral("setopt --disasm ext")
                                              : QStringLiteral("setopt --disasm uae"));
}

void EmulatorHost::profileOn()
{
    // Kept across a resume(): collection starts at the continue itself, so a
    // pruned `profile on` collects nothing while the UI says "Collecting".
    enqueueIntent(QStringLiteral("profile on"), ResponseKind::None, true);
}

void EmulatorHost::profileOff()
{
    enqueueIntent(QStringLiteral("profile off"), ResponseKind::None, true);
}

void EmulatorHost::profileSave(const QString &path)
{
    Pending p;
    p.text = QStringLiteral("profile save %1").arg(path);
    p.profilePath = path;
    p.keepOnResume = true;
    enqueue(p);
}

void EmulatorHost::writeRegister(const QString &name, quint32 value)
{
    // The '=' is mandatory in Hatari's register-set syntax, and a successful
    // set prints nothing — the caller refreshes to show the new value.
    enqueueIntent(QStringLiteral("r %1=$%2").arg(name).arg(value, 0, 16));
}

void EmulatorHost::writeMemoryByte(quint32 address, quint8 value)
{
    // `w b <addr> <value>` writes one byte and prints nothing on success.
    enqueueIntent(QStringLiteral("w b $%1 $%2").arg(address, 0, 16).arg(value, 0, 16));
}

void EmulatorHost::readDisassembly()
{
    // At the PC, which is what the entry attach and the stop refresh want.
    enqueueIntent(QStringLiteral("d"), ResponseKind::Disassembly);
}

void EmulatorHost::readDisassemblyAt(quint32 address)
{
    Pending p;
    p.text = QStringLiteral("d $%1").arg(address, 0, 16);
    p.kind = ResponseKind::Disassembly;
    // The caller asked about one address and wants the text back; the snapshot
    // is filled as for any disassembly read.
    p.reportDisassembly = true;
    p.disassemblyAddress = address;
    enqueue(p);
}

void EmulatorHost::readHistory(int count)
{
    // `history <count>` prints the last count recorded PCs, one per line.
    Pending p;
    p.text = QStringLiteral("history %1").arg(count);
    p.reportHistory = true;
    enqueue(p);
}

void EmulatorHost::onPrompt()
{
    m_promptCount += 1;

    // A prompt for a command that already timed out is not a completion: the
    // command's slot is gone, so this prompt is simply swallowed. Otherwise the
    // next command would be completed by the previous command's output.
    if (m_owedPrompts > 0) {
        m_owedPrompts -= 1;
        // Drain the timed-out command's tail before the slot (or the queue) is
        // released. The prompt and the response travel on different pipes, so
        // the owed prompt can be readable while the command's last response
        // lines are still unread on stderr — or, because a command this transport
        // gave up on is still running in the emulator, not written yet. Without
        // this they land in the next command's response (the same contamination
        // completeCurrent() drains against, with the wider window that a tail
        // still on its way needs — MIN-1).
        drainStderr(kOwedTailDrainWaitMs);
        if (!m_haveCurrent)
            dispatchNext();
        return;
    }

    // The debugger is now idle at a prompt, so its entry dump is complete.
    if (m_awaitingEntryPrompt) {
        m_awaitingEntryPrompt = false;
        dispatchNext();
        return;
    }

    // A prompt means the previous command finished and the debugger is reading
    // again. Completion cannot happen immediately: the command's output travels
    // on stderr, and the prompt on stdout, and the two pipes have no ordering
    // guarantee between them — the prompt can be readable before the tail of the
    // response is. Wait for a short quiet period on stderr instead. A fixed
    // zero-delay hop is not enough; it loses the race whenever the response is
    // large, such as a break-in session dump.
    if (m_haveCurrent && m_promptCount >= m_promptTarget) {
        m_settleTimer->start(kSettleMs);
        return;
    }

    // Nothing in flight, yet the debugger is sitting at a prompt: it must have
    // just entered — a breakpoint hit, an exception, or a manual break-in.
    //
    // This cannot be detected from output text. The "You have entered debug
    // mode" banner is a `static` in DebugUI that is set to NULL after its first
    // print, so it appears exactly once per session and says nothing about
    // subsequent stops. The prompt is the reliable signal, and it is structural
    // rather than parsed.
    //
    // Deliberately not gated on the queue being empty. `dispatchNext` re-queues
    // a command rather than sending it while emulation is running, so a command
    // queued just before a resume leaves the queue non-empty — and testing for
    // emptiness here would miss the entry entirely and stall until the command
    // timeout. A prompt with nothing in flight means the debugger is waiting for
    // us, whatever is queued; dispatching is safe once we record the stop.
    if (!m_haveCurrent && !m_stopped) {
        m_stopped = true;
        emit stoppedChanged(true);
        dispatchNext();
    }
}

void EmulatorHost::onCommandTimeout()
{
    if (!m_haveCurrent)
        return;

    // The command is still executing (step-over legitimately blocks until its
    // internal breakpoint hits). Do not resend: just record that one prompt is
    // owed, release the slot, and report what arrived so far.
    m_owedPrompts += 1;
    m_settleTimer->stop();
    m_haveCurrent = false;
    const QString command = m_current.text;
    const QString response = m_current.raw.trimmed();
    m_current = Pending();
    emit errorOccurred(
        tr("The debugger did not respond to '%1' in time.").arg(command));
    emit commandFinished(command, response);
    dispatchNext();
}


void EmulatorHost::handleStderrLine(const QString &line)
{
    if (line.contains(QLatin1String("You have entered debug mode"))) {
        emit logLine(line);
        // The banner arrives on stderr amid the multi-KB entry dump, while the
        // entry prompt is 2 bytes on stdout — the prompt's notifier can fire
        // first. If it did, m_stopped is already set and the prompt has been
        // consumed, so this early-out is what keeps a later `true` from
        // re-arming the guard: a guard set after the prompt was handled would
        // swallow the NEXT stop's prompt (a pause or a breakpoint), leaving the
        // session looking like it is still running. That is also why the guard
        // is set exactly once, here, *before* the stop below and never again.
        if (m_stopped)
            return;
        m_stopped = true;
        // Arm the guard before publishing the stop. stoppedChanged(true) runs
        // its handlers synchronously, and MainWindow's queues the entry attach
        // (`symbols prg` / `info basepage` / `r` / `d`) there: with the guard
        // armed afterwards, the first command was already written to stdin
        // while the debugger was still printing its entry dump, so the rest of
        // the dump was appended to that command's response (MAJ-14).
        m_awaitingEntryPrompt = true;
        emit stoppedChanged(true);
        return;
    }

    if (m_haveCurrent) {
        m_current.raw += (line + QLatin1Char('\n')).toUtf8();
        // More output arrived, so the response is not complete yet: extend the
        // quiet period. Only a prompt that has actually arrived since this
        // command was dispatched may start it. m_promptTarget is the count *at
        // dispatch*, so testing `>=` here would be true from the command's own
        // first output byte, and any 40 ms gap in that output would complete the
        // command early — dispatching the next one while the debugger is still
        // executing this one, after which every response carries the previous
        // command's output for the rest of the session. onPrompt() increments
        // before comparing, which is why its `>=` means "a prompt arrived"; this
        // site does not, so it must use `>`.
        if (m_promptCount > m_promptTarget)
            m_settleTimer->start(kSettleMs);
    }

    emit logLine(line);
}

void EmulatorHost::parseRegisters(const QString &response)
{
    // Prefer the instruction line: it is the address actually executing. The
    // `CPU=` header only appears in the debugger's entry dump, not in an `r`
    // response.
    auto pcLine = pcLineRe().match(response);
    if (pcLine.hasMatch()) {
        m_state.pc = pcLine.captured(1).toUInt(nullptr, 16);
    } else {
        auto cpu = cpuRe().match(response);
        if (cpu.hasMatch())
            m_state.pc = cpu.captured(1).toUInt(nullptr, 16);
    }

    auto it = dataRegRe().globalMatch(response);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString name = m.captured(1).toUpper();
        const quint32 value = m.captured(2).toUInt(nullptr, 16);
        const int index = name.mid(1).toInt();
        if (name.startsWith(QLatin1Char('D')))
            m_state.regs.d[index] = value;
        else
            m_state.regs.a[index] = value;
    }

    auto stack = stackRegRe().match(response);
    if (stack.hasMatch()) {
        m_state.regs.usp = stack.captured(1).toUInt(nullptr, 16);
        m_state.regs.isp = stack.captured(2).toUInt(nullptr, 16);
    }

    auto status = statusRe().match(response);
    if (status.hasMatch()) {
        m_state.regs.sr = static_cast<quint16>(status.captured(1).toUInt(nullptr, 16));
        auto flags = flagRe().globalMatch(response);
        while (flags.hasNext()) {
            const auto f = flags.next();
            const bool on = f.captured(2) == QLatin1String("1");
            switch (f.captured(1).at(0).unicode()) {
            case 'X': m_state.regs.flagX = on; break;
            case 'N': m_state.regs.flagN = on; break;
            case 'Z': m_state.regs.flagZ = on; break;
            case 'V': m_state.regs.flagV = on; break;
            case 'C': m_state.regs.flagC = on; break;
            default: break;
            }
        }
        m_state.regs.valid = true;
    }
    // No emission here: completeCurrent owns it, so a refresh batch reports
    // once with the complete snapshot.
}

void EmulatorHost::parseBasepage(const QString &response)
{
    auto it = basepageRe().globalMatch(response);
    while (it.hasNext()) {
        const auto m = it.next();
        const quint32 value = m.captured(2).toUInt(nullptr, 16);
        const QString what = m.captured(1);
        if (what == QLatin1String("Text segment"))
            m_state.textBase = value;
        else if (what == QLatin1String("Data segment"))
            m_state.dataBase = value;
        else if (what == QLatin1String("BSS segment"))
            m_state.bssBase = value;
    }
    // Emission is completeCurrent's, so a refresh batch reports once.
}

void EmulatorHost::parseDisassembly(const QString &response)
{
    hataritext::parseDisassembly(response, &m_state);
    // Emission is completeCurrent's, so a refresh batch reports once.
}

} // namespace pist
