// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QObject>
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
class RemoteControl : public QObject
{
    Q_OBJECT

public:
    explicit RemoteControl(MainWindow *window, QObject *parent = nullptr);

    /// Listen on 127.0.0.1:<port>. Returns false and sets `error` on failure.
    bool listen(quint16 port, QString *error);

    /// The port actually bound, for reporting to the user.
    quint16 boundPort() const;

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

    MainWindow *m_window;
    QTcpServer *m_server = nullptr;
};

} // namespace pist
