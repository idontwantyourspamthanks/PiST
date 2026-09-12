// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "control/RemoteControl.h"

#include "ui/MainWindow.h"
#include "ui/EmbedX11.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace pist {

namespace {
// How long a synchronous `build` or `run` may take before the client is told it
// failed rather than left waiting forever. A cold vasm build or a TOS boot is
// seconds, not minutes.
constexpr int kOpTimeoutMs = 120000;
} // namespace

RemoteControl::RemoteControl(MainWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
}

bool RemoteControl::listen(quint16 port, QString *error)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &RemoteControl::onNewConnection);
    // Localhost only. The protocol has no authentication, so it must not be
    // reachable from another machine.
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        if (error)
            *error = m_server->errorString();
        return false;
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
        connect(client, &QTcpSocket::readyRead, this, [this, client] { onReadyRead(client); });
        connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
    }
}

void RemoteControl::onReadyRead(QTcpSocket *client)
{
    // Commands are whole lines; a partial line waits for the rest of it.
    while (client->canReadLine())
        execute(client, QString::fromUtf8(client->readLine()).trimmed());
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
    client->write(text.toUtf8());
    if (!text.endsWith(QLatin1Char('\n')))
        client->write("\n");
    client->write(".\n");
}

void RemoteControl::execute(QTcpSocket *client, const QString &line)
{
    const QString cmd = line.section(QLatin1Char(' '), 0, 0);
    const QString arg = line.section(QLatin1Char(' '), 1).trimmed();
    QPointer<QTcpSocket> guarded(client);

    if (cmd == QLatin1String("open")) {
        QMetaObject::invokeMethod(m_window, "openPath", Qt::DirectConnection,
                                  Q_ARG(QString, arg));
        reply(client, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("build") || cmd == QLatin1String("run")) {
        // These are asynchronous: build compiles, run builds and then starts the
        // emulator. Answer only once the operation has genuinely finished, which
        // is what an agent actually needs — "started" is no use for sequencing a
        // screenshot. A nested event loop gives synchronous semantics without
        // freezing the UI or the other connections.
        bool finished = false, success = false;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);

        QMetaObject::Connection done;
        if (cmd == QLatin1String("build")) {
            done = connect(m_window, &MainWindow::buildCompleted, this,
                           [&](bool ok) { finished = true; success = ok; loop.quit(); });
        } else {
            done = connect(m_window, &MainWindow::sessionRunningChanged, this,
                           [&](bool running) {
                               if (running) { finished = true; success = true; loop.quit(); }
                           });
        }
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(kOpTimeoutMs);

        QMetaObject::invokeMethod(m_window, cmd == QLatin1String("build") ? "build" : "run",
                                  Qt::DirectConnection);
        loop.exec();
        disconnect(done);

        if (!guarded)
            return;
        if (finished && success)
            reply(guarded, QStringLiteral("ok"));
        else if (finished)
            reply(guarded, QStringLiteral("error ") + cmd + QStringLiteral(" failed"));
        else
            reply(guarded, QStringLiteral("error ") + cmd + QStringLiteral(" timed out"));

    } else if (cmd == QLatin1String("stop")) {
        QMetaObject::invokeMethod(m_window, "stopSession", Qt::DirectConnection);
        reply(client, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("step")) {
        QMetaObject::invokeMethod(m_window, "step", Qt::DirectConnection);
        reply(client, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("stepover")) {
        QMetaObject::invokeMethod(m_window, "stepOver", Qt::DirectConnection);
        reply(client, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("continue")) {
        QMetaObject::invokeMethod(m_window, "resume", Qt::DirectConnection);
        reply(client, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("breakpoint")) {
        QMetaObject::invokeMethod(m_window, "toggleBreakpointAtLine", Qt::DirectConnection,
                                  Q_ARG(int, arg.toInt()));
        reply(client, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("watchpoint")) {
        // Watchpoint breaks when the value at an address changes.
        QString error;
        if (m_window->addWatchpointAddress(arg, &error))
            reply(client, QStringLiteral("ok"));
        else
            reply(client, QStringLiteral("error ") + error);

    } else if (cmd == QLatin1String("screenshot")) {
        const QString path = arg.isEmpty() ? QStringLiteral("/tmp/pist-screenshot.png") : arg;
        // Captured through XGetImage (src/ui/EmbedX11.cpp), because
        // QScreen::grabWindow returns black under XWayland on this setup, and
        // XGetImage also picks up the reparented foreign window (the embedded
        // emulator), which QWidget::grab cannot see. The window is raised first
        // so it is not some other window's pixels being read.
        m_window->raise();
        m_window->activateWindow();
        QGuiApplication::processEvents();
        const QImage img = captureWindowImage(m_window->winId());
        if (!img.isNull() && img.save(path))
            reply(client, QStringLiteral("ok"));
        else
            reply(client, QStringLiteral("error could not save screenshot to ") + path);

    } else if (cmd == QLatin1String("console")) {
        replyBlock(client, m_window->debugConsoleText());

    } else if (cmd == QLatin1String("state")) {
        replyBlock(client, m_window->stateSummary());

    } else if (cmd == QLatin1String("help")) {
        replyBlock(client, QStringLiteral(
            "open <path>      open a source file\n"
            "build            assemble, answering when the build finishes\n"
            "run              build and start the emulator, answering when running\n"
            "stop             stop the emulator session\n"
            "step             step one instruction\n"
            "stepover         step over a subroutine\n"
            "continue         resume execution\n"
            "breakpoint <n>   toggle a breakpoint at source line n\n"
            "watchpoint <a>   break when the value at address <a> changes (optional .b/.w/.l)\n"
            "screenshot <f>   save the window to <f> (default /tmp/pist-screenshot.png)\n"
            "console          the build & debug console text\n"
            "state            registers and PC\n"
            "quit             close the IDE\n"));

    } else if (cmd == QLatin1String("quit")) {
        reply(client, QStringLiteral("ok"));
        QCoreApplication::quit();

    } else if (!cmd.isEmpty()) {
        reply(client, QStringLiteral("error unknown command: ") + cmd);
    }
}

} // namespace pist
