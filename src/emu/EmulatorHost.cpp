// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/EmulatorHost.h"

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


/// A prompt is the literal `> ` written before each blocking read. It carries no
/// trailing newline, so it is counted at the start of a stream or after a
/// newline.
const QRegularExpression &promptRe()
{
    static const QRegularExpression re(QStringLiteral(R"((?:^|\n)> )"));
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

int countPrompts(const QString &text)
{
    int count = 0;
    auto it = promptRe().globalMatch(text);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

} // namespace

EmulatorHost::EmulatorHost(QObject *parent)
    : IDebugBackend(parent)
{
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
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(2000);
        }
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
    // TEMP DIAGNOSTIC (macOS CI stop-detection failures; remove after)
    emit logLine(QStringLiteral("[diag] stdout chunk: ")
                 + QString::fromUtf8(data).replace(QLatin1Char('\n'), QStringLiteral("\\n"))
                       .replace(QLatin1Char('\r'), QStringLiteral("\\r")));

    const QString previous = m_stdoutText;
    m_stdoutText += QString::fromUtf8(data);

    // Bound memory while preserving the not-yet-logged tail.
    if (m_stdoutText.size() > 65536) {
        const int drop = m_stdoutText.size() - 32768;
        m_stdoutText.remove(0, drop);
        m_stdoutLoggedChars = qMax(0, m_stdoutLoggedChars - drop);
    }

    const int before = countPrompts(previous);
    const int after = countPrompts(m_stdoutText);
    for (int i = 0; i < qMax(0, after - before); ++i)
        onPrompt();

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

void EmulatorHost::command(const QString &commandText, quint32 dumpAddress, bool stackDump,
                           int dumpTag)
{
    Pending p;
    p.text = commandText;
    p.dumpAddress = dumpAddress;
    p.stackDump = stackDump;
    p.dumpTag = dumpTag;
    enqueue(p);
}

void EmulatorHost::enqueueSnapshotCommand(const QString &command, bool last)
{
    Pending p;
    p.text = command;
    p.batchMember = true;
    p.batchEnd = last;
    enqueue(p);
}

void EmulatorHost::dispatchNext()
{
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
    m_settleTimer->stop();
    m_stderrAtDispatch = m_stderrBuffer.size();

    m_process->write(m_current.text.toUtf8() + "\n");
    m_commandTimeout->start(kCommandTimeoutMs);
}

void EmulatorHost::processStderrData()
{
    if (!m_process)
        return;

    m_stderrBuffer += m_process->readAllStandardError();

    int nl;
    while ((nl = m_stderrBuffer.indexOf('\n')) >= 0) {
        const QByteArray raw = m_stderrBuffer.left(nl);
        m_stderrBuffer.remove(0, nl + 1);
        handleStderrLine(QString::fromUtf8(raw).remove(QLatin1Char('\r')));
    }

    // On builds without readline the debugger prompt is written to stderr rather
    // than stdout, with no trailing newline. Checked only after every complete
    // line is consumed, or a trailing prompt would be counted once per line still
    // in the buffer. The `> cmd` echoes from DebugUI_ParseLine/ParseFile cannot be
    // confused with it, because those always end in a newline.
    //
    // A prompt already sitting at the front of the buffer is stale: it is the one
    // from *before* the command we are waiting on, so only a prompt that has
    // arrived since is a completion signal. Tracked by remembering the length the
    // buffer had when the command was dispatched.
    if (stderrEndsWithPrompt(m_stderrBuffer) && m_stderrBuffer.size() > m_stderrAtDispatch)
        onPrompt();
}

void EmulatorHost::drainStderr()
{
    if (!m_process)
        return;

    // The response travels on stderr and the completion prompt on stdout, and the
    // two pipes are independent. Hatari flushes the response *before* printing the
    // next prompt, so by the time the prompt is readable the response is already
    // in its pipe — but Qt may not have read it out yet.
    //
    // `bytesAvailable()` reports Qt's internal buffer, not the pipe, so simply
    // reading in a loop returns nothing when the notifier has not run yet. That
    // was the earlier mistake: the loop drained an empty buffer and the command
    // completed without the response. `waitForReadyRead` waits on the pipe
    // itself and returns as soon as anything arrives, which for data already
    // written is immediate — this is not a sleep, it is a read that can block
    // only if there is genuinely nothing there.
    int guard = 0;
    while (guard++ < 8) {
        if (m_process->bytesAvailable() > 0) {
            processStderrData();
            continue;
        }
        if (!m_process->waitForReadyRead(20))
            break;
        processStderrData();
    }
}

void EmulatorHost::completeCurrent()
{
    if (!m_haveCurrent)
        return;

    // Take anything still queued before deciding the response is complete.
    drainStderr();

    m_commandTimeout->stop();
    m_settleTimer->stop();

    const QString commandText = m_current.text;
    const QString response = m_current.raw.trimmed();
    m_current.response = response;

    bool parsedState = false;
    if (commandText == QLatin1String("r")) {
        parseRegisters(response);
        parsedState = true;
    } else if (commandText == QLatin1String("info basepage")) {
        parseBasepage(response);
        parsedState = true;
    } else if (commandText.startsWith(QLatin1Char('d'))) {
        parseDisassembly(response);
        parsedState = true;
    } else if (commandText.startsWith(QLatin1Char('m')) && commandText.contains(QLatin1Char(' '))) {
        if (m_current.stackDump)
            emit stackDumpReady(m_current.dumpAddress, response);
        else
            emit memoryDumpReady(m_current.dumpAddress, response, m_current.dumpTag);
    }

    // The parse functions only fill m_state. Emission is decided here because
    // it is a property of the batch, not of one response: a snapshot queued by
    // refresh() is incomplete until its last command has been parsed, while a
    // standalone state command still reports an update of its own.
    if (parsedState && (!m_current.batchMember || m_current.batchEnd))
        emit stateUpdated(m_state);

    m_haveCurrent = false;
    const QString text = m_current.text;
    m_current = Pending();
    emit commandFinished(text, response);

    dispatchNext();
}

void EmulatorHost::step()
{
    command(QStringLiteral("s"));
}

void EmulatorHost::stepOver()
{
    command(QStringLiteral("n"));
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

    // Keep `b` / `b all` / watchpoint commands; drop dumps and register
    // queries. Continue is enabled at the entry stop, which is before
    // armBreakpoints() has been flushed, and writing `c` immediately used to
    // discard those unsent arms so a pre-Run breakpoint never fired. `profile`
    // control commands are kept too: they take effect at the continue itself
    // (Profile_CpuStart runs in DebugCpu_SetDebugging), so a `profile on`
    // armed at a stop and pruned here would silently collect nothing.
    QQueue<Pending> arms;
    while (!m_queue.isEmpty()) {
        const Pending p = m_queue.dequeue();
        if (isBreakpointCommand(p.text) || p.text.startsWith(QLatin1String("profile ")))
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
    enqueueSnapshotCommand(QStringLiteral("r"), false);
    enqueueSnapshotCommand(QStringLiteral("info basepage"), false);
    enqueueSnapshotCommand(QStringLiteral("d"), true);
}

void EmulatorHost::clearBreakpoints()
{
    // `b all` removes every conditional breakpoint. Note this also removes the

    // bootstrap entry breakpoint if it somehow still exists, which is fine
    // because it is set with `:once` and has already fired by this point.
    command(QStringLiteral("b all"));
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
        command(cmd);
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
    command(condition);
}

void EmulatorHost::requestMemoryDump(quint32 address, int length, int tag)
{
    // The address carries a `$` prefix so it is read as hex, but the count is a
    // bare number and the debugger reads that as **decimal**. Verified against
    // Hatari 2.6.1: `m $12596 100` returns 112 bytes (7 rows of 16), i.e. 100
    // bytes rounded up to a row; a hex reading would have returned 256.
    const QString cmd = QStringLiteral("m $%1 %2")
                            .arg(address, 0, 16)
                            .arg(length);
    command(cmd, address, false, tag);
}

void EmulatorHost::requestStackDump(quint32 address, int length)
{
    const QString cmd = QStringLiteral("m $%1 %2")
                            .arg(address, 0, 16)
                            .arg(length);
    command(cmd, address, true);
}

void EmulatorHost::dumpRegisters()
{
    command(QStringLiteral("r"));
}

void EmulatorHost::onPrompt()
{
    m_promptCount += 1;
    // TEMP DIAGNOSTIC (macOS CI stop-detection failures; remove after)
    emit logLine(QStringLiteral("[diag] prompt: inflight=%1 stopped=%2 owed=%3 count=%4 target=%5")
                     .arg(m_haveCurrent).arg(m_stopped).arg(m_owedPrompts)
                     .arg(m_promptCount).arg(m_promptTarget));

    // A prompt for a command that already timed out is not a completion: the
    // command's slot is gone, so this prompt is simply swallowed. Otherwise the
    // next command would be completed by the previous command's output.
    if (m_owedPrompts > 0) {
        m_owedPrompts -= 1;
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
        // consumed; setting m_awaitingEntryPrompt now would leave it stale,
        // and the NEXT stop's prompt (a pause or breakpoint) would be
        // swallowed as "the entry prompt", leaving the session running
        // forever from the UI's point of view.
        if (m_stopped)
            return;
        m_stopped = true;
        emit stoppedChanged(true);
        // Do NOT dispatch yet. After announcing entry the debugger prints its
        // session dump (autoloaded symbols, registers, the instruction at the
        // PC) and only then reaches its prompt. Dispatching now would attribute
        // that dump to the first command; wait for the prompt instead.
        m_awaitingEntryPrompt = true;
        return;
    }

    if (m_haveCurrent) {
        m_current.raw += (line + QLatin1Char('\n')).toUtf8();
        // More output arrived, so the response is not complete yet: extend the
        // quiet period.
        if (m_promptCount >= m_promptTarget)
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
