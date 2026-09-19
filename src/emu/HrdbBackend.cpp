// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/HrdbBackend.h"

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
        if (isRunning()) {
            emit errorOccurred(tr("The remote-debug connection dropped."));
            emit logLine(tr("Remote-debug socket closed."));
        }
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
    m_ready = false;
    m_stopped = false;
    m_queue.clear();
    m_haveCurrent = false;
    m_current = Pending();

    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->disconnectFromHost();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(2000);
        }
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

    // State reads against a running machine return an arbitrary PC: HRDB
    // services the socket while emulating, unlike the native transport, whose
    // starved stdin provides the gating for free. Defer them to the next stop
    // (the !status handler dispatches) so a snapshot can never capture
    // mid-run state. Control commands — run, break, bp — go out immediately.
    // Scan past deferred reads rather than blocking on the queue head: a
    // deferred read must not starve a `break` or `bp` queued behind it. One
    // command is outstanding at a time, so the reorder is safe.
    int next = -1;
    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_stopped || !m_queue.at(i).needsStop) {
            next = i;
            break;
        }
    }
    if (next < 0)
        return;

    m_current = m_queue.takeAt(next);
    m_haveCurrent = true;
    if (m_current.captureStderr)
        m_stderrConsumed = m_stderrText.size();
    m_socket->write(m_current.wire.toUtf8() + '\0');
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
    const QList<QByteArray> fields = message.split('\x01');
    const Pending done = m_current;
    m_haveCurrent = false;
    m_current = Pending();

    QString response;

    if (fields.first() == "NG") {
        response = tr("NG (error %1)").arg(QString::fromLatin1(fields.value(1)));
        emit errorOccurred(tr("Command '%1' failed: %2").arg(done.text, response));
    } else if (done.wire.startsWith(QLatin1String("regs"))) {
        parseRegPairs(fields);
        response = QString::fromUtf8(message);
    } else if (done.wire.startsWith(QLatin1String("mem "))) {
        // `OK <addr> <count> <uuencoded payload>` — uuencode never emits 0x01,
        // so the field split kept the payload whole.
        const QByteArray uu = fields.mid(3).join('\x01');
        response = formatMemoryDump(done.dumpAddress, done.memBytes, uu);
        if (done.stackDump)
            emit stackDumpReady(done.dumpAddress, response);
        else
            emit memoryDumpReady(done.dumpAddress, response, done.dumpTag);
    } else if (done.captureStderr) {
        response = readStderrTail().trimmed();
        // A console `d` is parsed into the state snapshot exactly as the
        // native backend parses its stderr text — same format, same parser.
        if (done.text.startsWith(QLatin1Char('d')))
            hataritext::parseDisassembly(response, &m_state);
    } else {
        response = QString::fromUtf8(message);
    }

    // Batch contract: stateUpdated once per refresh batch, or once for a
    // standalone state command — identical to the native backend's rule.
    const bool fillsState = done.wire.startsWith(QLatin1String("regs"))
        || done.text.startsWith(QLatin1Char('d'));
    if (fillsState && (!done.batchMember || done.batchEnd))
        emit stateUpdated(m_state);

    // Typed replies that are NOT on the fork's stderr stream need an explicit
    // log emission so typed console commands produce visible output. The `else`
    // branch (OK-style acks for bp/break/run/step) is exactly what a typed
    // command hits; regs and mem already surface through stateUpdated/memoryDumpReady
    // and their raw field blobs would be noise in the log. captureStderr
    // commands already stream via readyReadStandardError. Internal control
    // commands (step/resume from F10/F9) must stay silent — they never log on
    // the native backend either. A copy keeps the formatted text out of the
    // commandFinished response, which RemoteControl's cmd also consumes.
    if (!done.captureStderr && !done.wire.startsWith(QLatin1String("regs"))
        && !done.wire.startsWith(QLatin1String("mem ")) && !response.isEmpty()
        && done.consoleOrigin) {
        emit logLine(QString(response).replace(QLatin1Char('\x01'), QLatin1String(" | ")));
    }
    emit commandFinished(done.text, response);
    dispatchNext();
}
bool HrdbBackend::translateCommand(const QString &commandText, Pending *out)
{
    out->text = commandText;
    if (commandText == QLatin1String("r")) {
        out->wire = QStringLiteral("regs");
        out->needsStop = true;
    } else if (commandText == QLatin1String("s")) {
        out->wire = QStringLiteral("step");
    } else if (commandText == QLatin1String("c")) {
        out->wire = QStringLiteral("run");
    } else if (commandText.startsWith(QLatin1String("b "))) {
        // `b <expr>` and the watchpoint form both map to `bp <expr>`; the fork
        // passes the expression to BreakCond_Command unchanged, so our
        // breakpoint planning (and the self-inequality watchpoints) port as-is.
        out->wire = QStringLiteral("bp ") + commandText.mid(2);
    } else if (commandText.startsWith(QLatin1String("w b $"))) {
        // `w b $addr $val` → `memset <addr-hex> 1 <val-hex>`: the fork's
        // memset takes three arguments with a byte *count* in the middle, and
        // the value must be two hex digits (the parser reads pairs).
        const QStringList parts = commandText.split(QLatin1Char(' '));
        if (parts.size() == 4) {
            bool okAddr = false, okVal = false;
            const quint32 addr = parts.at(2).mid(1).toUInt(&okAddr, 16);
            const quint32 val = parts.at(3).mid(1).toUInt(&okVal, 16);
            if (okAddr && okVal)
                out->wire = QStringLiteral("memset %1 1 %2")
                                .arg(addr, 0, 16)
                                .arg(val, 2, 16, QLatin1Char('0'));
        }
        if (out->wire.isEmpty()) {
            emit errorOccurred(tr("Cannot translate '%1' for the HRDB transport.")
                                   .arg(commandText));
            return false;
        }
    } else {
        out->wire = QStringLiteral("console ") + commandText;
        out->captureStderr = true;
        out->needsStop = true;
    }
    return true;
}

void HrdbBackend::consoleCommand(const QString &commandText)
{
    Pending p;
    p.consoleOrigin = true;
    if (!translateCommand(commandText, &p))
        return;
    enqueue(p);
}

QString HrdbBackend::readStderrTail()
{
    // The fork flushes the console output before sending the OK we just
    // completed on, so the response is already in the pipe — but Qt may not
    // have read it out yet. waitForReadyRead on the pipe itself (not
    // bytesAvailable, which reports Qt's buffer) returns as soon as anything
    // arrives; for data already written that is immediate. Same reasoning as
    // the native backend's drainStderr.
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

QString HrdbBackend::formatMemoryDump(quint32 address, quint32 memBytes,
                                      const QByteArray &uu)
{
    const QByteArray bytes = uudecode(uu, memBytes);
    // The character column comes from the shared printable-range renderer (the
    // memory view uses the same one), so the two cannot drift on what is
    // printable. Rendered once, then sliced per row.
    const QVector<quint8> byteVals(bytes.cbegin(), bytes.cend());
    const QString allChars = renderMemoryChars(byteVals);

    // Render as upstream `m` output — `%08X: HH HH …  chars` rows of 16 — so
    // parseMemoryDump (and the views) see exactly the native format.
    QString text;
    for (int row = 0; row < bytes.size(); row += 16) {
        const int count = qMin(16, bytes.size() - row);
        QString line = QStringLiteral("%1:").arg(address + row, 8, 16, QLatin1Char('0'));
        for (int i = 0; i < count; ++i)
            line += QStringLiteral(" %1").arg(quint8(bytes[row + i]), 2, 16, QLatin1Char('0'));
        text += line + QStringLiteral("  ") + allChars.mid(row, count) + QLatin1Char('\n');
    }
    return text;
}

void HrdbBackend::command(const QString &commandText, quint32 dumpAddress,
                          bool stackDump, int dumpTag)
{
    Pending p;
    p.dumpAddress = dumpAddress;
    p.stackDump = stackDump;
    p.dumpTag = dumpTag;
    if (!translateCommand(commandText, &p))
        return;
    enqueue(p);
}

void HrdbBackend::clearBreakpoints()
{
    // No typed clear-all exists, but `b all` is a debugui command and the
    // console runs those. Not stop-gated: arming after a rebuild must not
    // wait for a stop.
    Pending p;
    p.text = QStringLiteral("b all");
    p.wire = QStringLiteral("console b all");
    p.captureStderr = true;
    enqueue(p);
}

void HrdbBackend::armBreakpoint(const QString &condition)
{
    command(condition);
}

void HrdbBackend::requestMemoryDump(quint32 address, int length, int tag)
{
    // Protocol 0x1006 wants bare hex for address and count.
    Pending p;
    p.text = QStringLiteral("m $%1 %2").arg(address, 0, 16).arg(length);
    p.wire = QStringLiteral("mem %1 %2").arg(address, 0, 16).arg(length, 0, 16);
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
    p.dumpAddress = address;
    p.memBytes = quint32(length);
    p.stackDump = true;
    p.needsStop = true;
    enqueue(p);
}

void HrdbBackend::dumpRegisters()
{
    command(QStringLiteral("r"));
}

void HrdbBackend::refresh()
{
    if (!isRunning()) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }

    // regs fills registers and bases; the disassembly comes back as text via
    // the console. stateUpdated fires once, when the disassembly completes.
    Pending regs;
    regs.text = QStringLiteral("r");
    regs.wire = QStringLiteral("regs");
    regs.batchMember = true;
    regs.needsStop = true;
    m_queue.enqueue(regs);

    Pending disasm;
    disasm.text = QStringLiteral("d");
    disasm.wire = QStringLiteral("console d");
    disasm.batchMember = true;
    disasm.batchEnd = true;
    disasm.captureStderr = true;
    disasm.needsStop = true;
    m_queue.enqueue(disasm);

    dispatchNext();
}

void HrdbBackend::step()
{
    command(QStringLiteral("s"));
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
    // both there and while running. needsStop is false so a running session
    // is not deferred until the next breakpoint — translateCommand would
    // otherwise gate console passthroughs on a stop.
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

    // Keep pending `b` commands (arming after the entry stop); drop dumps.
    // Writing `run` with those discarded is why a pre-Run breakpoint missed.
    QQueue<Pending> kept;
    while (!m_queue.isEmpty()) {
        const Pending p = m_queue.dequeue();
        if (isBreakpointCommand(p.text))
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
