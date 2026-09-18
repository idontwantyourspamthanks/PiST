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
#include <QMessageBox>

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
    void watchpointCommandValidatesAddress();
    void buildRespondsWhenFinished();
    void cmdDisconnectDuringWaitSurvives();
    void runWithoutSourceFailsFast();
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

void TstRemoteControl::watchpointCommandValidatesAddress()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // A valid address is accepted; the arming itself is best-effort with no
    // session, so this pins the validation, not the emulator.
    QCOMPARE(roundTrip(s.client, "watchpoint $12345"), QStringLiteral("ok"));
    QCOMPARE(roundTrip(s.client, "watchpoint 4ba.l"), QStringLiteral("ok"));

    // Bad input is an error, not a silent no-op.
    QVERIFY(roundTrip(s.client, "watchpoint notanaddress").startsWith(QStringLiteral("error")));
    QVERIFY(roundTrip(s.client, "watchpoint 0").startsWith(QStringLiteral("error")));
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

void TstRemoteControl::cmdDisconnectDuringWaitSurvives()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // `cmd` with no emulator session never receives commandFinished, so the
    // server's nested wait runs to its full timeout. A client that gives up
    // and disconnects inside that window deletes the server-side socket (the
    // disconnected -> deleteLater in onNewConnection runs inside the nested
    // loop); both the reply after the loop and the read-line loop in
    // onReadyRead must survive that deletion.
    //
    // The abort has to fire *inside* the server's nested loop, the way a real
    // peer's disconnect arrives through the socket notifier: scheduling it as
    // a timer makes it detonate while the nested loop is pumping events (a
    // plain abort() after write() would run only after the nested loop has
    // already finished and blocked this thread for the full timeout).
    QTimer::singleShot(300, &s.client, [&s] { s.client.abort(); });
    s.client.write("cmd info mfp\n");

    // Outlive the server's 10 s command timeout: the reply is written here,
    // to a socket that was destroyed inside the nested loop.
    QTest::qWait(10600);

    // Observable recovery: the server must still serve a fresh client.
    QTcpSocket next;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&next, &QTcpSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    next.connectToHost(QHostAddress::LocalHost, s.control.boundPort());
    timer.start(5000);
    loop.exec();
    QVERIFY(next.state() == QAbstractSocket::ConnectedState);
    QVERIFY(roundTripBlock(next, "help").endsWith(QLatin1String("\n.\n")));
}


void TstRemoteControl::runWithoutSourceFailsFast()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // No source is open, so run() is refused and buildCompleted(false) must
    // end the wait immediately instead of the client sitting out the
    // operation timeout. The refusal's modal dialog is dismissed whenever it
    // appears: command execution is queued, so a single-shot timer can fire
    // before the dialog exists and would then miss it entirely.
    auto *dismiss = new QTimer(&s.control);
    dismiss->setInterval(50);
    QObject::connect(dismiss, &QTimer::timeout, [&s, dismiss] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            box->accept();
            dismiss->stop();
        }
    });
    dismiss->start();

    const QString reply = roundTrip(s.client, "run");
    QVERIFY2(reply.startsWith(QStringLiteral("error")),
             qPrintable(QStringLiteral("run replied: %1\nconsole: %2")
                            .arg(reply, s.window.debugConsoleText().right(400))));
}
QTEST_MAIN(TstRemoteControl)
#include "tst_remotecontrol.moc"
