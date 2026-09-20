// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QObject>
#include <QSet>
#include <QString>

class QTcpServer;
class QTcpSocket;

namespace pist {

class MainWindow;

/// A remote-control interface for driving the IDE from another program: an AI
/// agent, a script, or a test harness. It exists because the alternative —
/// synthesising keyboard and mouse events against the window — is brittle, and
/// because an increasing number of users drive their tools with agents that are
/// far better served by a stable text protocol than by pretending to be a
/// person at a keyboard.
///
/// The design mirrors the emulator's own control socket, which PiST already
/// speaks: a simple line-based protocol over a localhost socket. See the README
/// for the command set and framing. The server is off unless a port is given;
/// an IDE that opens a listening socket by default would be a surprise.
///
/// Two kinds of client share the one listener. An ordinary client is
/// request/response: it writes a command and reads the answer. A client that
/// sends `watch` becomes a *watcher* and additionally receives unsolicited
/// `event` lines as the IDE's state changes — so an agent no longer has to poll
/// `state` to notice that a breakpoint was hit. Watching is strictly additive:
/// a watcher still gets ordinary replies to ordinary commands, and a watcher
/// that never sends `watch`-related traffic behaves exactly as before.
class RemoteControl : public QObject
{
    Q_OBJECT

public:
    explicit RemoteControl(MainWindow *window, QObject *parent = nullptr);
    ~RemoteControl() override;

    /// The well-known file a listening instance publishes its address to, so a
    /// `pist-mcp` shim with no --port can find the running IDE. GenericData-
    /// Location-based, because the shim and the IDE have different application
    /// names and must compute the same path.
    static QString discoveryFilePath();

    /// Listen on 127.0.0.1:<port>. Returns false and sets `error` on failure.
    bool listen(quint16 port, QString *error);

    /// The per-session token a client must present as `auth <token>` before
    /// anything else. Generated at listen(); published next to the port in
    /// the discovery file (written owner-only, as is its directory), because
    /// on a shared machine the port alone is discoverable by any user.
    QString token() const { return m_token; }

    /// The port actually bound, for reporting to the user.
    quint16 boundPort() const;

    /// Push one event to every watching client. `name` is the event kind
    /// (`stopped`, `running`, …) and `detail` is optional trailing text
    /// (`pc=0x12596`), so the wire line is `event <name>` or
    /// `event <name> <detail>`.
    ///
    /// MainWindow calls this from the backend's state signals. Publishing with
    /// no watchers is a no-op, so the IDE can publish unconditionally without
    /// knowing whether anyone is listening. Newlines in either argument are
    /// flattened to spaces: the protocol is line-framed and an event must stay
    /// one line, whatever the caller passes.
    void publishEvent(const QString &name, const QString &detail = QString());

    /// How many clients are currently watching. Diagnostics and tests.
    int watcherCount() const;

private slots:
    void onNewConnection();

private:
    void onReadyRead(class QTcpSocket *client);
    void queueCommand(class QTcpSocket *client, const QString &line);
    void execute(class QTcpSocket *client, const QString &line);

    /// One-line reply: "ok" or "error <message>".
    void reply(class QTcpSocket *client, const QString &line);

    /// Multi-line reply, terminated by a line containing only '.', so the client
    /// always knows where the data ends.
    void replyBlock(class QTcpSocket *client, const QString &text);

    /// Run one debugger command with synchronous semantics (a nested loop, as
    /// `cmd` has always had), returning its response. Empty on timeout or
    /// when another blocking command owns the wait — the caller words the
    /// error, since it knows what it asked.
    QString blockingDebugCommand(const QString &command, int timeoutMs);

    MainWindow *m_window;
    QTcpServer *m_server = nullptr;
    /// What listen() wrote to the discovery file, so the destructor only
    /// removes a file that is still ours.
    QString m_publishedAddress;
    /// Clients that asked for events. QPointer-like liveness is handled by
    /// dropping the entry in the socket's destroyed handler, so a socket can
    /// never be written to after Qt has deleted it.

    /// Generated at listen(). See token().
    QString m_token;
    /// Connections that have not yet presented the token. Everything they say
    /// before a valid `auth` line is rejected and drops the connection.
    QSet<class QTcpSocket *> m_pendingAuth;
    QSet<class QTcpSocket *> m_watchers;

    /// A blocking command (build/run/cmd) is waiting on its nested loop. The
    /// nested loop services every other connection, so without this a second
    /// blocking command would wait on the same signals and be answered with
    /// the first one's result.
    bool m_busy = false;
};

} // namespace pist
