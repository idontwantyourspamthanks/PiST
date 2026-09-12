// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Tests for the remote-control socket (src/control/RemoteControl.cpp): the
// protocol an AI agent or script uses to drive the IDE. These pin the framing
// and the synchronous command responses, because an agent has no way to recover
// from a protocol that deadlocks or answers at the wrong time.
//
// The emulator-dependent commands (run, step) are covered by the GUI and
// emulator suites; here the focus is the protocol itself, which must work with
// no emulator and no display.

#include "control/RemoteControl.h"
#include "ui/MainWindow.h"

#include <QStandardPaths>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

class TstRemoteControl : public QObject
{
    Q_OBJECT

private slots:
    void helpListsCommands();
    void unknownCommandErrors();
    void stateWithoutSessionReportsNoState();
    void consoleReturnsTextAndTerminator();
    void partialLineIsBuffered();
    void screenshotWithoutDisplayFailsGracefully();
    void buildRespondsWhenFinished();
};

namespace {

/// A control client bound to one MainWindow, listening on an OS-assigned port.
struct Session
{
    MainWindow window;
    RemoteControl control{&window};
    QTcpSocket client;

    bool start(QString *error)
    {
        if (!control.listen(0, error))
            return false;

        // Pump a full event loop rather than call QTcpSocket::waitForConnected:
        // that wait watches only the client socket's notifier, so in a
        // same-process test the server's newConnection would never be delivered
        // and every command would go unanswered. In production the application
        // runs a continuous event loop, so this is purely a test-harness concern.
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&client, &QTcpSocket::connected, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        client.connectToHost(QHostAddress::LocalHost, control.boundPort());
        timer.start(5000);
        loop.exec();
        return client.state() == QAbstractSocket::ConnectedState;
    }
};

/// Send a command and read the reply, pumping a full event loop so the server
/// (same process) gets to run. Reads until a complete line for one-line replies,
/// or until the block terminator for block replies.
QString exchange(QTcpSocket &client, const QByteArray &command, bool block)
{
    QByteArray received;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&client, &QTcpSocket::readyRead, &loop, [&] {
        received += client.readAll();
        const bool done = block ? received.endsWith("\n.\n") : received.contains('\n');
        if (done)
            loop.quit();
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    client.write(command + "\n");
    timer.start(5000);
    loop.exec();
    return QString::fromUtf8(received);
}

QString roundTrip(QTcpSocket &client, const QByteArray &command)
{
    return exchange(client, command, false).trimmed();
}

QString roundTripBlock(QTcpSocket &client, const QByteArray &command)
{
    return exchange(client, command, true);
}

} // namespace

void TstRemoteControl::helpListsCommands()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    const QString reply = roundTripBlock(s.client, "help");
    QVERIFY(reply.contains(QStringLiteral("run")));
    QVERIFY(reply.contains(QStringLiteral("screenshot")));
    QVERIFY(reply.contains(QStringLiteral("state")));
    // A block reply is terminated so the client always knows where it ends.
    QVERIFY(reply.endsWith(QLatin1String("\n.\n")));
}

void TstRemoteControl::unknownCommandErrors()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    QVERIFY(roundTrip(s.client, "frobnicate").startsWith(QStringLiteral("error")));
}

void TstRemoteControl::stateWithoutSessionReportsNoState()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // With no emulator running, state must still answer, not hang.
    const QString reply = roundTripBlock(s.client, "state");
    QVERIFY(reply.contains(QStringLiteral("no session state")));
}

void TstRemoteControl::consoleReturnsTextAndTerminator()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    const QString reply = roundTripBlock(s.client, "console");
    QVERIFY(reply.endsWith(QLatin1String("\n.\n")));
}

void TstRemoteControl::partialLineIsBuffered()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // A command split across two writes must be assembled before it runs.
    s.client.write("unkn");
    QTest::qWait(50);
    const QString reply = roundTrip(s.client, "own");
    QVERIFY(reply.startsWith(QStringLiteral("error")));
}

void TstRemoteControl::screenshotWithoutDisplayFailsGracefully()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // Offscreen there is no X11 display to capture, so the command must report
    // failure rather than crash or hang.
    const QString reply = roundTrip(s.client, "screenshot /tmp/pist-rc-test.png");
    QVERIFY(!reply.isEmpty());
}

void TstRemoteControl::buildRespondsWhenFinished()
{
    // The whole point of answering build synchronously: an agent must not have to
    // poll. This asserts the reply arrives only after the build is really done.
    if (QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot")).isEmpty())
        QSKIP("needs vasmm68k_mot");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.path() + QStringLiteral("/prog.s");
    {
        QFile f(source);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("_start:\n\trts\n");
    }

    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    QCOMPARE(roundTrip(s.client, "open " + source.toUtf8()), QStringLiteral("ok"));

    const QString reply = roundTrip(s.client, "build");
    QVERIFY2(reply == QStringLiteral("ok"),
             qPrintable(QStringLiteral("build replied: %1").arg(reply)));
}

QTEST_MAIN(TstRemoteControl)
#include "tst_remotecontrol.moc"
