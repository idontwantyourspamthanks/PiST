// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The MCP shim's transport: one connection to the IDE's remote-control socket
// (src/control/RemoteControl.cpp). It exists because the MCP server has to speak
// two protocols at once — JSON-RPC to the agent, and PiST's line protocol to the
// IDE — and keeping the line protocol in its own class leaves the MCP side to
// deal only with JSON.

#pragma once

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

class QTcpSocket;
class QTimer;

namespace pist::mcp {

/// One TCP connection to a running PiST, speaking the remote-control protocol.
///
/// The protocol is line-framed with two reply shapes: a single line (`ok`, or
/// `error <message>`), and a *block* that ends with a line holding only `.`.
/// A connection may also be turned into an event stream with `watch`, after
/// which the server pushes `event <name> [detail]` lines.
///
/// Requests are queued: the remote protocol serialises (a `build` or `run` holds
/// a nested event loop and refuses a second blocking command), and queuing here
/// means an MCP client that pipelines requests still gets correct answers in
/// order rather than `error busy`. Each request carries an opaque token so the
/// caller can correlate answers to its own outstanding work.
class ControlClient : public QObject
{
    Q_OBJECT

public:
    explicit ControlClient(QObject *parent = nullptr);

    /// Where to connect. A port of 0 means "not configured": every request then
    /// fails with a message the agent can act on.
    void setEndpoint(const QString &host, quint16 port);
    bool hasEndpoint() const { return m_port != 0; }

    /// The session token the IDE expects as the first line (its discovery
    /// file carries it). Empty means the server predates authentication.
    void setToken(const QString &token) { m_token = token; }

    /// Queue a command. `block` says which reply framing to expect — it must
    /// match the verb (help/state/console/cmd are blocks, everything else a
    /// line), because only the caller knows the command it sent.
    void request(const QString &command, bool block, quint64 token);

    /// Send `watch` on this connection and start receiving events. Intended for
    /// a connection that issues no requests: events and a block reply share one
    /// stream, and keeping them on separate sockets is what makes the framing
    /// unambiguous.
    void subscribe();
    bool isSubscribed() const { return m_subscribed; }

    bool isConnected() const;
    /// How many commands are queued or awaiting a reply.
    int pendingCount() const { return m_queue.size() + (m_awaitingReply ? 1 : 0); }

signals:
    /// The answer to a request, with the token it was sent under. `text` is the
    /// line as-is (`ok`, `error …`) or the block body without its `.`
    /// terminator, which is framing rather than data.
    void replied(quint64 token, const QString &text);
    /// The request could not be answered: not configured, connection refused,
    /// dropped mid-flight, or timed out. `message` is for the agent.
    void failed(quint64 token, const QString &message);

    /// The answer to a `watch`: `ok` means events will follow on this
    /// connection. A failure carries the IDE's own error line.
    void subscribeReplied(bool ok, const QString &error);

    /// One pushed event from a subscribed connection.
    void event(const QString &name, const QString &detail);
    void connectionChanged(bool connected);

private:
    void connectNow();
    void flush();
    void sendRaw(const QString &line);
    void onReadyRead();
    void onDisconnected();
    void onSocketError();
    void dispatchLine(const QByteArray &line);
    void completeFlight(const QString &text, bool ok, const QString &message);
    void failEverything(const QString &message);

    struct Pending {
        QString command;
        bool block = false;
        quint64 token = 0;
    };

    QString m_host = QStringLiteral("127.0.0.1");
    quint16 m_port = 0;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_replyTimer = nullptr;
    /// Re-establishes an event subscription dropped by a disconnect.
    QTimer *m_reconnectTimer = nullptr;

    QQueue<Pending> m_queue;
    Pending m_inFlight;
    bool m_awaitingReply = false;

    /// Bytes read but not yet terminated by a newline.
    QByteArray m_partial;
    /// Lines of the block reply being assembled.
    QStringList m_blockLines;

    bool m_subscribed = false;
    /// The session token sent on connect, and whether its ack is outstanding.
    QString m_token;
    bool m_authAckPending = false;
    /// A `watch` sent and not yet acknowledged. Its reply is recognised by
    bool m_watchAckPending = false;
    /// A `watch` still to be sent, because the socket was not up yet.
    bool m_subscribeWhenConnected = false;
};

} // namespace pist::mcp
