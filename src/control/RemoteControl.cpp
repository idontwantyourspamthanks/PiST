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

#include <utility>

namespace pist {

namespace {
// How long a synchronous `build` or `run` may take before the client is told it
// failed rather than left waiting forever. A cold vasm build or a TOS boot is
// seconds, not minutes.
constexpr int kOpTimeoutMs = 120000;

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
        // A watcher that disconnects must leave the set, or publishEvent would
        // write to a deleted socket. deleteLater is the only deletion path, and
        // `destroyed` is emitted from it, so removing on destroyed — rather than
        // on disconnected — also covers a socket deleted for any other reason.
        connect(client, &QObject::destroyed, this, [this, client] {
            m_watchers.remove(client);
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
    while (client->canReadLine())
        queueCommand(client, QString::fromUtf8(client->readLine()).trimmed());
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
    client->write(text.toUtf8());
    if (!text.endsWith(QLatin1Char('\n')))
        client->write("\n");
    client->write(".\n");
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

    if (cmd == QLatin1String("open")) {
        QMetaObject::invokeMethod(m_window, "openPath", Qt::DirectConnection,
                                  Q_ARG(QString, arg));
        reply(guarded, QStringLiteral("ok"));

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
            done = connect(m_window, &MainWindow::buildCompleted, this,
                           [&](bool ok) { finished = true; success = ok; loop.quit(); });
        } else {
            done = connect(m_window, &MainWindow::sessionRunningChanged, this,
                           [&](bool running) {
                               if (running) { finished = true; success = true; loop.quit(); }
                           });
            // A build that fails or is refused ends the wait too: `run` must
            // not sit out the full timeout when the answer is already known
            // (buildCompleted(false) now fires for refusals as well).
            failed = connect(m_window, &MainWindow::buildCompleted, this,
                             [&](bool ok) {
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
            QMetaObject::invokeMethod(m_window,
                                      cmd == QLatin1String("build") ? "build" : "run",
                                      Qt::DirectConnection);
        });
        loop.exec();
        m_busy = false;
        disconnect(done);
        disconnect(failed);
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
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("cmd")) {
        // An arbitrary debugger command, passed through verbatim (e.g. "info mfp").
        // Only meaningful while stopped. The response comes back as a block.
        if (line.length() > 4) {
            if (m_busy) {
                reply(guarded, QStringLiteral("error busy: another command is still running"));
                return;
            }
            const QString dbg = line.mid(4);
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            QString response;
            // Match on the command text: the window routes debugger responses
            // through one signal, and the hardware view's own `info <subject>`
            // refresh uses it too, so an unrelated response must not be
            // delivered to this waiter.
            QMetaObject::Connection c = connect(m_window, &MainWindow::debugCommandFinished,
                                                &loop, [&](const QString &command, const QString &r) {
                if (command != dbg)
                    return;
                response = r;
                loop.quit();
            });
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(10000);
            m_busy = true;
            m_window->debugCommand(dbg);
            loop.exec();
            m_busy = false;
            disconnect(c);
            replyBlock(guarded, response.isEmpty() ? QStringLiteral("(no response)") : response);
        } else {
            reply(guarded, QStringLiteral("error usage: cmd <debugger command>"));
        }

    } else if (cmd == QLatin1String("step")) {
        QMetaObject::invokeMethod(m_window, "step", Qt::DirectConnection);
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("stepover")) {
        QMetaObject::invokeMethod(m_window, "stepOver", Qt::DirectConnection);
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("continue")) {
        QMetaObject::invokeMethod(m_window, "resume", Qt::DirectConnection);
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("breakpoint")) {
        QMetaObject::invokeMethod(m_window, "toggleBreakpointAtLine", Qt::DirectConnection,
                                  Q_ARG(int, arg.toInt()));
        reply(guarded, QStringLiteral("ok"));

    } else if (cmd == QLatin1String("setreg")) {
        const QString name = arg.section(QLatin1Char(' '), 0, 0);
        quint32 value = 0;
        if (name.isEmpty() || !parseValue(arg.section(QLatin1Char(' '), 1), &value)) {
            reply(guarded, QStringLiteral("error usage: setreg <name> <value>"));
        } else if (m_window->setRegister(name, value)) {
            reply(guarded, QStringLiteral("ok"));
        } else {
            reply(guarded, QStringLiteral("error not stopped"));
        }

    } else if (cmd == QLatin1String("setmem")) {
        quint32 address = 0, value = 0;
        if (!parseValue(arg.section(QLatin1Char(' '), 0, 0), &address)
            || !parseValue(arg.section(QLatin1Char(' '), 1), &value)) {
            reply(guarded, QStringLiteral("error usage: setmem <addr> <value>"));
        } else if (m_window->setMemoryByte(address, value)) {
            reply(guarded, QStringLiteral("ok"));
        } else {
            reply(guarded, QStringLiteral("error not stopped"));
        }

    } else if (cmd == QLatin1String("watchpoint")) {
        // Watchpoint breaks when the value at an address changes.
        QString error;
        if (m_window->addWatchpointAddress(arg, &error))
            reply(guarded, QStringLiteral("ok"));
        else
            reply(guarded, QStringLiteral("error ") + error);

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
            reply(guarded, QStringLiteral("ok"));
        else
            reply(guarded, QStringLiteral("error could not save screenshot to ") + path);

    } else if (cmd == QLatin1String("console")) {
        replyBlock(guarded, m_window->debugConsoleText());

    } else if (cmd == QLatin1String("state")) {
        replyBlock(guarded, m_window->stateSummary());

    } else if (cmd == QLatin1String("help")) {
        replyBlock(guarded, QStringLiteral(
            "open <path>      open a source file\n"
            "build            assemble, answering when the build finishes\n"
            "run              build and start the emulator, answering when running\n"
            "stop             stop the emulator session\n"
            "step             step one instruction\n"
            "stepover         step over a subroutine\n"
            "continue         resume execution\n"
            "breakpoint <n>   toggle a breakpoint at source line n\n"
            "setreg <n> <v>   write register <n> to <v> (when stopped)\n"
            "setmem <a> <v>   write memory byte at <a> to <v> (when stopped)\n"
            "watchpoint <a>   break when the value at address <a> changes (optional .b/.w/.l)\n"
            "screenshot <f>   save the window to <f> (default /tmp/pist-screenshot.png)\n"
            "console          the build & debug console text\n"
            "state            registers and PC\n"
            "watch            receive events as the session changes state\n"
            "unwatch          stop receiving events (connection stays open)\n"
            "quit             close the IDE\n"
            "\n"
            "build, run and cmd block until they finish; one at a time — a second\n"
            "blocking command while one is waiting is answered 'error busy'.\n"
            "watch turns this connection into an event stream: after the 'ok', the\n"
            "server pushes 'event <name> [detail]' lines (e.g. 'event stopped\n"
            "pc=0x12596', 'event running') until unwatch or disconnect.\n"));

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
        reply(guarded, QStringLiteral("ok"));
        QCoreApplication::quit();

    } else if (!cmd.isEmpty()) {
        reply(guarded, QStringLiteral("error unknown command: ") + cmd);
    }
}

} // namespace pist
