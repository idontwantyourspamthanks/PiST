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

#include <functional>

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

    /// Where a listening PiST says it is: the address a *running* IDE publishes
    /// to its discovery file. A port of 0 means nothing was published.
    struct Discovery
    {
        QString host;
        quint16 port = 0;
        QString token;
        /// The control protocol the IDE speaks, or 0 when the file carries no
        /// version — an IDE older than the versioned discovery line, whose
        /// vocabulary is by definition the one this shim was written against.
        int protocolVersion = 0;
    };

    /// The well-known file a listening instance publishes
    /// `host port token [protocol-version]` to (RemoteControl::discoveryFilePath
    /// on the IDE side; spelled out here because the shim is a separate binary
    /// that does not link the IDE).
    static QString discoveryFilePath();
    /// Read one discovery file — `path` empty means the well-known one. An
    /// absent, unreadable or malformed file is `Discovery{}`, not an error: the
    /// shim then keeps the endpoint it was configured with.
    static Discovery readDiscoveryFile(const QString &path = QString());

    /// Ask `resolver` where the IDE is before every dial. An IDE that restarts
    /// mints a new session token and may bind a different control port, so the
    /// address this client was configured with goes stale the moment it does —
    /// and the shim, which outlives the IDE for as long as the agent's session
    /// lasts, would then be told `error auth required` once and go silent.
    using DiscoveryResolver = std::function<Discovery()>;
    void setDiscoveryResolver(DiscoveryResolver resolver);

    /// Where to connect. A port of 0 means "not configured": every request then
    /// fails with a message the agent can act on.
    void setEndpoint(const QString &host, quint16 port);

    /// Whether the published endpoint speaks a control vocabulary this shim
    /// knows. False only when the IDE *published* a version that differs from
    /// control::kControlProtocolVersion: a mismatched pist/pist-mcp pair is
    /// then refused before the dial with both versions named, instead of
    /// connecting and failing verb by verb once an agent reaches for something
    /// the other half does not have (MIN-52).
    bool protocolCompatible() const;
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
    /// Re-resolve the endpoint from the discovery file. True when it published
    /// an address that differs from the one in force, so the caller knows a
    /// retry is worth attempting.
    bool applyDiscovery();
    /// Arm the retry that re-establishes a dropped event subscription. A dial
    /// that fails re-arms it too: an IDE restart can take longer than one tick.
    void scheduleReconnect();
    /// Open and forget the current socket. Its signals are disconnected first,
    /// so the teardown of a connection that has already been replaced cannot be
    /// read as the new connection's state.
    void dropSocket();
    void flush();
    /// (Re)arm the timer that answers a `watch` whose acknowledgement never
    /// arrives. Without it a subscription on a socket that stops answering is
    /// in neither the queue nor the in-flight slot, so nothing ever replies to
    /// the waiting `pist_watch` (MIN-8).
    void armWatchAckTimer();
    /// Write one already-framed control line, appending its terminator.
    /// Returns false — writing nothing — when `line` is not a single line, so
    /// the caller can fail the request instead of letting it time out.
    bool sendRaw(const QString &line);
    void onReadyRead(QTcpSocket *socket);
    void onDisconnected(QTcpSocket *socket);
    void onSocketError(QTcpSocket *socket);
    void onConnected(QTcpSocket *socket);
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
    /// Answers a `watch` the IDE never acknowledges.
    QTimer *m_watchTimer = nullptr;

    /// Consulted before a dial (see setDiscoveryResolver). Empty: the endpoint
    /// is taken as configured, and an address never changes under it.
    DiscoveryResolver m_resolver;
    /// A dial was retried because the IDE refused the token; a refusal of the
    /// retried connection ends the attempt rather than looping.
    bool m_authRetried = false;

    QQueue<Pending> m_queue;
    Pending m_inFlight;
    bool m_awaitingReply = false;

    /// The control protocol the published endpoint speaks, as read from the
    /// discovery file. 0: none published (or none re-read since).
    int m_endpointProtocolVersion = 0;
    /// Bytes read but not yet terminated by a newline.
    QByteArray m_partial;
    /// Lines of the block reply being assembled.
    QStringList m_blockLines;
    /// The block's status header (`ok`/`error`) has been consumed, and what it
    /// said. A block's first line is its status, never its body: a body that
    /// merely begins with "error" is data, not a failed reply (MIN-9).
    bool m_blockStatusSeen = false;
    bool m_blockOk = true;
    /// Body bytes buffered for the block in flight, so a runaway reply is
    /// capped rather than growing memory without limit (MIN-77).
    qint64 m_blockBytes = 0;

    bool m_subscribed = false;
    /// The client should be watching for events: set by subscribe(), kept across
    /// a disconnect (and a failed dial), so a dropped subscription is
    /// re-established however long the IDE takes to come back.
    bool m_watchWanted = false;
    /// The session token sent on connect, and whether its ack is outstanding.
    QString m_token;
    bool m_authAckPending = false;
    /// A `watch` sent and not yet acknowledged. Its reply is recognised by
    bool m_watchAckPending = false;
    /// A `watch` still to be sent, because the socket was not up yet.
    bool m_subscribeWhenConnected = false;
};

} // namespace pist::mcp
