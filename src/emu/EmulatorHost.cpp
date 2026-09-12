// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/EmulatorHost.h"

#include "emu/HatariProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
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

/// `$0125a2 60fe                     bra.b     $125a2`
///
/// The address prefix depends on which disassembler engine is active, which is
/// a *user configuration* setting (`bDisasmUAE`), not a build property:
///
///   UAE engine (the default)   `00012596 7001      moveq #$01,d0`
///   Capstone engine            `$00012596 7001     moveq #$01,d0`
///
/// So the `$` is optional here. The register dump's inline instruction line has
/// no byte column and is deliberately not matched.
const QRegularExpression &disasmRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\$?([0-9A-Fa-f]{6,8})\s+((?:[0-9A-Fa-f]{2,4}\s+)*)(\S.*)?$)"),
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
    : QObject(parent)
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
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("cannot create session directory '%1'").arg(directory);
        return {};
    }

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

void EmulatorHost::openSocketServer(QString *error)
{
    // No socket requested: the emulator build has no support for one (it is
    // compiled only under HAVE_UNIX_DOMAIN_SOCKETS), so nothing will connect.
    // This is not an error — the session runs with stdin/stderr only.
    if (m_config.controlSocketPath.isEmpty())
        return;

    // Hatari is the *client*: it calls connect() and never binds, so the IDE has
    // to be listening before the process starts. However, the socket is only
    // serviced from the SDL event pump while emulation is running, so it carries
    // control commands only, never debugger commands (docs/PLAN.md §3.3).
    m_server = new QLocalServer(this);

    QLocalServer::removeServer(m_config.controlSocketPath);

    if (!m_server->listen(m_config.controlSocketPath)) {
        if (error)
            *error = QStringLiteral("cannot listen on control socket '%1': %2")
                         .arg(m_config.controlSocketPath, m_server->errorString());
        delete m_server;
        m_server = nullptr;
        return;
    }

    connect(m_server, &QLocalServer::newConnection, this, [this] {
        m_socket = m_server->nextPendingConnection();
        if (!m_socket)
            return;
        connect(m_socket, &QLocalSocket::readyRead, this, &EmulatorHost::handleSocketData);
        connect(m_socket, &QLocalSocket::disconnected, this, [this] {
            m_socket->deleteLater();
            m_socket = nullptr;
        });
    });
}

void EmulatorHost::closeSocketServer()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (!m_config.controlSocketPath.isEmpty())
        QLocalServer::removeServer(m_config.controlSocketPath);
}

bool EmulatorHost::start(const SessionConfig &config, QString *error)
{
    stop();
    m_config = config;
    m_state = MachineState();
    m_stopped = false;
    m_queue.clear();
    m_haveCurrent = false;
    m_current = Pending();
    m_promptCount = 0;
    m_promptTarget = 0;
    m_awaitingEntryPrompt = false;
    m_stdoutLoggedChars = 0;
    m_stdoutText.clear();

    if (!QFileInfo::exists(config.sessionDir) && !QDir().mkpath(config.sessionDir)) {
        if (error)
            *error = tr("cannot create session directory '%1'").arg(config.sessionDir);
        return false;
    }

    openSocketServer(error);
    // A socket is optional: it is unavailable on Windows, and the session works
    // over stdin/stderr without it.
    if (!m_config.controlSocketPath.isEmpty() && !m_server)
        return false;

    m_process = new QProcess(this);

    // Isolate the session from the user's real configuration: Hatari always
    // loads it unless HATARI_TEST is set, and CLI arguments only override what
    // we pass explicitly (docs/PLAN.md §5 rule 7).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), config.sessionDir);
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), config.sessionDir);
    m_process->setProcessEnvironment(env);

    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderrBuffer += m_process->readAllStandardError();
        int nl;
        while ((nl = m_stderrBuffer.indexOf('\n')) >= 0) {
            const QByteArray raw = m_stderrBuffer.left(nl);
            m_stderrBuffer.remove(0, nl + 1);
            handleStderrLine(QString::fromUtf8(raw).remove(QLatin1Char('\r')));
        }
        // On builds without readline the debugger prompt is written to stderr
        // instead of stdout, with no trailing newline. Check only after every
        // complete line has been consumed, otherwise a trailing prompt would be
        // counted once per line still sitting in the buffer. It is safe from
        // confusion with the `> cmd` echoes emitted by DebugUI_ParseLine and
        // DebugUI_ParseFile, because those always end in a newline.
        if (stderrEndsWithPrompt(m_stderrBuffer))
            onPrompt();
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
                closeSocketServer();
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
    m_queue.clear();
    m_haveCurrent = false;
    if (m_commandTimeout)
        m_commandTimeout->stop();

    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->kill();
            m_process->waitForFinished(2000);
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    closeSocketServer();
}

void EmulatorHost::handleStdoutData(const QByteArray &data)
{
    if (data.isEmpty())
        return;

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

bool EmulatorHost::control(const QString &hatariCommand)
{
    if (!m_socket || m_socket->state() != QLocalSocket::ConnectedState)
        return false;
    m_socket->write("hatari-" + hatariCommand.toUtf8() + "\n");
    m_socket->flush();
    return true;
}

void EmulatorHost::command(const QString &commandText)
{
    if (!isRunning()) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }
    Pending p;
    p.text = commandText;
    m_queue.enqueue(p);
    dispatchNext();
}

void EmulatorHost::dispatchNext()
{
    if (m_haveCurrent || m_queue.isEmpty() || !m_process
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

    m_process->write(m_current.text.toUtf8() + "\n");
    m_commandTimeout->start(kCommandTimeoutMs);
}

void EmulatorHost::completeCurrent()
{
    if (!m_haveCurrent)
        return;

    m_commandTimeout->stop();
    m_settleTimer->stop();

    const QString commandText = m_current.text;
    const QString response = m_current.raw.trimmed();
    m_current.response = response;

    if (commandText == QLatin1String("r"))
        parseRegisters(response);
    else if (commandText == QLatin1String("info basepage"))
        parseBasepage(response);
    else if (commandText.startsWith(QLatin1Char('d')))
        parseDisassembly(response);
    else if (commandText.startsWith(QLatin1Char('m')) && commandText.contains(QLatin1Char(' ')))
        emit memoryDumpReady(m_pendingDumpAddress, response);

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

void EmulatorHost::resume()
{
    if (!m_process || m_process->state() != QProcess::Running)
        return;
    m_stopped = false;
    emit stoppedChanged(false);
    m_process->write("c\n");
}

void EmulatorHost::refresh()
{
    command(QStringLiteral("r"));
    command(QStringLiteral("info basepage"));
    command(QStringLiteral("d"));
}

void EmulatorHost::clearBreakpoints()
{
    // `b all` removes every conditional breakpoint. Note this also removes the
    // bootstrap entry breakpoint if it somehow still exists, which is fine
    // because it is set with `:once` and has already fired by this point.
    command(QStringLiteral("b all"));
}

void EmulatorHost::armBreakpoint(const QString &condition)
{
    command(condition);
}

void EmulatorHost::requestMemoryDump(quint32 address, int length)
{
    // `m <address> <count>`; the debugger's own number base is hex.
    const QString cmd = QStringLiteral("m $%1 %2")
                            .arg(address, 0, 16)
                            .arg(length);
    m_pendingDumpAddress = address;
    command(cmd);
}

void EmulatorHost::dumpRegisters()
{
    command(QStringLiteral("r"));
}

void EmulatorHost::onPrompt()
{
    m_promptCount += 1;

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
    if (!m_haveCurrent && m_queue.isEmpty() && !m_stopped) {
        m_stopped = true;
        emit stoppedChanged(true);
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

void EmulatorHost::handleSocketData()
{
    if (!m_socket)
        return;
    m_socketBuffer += m_socket->readAll();
    if (!m_socketBuffer.trimmed().isEmpty()) {
        const QString text = QString::fromUtf8(m_socketBuffer).trimmed();
        // The only reply Hatari writes on this channel is the embed size.
        if (text.contains(QLatin1Char('x')))
            emit logLine(tr("Emulator window size: %1").arg(text));
        m_socketBuffer.clear();
    }
}

void EmulatorHost::handleStderrLine(const QString &line)
{
    if (line.contains(QLatin1String("You have entered debug mode"))) {
        m_stopped = true;
        emit stoppedChanged(true);
        emit logLine(line);
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

    emit stateUpdated(m_state);
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
    emit stateUpdated(m_state);
}

void EmulatorHost::parseDisassembly(const QString &response)
{
    m_state.disassembly.clear();

    QString pendingLabel;
    const QStringList lines = response.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed == QLatin1String("(PC)"))
            continue;

        // A bare `name:` line labels the instruction that follows.
        if (trimmed.endsWith(QLatin1Char(':')) && !trimmed.contains(QLatin1Char(' '))) {
            pendingLabel = trimmed.left(trimmed.length() - 1);
            continue;
        }

        auto m = disasmRe().match(trimmed);
        if (!m.hasMatch())
            continue;

        DisasmLine dl;
        dl.address = m.captured(1).toUInt(nullptr, 16);
        dl.bytes = m.captured(2).trimmed();
        dl.instruction = m.captured(3).trimmed();
        dl.label = pendingLabel;
        pendingLabel.clear();
        dl.isCurrentPc = (dl.address == m_state.pc);
        m_state.disassembly.append(dl);
    }

    emit stateUpdated(m_state);
}

} // namespace pist
