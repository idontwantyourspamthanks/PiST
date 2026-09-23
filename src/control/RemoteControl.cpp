// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "control/RemoteControl.h"

#include "control/ControlProtocol.h"
#include "emu/HatariTextParse.h"
#include "emu/HexFormat.h"
#include "emu/MachineState.h"
#include "emu/MemoryDump.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace pist {

namespace {
// How long a synchronous `build` or `run` may take before the client is told it
// failed rather than left waiting forever. A cold vasm build or a TOS boot is
// seconds, not minutes.
constexpr int kOpTimeoutMs = 120000;

/// The most a connection may buffer while it has not yet produced a complete
/// line. A command is one line of text — the longest is a path or a debugger
/// command — so this is generous for a real client and small enough that
/// holding it costs nothing. It is the read buffer's size (set on accept) and
/// the threshold at which an unframed connection is dropped, because the auth
/// gate does not apply until a whole line has arrived: without it, any local
/// user could connect and stream bytes until the IDE ran out of memory.
constexpr qint64 kMaxPendingLineBytes = 64 * 1024;

/// Parse a value as hexadecimal, tolerating the $ and 0x prefixes Hatari itself
/// accepts. Bare decimal-looking text is also read as hex, which matches the
/// debugger's number base.
bool parseValue(const QString &text, quint32 *out)
{
    QString t = text.trimmed();
    if (t.startsWith(QLatin1Char('$')))
        t = t.mid(1);
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        t = t.mid(2);
    bool ok = false;
    const quint32 v = t.toUInt(&ok, 16);
    if (ok)
        *out = v;
    return ok;
}

/// The command list `help` opens with, generated from the vocabulary table so a
/// verb cannot be dispatchable without appearing in it (MIN-52).
QString verbList()
{
    QStringList lines;
    for (const control::Verb &verb : control::kVerbs)
        lines << QString::fromLatin1(verb.usage);
    return lines.join(QLatin1Char('\n'));
}

/// The prose half of `help`: the protocol's pacing and framing rules, which no
/// vocabulary row can carry.
QString helpTrailer()
{
    return QStringLiteral(
        "\n"
        "\n"
        "Send one command and wait for its reply before sending the next.\n"
        "build, run, cmd and profile stop block until they finish; one at a\n"
        "time — a second blocking command while one is waiting is answered\n"
        "'error busy'. A blocking command's wait services this connection, so\n"
        "a command pipelined behind one is executed from inside that wait and\n"
        "can answer before it: replies to pipelined commands may arrive out of\n"
        "order.\n"
        "Block replies begin with a status line, 'ok' or 'error', then the\n"
        "body, then a line holding only '.'; a body line that starts with '.'\n"
        "carries an extra leading '.' (strip one) so it cannot be confused\n"
        "with the terminator.\n"
        "watch turns this connection into an event stream: after the 'ok', the\n"
        "server pushes 'event <name> [detail]' lines (e.g. 'event stopped\n"
        "pc=0x12596', 'event running') until unwatch or disconnect.\n");
}

} // namespace

RemoteControl::RemoteControl(control::ControlHost *host, QObject *parent)
    : QObject(parent)
    , m_host(host)
{
}

RemoteControl::~RemoteControl()
{
    // Leave no stale address behind: a shim that reads a dead file connects
    // to nothing and gets a refused connection rather than silence. If the
    // file is not ours (a second IDE overwrote it), leave it alone.
    QFile file(discoveryFilePath());
    if (file.exists() && file.open(QIODevice::ReadOnly)
        && QString::fromUtf8(file.readAll()).trimmed() == m_publishedAddress) {
        file.close();
        QFile::remove(discoveryFilePath());
    }
}

QString RemoteControl::discoveryFilePath()
{
    // GenericDataLocation is application-name-independent: pist (org/app
    // "PiST") and pist-mcp must land on the same path, which AppDataLocation
    // would not give them.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/PiST/PiST/control-port");
}

bool RemoteControl::listen(quint16 port, QString *error)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &RemoteControl::onNewConnection);
    // Localhost only, but on a shared machine that still means every local
    // user: the per-session token is what keeps "reachable" from meaning
    // "drivable by anyone who can read the port number".
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        if (error)
            *error = m_server->errorString();
        return false;
    }

    m_token = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Publish the address for zero-configuration shims — but only for a port
    // the caller chose. Tests bind port 0 (OS-assigned) by the hundred; they
    // must not overwrite a real IDE's discovery file.
    if (port == 0)
        return true;
    m_publishedAddress = QStringLiteral("127.0.0.1 %1 %2 %3")
                             .arg(m_server->serverPort())
                             .arg(m_token)
                             .arg(control::kControlProtocolVersion);
    const QString dirPath = QFileInfo(discoveryFilePath()).absolutePath();
    QDir().mkpath(dirPath);
    // Owner-only, directory and file: the token is the credential, and a
    // world-readable credential is no credential.
    QFile::setPermissions(dirPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ExeOwner);
    QFile file(discoveryFilePath());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(m_publishedAddress.toUtf8() + '\n');
        file.close();
        QFile::setPermissions(discoveryFilePath(),
                              QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    return true;
}

quint16 RemoteControl::boundPort() const
{
    return m_server ? m_server->serverPort() : 0;
}

void RemoteControl::onNewConnection()
{
    while (QTcpSocket *client = m_server->nextPendingConnection()) {
        client->setParent(this);
        // Cap what a peer can make this process hold before it has said
        // anything meaningful: the socket stops reading from the OS once the
        // buffer is full, and onReadyRead drops a connection that fills it
        // without a complete line. Unauthenticated peers reach this, so the
        // buffer is the only thing between a local user and the IDE's memory.
        client->setReadBufferSize(kMaxPendingLineBytes);
        connect(client, &QTcpSocket::readyRead, this, [this, client] { onReadyRead(client); });
        connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        // Unauthenticated until it presents the token (see onReadyRead).
        m_pendingAuth.insert(client);
        // A watcher that disconnects must leave the set, or publishEvent would
        // write to a deleted socket. deleteLater is the only deletion path, and
        // `destroyed` is emitted from it, so removing on destroyed — rather than
        // on disconnected — also covers a socket deleted for any other reason.
        connect(client, &QObject::destroyed, this, [this, client] {
            m_watchers.remove(client);
            m_pendingAuth.remove(client);
        });
    }
}

void RemoteControl::publishEvent(const QString &name, const QString &detail)
{
    if (m_watchers.isEmpty())
        return;

    // The protocol is line-framed, so an event must be exactly one line: a
    // caller-supplied newline would otherwise be read as a second, malformed
    // line by the client. Flatten rather than reject — a publish is not a place
    // to fail an agent's subscription.
    const auto flatten = [](QString text) {
        return text.replace(QLatin1Char('\n'), QLatin1Char(' '))
            .replace(QLatin1Char('\r'), QLatin1Char(' '));
    };
    QString line = QStringLiteral("event ") + flatten(name);
    const QString tail = flatten(detail).trimmed();
    if (!tail.isEmpty())
        line += QLatin1Char(' ') + tail;
    line += QLatin1Char('\n');
    const QByteArray bytes = line.toUtf8();

    for (QTcpSocket *client : std::as_const(m_watchers))
        client->write(bytes);
}

int RemoteControl::watcherCount() const
{
    return m_watchers.size();
}

void RemoteControl::onReadyRead(QTcpSocket *client)
{
    // A connection that has filled its read buffer without producing a line is
    // not going to produce one within any sane limit, whether it is
    // authenticated or not: the line is dropped, not truncated, because a line
    // the server silently altered is not the one the client sent. This is what
    // makes the unauthenticated window bounded — nothing about the peer is
    // known yet, and it is holding the whole buffer by itself.
    if (!client->canReadLine() && client->bytesAvailable() >= kMaxPendingLineBytes) {
        client->abort();
        return;
    }

    // Commands are whole lines; a partial line waits for the rest of it.
    //
    // Lines are executed queued, on a fresh dispatch, never inside this
    // readyRead handler: the blocking commands (build/run/cmd) spin nested
    // event loops, and a nested loop running inside the socket's own
    // dispatch delivers a disconnected peer's deferred delete
    // (onNewConnection) while the notifier frame that called us is still
    // suspended beneath it — Qt then resumes that frame into a destroyed
    // socket (QAbstractSocketPrivate::canReadNotification after the
    // emission). One loop hop later there is no socket dispatch on the
    // stack; the QPointer covers the gap between queuing and running.
    while (client->canReadLine()) {
        const QString line = QString::fromUtf8(client->readLine()).trimmed();
        // The first line a connection says must be the token. Anything else —
        // or the wrong token — is an error and the end of the connection.
        if (m_pendingAuth.contains(client)) {
            if (line == QLatin1String("auth ") + m_token) {
                m_pendingAuth.remove(client);
                reply(client, QStringLiteral("ok"));
                continue;
            }
            reply(client, QStringLiteral("error auth required"));
            client->disconnectFromHost();
            // disconnect is asynchronous and the socket stays readable, so
            // stop here — and keep the client in m_pendingAuth, so any line
            // arriving in the shutdown window hits this gate again instead of
            // executing unauthenticated.
            break;
        }
        queueCommand(client, line);
    }
}

void RemoteControl::queueCommand(QTcpSocket *client, const QString &line)
{
    QPointer<QTcpSocket> guarded(client);
    QMetaObject::invokeMethod(
        this, [this, guarded, line] {
            if (guarded)
                execute(guarded, line);
        },
        Qt::QueuedConnection);
}

void RemoteControl::reply(QTcpSocket *client, const QString &line)
{
    if (client)
        client->write((line + QLatin1Char('\n')).toUtf8());
}

void RemoteControl::replyBlock(QTcpSocket *client, const QString &text)
{
    if (!client)
        return;
    // An explicit status line, not a body sniff: the body may legitimately
    // begin with "error" (vasm's "error 2 in line", the debugger's own text) and
    // must not be read as a failed reply (MIN-9).
    client->write("ok\n");
    writeBlockBody(client, text);
}

void RemoteControl::replyBlockError(QTcpSocket *client, const QString &text)
{
    if (!client)
        return;
    client->write("error\n");
    writeBlockBody(client, text);
}

void RemoteControl::writeBlockBody(QTcpSocket *client, const QString &text)
{
    // Dot-stuffing (RFC 5321 style): a body line that begins with '.' is sent
    // with one extra leading '.', so a line that is *only* '.' cannot be read as
    // the terminator and truncate the block. The client strips one leading dot
    // from any line that starts with "..". A body line holding only '.' is
    // exactly what assembly source, a disassembly listing or a debugger
    // transcript can contain, and it used to end the reply early (MIN-9).
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        // A single trailing newline is the body's own terminator, not a blank
        // final line.
        if (i == lines.size() - 1 && lines.at(i).isEmpty() && lines.size() > 1)
            break;
        const QString line = lines.at(i);
        client->write((line.startsWith(QLatin1Char('.')) ? QLatin1Char('.') + line : line).toUtf8());
        client->write("\n");
    }
    client->write(".\n");
}

QString RemoteControl::blockingDebugCommand(const QString &command, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QString response;
    // Match on the command text: this is the client's own command, echoed back
    // through debugCommandFinished, so the wait is for its own text and no
    // other response can satisfy it. (The verbs that ask the IDE for data —
    // readmem, disasm — carry an address instead: see waitForDebugRead.)
    QMetaObject::Connection c = m_host->onDebugCommandFinished(
        &loop, [&](const QString &finished, const QString &r) {
            if (finished != command)
                return;
            response = r;
            loop.quit();
        });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    m_busy = true;
    m_host->debugCommand(command);
    loop.exec();
    m_busy = false;
    QObject::disconnect(c);
    return response;
}

QString RemoteControl::waitForDebugRead(quint32 address, int timeoutMs)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QString response;
    // The IDE relays a typed read's answer with the address it was asked
    // about, so a stale answer (a read whose verb already timed out) cannot
    // satisfy this waiter, and no command text is matched anywhere.
    QMetaObject::Connection c = m_host->onDebugReadFinished(
        &loop, [&](quint32 at, const QString &r) {
            if (at != address)
                return;
            response = r;
            loop.quit();
        });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    loop.exec();
    QObject::disconnect(c);
    return response;
}

bool RemoteControl::waitForDebugMemory(quint32 address, int timeoutMs, QList<MemoryRow> *rows)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool arrived = false;
    // Same contract as waitForDebugRead, one event over: a memory read answers
    // with the rows the backend parsed from the dump (MAJ-45), so the verb
    // renders its JSON from them rather than parsing the dump its own way.
    QMetaObject::Connection c = m_host->onDebugReadMemoryFinished(
        &loop, [&](quint32 at, const QList<MemoryRow> &r) {
            if (at != address)
                return;
            *rows = r;
            arrived = true;
            loop.quit();
        });
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    loop.exec();
    QObject::disconnect(c);
    return arrived;
}

void RemoteControl::execute(QTcpSocket *client, const QString &line)
{
    const QString cmd = line.section(QLatin1Char(' '), 0, 0);
    const QString arg = line.section(QLatin1Char(' '), 1).trimmed();
    // Every branch replies through the guard, not the raw pointer: several
    // commands pump events before replying (modal dialogs from open/build/run
    // refusal paths, screenshot's processEvents), and a peer that disconnects
    // inside one of those loops is deleted before the reply is written.
    QPointer<QTcpSocket> guarded(client);

    // The vocabulary table owns the verb list (MIN-52): a word it does not
    // carry was never a command this server could answer, so it is refused
    // before any dispatch. Every row the dispatcher implements appears in
    // `help` and carries the shim's reply framing from that same row.
    if (!cmd.isEmpty() && !control::findVerb(cmd)) {
        reply(guarded, QStringLiteral("error unknown command: ") + cmd);
        return;
    }

    if (cmd == QLatin1String("open")) {
        // openPathQuiet, not openPath: the interactive path shows a modal on
        // failure, and a modal nobody can dismiss (offscreen, or an agent on
        // the other end) blocks this reply forever. A bare "ok" would tell
        // the agent the IDE is showing a file it never opened.
        const bool opened = m_host->openPathQuiet(arg);
        reply(guarded, opened ? QStringLiteral("ok")
                              : QStringLiteral("error could not open %1").arg(arg));

    } else if (cmd == QLatin1String("build") || cmd == QLatin1String("run")) {
        // These are asynchronous: build compiles, run builds and then starts the
        // emulator. Answer only once the operation has genuinely finished, which
        // is what an agent actually needs — "started" is no use for sequencing a
        // screenshot. A nested event loop gives synchronous semantics without
        // freezing the UI or the other connections.
        if (m_busy) {
            reply(guarded, QStringLiteral("error busy: another command is still running"));
            return;
        }
        m_busy = true;
        bool finished = false, success = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);

        QMetaObject::Connection done;
        QMetaObject::Connection failed;
        if (cmd == QLatin1String("build")) {
            done = m_host->onBuildCompleted(&loop,
                                            [&](bool ok) { finished = true; success = ok; loop.quit(); });
        } else {
            done = m_host->onSessionRunningChanged(&loop, [&](bool running) {
                if (running) { finished = true; success = true; loop.quit(); }
            });
            // A build that fails or is refused ends the wait too: `run` must
            // not sit out the full timeout when the answer is already known
            // (buildCompleted(false) now fires for refusals as well).
            failed = m_host->onBuildCompleted(&loop, [&](bool ok) {
                if (!ok) { finished = true; success = false; loop.quit(); }
            });
        }
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(kOpTimeoutMs);

        // Start the operation only once the wait loop is running: a
        // synchronous refusal emits buildCompleted(false) during the
        // invocation, and a quit() before exec() is lost — the wait would
        // then run to its full timeout for an answer that already happened.
        QTimer::singleShot(0, this, [this, cmd] {
            if (cmd == QLatin1String("build"))
                m_host->build();
            else
                m_host->run();
        });
        loop.exec();
        m_busy = false;
        QObject::disconnect(done);
        QObject::disconnect(failed);
        if (!guarded)
            return;
        if (finished && success)
            reply(guarded, QStringLiteral("ok"));
        else if (finished)
            reply(guarded, QStringLiteral("error ") + cmd + QStringLiteral(" failed"));
        else
            reply(guarded, QStringLiteral("error ") + cmd + QStringLiteral(" timed out"));

    } else if (cmd == QLatin1String("stop")) {
        m_host->stopSession();
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("cmd")) {
        // An arbitrary debugger command, passed through verbatim (e.g. "info mfp").
        // Only meaningful while stopped. The response comes back as a block.
        if (line.length() > 4) {
            const QString dbg = line.mid(4);
            if (m_busy) {
                // Errors on block-framed verbs go back as blocks: a client
                // waiting for the '.' terminator must never sit out its
                // timeout on a plain error line.
                replyBlockError(guarded, QStringLiteral("error busy: another command is still running"));
                return;
            }
            const QString response = blockingDebugCommand(dbg, 10000);
            replyBlock(guarded, response.isEmpty() ? QStringLiteral("(no response)") : response);
        } else {
            replyBlockError(guarded, QStringLiteral("error usage: cmd <debugger command>"));
        }

    } else if (cmd == QLatin1String("readmem")) {
        // readmem <addr> <len>: a JSON array of rows, so an agent reads bytes
        // instead of parsing the memdump text. Needs a stopped session.
        quint32 address = 0;
        bool lengthOk = false;
        const int length = arg.section(QLatin1Char(' '), 1, 1).toInt(&lengthOk);
        if (!parseValue(arg.section(QLatin1Char(' '), 0, 0), &address) || !lengthOk || length <= 0) {
            replyBlockError(guarded, QStringLiteral("error usage: readmem <addr> <len>"));
            return;
        }
        if (m_busy) {
            replyBlockError(guarded, QStringLiteral("error busy: another command is still running"));
            return;
        }
        m_busy = true;
        m_host->debugReadMemory(address, length);
        QList<MemoryRow> dump;
        const bool arrived = waitForDebugMemory(address, 10000, &dump);
        m_busy = false;
        if (!arrived) {
            replyBlockError(guarded, QStringLiteral("error no response (needs a stopped emulator session)"));
            return;
        }
        // Rendered from the rows the backend parsed out of the dump (MAJ-45):
        // one parse of the debugger's text, and the verb sees exactly what the
        // memory pane sees.
        QJsonArray rows;
        for (const MemoryRow &row : dump) {
            QJsonObject object;
            object.insert(QStringLiteral("address"),
                          QStringLiteral("0x") + hex::hex32(row.address, hex::Case::Lower));
            QString bytes;
            for (const quint8 byte : row.bytes)
                bytes += hex::hexByte(byte, hex::Case::Lower);
            object.insert(QStringLiteral("bytes"), bytes);
            rows.append(object);
        }
        replyBlock(guarded, QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));

    } else if (cmd == QLatin1String("disasm")) {
        // disasm [addr]: the debugger's disassembly as JSON rows
        // {address, bytes, text} — from the PC when no address is given. The PC
        // is taken from the machine state the last stop published: the
        // debugger's own bare `d` means "from the last disassembly address",
        // which is the PC only by accident.
        quint32 address = 0;
        if (arg.isEmpty()) {
            if (!parseValue(m_host->stateJson().value(QStringLiteral("pc")).toString(),
                            &address)) {
                replyBlockError(guarded,
                                QStringLiteral("error no disassembly address (needs a stopped "
                                               "emulator session)"));
                return;
            }
        } else if (!parseValue(arg, &address)) {
            replyBlockError(guarded, QStringLiteral("error usage: disasm [addr]"));
            return;
        }
        if (m_busy) {
            replyBlockError(guarded, QStringLiteral("error busy: another command is still running"));
            return;
        }
        m_busy = true;
        m_host->debugReadDisassembly(address);
        const QString response = waitForDebugRead(address, 10000);
        m_busy = false;
        if (response.isEmpty()) {
            replyBlockError(guarded, QStringLiteral("error no response (needs a stopped emulator session)"));
            return;
        }
        MachineState state;
        hataritext::parseDisassembly(response, &state);
        QJsonArray rows;
        for (const DisasmLine &disasm : state.disassembly) {
            QJsonObject object;
            object.insert(QStringLiteral("address"),
                          QStringLiteral("0x") + hex::hex32(disasm.address, hex::Case::Lower));
            object.insert(QStringLiteral("bytes"), disasm.bytes);
            object.insert(QStringLiteral("text"), disasm.instruction);
            rows.append(object);
        }
        replyBlock(guarded, QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));

    } else if (cmd == QLatin1String("symbols")) {
        // symbols [filter]: the build's symbol table as JSON; addresses appear
        // once the program map has live bases.
        replyBlock(guarded, QString::fromUtf8(
            QJsonDocument(m_host->symbolsJson(arg)).toJson(QJsonDocument::Compact)));


    } else if (cmd == QLatin1String("step")) {
        m_host->step();
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("stepover")) {
        m_host->stepOver();
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("continue")) {
        m_host->resume();
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("breakpoint")) {
        bool numeric = false;
        const int line = arg.toInt(&numeric);
        if (!numeric) {
            // Label form: resolve the symbol to its definition's file:line and
            // toggle there. The reply names where it landed (and the address
            // once the map has live bases), so the agent can check it meant
            // the same `count` the IDE did.
            if (arg.isEmpty()) {
                reply(guarded, QStringLiteral("error usage: breakpoint <line|label>"));
                return;
            }
            QString detail;
            m_host->toggleBreakpointAtLabel(arg, &detail);
            reply(guarded, detail);
            return;
        }
        const bool toggled = m_host->toggleBreakpointAtLine(line);
        if (!toggled) {
            reply(guarded, QStringLiteral("error no source file open, or an invalid line number"));
            return;
        }
        // "ok" reads as "this will fire", but arming skips lines with no
        // instruction, so say so when the map already knows that — an agent
        // told plain ok would wait on a stop that can never come. The note is
        // action-neutral: this verb toggles, so the call may have *removed*
        // the breakpoint rather than set it.
        reply(guarded, m_host->lineHasCode(line)
                          ? QStringLiteral("ok")
                          : QStringLiteral("ok; line %1 emits no code, so a breakpoint there cannot fire")
                                .arg(line));

    } else if (cmd == QLatin1String("setreg")) {
        const QString name = arg.section(QLatin1Char(' '), 0, 0);
        quint32 value = 0;
        if (name.isEmpty() || !parseValue(arg.section(QLatin1Char(' '), 1), &value)) {
            reply(guarded, QStringLiteral("error usage: setreg <name> <value>"));
        } else if (m_host->setRegister(name, value)) {
            reply(guarded, QStringLiteral("ok"));
        } else {
            reply(guarded, QStringLiteral("error not stopped"));
        }

    } else if (cmd == QLatin1String("setmem")) {
        quint32 address = 0, value = 0;
        if (!parseValue(arg.section(QLatin1Char(' '), 0, 0), &address)
            || !parseValue(arg.section(QLatin1Char(' '), 1), &value)) {
            reply(guarded, QStringLiteral("error usage: setmem <addr> <value>"));
        } else if (m_host->writeMemoryByte(address, value)) {
            reply(guarded, QStringLiteral("ok"));
        } else {
            reply(guarded, QStringLiteral("error not stopped"));
        }

    } else if (cmd == QLatin1String("watchpoint")) {
        // Watchpoint breaks when the value at an address changes.
        QString error;
        if (m_host->addWatchpointAddress(arg, &error))
            reply(guarded, QStringLiteral("ok"));
        else
            reply(guarded, QStringLiteral("error ") + error);

    } else if (cmd == QLatin1String("tabs")) {
        replyBlock(guarded, QString::fromUtf8(
            QJsonDocument(m_host->tabsJson()).toJson(QJsonDocument::Compact)));

    } else if (cmd == QLatin1String("save")) {
        // Quiet save of the current document; an untitled document can't be
        // named from here, so that is an error, not a dialog.
        reply(guarded, m_host->saveCurrentDocument()
                          ? QStringLiteral("ok")
                          : QStringLiteral("error nothing to save, or the write failed"));


    } else if (cmd == QLatin1String("screenshot")) {
        const QString path = arg.isEmpty() ? QStringLiteral("/tmp/pist-screenshot.png") : arg;
        // The capture itself belongs to the UI: it needs the window handle and
        // XGetImage (QScreen::grabWindow returns black under XWayland on this
        // setup, and XGetImage also picks up the reparented foreign window —
        // the embedded emulator — which QWidget::grab cannot see). The window
        // is raised first so it is not some other window's pixels being read.
        if (m_host->saveScreenshot(path))
            reply(guarded, QStringLiteral("ok"));
        else
            reply(guarded, QStringLiteral("error could not save screenshot to ") + path);

    } else if (cmd == QLatin1String("console")) {
        replyBlock(guarded, m_host->debugConsoleText());

    } else if (cmd == QLatin1String("state")) {
        replyBlock(guarded, m_host->stateSummary());

    } else if (cmd == QLatin1String("read")) {
        // The current document as JSON {path, text}. JSON rather than raw
        // source text: a block ends on a line holding only '.', and assembly
        // source can legitimately contain one — echoing bytes into the block
        // channel would truncate the read there. The error goes back as a
        // block too, because a block-framed wait would otherwise sit out its
        // timeout on a plain error line.
        const QJsonObject document = m_host->documentJson();
        if (document.isEmpty())
            replyBlockError(guarded, QStringLiteral("error no source file is open"));
        else
            replyBlock(guarded, QString::fromUtf8(
                QJsonDocument(document).toJson(QJsonDocument::Compact)));


    } else if (cmd == QLatin1String("statejson")) {
        replyBlock(guarded, QString::fromUtf8(
            QJsonDocument(m_host->stateJson()).toJson(QJsonDocument::Compact)));

    } else if (cmd == QLatin1String("problems")) {
        replyBlock(guarded, QString::fromUtf8(
            QJsonDocument(m_host->problemsJson()).toJson(QJsonDocument::Compact)));

    } else if (cmd == QLatin1String("profile")) {
        const QString sub = arg.section(QLatin1Char(' '), 0, 0);
        if (sub == QLatin1String("start")) {
            m_host->profileStart();
            reply(guarded, QStringLiteral("ok"));
        } else if (sub == QLatin1String("stop")) {
            // Block until the save is parsed: an agent that gets "ok" and
            // immediately asks for results must never see the previous run's
            // data (or nothing). Same nested-loop pattern as build/run — and the
            // same gate, because the nested loop services every other
            // connection: without it a second client's build/run/cmd runs inside
            // this wait (stacking loops, and a run can restart the emulator
            // session while the save is still being parsed), and this verb would
            // in turn be invoked synchronously inside a build that is already
            // waiting.
            if (m_busy) {
                reply(guarded, QStringLiteral("error busy: another command is still running"));
                return;
            }
            m_busy = true;
            const bool started = m_host->profileStop();
            if (!started) {
                m_busy = false;
                reply(guarded, QStringLiteral(
                    "error needs a stopped emulator session with a profile running"));
                return;
            }
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            bool ready = false, ok = false;
            QMetaObject::Connection c = m_host->onProfileResultsReady(&loop, [&](bool parsed) {
                ready = true;
                ok = parsed;
                loop.quit();
            });
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(15000);
            loop.exec();
            QObject::disconnect(c);
            // Cleared before the reply, on every exit path: the client that acts
            // on this answer must not be the one refused next because of it.
            m_busy = false;
            if (!ready)
                reply(guarded, QStringLiteral("error profile save timed out"));
            else if (!ok)
                reply(guarded, QStringLiteral("error profile save could not be parsed (see console)"));
            else
                reply(guarded, QStringLiteral("ok"));
        } else if (sub == QLatin1String("results")) {
            replyBlock(guarded, QString::fromUtf8(
                QJsonDocument(m_host->profilerResultsJson()).toJson(QJsonDocument::Compact)));
        } else {
            reply(guarded, QStringLiteral("error usage: profile start|stop|results"));
        }

    } else if (cmd == QLatin1String("help")) {
        replyBlock(guarded, verbList() + helpTrailer());

    } else if (cmd == QLatin1String("watch")) {
        // Turn this connection into an event stream. The reply is sent first so
        // the client can safely switch to reading events the moment it sees
        // `ok` — otherwise an event published between the request and the reply
        // would interleave ahead of it (same socket, same order) and be read as
        // the reply. Idempotent: a second `watch` re-confirms rather than
        // double-subscribing, which QSet would collapse anyway.
        m_watchers.insert(client);
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("unwatch")) {
        // Stop events but keep the connection: the client can go back to plain
        // request/response. Idempotent, and an `ok` either way, so a client
        // doing best-effort cleanup never has to distinguish "was watching".
        m_watchers.remove(client);
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("quit")) {
        // Deliberately not exposed as an MCP tool: an agent that can close
        // the user's IDE unprompted is a foot-gun, and whoever drives the
        // socket by hand can say `quit` themselves.
        reply(guarded, QStringLiteral("ok"));
        QCoreApplication::quit();

    } else if (!cmd.isEmpty()) {
        // Unreachable while the table and the branches above agree: the gate at
        // the top of this function already refused any word the table does not
        // carry. A row with no branch would land here, and the peer gets an
        // answer rather than a connection that waits forever (MIN-52).
        reply(guarded, QStringLiteral("error unknown command: ") + cmd);
    }
}

} // namespace pist
