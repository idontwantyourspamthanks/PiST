// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The MCP server: a stdio JSON-RPC 2.0 endpoint that forwards to a running PiST
// over the remote-control socket. It exists so an agent (Claude, Cursor, …)
// discovers the IDE as a native tool set instead of an agent having to be taught
// a bespoke line protocol.
//
// Framing: MCP's stdio transport is *newline-delimited JSON* — one JSON-RPC
// message per line, no embedded newlines, no Content-Length headers. That is
// true of every published revision of the spec (2024-11-05 through the
// 2025-06-18 / 2025-11-25 / draft versions); the Content-Length framing belongs
// to LSP, a different protocol that MCP is often confused with. See the
// transports specification for the exact wording.

#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include "control/mcp/ControlClient.h"

namespace pist::mcp {

/// One MCP session over stdio.
///
/// Reads newline-delimited JSON-RPC from `readLine`/stdin, answers on stdout,
/// and forwards tool calls to the IDE. The class is deliberately I/O-agnostic
/// about *writes* (a `sendLine` signal) so the same server can be driven over a
/// socket in tests; only reading is bound to a QIODevice.
class McpServer : public QObject
{
    Q_OBJECT

public:
    /// `controlPort` 0 means the IDE's address was not configured, which is
    /// reported to the agent on the first tool call rather than at startup —
    /// the MCP handshake must still succeed so the client sees a server.
    McpServer(const QString &controlHost, quint16 controlPort, QObject *parent = nullptr);

    /// The session token both connections present on connect (the IDE's
    /// discovery file carries it). Forwarded to both ControlClients.
    void setToken(const QString &token);

    /// Re-resolve the IDE's address before every dial (see
    /// ControlClient::setDiscoveryResolver): the shim outlives the IDE it
    /// drives, and a restarted IDE is a new session on a possibly new port with
    /// a new token. Forwarded to both connections — the event one has to
    /// reconnect on its own, without an agent asking for anything.
    void setDiscoveryResolver(ControlClient::DiscoveryResolver resolver);

    /// Feed one line (without the newline) as received on stdin. A JSON syntax
    /// error produces a JSON-RPC parse-error reply, as the spec requires.
    void handleLine(const QByteArray &line);

    /// The tool definitions, in the shape `tools/list` returns. Public so the
    /// list can be asserted directly.
    static QJsonArray tools();

signals:
    /// One line of output, to be written to stdout followed by a newline. The
    /// shim writes exactly this and nothing else: anything else on stdout
    /// corrupts the MCP stream.
    void sendLine(const QByteArray &line);

    /// Diagnostics for stderr. Never stdout.
    void logLine(const QString &line);

private:
    void dispatch(const QJsonObject &message);
    void handleInitialize(const QJsonValue &id, const QJsonObject &params);
    void handleToolsList(const QJsonValue &id);
    void handleToolsCall(const QJsonValue &id, const QJsonObject &params);
    void handleResourcesList(const QJsonValue &id);
    void handleResourcesRead(const QJsonValue &id, const QJsonObject &params);
    void handleResourcesSubscribe(const QJsonValue &id, const QJsonObject &params, bool on);
    void handlePromptsList(const QJsonValue &id);
    void handlePromptsGet(const QJsonValue &id, const QJsonObject &params);

    /// Begin the event subscription that backs the `pist_watch` tool, if it is
    /// not already running.
    void ensureSubscribed();

    void sendResult(const QJsonValue &id, const QJsonObject &result);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendRaw(const QJsonObject &message);

    /// Translate one PiST reply line ("ok" / "error …") into an MCP tool result.
    void deliverReply(quint64 token, const QString &text);
    void deliverFailure(quint64 token, const QString &message);

    /// The JSON-RPC id of an in-flight tool call. The answer is shaped into a
    /// tool result and sent under this id when the IDE replies.
    struct Outstanding {
        QJsonValue id;
        QString tool;
        /// A chained answer's context: the original error text when this is
        /// the `problems` follow-up to a failed pist_build.
        QString detail;
        /// Whether the verb was sent expecting a block reply. A block carries
        /// an explicit ok/error status (its body is never sniffed), so this is
        /// what keeps a block body that starts with "error" — a compiler
        /// diagnostic, a command's own output — from being reported as a failed
        /// call (MIN-9).
        bool block = false;
    };

    QString m_host;
    quint16 m_port;

    /// The connection ordinary commands use, and the separate one events use.
    /// One socket carries one reply framing at a time, and an event could
    /// otherwise arrive between a `state` request and its block reply.
    ControlClient *m_control = nullptr;
    ControlClient *m_events = nullptr;

    QHash<quint64, Outstanding> m_outstanding;
    quint64 m_nextToken = 1;

    /// Events seen since the last `pist_watch` call, so a call that blocks
    /// returns them rather than only the acknowledgement.
    QStringList m_recentEvents;
    /// A `pist_watch` call waiting for its `watch` acknowledgement.
    QJsonValue m_pendingWatchId;
    bool m_havePendingWatch = false;

    /// URIs with an active resources/subscribe (pist://state only: it is the
    /// one resource whose updates map onto session events).
    QSet<QString> m_resourceSubs;

    bool m_initialized = false;
};

} // namespace pist::mcp
