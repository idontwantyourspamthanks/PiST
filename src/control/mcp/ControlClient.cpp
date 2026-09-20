// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "control/mcp/ControlClient.h"

#include <QTcpSocket>
#include <QTimer>

namespace pist::mcp {

namespace {
/// A `build` or `run` may legitimately take the IDE's own two-minute operation
/// window before it answers, so this must sit above that rather than race it.
constexpr int kReplyTimeoutMs = 180000;
/// How long to wait for the TCP connection itself before calling the IDE
/// unreachable. Localhost either accepts at once or is not listening.
constexpr int kConnectTimeoutMs = 5000;
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

bool ControlClient::isConnected() const
{
    return m_socket && m_socket->state() == QAbstractSocket::ConnectedState;
}

void ControlClient::connectNow()
{
    if (!hasEndpoint() || m_socket)
        return;

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::readyRead, this, &ControlClient::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ControlClient::onDisconnected);
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        if (m_connectTimer)
            m_connectTimer->stop();
        emit connectionChanged(true);
        // A subscription asked for before the socket existed goes out now, so
        // an event connection is established without the shim having to retry.
        if (m_subscribeWhenConnected) {
            m_subscribeWhenConnected = false;
            m_watchAckPending = true;
            sendRaw(QStringLiteral("watch"));
        }
        flush();
    });
    connect(m_socket, &QAbstractSocket::errorOccurred, this, &ControlClient::onSocketError);

    // One timer for the object's lifetime: connectNow() runs again on every
    // retry, and allocating a timer per attempt leaked one per failure.
    if (!m_connectTimer) {
        m_connectTimer = new QTimer(this);
        m_connectTimer->setSingleShot(true);
        connect(m_connectTimer, &QTimer::timeout, this, [this] {
            if (!isConnected())
                onSocketError();
        });
    }
    m_connectTimer->start(kConnectTimeoutMs);

    m_socket->connectToHost(m_host, m_port);
}

void ControlClient::request(const QString &command, bool block, quint64 token)
{
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
    // Arm the request whether or not the socket is up: the connected handler
    // sends it, so a subscription asked for before the IDE is reachable (or
    // after a failed attempt) still lands once the connection succeeds.
    m_subscribeWhenConnected = true;
    if (isConnected()) {
        m_subscribeWhenConnected = false;
        m_watchAckPending = true;
        sendRaw(QStringLiteral("watch"));
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

    m_inFlight = m_queue.dequeue();
    m_awaitingReply = true;
    m_blockLines.clear();

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

    sendRaw(m_inFlight.command);
}

void ControlClient::sendRaw(const QString &line)
{
    if (!m_socket)
        return;
    m_socket->write(line.toUtf8() + '\n');
}

void ControlClient::onReadyRead()
{
    m_partial += m_socket->readAll();
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

    if (m_watchAckPending) {
        m_watchAckPending = false;
        const bool ok = line == QLatin1String("ok");
        m_subscribed = ok;
        emit subscribeReplied(ok, ok ? QString() : line);
        return;
    }
    if (!m_awaitingReply)
        return; // Unsolicited line with no request outstanding: ignore it.

    if (m_inFlight.block) {
        if (line == QLatin1String(".")) {
            completeFlight(m_blockLines.join(QLatin1Char('\n')), true, QString());
        } else {
            m_blockLines << line;
        }
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

void ControlClient::onSocketError()
{
    if (m_connectTimer)
        m_connectTimer->stop();
    if (!m_socket)
        return;

    const QString message = QStringLiteral("cannot reach PiST on %1:%2: %3")
                                .arg(m_host)
                                .arg(m_port)
                                .arg(m_socket->errorString());
    failEverything(message);
}

void ControlClient::onDisconnected()
{
    emit connectionChanged(false);
    const bool wasSubscribed = m_subscribed;
    m_subscribed = false;
    m_watchAckPending = false;
    m_partial.clear();
    failEverything(QStringLiteral("PiST closed the connection"));

    // An agent's client keeps the shim alive across an IDE restart, so a
    // dropped event subscription is re-established rather than silently going
    // quiet. The delay keeps a repeatedly failing dial from spinning.
    if (wasSubscribed) {
        m_subscribeWhenConnected = true;
        if (!m_reconnectTimer) {
            m_reconnectTimer = new QTimer(this);
            m_reconnectTimer->setSingleShot(true);
            connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
                if (!isConnected())
                    connectNow();
            });
        }
        m_reconnectTimer->start(1000);
    }
}

void ControlClient::failEverything(const QString &message)
{
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

    // Drop the socket so the next request dials again. The IDE may not have been
    // started when the agent's client connected the shim, and a client that
    // starts the IDE afterwards must not be permanently locked out of a socket
    // that only ever failed to connect.
    if (m_socket) {
        if (m_connectTimer)
            m_connectTimer->stop();
        QTcpSocket *dead = m_socket;
        m_socket = nullptr;
        dead->deleteLater();
    }
}

} // namespace pist::mcp
