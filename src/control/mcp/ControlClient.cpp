// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "control/mcp/ControlClient.h"

#include "control/ControlProtocol.h"

#include <QFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

namespace pist::mcp {

namespace {
/// A `build` or `run` may legitimately take the IDE's own two-minute operation
/// window before it answers, so this must sit above that rather than race it.
constexpr int kReplyTimeoutMs = 180000;
/// How long to wait for the TCP connection itself before calling the IDE
/// unreachable. Localhost either accepts at once or is not listening.
constexpr int kConnectTimeoutMs = 5000;
/// How long before re-dialing after a connection is lost while an event
/// subscription is wanted. Long enough that a repeatedly failing dial costs
/// nothing, short enough that a restarted IDE is picked up promptly.
constexpr int kReconnectDelayMs = 1000;

/// How long the shim waits for a `watch` acknowledgement before answering the
/// waiting tool with a failure. A subscription is neither in the request queue
/// nor in flight (it is written raw), so without this nothing replies to a
/// `pist_watch` the IDE never answers (MIN-8). The IDE's own command timeouts
/// are 10 s; this sits beside them.
constexpr int kWatchAckTimeoutMs = 10000;

/// The most reply data the shim will hold for one flight. Far above any real
/// reply (the largest is a source file or a disassembly listing) and small
/// enough that a runaway one aborts with an error instead of growing memory
/// without limit (MIN-77).
constexpr qint64 kMaxReplyBytes = 16 * 1024 * 1024;

/// True when `text` can be one line of the protocol, which is framed on '\n':
/// an embedded break would be read by the IDE as a second verb (including the
/// ones the MCP catalog deliberately withholds, `quit` above all). The shim
/// validates tool arguments before it frames a command (see McpServer), and
/// this is the gate at the socket itself, so no caller can put a command on the
/// wire that is not the one it asked for.
bool isOneLine(const QString &text)
{
    return !text.contains(QLatin1Char('\n')) && !text.contains(QLatin1Char('\r'));
}
} // namespace

ControlClient::ControlClient(QObject *parent)
    : QObject(parent)
{
}

void ControlClient::setEndpoint(const QString &host, quint16 port)
{
    m_host = host;
    m_port = port;
}

void ControlClient::setDiscoveryResolver(DiscoveryResolver resolver)
{
    m_resolver = std::move(resolver);
}

QString ControlClient::discoveryFilePath()
{
    // GenericDataLocation is application-name-independent: this shim and the
    // IDE have different application names and must land on the same path.
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
           + QStringLiteral("/PiST/PiST/control-port");
}

ControlClient::Discovery ControlClient::readDiscoveryFile(const QString &path)
{
    QFile file(path.isEmpty() ? discoveryFilePath() : path);
    if (!file.open(QIODevice::ReadOnly))
        return Discovery();
    // `host port token [protocol-version]`, one line — the shape
    // RemoteControl::listen writes. A file without a usable port has published
    // no address.
    const QStringList parts = QString::fromUtf8(file.readAll()).trimmed()
                                  .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool ok = false;
    const uint port = parts.value(1).toUInt(&ok);
    if (!ok || port == 0 || port > 65535)
        return Discovery();
    // A missing field is 0 ("no version published"), which is not the same as
    // an unreadable one: the handshake below accepts it.
    bool versionOk = false;
    const int version = parts.value(3).toInt(&versionOk);
    if (!versionOk)
        return Discovery{parts.value(0), quint16(port), parts.value(2), 0};
    return Discovery{parts.value(0), quint16(port), parts.value(2), version};
}

bool ControlClient::protocolCompatible() const
{
    return m_endpointProtocolVersion == 0
        || m_endpointProtocolVersion == control::kControlProtocolVersion;
}

bool ControlClient::applyDiscovery()
{
    if (!m_resolver)
        return false;
    const Discovery found = m_resolver();
    if (found.port == 0)
        return false; // Nothing published: keep the endpoint in force.

    // Only adopt what was published: an entry that omits the token cannot be a
    // live session's, and dropping ours would guarantee a refusal.
    const QString host = found.host.isEmpty() ? m_host : found.host;
    const QString token = found.token.isEmpty() ? m_token : found.token;
    const bool changed = host != m_host || found.port != m_port || token != m_token;
    m_host = host;
    m_port = found.port;
    m_token = token;
    // The published protocol version is part of what is adopted: an IDE that
    // restarted with a different release speaks through this new entry.
    m_endpointProtocolVersion = found.protocolVersion;
    return changed;
}

bool ControlClient::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void ControlClient::connectNow()
{
    if (m_socket)
        return;

    // Where the IDE is *now*, before every dial: a restart mints a new session
    // token and may bind a different control port, so the address this client
    // resolves at one moment can be the previous session's by the next. The
    // resolver decides what may be overridden (main.cpp keeps an explicitly
    // configured --port/--host), so applying it here cannot lose a pin.
    applyDiscovery();
    if (!hasEndpoint())
        return;

    // The published protocol version is the handshake: a shim and an IDE from
    // different releases share a wire but not a vocabulary, so the pair is
    // refused here — naming both versions — rather than after a connection
    // whose first unknown verb answers "error unknown command" to an agent that
    // cannot act on it (MIN-52).
    if (!protocolCompatible()) {
        failEverything(QStringLiteral("PiST speaks control protocol %1 and this shim speaks %2 — "
                                      "upgrade the half that is older")
                           .arg(m_endpointProtocolVersion)
                           .arg(control::kControlProtocolVersion));
        return;
    }

    m_socket = new QTcpSocket(this);
    QTcpSocket *const socket = m_socket;
    // Each handler is given the socket it belongs to: a connection that a retry
    // has already replaced is still alive for a turn of the event loop, and its
    // delayed error or disconnect must not be read as the new one's state.
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        onDisconnected(socket);
    });
    connect(socket, &QAbstractSocket::errorOccurred, this, [this, socket] {
        onSocketError(socket);
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket] { onConnected(socket); });

    // One timer for the object's lifetime: connectNow() runs again on every
    // retry, and allocating a timer per attempt leaked one per failure.
    if (!m_connectTimer) {
        m_connectTimer = new QTimer(this);
        m_connectTimer->setSingleShot(true);
        connect(m_connectTimer, &QTimer::timeout, this, [this] {
            if (!isConnected())
                onSocketError(m_socket);
        });
    }
    m_connectTimer->start(kConnectTimeoutMs);

    m_socket->connectToHost(m_host, m_port);
}

void ControlClient::onConnected(QTcpSocket *socket)
{
    if (socket != m_socket)
        return;
    if (m_connectTimer)
        m_connectTimer->stop();
    emit connectionChanged(true);
    // The token goes first, always: the server rejects anything said before it
    // and drops the connection.
    if (!m_token.isEmpty()) {
        m_authAckPending = true;
        sendRaw(QStringLiteral("auth ") + m_token);
    }
    // A subscription asked for before the socket existed — or dropped with a
    // previous connection, or lost to a dial that failed while the IDE was
    // restarting — goes out now, so an event connection is established without
    // the shim having to ask again.
    if (m_watchWanted && !m_subscribed) {
        m_subscribeWhenConnected = false;
        m_watchAckPending = true;
        sendRaw(QStringLiteral("watch"));
        armWatchAckTimer();
    }
    flush();
}

void ControlClient::request(const QString &command, bool block, quint64 token)
{
    // Refused before it is queued rather than when it reaches the front: the
    // caller is answered now, not after whatever is already in flight, and a
    // command that is not one line never reaches the wire.
    if (!isOneLine(command)) {
        emit failed(token, QStringLiteral(
            "refusing to send a control command containing a line break"));
        return;
    }
    m_queue.enqueue(Pending{command, block, token});
    if (!isConnected())
        connectNow();
    if (!isConnected() && !hasEndpoint()) {
        // No endpoint: answer every queued request now rather than leaving the
        // agent waiting for a connection that will never be attempted.
        while (!m_queue.isEmpty())
            emit failed(m_queue.dequeue().token, QStringLiteral(
                "PiST remote control is not configured: pass --port <n> or set "
                "PIST_CONTROL_PORT to the port PiST was started with"));
        return;
    }
    flush();
}

void ControlClient::subscribe()
{
    if (m_subscribed || m_watchAckPending)
        return;
    // An IDE that published its address after this shim started is reachable
    // even though nothing has dialed yet — the same re-resolve request() gets
    // through connectNow() before it decides there is nowhere to go.
    applyDiscovery();
    if (!hasEndpoint()) {
        // No dial will ever be attempted, so answer now — arming
        // m_subscribeWhenConnected here would wait on a connection that
        // cannot exist (the same "no endpoint" answer request() gives).
        emit subscribeReplied(false, QStringLiteral(
            "PiST remote control is not configured: pass --port <n> or set "
            "PIST_CONTROL_PORT to the port PiST was started with"));
        return;
    }
    // Arm the request whether or not the socket is up: the connected handler
    // sends it, so a subscription asked for before the IDE is reachable (or
    // after a failed attempt) still lands once the connection succeeds.
    //
    // From here on the subscription is *wanted*, whatever happens to the
    // connection: a client that drops it and is never told otherwise is exactly
    // the silent event stream the shim must not become.
    m_watchWanted = true;
    m_subscribeWhenConnected = true;
    if (isConnected()) {
        m_subscribeWhenConnected = false;
        m_watchAckPending = true;
        sendRaw(QStringLiteral("watch"));
        armWatchAckTimer();
        return;
    }
    // No-op when a dial is already in flight; the point is to start one when the
    // previous attempt has failed and dropped its socket.
    connectNow();
}

void ControlClient::flush()
{
    if (m_awaitingReply || m_queue.isEmpty())
        return;
    if (!isConnected()) {
        connectNow();
        return;
    }
    // Nothing goes out until the session token has been accepted: the server
    // reads the auth line before anything else and drops a connection whose
    // token it refuses, so a command pipelined behind it would be executed
    // never — while the shim counted it in flight on a connection that cannot
    // answer it.
    if (m_authAckPending)
        return;

    m_inFlight = m_queue.dequeue();
    m_awaitingReply = true;
    m_blockLines.clear();
    m_blockStatusSeen = false;
    m_blockOk = true;
    m_blockBytes = 0;

    if (!m_replyTimer) {
        m_replyTimer = new QTimer(this);
        m_replyTimer->setSingleShot(true);
        connect(m_replyTimer, &QTimer::timeout, this, [this] {
            if (!m_awaitingReply)
                return;
            const quint64 token = m_inFlight.token;
            m_awaitingReply = false;
            m_inFlight = Pending();
            m_blockLines.clear();
            emit failed(token, QStringLiteral("PiST did not answer in time"));
            flush();
        });
    }
    m_replyTimer->start(kReplyTimeoutMs);

    // request() refuses a command with a break in it before it is ever queued,
    // so this cannot fire through the public entry point; it is the second gate
    // at the socket, and it answers rather than leaving the flight to time out.
    if (!sendRaw(m_inFlight.command)) {
        completeFlight(QString(), false,
                       QStringLiteral("refusing to send a control command containing a line break"));
        return;
    }
}

void ControlClient::armWatchAckTimer()
{
    if (!m_watchTimer) {
        m_watchTimer = new QTimer(this);
        m_watchTimer->setSingleShot(true);
        connect(m_watchTimer, &QTimer::timeout, this, [this] {
            if (!m_watchAckPending)
                return;
            m_watchAckPending = false;
            m_subscribeWhenConnected = false;
            emit subscribeReplied(false,
                                  QStringLiteral("PiST did not answer the watch "
                                                 "subscription in time"));
        });
    }
    m_watchTimer->start(kWatchAckTimeoutMs);
}

bool ControlClient::sendRaw(const QString &line)
{
    if (!m_socket)
        return false;
    // One line is what the IDE frames on: writing a second verb here would be
    // executing something the caller never asked for.
    if (!isOneLine(line))
        return false;
    m_socket->write(line.toUtf8() + '\n');
    return true;
}

void ControlClient::onReadyRead(QTcpSocket *socket)
{
    if (socket != m_socket)
        return;
    m_partial += socket->readAll();
    // A reply that never terminates a line is still bounded: the buffer is what
    // would otherwise grow without limit (MIN-77).
    if (m_partial.size() > kMaxReplyBytes) {
        failEverything(QStringLiteral("PiST's reply exceeded the %1 MiB buffer limit")
                           .arg(kMaxReplyBytes / (1024 * 1024)));
        return;
    }
    int nl = 0;
    while ((nl = m_partial.indexOf('\n')) >= 0) {
        QByteArray line = m_partial.left(nl);
        m_partial.remove(0, nl + 1);
        if (line.endsWith('\r'))
            line.chop(1);
        dispatchLine(line);
    }
}

void ControlClient::dispatchLine(const QByteArray &rawLine)
{
    const QString line = QString::fromUtf8(rawLine);

    // An event is recognised by its verb, before any reply framing: a subscribed
    // connection carries events interleaved with its own replies, and this is
    // what tells the two apart on one stream.
    if (line.startsWith(QLatin1String("event "))) {
        const QString rest = line.mid(6).trimmed();
        const QString name = rest.section(QLatin1Char(' '), 0, 0);
        const QString detail = rest.section(QLatin1Char(' '), 1).trimmed();
        if (!name.isEmpty())
            emit event(name, detail);
        return;
    }

    // The authentication ack arrives before any other reply, so it is checked
    // first — before the watch ack, which follows it on an event connection.
    if (m_authAckPending) {
        m_authAckPending = false;
        if (line != QLatin1String("ok")) {
            // A token the IDE refuses is a session that has ended: the IDE
            // restarted and mints a new token at every start, so the address
            // this client holds is the old one's. Re-read the discovery file and
            // dial again once with what it publishes now, rather than reporting
            // a failure the agent can do nothing about. Only when the file
            // actually changed: a wrong token that is also the published one
            // must not become a dial loop.
            if (!m_authRetried && applyDiscovery()) {
                m_authRetried = true;
                // Dropped from inside this socket's own readyRead: the refused
                // connection cannot carry the queued requests, and dropping it
                // disconnects its signals, so its teardown cannot fail the
                // request that is waiting to go out on the connection that
                // follows.
                dropSocket();
                connectNow();
                return;
            }
            failEverything(QStringLiteral("authentication rejected by PiST: ") + line);
            return;
        }
        m_authRetried = false;
        // Whatever was waiting on the token's acceptance goes out now.
        flush();
        return;
    }

    if (m_watchAckPending) {
        m_watchAckPending = false;
        if (m_watchTimer)
            m_watchTimer->stop();
        const bool ok = line == QLatin1String("ok");
        m_subscribed = ok;
        if (!ok) {
            // The IDE refused the subscription itself, which is not something a
            // reconnect can fix: stop re-establishing it.
            m_watchWanted = false;
        }
        emit subscribeReplied(ok, ok ? QString() : line);
        return;
    }
    if (!m_awaitingReply)
        return; // Unsolicited line with no request outstanding: ignore it.

    if (m_inFlight.block) {
        // The first line of a block is its explicit status — never body text.
        // Deciding the outcome from it, rather than from whether the body
        // starts with "error", is what keeps a compiler's "error 2 in line" (or
        // any command output) from being reported as a failed request (MIN-9).
        if (!m_blockStatusSeen) {
            m_blockStatusSeen = true;
            m_blockOk = line == QLatin1String("ok");
            return;
        }
        if (line == QLatin1String(".")) {
            const QString body = m_blockLines.join(QLatin1Char('\n'));
            if (m_blockOk)
                completeFlight(body, true, QString());
            else
                completeFlight(QString(), false, body);
            return;
        }
        m_blockBytes += rawLine.size() + 1;
        if (m_blockBytes > kMaxReplyBytes) {
            failEverything(QStringLiteral("PiST's reply exceeded the %1 MiB buffer limit")
                               .arg(kMaxReplyBytes / (1024 * 1024)));
            return;
        }
        // Dot-unescape: the sender prefixes one extra '.' to any body line that
        // starts with one, so a lone '.' line cannot terminate the block early
        // (MIN-9).
        m_blockLines << (line.startsWith(QLatin1String("..")) ? line.mid(1) : line);
        return;
    }
    completeFlight(line, true, QString());
}

void ControlClient::completeFlight(const QString &text, bool ok, const QString &message)
{
    const quint64 token = m_inFlight.token;
    m_awaitingReply = false;
    m_inFlight = Pending();
    m_blockLines.clear();
    if (m_replyTimer)
        m_replyTimer->stop();

    if (ok)
        emit replied(token, text);
    else
        emit failed(token, message);

    flush();
}

void ControlClient::onSocketError(QTcpSocket *socket)
{
    if (!socket || socket != m_socket)
        return;
    if (m_connectTimer)
        m_connectTimer->stop();

    const QString message = QStringLiteral("cannot reach PiST on %1:%2: %3")
                                .arg(m_host)
                                .arg(m_port)
                                .arg(socket->errorString());
    failEverything(message);

    // A dial can fail because the IDE is restarting, and a wanted subscription
    // has to outlive that: without re-arming here the retry the disconnect
    // scheduled is the last one, and an IDE that takes longer than a second to
    // come back leaves the subscription dead for the rest of the session.
    scheduleReconnect();
}

void ControlClient::onDisconnected(QTcpSocket *socket)
{
    if (socket != m_socket)
        return;
    emit connectionChanged(false);
    // m_watchAckPending is left for failEverything, which turns it into a
    // failed subscription answer rather than dropping it silently.
    failEverything(QStringLiteral("PiST closed the connection"));

    // An agent's client keeps the shim alive across an IDE restart, so a
    // dropped event subscription is re-established rather than silently going
    // quiet. The delay keeps a repeatedly failing dial from spinning.
    scheduleReconnect();
}

void ControlClient::scheduleReconnect()
{
    if (!m_watchWanted || !hasEndpoint())
        return;
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
            if (!isConnected())
                connectNow();
        });
    }
    m_reconnectTimer->start(kReconnectDelayMs);
}

void ControlClient::dropSocket()
{
    if (m_connectTimer)
        m_connectTimer->stop();
    if (!m_socket)
        return;
    QTcpSocket *const dead = m_socket;
    m_socket = nullptr;
    // The socket is already replaced in every sense that matters: its signals
    // are what would otherwise deliver its delayed error, disconnect or reply
    // into the state of the connection that follows it.
    dead->disconnect(this);
    m_partial.clear();
    dead->deleteLater();
}

void ControlClient::failEverything(const QString &message)
{
    // Whatever the connection carried is gone with it: a subscription on it is
    // no longer live (m_watchWanted is what keeps it coming back), and a token
    // retry has run its course.
    m_subscribed = false;
    m_authRetried = false;

    if (m_awaitingReply) {
        // completeFlight clears the in-flight slot and restarts the queue; the
        // queue is drained separately below because the socket is gone.
        const quint64 token = m_inFlight.token;
        m_awaitingReply = false;
        m_inFlight = Pending();
        m_blockLines.clear();
        if (m_replyTimer)
            m_replyTimer->stop();
        emit failed(token, message);
    }
    while (!m_queue.isEmpty())
        emit failed(m_queue.dequeue().token, message);

    // A pending watch subscription is answered too: subscribe() writes raw
    // and starts no reply timer, so it is in neither the queue nor the
    // in-flight slot — and without this a pist_watch against an unreachable
    // IDE hangs on a reply that only a live socket could produce.
    if (m_watchAckPending || m_subscribeWhenConnected) {
        m_watchAckPending = false;
        m_subscribeWhenConnected = false;
        if (m_watchTimer)
            m_watchTimer->stop();
        emit subscribeReplied(false, message);
    }

    // Drop the socket so the next request dials again. The IDE may not have been
    // started when the agent's client connected the shim, and a client that
    // starts the IDE afterwards must not be permanently locked out of a socket
    // that only ever failed to connect.
    dropSocket();
}

} // namespace pist::mcp
