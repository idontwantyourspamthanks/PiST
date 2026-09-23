// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HrdbBackend.h"

#include "build/ProcessUtil.h"
#include "emu/EmulatorHost.h"
#include "emu/HatariTextParse.h"
#include "emu/MemoryDump.h"
#include "emu/Paths.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTcpSocket>
#include <QTimer>

namespace pist {

namespace {

constexpr quint16 kHrdbPort = 56001;

/// How long to wait for the handshake before declaring the session broken.
/// A stock Hatari has no listener (connection refused forever), and a fork
/// that lost the 56001 bind race (another PiST session or the hrdb GUI holds
/// it) runs with no debug endpoint at all — both must fail, not hang.
constexpr int kHandshakeTimeoutMs = 5000;
/// Per-command watchdog: if the fork never answers a dispatched command (a dead
/// or wedged debug link), fail it rather than block the queue forever. Matches
/// the native backend's kCommandTimeoutMs (finding 11).
constexpr int kCommandTimeoutMs = 10000;

/// `!status` notification field 1 (and `status` reply field 2): the fork
/// sends 0 while stopped in its remote break loop, 1 while running — the
/// inverse of the comment over RemoteDebug_status (verified live; see
/// docs/PLAN.md §9).
constexpr int kHrdbStopped = 0;

bool flagFromSr(quint16 sr, int bit)
{
    return ((sr >> bit) & 1) != 0;
}

/// Decode the fork's uuencode-ish `mem` payload: each group of 4 chars
/// (32-based) carries 3 bytes, big-endian within the group; the final group is
/// zero-padded, so decode at most expectedBytes.
QByteArray uudecode(const QByteArray &uu, quint32 expectedBytes)
{
    QByteArray out;
    out.reserve(expectedBytes);
    for (int i = 0; i + 3 < uu.size() && out.size() < int(expectedBytes); i += 4) {
        quint32 accum = 0;
        for (int j = 0; j < 4; ++j)
            accum = (accum << 6) | quint8(uu[i + j] - 32);
        out.append(char((accum >> 16) & 0xff));
        if (out.size() < int(expectedBytes))
            out.append(char((accum >> 8) & 0xff));
        if (out.size() < int(expectedBytes))
            out.append(char(accum & 0xff));
    }
    return out;
}

} // namespace

HrdbBackend::HrdbBackend(QObject *parent)
    : IDebugBackend(parent)
{
    // Same contract as the native backend (MAJ-45): the dump rows and the
    // hardware summary are signal payloads, so they are registered before any
    // connection can carry them.
    qRegisterMetaType<QList<MemoryRow>>("QList<MemoryRow>");
    qRegisterMetaType<HardwareSummary>("HardwareSummary");

    m_connectRetry = new QTimer(this);
    m_connectRetry->setInterval(100);
    connect(m_connectRetry, &QTimer::timeout, this, [this] {
        if (m_socket && m_socket->state() == QAbstractSocket::UnconnectedState)
            m_socket->connectToHost(QStringLiteral("127.0.0.1"), kHrdbPort);
    });

    m_handshakeWatchdog = new QTimer(this);
    m_handshakeWatchdog->setSingleShot(true);
    m_handshakeWatchdog->setInterval(kHandshakeTimeoutMs);
    connect(m_handshakeWatchdog, &QTimer::timeout, this, [this] {
        if (!m_ready && isRunning()) {
            emit errorOccurred(
                tr("No HRDB handshake on 127.0.0.1:56001. Either this Hatari is not the "
                   "hrdb-main fork (stock Hatari has no listener), or port 56001 is "
                   "already held by another HRDB client or PiST session — the fork "
                   "supports only one."));
            stop();
        }
    });

    m_commandWatchdog = new QTimer(this);
    m_commandWatchdog->setSingleShot(true);
    m_commandWatchdog->setInterval(kCommandTimeoutMs);
    connect(m_commandWatchdog, &QTimer::timeout, this, [this] {
        if (!m_haveCurrent)
            return;
        // The fork never answered: fail the in-flight command so the caller's
        // commandFinished fires (no upstream 120 s hang) and release the slot.
        // The queue is *not* advanced here: the reply may still be on its way,
        // and dispatching into it would let the late reply complete the next
        // command — the stream then runs one reply behind for the rest of the
        // session. The swallow below owns the release (MIN-2).
        const QString command = m_current.text;
        m_haveCurrent = false;
        m_current = Pending();
        emit errorOccurred(tr("The HRDB debugger did not respond to '%1' in time.").arg(command));
        emit commandFinished(command, QString());
        m_owedReply = true; // a late reply to this failed command must be swallowed
    });
    // The control socket carries the embedded display's video-size reports on
    // the fork (its debugger channels are HRDB's; the socket is upstream's).
    connect(&m_embedSocket, &EmbedSocket::sizeReported, this,
            [this](int width, int height) { emit embeddedSizeChanged(width, height); });
    connect(&m_embedSocket, &EmbedSocket::logLine, this,
            [this](const QString &line) { emit logLine(line); });
}

HrdbBackend::~HrdbBackend()
{
    stop();
}

bool HrdbBackend::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

bool HrdbBackend::start(const SessionConfig &config, QString *error)
{
    stop();
    m_state = MachineState();
    m_stopped = false;
    m_ready = false;
    m_queue.clear();
    m_haveCurrent = false;
    m_owedReply = false;
    m_current = Pending();
    m_buffer.clear();
    m_stderrText.clear();
    m_stderrConsumed = 0;

    if (!paths::ensureDirectory(config.sessionDir, error))
        return false;
    m_sessionDir = config.sessionDir;

    m_process = new QProcess(this);

    // Same config isolation + embedded-display wiring as the native backend.
    m_process->setProcessEnvironment(makeSessionEnvironment(config));

    // readStderrTail() drains the process's stderr with `bytesAvailable()` and
    // `waitForReadyRead()`, and both act on the process's *current* read
    // channel — stdout by default. Pointing it at stderr makes the drain
    // inspect (and wait on) the pipe the console response actually travels on,
    // instead of the stdout buffer that is always empty at that moment (and a
    // full 20 ms timeout every time). All other reads here are channel-specific
    // (readAllStandardOutput/readAllStandardError, each with its own readyRead
    // signal connected below), so the switch changes nothing else (MAJ-13).
    m_process->setReadChannel(QProcess::StandardError);

    // The session argv is the stock one, control socket included: the fork
    // keeps the upstream option, and it carries the embedded display's
    // video-size reports. HRDB replaces only the debugger channels. The
    // bootstrap parse file stays too: the fork runs --parse at launch, and
    // the entry breakpoint it arms fires into the remote break loop, which
    // then waits for our connect — airtight, unlike a socket-armed
    // `bp pc = TEXT …`, which races TOS boot.
    //
    // The IDE listens before the process starts (Hatari connects; it never
    // binds). The embed-info request goes out on every HRDB session that has
    // the socket, not only embedded ones: the reply is how the seam is
    // exercised headlessly, and it costs one informational log line. (Native
    // keeps the parentWindowId gate in EmulatorHost::openSocketServer.)
    if (!config.controlSocketPath.isEmpty()) {
        m_embedSocket.setRequestOnConnect(true);
        if (!m_embedSocket.listen(config.controlSocketPath, error))
            return false;
    }

    // All console-command output (`d`, `info`, history, register writes…)
    // lands on the process's stderr in order: debugOutput and the
    // disassembler's TraceFile both default to it, and the fork's console
    // handler fflushes before replying OK on the socket. So a console
    // command's response is the stderr tail since its dispatch, drained when
    // its OK arrives — the same pipe-before-completion ordering the native
    // backend relies on, with the socket OK replacing the prompt.
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(m_process->readAllStandardError());
        m_stderrText += text;
        // Bound the buffer while preserving the unconsumed tail.
        if (m_stderrText.size() > 65536) {
            const int drop = m_stderrText.size() - 32768;
            m_stderrText.remove(0, drop);
            m_stderrConsumed = qMax<qint64>(0, m_stderrConsumed - drop);
        }
        for (const QString &line : text.split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty())
                emit logLine(line.trimmed());
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        const QString text = QString::fromUtf8(m_process->readAllStandardOutput());
        for (const QString &line : text.split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty())
                emit logLine(line.trimmed());
    });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            emit errorOccurred(tr("Failed to start Hatari. Check the executable path."));
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                m_stopped = false;
                m_ready = false;
                m_haveCurrent = false;
                m_queue.clear();
                m_connectRetry->stop();
                m_handshakeWatchdog->stop();
                emit stoppedChanged(false);
                emit runningChanged(false);
                emit logLine(tr("Hatari exited (code %1).").arg(code));
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

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::readyRead, this, &HrdbBackend::onSocketData);
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        if (!isRunning())
            return;
        // The debug channel died. Fail the in-flight command before stop() clears
        // it (so the caller's commandFinished fires, not a hang), then end the
        // session — otherwise m_ready/m_haveCurrent stay set, dispatchNext writes
        // into the dead socket forever, and the UI shows a live session with a
        // dead debug channel (finding 11).
        emit errorOccurred(tr("The remote-debug connection dropped."));
        emit logLine(tr("Remote-debug socket closed."));
        if (m_haveCurrent) {
            const QString command = m_current.text;
            m_haveCurrent = false;
            m_current = Pending();
            emit commandFinished(command, QString());
        }
        stop();
    });
    m_socket->connectToHost(QStringLiteral("127.0.0.1"), kHrdbPort);
    m_connectRetry->start();
    m_handshakeWatchdog->start();

    emit runningChanged(true);
    return true;
}

void HrdbBackend::stop()
{
    const bool hadSession = m_process != nullptr || m_stopped;
    m_sessionDir.clear();

    m_embedSocket.close();
    m_connectRetry->stop();
    m_handshakeWatchdog->stop();
    m_commandWatchdog->stop();
    m_ready = false;
    m_stopped = false;
    m_queue.clear();
    m_haveCurrent = false;
    m_owedReply = false;
    m_current = Pending();

    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_process) {
        killAndRelease(m_process, this);
        m_process->deleteLater();
        m_process = nullptr;
    }

    if (hadSession) {
        emit stoppedChanged(false);
        emit runningChanged(false);
    }
}

void HrdbBackend::enqueue(Pending pending)
{
    if (!isRunning()) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }
    m_queue.enqueue(pending);
    dispatchNext();
}

void HrdbBackend::dispatchNext()
{
    if (m_haveCurrent || m_queue.isEmpty() || !m_ready || !m_socket)
        return;

    // A command timed out and its reply is still owed: the next reply that
    // arrives belongs to it, not to anything sent now, so hold the queue until
    // the swallow consumes it (mirrors the native transport's m_owedPrompts
    // guard — MIN-2). Without this, a *missing* reply (the link is silent, not
    // merely late) makes the next command's reply get swallowed as the owed one
    // and every command afterwards is answered by its predecessor.
    if (m_owedReply)
        return;

    // Commands that run from the fork's remote break loop are held while the
    // emulator runs: HRDB services the socket either way, unlike the native
    // transport, whose starved stdin provides the gating for free. Gate them
    // until the next stop (the !status handler dispatches) so a snapshot can
    // never capture mid-run state and a debugger command never runs against a
    // moving machine. Control commands — run, break, bp — go out immediately.
    // Scan past deferred requests rather than blocking on the queue head: one
    // must not starve a `break` or `bp` queued behind it. One command is
    // outstanding at a time, so the reorder is safe.
    int next = -1;
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_stopped || !m_queue.at(i).needsStop) {
            next = i;
            break;
        }
    }
    if (next < 0) {
        // Nothing can go out yet. A request PiST issued internally stays quiet
        // (the native transport defers the same reads just as silently, and a
        // pane refresh is nobody's question); a command the *user* is waiting
        // on says so, because its answer will not come until the emulator
        // stops — the message is the visible part of a decision that used to
        // be taken silently per command text.
        for (Pending &pending : m_queue) {
            if (pending.freeText && !pending.deferAnnounced) {
                pending.deferAnnounced = true;
                emit logLine(tr("'%1' waits for the emulator to stop.").arg(pending.text));
            }
        }
        return;
    }

    m_current = m_queue.takeAt(next);
    m_haveCurrent = true;
    if (m_current.captureStderr)
        m_stderrConsumed = m_stderrText.size();
    m_socket->write(m_current.wire.toUtf8() + '\0');
    // The fork has the command in flight; start the watchdog so a dead link that
    // never answers fails the command instead of wedging the queue (finding 11).
    m_commandWatchdog->start();
}

void HrdbBackend::onSocketData()
{
    m_buffer += m_socket->readAll();

    int nul;
    while ((nul = m_buffer.indexOf('\0')) >= 0) {
        const QByteArray message = m_buffer.left(nul);
        m_buffer.remove(0, nul + 1);

        if (message.isEmpty())
            continue;
        if (message.startsWith('!')) {
            handleNotification(message);
            continue;
        }
        // Any other message is the reply to the current command. The fork
        // answers commands in order and only one is outstanding.
        if (m_owedReply) {
            // A late reply to a command the watchdog already failed — swallow it
            // so it can't be mis-attributed to the next command (native's
            // owed-prompts guard, finding 11). The stream is back in step, so
            // the held queue may advance (MIN-2).
            m_owedReply = false;
            if (!m_haveCurrent)
                dispatchNext();
            continue;
        }
        if (m_haveCurrent)
            completeCurrent(message);
        else
            emit logLine(QString::fromUtf8(message));
    }
}

void HrdbBackend::handleNotification(const QByteArray &message)
{
    const QList<QByteArray> fields = message.split('\x01');

    if (fields.first() == "!connected") {
        // Handshake: `!connected <protocol-id>`. A different listener holding
        // 56001 (or a fork version skew) must fail loudly, not hang.
        const QString protocol = QString::fromLatin1(fields.value(1));
        if (protocol != QLatin1String("100A")) {
            emit errorOccurred(tr("Unexpected HRDB protocol %1 on 56001 — is something "
                                  "else holding the port?").arg(protocol));
            stop();
            return;
        }
        emit logLine(tr("Remote debug connected (protocol %1).").arg(protocol));
        m_ready = true;
        m_connectRetry->stop();
        m_handshakeWatchdog->stop();
        dispatchNext();
        return;
    }

    if (fields.first() == "!status") {
        // `!status <running> <pc> …`: 1 while emulating, 0 while stopped in
        // the remote break loop (a breakpoint, an exception, or `break`).
        // Same-state duplicates (e.g. the re-entry after a step) carry no
        // transition and are suppressed; the PC arrives via the queued `regs`.
        const bool stopped = fields.value(1).toInt() == kHrdbStopped;
        if (stopped == m_stopped)
            return;
        m_stopped = stopped;
        emit stoppedChanged(stopped);
        if (stopped)
            // Deferred state reads can now be answered against a stopped
            // machine.
            dispatchNext();
        return;
    }

    // !config / !symbols / !profile and the rest are informational.
    emit logLine(QString::fromUtf8(message).replace(QLatin1Char('\x01'),
                                                    QLatin1String(" | ")));
}

void HrdbBackend::completeCurrent(const QByteArray &message)
{
    // A reply arrived: cancel the per-command watchdog.
    m_commandWatchdog->stop();
    const QList<QByteArray> fields = message.split('\x01');
    const Pending done = m_current;
    m_haveCurrent = false;
    m_current = Pending();

    QString response;
    // Whether this response fills part of MachineState (drives the one
    // stateUpdated per batch, below). Set from the carried kind, not the text.
    bool fillsState = false;

    if (fields.first() == "NG") {
        response = tr("NG (error %1)").arg(QString::fromLatin1(fields.value(1)));
        emit errorOccurred(tr("Command '%1' failed: %2").arg(done.text, response));
    } else if (done.kind == ResponseKind::Registers) {
        // The typed `regs` reply: key/value pairs, including the live
        // TEXT/DATA/BSS bases.
        parseRegPairs(fields);
        fillsState = true;
        response = QString::fromUtf8(message);
    } else if (done.kind == ResponseKind::MemoryDump) {
        // `OK <addr> <count> <uuencoded payload>` — uuencode never emits 0x01,
        // so the field split kept the payload whole. Only a
        // requestMemoryDump()/requestStackDump() pending carries this kind, so
        // the dump is always published to the pane that asked, with its tag.
        //
        // Decoded once, into the rows every consumer reads (MAJ-45); the text
        // is rendered from those rows for the log, so the two cannot disagree.
        const QByteArray uu = fields.mid(3).join('\x01');
        const QList<MemoryRow> rows = memoryRows(done.dumpAddress, done.memBytes, uu);
        response = renderMemoryDump(rows);
        if (done.stackDump)
            emit stackDumpReady(done.dumpAddress, rows);
        else
            emit memoryDumpReady(done.dumpAddress, rows, done.dumpTag);
    } else if (done.captureStderr) {
        response = readStderrTail().trimmed();
        // A disassembly read is parsed into the state snapshot exactly as the
        // native backend parses its stderr text — same format, same parser.
        // Only a request that asked for it is parsed (the kind travels with the
        // request): this used to key on the command text, so a `db pc = $X
        // :once` wiped the cached disassembly that stepOver() reads its
        // fall-through address from, and a console `memdump` was published as a
        // pane dump (MAJ-12).
        if (done.kind == ResponseKind::Disassembly) {
            hataritext::parseDisassembly(response, &m_state);
            fillsState = true;
            // A caller that asked about one address wants the text itself as
            // well (the remote `disasm` verb).
            if (done.reportDisassembly)
                emit disassemblyReady(done.disassemblyAddress, response);
        } else if (!done.infoSubject.isEmpty()) {
            // A typed `info <subject>` report. It is not part of MachineState:
            // it is parsed into its summary here and goes to the caller that
            // asked, named by its subject (MAJ-45).
            emit hardwareInfoReady(done.infoSubject, hataritext::parseHardwareInfo(response));
        }
    } else {
        response = QString::fromUtf8(message);
    }

    // Batch contract: stateUpdated once per refresh batch, or once for a
    // standalone state command — identical to the native backend's rule. Driven
    // by the kind the request carried, never by the wire or command text.
    if (fillsState && (!done.batchMember || done.batchEnd))
        emit stateUpdated(m_state);

    // A profile save is complete whatever the fork answered: the caller reads
    // the file, and a save that failed reports that there rather than leaving
    // the UI on "Saving…".
    if (!done.profilePath.isEmpty())
        emit profileSaveFinished(done.profilePath);

    // The execution-path read's text, for the pane that asked (a typed request
    // reports its own answer rather than a caller matching command text).
    if (done.reportHistory)
        emit historyReady(response);

    // No ack is logged here: every request that carries a user's text runs
    // through the console, whose output is already streamed line by line by the
    // stderr reader (readyReadStandardError), and a typed request's structured
    // reply would be noise beside the state/ pane signals it already drives.
    emit commandFinished(done.text, response);
    dispatchNext();
}

HrdbBackend::Pending HrdbBackend::consoleRequest(const QString &text, const QString &wireBody,
                                                 ResponseKind kind, bool keepOnResume)
{
    Pending p;
    p.text = text;
    p.wire = QStringLiteral("console ") + wireBody;
    p.kind = kind;
    // The output lands on the process's stderr, flushed before the OK that
    // completes this command, so the response is the stderr tail since dispatch.
    p.captureStderr = true;
    // The fork's console handler runs the debugger's own command set, which
    // exists only inside its remote break loop: dispatched while the emulator
    // runs, it would be answered against a moving machine. The queue holds it
    // for the next stop — the gating is stated here, once, instead of being
    // guessed per command text.
    p.needsStop = true;
    p.keepOnResume = keepOnResume;
    return p;
}

HrdbBackend::Pending HrdbBackend::freeTextRequest(const QString &text)
{
    Pending p = consoleRequest(text, text);
    // A user's command: if the queue has to hold it, the caller is told why
    // rather than left waiting on a deferral nobody announced.
    p.freeText = true;
    p.keepOnResume = survivesResume(text);
    return p;
}

void HrdbBackend::command(const QString &commandText)
{
    // Free text only — the user's console and the remote `cmd` verb. There is
    // no translation to do: the fork's `console <cmd>` runs any debugui
    // command, so the user's text reaches the debugger as written. The prefix
    // table that used to rewrite `r`/`s`/`c`/`b …`/`w b …` here is gone with
    // the internal callers that needed it, which is what made a command's
    // meaning depend on its spelling (CRIT-6, MAJ-12).
    if (commandText.trimmed().isEmpty()) {
        // Nothing to run: reported, never enqueued (the fork would answer an
        // empty console line with an OK that means nothing).
        emit errorOccurred(tr("Cannot run an empty debugger command."));
        return;
    }
    enqueue(freeTextRequest(commandText));
}

void HrdbBackend::consoleCommand(const QString &commandText)
{
    command(commandText);
}

QString HrdbBackend::readStderrTail()
{
    // The fork flushes the console output before sending the OK we just
    // completed on, so the response is already in the pipe — but Qt may not
    // have read it out yet. Both of these act on the process's *current* read
    // channel, which start() points at stderr: `waitForReadyRead` returns as
    // soon as anything arrives there, which for data already written is
    // immediate. Same reasoning as the native backend's drainStderr (MAJ-13).
    int guard = 0;
    while (m_process && guard++ < 8) {
        if (m_process->bytesAvailable() > 0) {
            m_stderrText += QString::fromUtf8(m_process->readAllStandardError());
            continue;
        }
        if (!m_process->waitForReadyRead(20))
            break;
        m_stderrText += QString::fromUtf8(m_process->readAllStandardError());
    }
    const QString tail = m_stderrText.mid(int(m_stderrConsumed));
    m_stderrConsumed = m_stderrText.size();
    return tail;
}

void HrdbBackend::parseRegPairs(const QList<QByteArray> &fields)
{
    // `OK` then key/value pairs: D0..D7, A0..A7, PC, USP, ISP, SR, and the
    // Hatari variables including TEXT/DATA/BSS (the live section bases). The
    // fork double-separates after OK (send_sep, then send_key_value's own
    // leading separator), so empty fields must be skipped, not paired.
    QList<QByteArray> pairs;
    for (const QByteArray &f : fields.mid(1))
        if (!f.isEmpty())
            pairs.append(f);
    for (int i = 0; i + 1 < pairs.size(); i += 2) {
        const QByteArray key = pairs.at(i);
        const quint32 value = pairs.at(i + 1).toUInt(nullptr, 16);

        if (key.size() == 2 && (key[0] == 'D' || key[0] == 'A')
            && key[1] >= '0' && key[1] <= '7') {
            if (key[0] == 'D')
                m_state.regs.d[key[1] - '0'] = value;
            else
                m_state.regs.a[key[1] - '0'] = value;
        } else if (key == "PC") {
            m_state.pc = value;
        } else if (key == "USP") {
            m_state.regs.usp = value;
        } else if (key == "ISP") {
            m_state.regs.isp = value;
        } else if (key == "SR") {
            m_state.regs.sr = quint16(value);
            m_state.regs.flagX = flagFromSr(quint16(value), 4);
            m_state.regs.flagN = flagFromSr(quint16(value), 3);
            m_state.regs.flagZ = flagFromSr(quint16(value), 2);
            m_state.regs.flagV = flagFromSr(quint16(value), 1);
            m_state.regs.flagC = flagFromSr(quint16(value), 0);
            m_state.regs.valid = true;
        } else if (key == "TEXT") {
            m_state.textBase = value;
        } else if (key == "DATA") {
            m_state.dataBase = value;
        } else if (key == "BSS") {
            m_state.bssBase = value;
        }
    }
}

QList<MemoryRow> HrdbBackend::memoryRows(quint32 address, quint32 memBytes,
                                         const QByteArray &uu)
{
    // The fork's reply is the payload itself, so the rows are decoded rather
    // than parsed out of text (MAJ-45): the native transport parses Hatari's
    // `m` output into the same rows. Rows of 16 bytes, as both print them.
    const QByteArray bytes = uudecode(uu, memBytes);
    QList<MemoryRow> rows;
    for (int row = 0; row < bytes.size(); row += 16) {
        MemoryRow parsed;
        parsed.address = address + quint32(row);
        const int count = qMin(16, bytes.size() - row);
        parsed.bytes.reserve(count);
        for (int i = 0; i < count; ++i)
            parsed.bytes.append(quint8(bytes[row + i]));
        rows.append(parsed);
    }
    return rows;
}

void HrdbBackend::clearBreakpoints()
{
    // No typed clear-all exists, but `b all` is a debugui command and the
    // console runs those. Not stop-gated: arming after a rebuild must not
    // wait for a stop.
    Pending p = consoleRequest(QStringLiteral("b all"), QStringLiteral("b all"));
    p.needsStop = false;
    p.keepOnResume = true;
    enqueue(p);
}

void HrdbBackend::armBreakpoint(const QString &condition)
{
    // The planner's condition is a Hatari `b …` command (debug/Breakpoint.h,
    // debug/Watchpoint.h). The fork's typed `bp` takes the expression alone and
    // hands it to the same BreakCond parser the debugger's own `b` uses, so the
    // transport difference is just the leading verb — and anything that is not
    // a `b …` command is reported rather than guessed at.
    if (!condition.startsWith(QLatin1String("b "))) {
        emit errorOccurred(tr("Cannot arm '%1': an HRDB breakpoint is a Hatari `b …` "
                              "condition.")
                               .arg(condition));
        return;
    }
    Pending p;
    p.text = condition;
    p.wire = QStringLiteral("bp ") + condition.mid(2);
    p.keepOnResume = true;
    enqueue(p);
}

void HrdbBackend::breakAtAddressOnce(quint32 address)
{
    // `bp` is the fork's typed breakpoint command; `:once` removes the
    // breakpoint on the hit, so a fired one needs no delete afterwards.
    Pending p;
    p.text = QStringLiteral("b pc = $%1 :once").arg(address, 0, 16);
    p.wire = QStringLiteral("bp pc = $%1 :once").arg(address, 0, 16);
    p.keepOnResume = true;
    enqueue(p);
}

void HrdbBackend::requestMemoryDump(quint32 address, int length, int tag)
{
    // Protocol 0x1006 wants bare hex for address and count.
    Pending p;
    p.text = QStringLiteral("m $%1 %2").arg(address, 0, 16).arg(length);
    p.wire = QStringLiteral("mem %1 %2").arg(address, 0, 16).arg(length, 0, 16);
    p.kind = ResponseKind::MemoryDump;
    p.dumpAddress = address;
    p.memBytes = quint32(length);
    p.dumpTag = tag;
    p.needsStop = true;
    enqueue(p);
}

void HrdbBackend::requestStackDump(quint32 address, int length)
{
    Pending p;
    p.text = QStringLiteral("m $%1 %2").arg(address, 0, 16).arg(length);
    p.wire = QStringLiteral("mem %1 %2").arg(address, 0, 16).arg(length, 0, 16);
    p.kind = ResponseKind::MemoryDump;
    p.dumpAddress = address;
    p.memBytes = quint32(length);
    p.stackDump = true;
    p.needsStop = true;
    enqueue(p);
}

void HrdbBackend::dumpRegisters()
{
    // The fork answers registers as typed key/value pairs (see parseRegPairs)
    // rather than as the native transport's register dump text.
    Pending p;
    p.text = QStringLiteral("r");
    p.wire = QStringLiteral("regs");
    p.kind = ResponseKind::Registers;
    p.needsStop = true;
    enqueue(p);
}

void HrdbBackend::loadSymbols()
{
    // `symbols prg` relocates the program's symbols against the live basepage;
    // only the debugger's own command set can do that, so it goes through the
    // console (there is no typed symbol-load).
    enqueue(consoleRequest(QStringLiteral("symbols prg"), QStringLiteral("symbols prg")));
}

void HrdbBackend::infoSubject(const QString &subject)
{
    Pending p = consoleRequest(QStringLiteral("info ") + subject,
                               QStringLiteral("info ") + subject);
    p.infoSubject = subject;
    enqueue(p);
}

void HrdbBackend::readBasepage()
{
    // The fork reports the live TEXT/DATA/BSS bases as register keys in its
    // typed `regs` reply, so that is what answers this read — the native
    // spelling stays as the label, so a caller sees the same request name on
    // both transports.
    Pending p;
    p.text = QStringLiteral("info basepage");
    p.wire = QStringLiteral("regs");
    p.kind = ResponseKind::Registers;
    p.needsStop = true;
    enqueue(p);
}

void HrdbBackend::setDisasmEngine(DisasmEngine engine)
{
    const QString command = engine == DisasmEngine::Ext
        ? QStringLiteral("setopt --disasm ext")
        : QStringLiteral("setopt --disasm uae");
    enqueue(consoleRequest(command, command));
}

void HrdbBackend::profileOn()
{
    // Kept across a resume(): collection starts at the continue itself, so a
    // pruned `profile on` collects nothing while the UI says "Collecting".
    Pending p = consoleRequest(QStringLiteral("profile on"), QStringLiteral("profile on"),
                               ResponseKind::None, true);
    enqueue(p);
}

void HrdbBackend::profileOff()
{
    enqueue(consoleRequest(QStringLiteral("profile off"), QStringLiteral("profile off"),
                           ResponseKind::None, true));
}

void HrdbBackend::profileSave(const QString &path)
{
    const QString command = QStringLiteral("profile save %1").arg(path);
    Pending p = consoleRequest(command, command, ResponseKind::None, true);
    p.profilePath = path;
    enqueue(p);
}

void HrdbBackend::writeRegister(const QString &name, quint32 value)
{
    // The debugger's own register-set form (the '=' is mandatory, and a
    // successful set prints nothing); the console runs it.
    const QString command = QStringLiteral("r %1=$%2").arg(name).arg(value, 0, 16);
    enqueue(consoleRequest(command, command));
}

void HrdbBackend::writeMemoryByte(quint32 address, quint8 value)
{
    // The fork's `memset` takes an address, a byte *count* and two hex digits
    // of data, rather than the debugger's `w b <addr> <value>`.
    Pending p;
    p.text = QStringLiteral("w b $%1 $%2").arg(address, 0, 16).arg(value, 0, 16);
    p.wire = QStringLiteral("memset %1 1 %2")
                 .arg(address, 0, 16)
                 .arg(value, 2, 16, QLatin1Char('0'));
    enqueue(p);
}

void HrdbBackend::readDisassembly()
{
    // At the PC: the entry attach's read and the stop refresh. The fork prints
    // the disassembly to stderr, so the text comes back as the console tail.
    enqueue(consoleRequest(QStringLiteral("d"), QStringLiteral("d"), ResponseKind::Disassembly));
}

void HrdbBackend::readDisassemblyAt(quint32 address)
{
    Pending p = consoleRequest(QStringLiteral("d $%1").arg(address, 0, 16),
                               QStringLiteral("d $%1").arg(address, 0, 16),
                               ResponseKind::Disassembly);
    // The caller asked about one address and wants the text back; the snapshot
    // is filled as for any disassembly read.
    p.reportDisassembly = true;
    p.disassemblyAddress = address;
    enqueue(p);
}

void HrdbBackend::readHistory(int count)
{
    // `history <count>` prints the last count recorded PCs, one per line; the
    // console runs it, and its output is the stderr tail like any other text
    // the debugger prints.
    const QString command = QStringLiteral("history %1").arg(count);
    Pending p = consoleRequest(command, command);
    p.reportHistory = true;
    enqueue(p);
}

void HrdbBackend::refresh()
{
    if (!isRunning()) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }

    // regs fills registers and bases; the disassembly comes back as text via
    // the console. stateUpdated fires once, when the disassembly completes.
    // Each command carries what its response is (MAJ-12).
    Pending regs;
    regs.text = QStringLiteral("r");
    regs.wire = QStringLiteral("regs");
    regs.kind = ResponseKind::Registers;
    regs.batchMember = true;
    regs.needsStop = true;
    m_queue.enqueue(regs);

    Pending disasm;
    disasm.text = QStringLiteral("d");
    disasm.wire = QStringLiteral("console d");
    disasm.kind = ResponseKind::Disassembly;
    disasm.batchMember = true;
    disasm.batchEnd = true;
    disasm.captureStderr = true;
    disasm.needsStop = true;
    m_queue.enqueue(disasm);

    dispatchNext();
}

void HrdbBackend::step()
{
    // The fork's typed single-step.
    Pending p;
    p.text = QStringLiteral("s");
    p.wire = QStringLiteral("step");
    enqueue(p);
}

void HrdbBackend::stepOver()
{
    // HRDB has no step-over frame, so synthesize the native `n`: for a
    // jsr/bsr, break once at the fall-through instruction and run; anything
    // else is a plain step. The fall-through address comes from the current
    // disassembly snapshot, which refresh() keeps live at every stop.
    if (m_stopped) {
        for (int i = 0; i < m_state.disassembly.size(); ++i) {
            const DisasmLine &line = m_state.disassembly.at(i);
            if (!line.isCurrentPc)
                continue;
            const QString insn = line.instruction.toLower();
            const bool isCall = insn.startsWith(QLatin1String("jsr"))
                || insn.startsWith(QLatin1String("bsr"));
            if (isCall && i + 1 < m_state.disassembly.size()) {
                const quint32 next = m_state.disassembly.at(i + 1).address;
                // Enqueued directly rather than via resume(): resume() drops
                // the queue, which would take the just-armed breakpoint with
                // it.
                Pending bp;
                bp.text = QStringLiteral("bp pc = $%1 :once").arg(next, 0, 16);
                bp.wire = bp.text;
                m_queue.enqueue(bp);
                Pending run;
                run.text = QStringLiteral("c");
                run.wire = QStringLiteral("run");
                m_queue.enqueue(run);
                m_stopped = false;
                emit stoppedChanged(false);
                dispatchNext();
                return;
            }
            break;
        }
    }
    step();
}

void HrdbBackend::pause()
{
    if (m_stopped || !isRunning())
        return;
    // Serviced at the next VBL; the resulting !status notification marks the
    // stop. The fork NGs a break that is already active, which the stopped
    // guard above prevents.
    Pending p;
    p.text = QStringLiteral("break");
    p.wire = QStringLiteral("break");
    enqueue(p);
}

void HrdbBackend::setFloppyImage(int drive, const QString &path)
{
    if (drive < 0 || drive > 1)
        return;
    if (!isRunning()) {
        emit logLine(tr("Floppy %1: %2 (applies when the emulator starts)")
                         .arg(QChar(QLatin1Char('A' + drive)),
                              path.isEmpty() ? tr("empty") : path));
        return;
    }
    // The control socket is SDL-pumped and unread in the remote break loop
    // (the typical session: stopped at entry). `console setopt` is serviced
    // both there and while running, so this request is deliberately not
    // stop-gated: a running insert must not be held until the next breakpoint.
    const QString cmd =
        floppySetoptCommand(drive, floppyImageForDebugger(m_sessionDir, drive, path));
    if (cmd.isEmpty())
        return;
    emit logLine(tr("Floppy %1: %2")
                     .arg(QChar(QLatin1Char('A' + drive)),
                          path.isEmpty() ? tr("ejected") : path));
    Pending p;
    p.text = cmd;
    p.wire = QStringLiteral("console ") + cmd;
    p.captureStderr = true;
    p.needsStop = false;
    enqueue(p);
}

void HrdbBackend::resume()
{
    if (!isRunning())
        return;
    if (!m_stopped)
        return;

    // Keep the pending arms (arming after the entry stop) and the profile
    // control commands queued in the same stack frame as this resume; drop
    // dumps and reads. Writing `run` with those discarded is why a pre-Run
    // breakpoint missed — and why a `profile on` armed by profileToCursor
    // collected nothing. Each request states its own keep rule (see
    // Pending::keepOnResume); a free-text command's comes from its own text.
    QQueue<Pending> kept;
    while (!m_queue.isEmpty()) {
        const Pending p = m_queue.dequeue();
        if (p.keepOnResume)
            kept.enqueue(p);
    }
    m_queue = kept;

    m_stopped = false;
    emit stoppedChanged(false);

    Pending p;
    p.text = QStringLiteral("c");
    p.wire = QStringLiteral("run");
    enqueue(p);
}

IDebugBackend *createBackend(BackendKind kind, QObject *parent)
{
    switch (kind) {
    case BackendKind::Hrdb:
        return new HrdbBackend(parent);
    case BackendKind::Native:
        break;
    }
    return new EmulatorHost(parent);
}

} // namespace pist
