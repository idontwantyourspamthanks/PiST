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

#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QSettings>
#include <QDir>
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
    void secondBlockingCommandIsRefusedWhileOneWaits();
    void readWithNoDocumentErrors();
    void readReturnsTheOpenDocument();
    void profileUsageErrorNamesTheSubverbs();
    void openFailureIsAnErrorNotASilentOk();
    void tabsListsTheOpenDocument();
    void saveWritesOrRefuses();
    void statejsonIsAJsonObject();
    void problemsIsAJsonArray();
    void symbolsAfterBuildListTheLabels();
    void breakpointByLabelResolvesAndToggles();
    void readmemAndDisasmRefuseWithoutASession();
    void wrongTokenIsRejectedAndDropped();
    void pipelinedCommandAfterBadAuthNeverExecutes();
    void commandBeforeAuthIsRejected();
    void unframedFloodIsDroppedAndTheServerKeepsServing();
    void profileStopIsBusyGatedAgainstOtherConnections();
    void initTestCase();
};

/// The remote-control server drives the window through its slots, and the
/// profile-stop path it serialises needs a window whose `profileStop` reports a
/// save under way and then holds the wait. A real one needs a live emulator
/// session (tst_gui drives that end to end); this stands in for the two things
/// that frame the wait — `profileStop`, which reports started, and the
/// `profileResultsReady` signal the test emits to release it — and nothing else,
/// so what is under test is the gate around the nested loop.
class ProfileWindow : public MainWindow
{
    Q_OBJECT

public:
    ProfileWindow() = default;

public slots:
    /// Report a save as started and do nothing: the parse that follows is the
    /// test's to trigger, exactly as a real save's parse is the IDE's.
    bool profileStop();
};

bool ProfileWindow::profileStop()
{
    return true;
}


void TstRemoteControl::initTestCase()
{
    // This suite builds a MainWindow, which reads and writes QSettings; the
    // redirect in main() is what keeps that off the developer's disk.
    QVERIFY2(QSettings().fileName().startsWith(QDir::tempPath()),
             qPrintable(QStringLiteral("QSettings resolves to %1, outside %2")
                            .arg(QSettings().fileName(), QDir::tempPath())));
}
namespace {

QString exchange(QTcpSocket &client, const QByteArray &command, bool block, int timeoutMs = 5000);
bool connectAndAuth(RemoteControl &control, QTcpSocket &client);

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
        return connectAndAuth(control, client);
    }
};

/// Connect `client` to `control` and present the session token, pumping a full
/// event loop rather than calling QTcpSocket::waitForConnected: that wait
/// watches only the client socket's notifier, so in a same-process test the
/// server's newConnection would never be delivered and every command would go
/// unanswered. In production the application runs a continuous event loop, so
/// this is purely a test-harness concern.
bool connectAndAuth(RemoteControl &control, QTcpSocket &client)
{
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&client, &QTcpSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    client.connectToHost(QHostAddress::LocalHost, control.boundPort());
    timer.start(5000);
    loop.exec();
    if (client.state() != QAbstractSocket::ConnectedState)
        return false;

    // Every connection opens with the session token.
    return exchange(client, "auth " + control.token().toUtf8(), false).trimmed()
           == QLatin1String("ok");
}

/// Send a command and read the reply, pumping a full event loop so the server
/// (same process) gets to run. Reads until a complete line for one-line replies,
/// or until the block terminator for block replies.
QString exchange(QTcpSocket &client, const QByteArray &command, bool block, int timeoutMs)
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
    timer.start(timeoutMs);
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

/// The body of a raw block reply, decoded the way a client does: drop the status
/// header line and the '.' terminator, and undo the dot-stuffing (a body line
/// that starts with '.' is sent with one extra leading '.').
QString blockBody(const QString &raw)
{
    QString text = raw;
    if (text.endsWith(QLatin1String("\n.\n")))
        text.chop(3);
    const int headerEnd = text.indexOf(QLatin1Char('\n'));
    if (headerEnd >= 0)
        text = text.mid(headerEnd + 1);
    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList unescaped;
    unescaped.reserve(lines.size());
    for (const QString &line : lines)
        unescaped.append(line.startsWith(QLatin1String("..")) ? line.mid(1) : line);
    return unescaped.join(QLatin1Char('\n'));
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
    // The block carries its status as an explicit first line, so a body line
    // beginning with "error" (or a lone '.') can never change the outcome.
    QVERIFY(reply.startsWith(QLatin1String("ok\n")));
    // NIT-8/MIN-12: `cmd` and `help` are commands themselves and were missing;
    // `read` returns JSON, not "path then its text".
    QVERIFY2(reply.contains(QStringLiteral("cmd <command>")), qPrintable(reply));
    QVERIFY2(reply.contains(QStringLiteral("help             this list")), qPrintable(reply));
    QVERIFY2(reply.contains(QStringLiteral("JSON {path, text}")), qPrintable(reply));
    QVERIFY2(!reply.contains(QStringLiteral("'path' then its text")), qPrintable(reply));
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
    QCOMPARE(exchange(next, "auth " + s.control.token().toUtf8(), false).trimmed(),
             QStringLiteral("ok"));
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

void TstRemoteControl::readWithNoDocumentErrors()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // The pristine editor has no file, so there is nothing to read back.
    QVERIFY(roundTrip(s.client, "read").startsWith(QStringLiteral("error")));
}

void TstRemoteControl::readReturnsTheOpenDocument()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("prog.s"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("\tmoveq\t#1,d0\n\trts\n");
    file.close();

    QCOMPARE(roundTrip(s.client, "open " + path.toUtf8()), QStringLiteral("ok"));
    QString reply = roundTripBlock(s.client, "read");
    reply = blockBody(reply);
    // JSON {path, text}: a lone '.' line in the source must not truncate the
    // read, which is why the document travels as JSON rather than raw text.
    const QJsonDocument doc = QJsonDocument::fromJson(reply.toUtf8());
    QCOMPARE(doc.object().value(QStringLiteral("path")).toString(), path);
    QVERIFY(doc.object().value(QStringLiteral("text")).toString()
                .contains(QStringLiteral("moveq\t#1,d0")));
}

void TstRemoteControl::statejsonIsAJsonObject()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));
    // No session: the JSON carries the flags and nothing else, and must parse.
    // roundTripBlock keeps the framing terminator, which is not JSON.
    QString reply = roundTripBlock(s.client, "statejson");
    reply = blockBody(reply);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply.toUtf8(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(reply));
    const QJsonObject object = doc.object();
    QCOMPARE(object.value(QStringLiteral("running")).toBool(), false);
    QCOMPARE(object.value(QStringLiteral("stopped")).toBool(), false);
}

void TstRemoteControl::problemsIsAJsonArray()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    QString reply = roundTripBlock(s.client, "problems");
    reply = blockBody(reply);
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(reply.toUtf8(), &parseError);
    QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(reply));
    QVERIFY(doc.isArray());
}

void TstRemoteControl::profileUsageErrorNamesTheSubverbs()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    const QString reply = roundTrip(s.client, "profile sideways");
    QVERIFY(reply.startsWith(QStringLiteral("error")));
    QVERIFY(reply.contains(QStringLiteral("start|stop|results")));
}

void TstRemoteControl::openFailureIsAnErrorNotASilentOk()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // A path that cannot be opened is an error, answered promptly — the
    // interactive open path shows a modal on failure, which offscreen would
    // hang this reply forever (and did, before openPathQuiet existed).
    const QString reply = exchange(s.client, "open /nonexistent/nothing.s", false);
    QVERIFY2(reply.startsWith(QStringLiteral("error")),
             qPrintable(QStringLiteral("reply: %1").arg(reply)));
}

void TstRemoteControl::tabsListsTheOpenDocument()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("prog.s"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("\trts\n");
    file.close();
    QCOMPARE(roundTrip(s.client, "open " + path.toUtf8()), QStringLiteral("ok"));

    QString reply = roundTripBlock(s.client, "tabs");
    reply = blockBody(reply);
    const QJsonDocument doc = QJsonDocument::fromJson(reply.toUtf8());
    QVERIFY(doc.isArray());
    bool found = false;
    for (const QJsonValue &entry : doc.array()) {
        const QJsonObject tab = entry.toObject();
        if (tab.value(QStringLiteral("path")).toString() == path) {
            found = true;
            QCOMPARE(tab.value(QStringLiteral("current")).toBool(), true);
        }
    }
    QVERIFY(found);
}

void TstRemoteControl::saveWritesOrRefuses()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // The pristine tab has no path; a name can't be chosen remotely.
    QVERIFY(roundTrip(s.client, "save").startsWith(QStringLiteral("error")));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("prog.s"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("\trts\n");
    file.close();
    QCOMPARE(roundTrip(s.client, "open " + path.toUtf8()), QStringLiteral("ok"));
    QCOMPARE(roundTrip(s.client, "save"), QStringLiteral("ok"));
}

void TstRemoteControl::symbolsAfterBuildListTheLabels()
{
    if (QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot")).isEmpty())
        QSKIP("needs vasmm68k_mot");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.path() + QStringLiteral("/prog.s");
    {
        QFile f(source);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("_start:\n\trts\ncount:\n\tmoveq #1,d0\n");
    }

    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));
    QCOMPARE(roundTrip(s.client, "open " + source.toUtf8()), QStringLiteral("ok"));
    QCOMPARE(roundTrip(s.client, "build"), QStringLiteral("ok"));

    QString reply = roundTripBlock(s.client, "symbols");
    reply = blockBody(reply);
    const QJsonDocument doc = QJsonDocument::fromJson(reply.toUtf8());
    QVERIFY(doc.isArray());
    QStringList names;
    for (const QJsonValue &entry : doc.array())
        names.append(entry.toObject().value(QStringLiteral("name")).toString());
    QVERIFY(names.contains(QStringLiteral("count")));

    // The filter narrows by name, case-insensitively.
    reply = roundTripBlock(s.client, "symbols COU");
    reply = blockBody(reply);
    const QJsonDocument filtered = QJsonDocument::fromJson(reply.toUtf8());
    QCOMPARE(filtered.array().size(), 1);
}

void TstRemoteControl::breakpointByLabelResolvesAndToggles()
{
    if (QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot")).isEmpty())
        QSKIP("needs vasmm68k_mot");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.path() + QStringLiteral("/prog.s");
    {
        QFile f(source);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("_start:\n\trts\ncount:\n\tmoveq #1,d0\n");
    }

    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));
    QCOMPARE(roundTrip(s.client, "open " + source.toUtf8()), QStringLiteral("ok"));
    QCOMPARE(roundTrip(s.client, "build"), QStringLiteral("ok"));

    // The reply names where the breakpoint landed: `count` is defined on its
    // own line (3), which emitted no code, so resolution lands on the moveq
    // (4) — a breakpoint on the label's own line could never fire.
    const QString reply = roundTrip(s.client, "breakpoint count");
    QVERIFY2(reply.startsWith(QStringLiteral("ok prog.s:4")),
             qPrintable(QStringLiteral("reply: %1").arg(reply)));

    // An unknown name is an error, not a silent no-op.
    QVERIFY(roundTrip(s.client, "breakpoint nosuch").startsWith(QStringLiteral("error")));
}

void TstRemoteControl::readmemAndDisasmRefuseWithoutASession()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // Usage is validated before any debugger round trip. These errors are
    // block-framed like the verbs themselves, with an explicit `error` status
    // line; reading them line-wise would leave the terminator queued and
    // desync the next exchange.
    QVERIFY(roundTripBlock(s.client, "readmem $100").startsWith(QLatin1String("error\n")));
    QVERIFY(blockBody(roundTripBlock(s.client, "readmem $100"))
                .startsWith(QStringLiteral("error usage")));
    QVERIFY(roundTripBlock(s.client, "disasm bogus").startsWith(QLatin1String("error\n")));
    QVERIFY(blockBody(roundTripBlock(s.client, "disasm bogus"))
                .startsWith(QStringLiteral("error usage")));

    // With no emulator session the debugger never answers, so the verb's
    // nested wait runs its full 10 s timeout before the refusal — the point
    // is that the client IS answered then, not left hanging.
    QByteArray received;
    QEventLoop loop;
    QObject::connect(&s.client, &QTcpSocket::readyRead, &loop, [&] {
        received += s.client.readAll();
        if (received.endsWith("\n.\n")) // the refusal is block-framed too
            loop.quit();
    });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    s.client.write("readmem $100 16\n");
    timer.start(15000);
    loop.exec();
    const QString reply = QString::fromUtf8(received);
    QVERIFY2(reply.startsWith(QStringLiteral("error")),
             qPrintable(QStringLiteral("reply: %1").arg(reply)));
}

void TstRemoteControl::wrongTokenIsRejectedAndDropped()
{
    Session s;
    QString error;
    // listen() itself still succeeds; the gate is per-connection.
    if (!s.control.listen(0, &error))
        QFAIL(qPrintable(error));

    QTcpSocket intruder;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&intruder, &QTcpSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    intruder.connectToHost(QHostAddress::LocalHost, s.control.boundPort());
    timer.start(5000);
    loop.exec();
    QVERIFY(intruder.state() == QAbstractSocket::ConnectedState);

    const QString reply = exchange(intruder, "auth not-the-token", false);
    QCOMPARE(reply.trimmed(), QStringLiteral("error auth required"));
    // And the connection is dropped: the socket does not stay usable.
    QTRY_VERIFY(intruder.state() != QAbstractSocket::ConnectedState);
}

void TstRemoteControl::pipelinedCommandAfterBadAuthNeverExecutes()
{
    Session s;
    QString error;
    if (!s.control.listen(0, &error))
        QFAIL(qPrintable(error));

    QTcpSocket intruder;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&intruder, &QTcpSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    intruder.connectToHost(QHostAddress::LocalHost, s.control.boundPort());
    timer.start(5000);
    loop.exec();
    QVERIFY(intruder.state() == QAbstractSocket::ConnectedState);

    // One burst: the bad auth and a pipelined `help`. The disconnect is
    // asynchronous, and the pipelined line must never execute — the reply
    // stream holds the auth error and nothing else. (`help` answers without
    // a session, which is what makes its absence meaningful: a `state` check
    // proves nothing with no session to report.)
    intruder.write("auth not-the-token\nhelp\n");
    QTest::qWait(1000);
    QString received = QString::fromUtf8(intruder.readAll());
    QVERIFY(received.contains(QLatin1String("error auth required")));
    QVERIFY(!received.contains(QLatin1String("open <path>")));

    // The shutdown window: a line sent *after* the refusal must hit the gate
    // again, not execute — the client stays pending until it is gone.
    received += exchange(intruder, "help", false);
    QVERIFY(!received.contains(QLatin1String("open <path>")));
    QTRY_VERIFY(intruder.state() != QAbstractSocket::ConnectedState);
}

void TstRemoteControl::commandBeforeAuthIsRejected()
{
    Session s;
    QString error;
    if (!s.control.listen(0, &error))
        QFAIL(qPrintable(error));

    QTcpSocket intruder;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&intruder, &QTcpSocket::connected, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    intruder.connectToHost(QHostAddress::LocalHost, s.control.boundPort());
    timer.start(5000);
    loop.exec();
    QVERIFY(intruder.state() == QAbstractSocket::ConnectedState);

    // A command as the very first line gets the same refusal as a bad token.
    const QString reply = exchange(intruder, "state", false);
    QCOMPARE(reply.trimmed(), QStringLiteral("error auth required"));
}

void TstRemoteControl::unframedFloodIsDroppedAndTheServerKeepsServing()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // A peer that connects and streams bytes containing no newline has said
    // nothing yet, so it is still *unauthenticated* while it does it — the auth
    // gate needs a complete first line, and a line that never ends never gets
    // that far. Before the cap, every byte it streamed accumulated in
    // QTcpSocket's buffer with no limit at all, so on a shared machine any
    // local user could grow the IDE's memory until the process died.
    QTcpSocket flooder;
    {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&flooder, &QTcpSocket::connected, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        flooder.connectToHost(QHostAddress::LocalHost, s.control.boundPort());
        timer.start(5000);
        loop.exec();
    }
    QVERIFY(flooder.state() == QAbstractSocket::ConnectedState);

    // Well past the 64 KiB cap, with no newline anywhere in it.
    flooder.write(QByteArray(1024 * 1024, 'x'));

    // Dropped promptly, rather than left holding the buffer waiting for a line
    // that is never coming.
    QTRY_VERIFY_WITH_TIMEOUT(flooder.state() != QAbstractSocket::ConnectedState, 5000);

    // The drop is per-connection: the server is unharmed and still serves its
    // authenticated client.
    QCOMPARE(roundTrip(s.client, "watchpoint $12345"), QStringLiteral("ok"));

    // And the cap constrains unterminated data, not command size: a long but
    // properly terminated line still frames and is answered.
    const QByteArray longButTerminated = QByteArray("symbols ") + QByteArray(8 * 1024, 'a');
    QVERIFY(roundTripBlock(s.client, longButTerminated).endsWith(QLatin1String("\n.\n")));
}

void TstRemoteControl::secondBlockingCommandIsRefusedWhileOneWaits()
{
    Session s;
    QString error;
    QVERIFY2(s.start(&error), qPrintable(error));

    // A second client on the same server.
    QTcpSocket other;
    {
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&other, &QTcpSocket::connected, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        other.connectToHost(QHostAddress::LocalHost, s.control.boundPort());
        timer.start(5000);
        loop.exec();
    }
    QVERIFY(other.state() == QAbstractSocket::ConnectedState);
    QCOMPARE(exchange(other, "auth " + s.control.token().toUtf8(), false).trimmed(),
             QStringLiteral("ok"));

    // The second command has to be written while the first one's wait is
    // already running, so it goes out from a timer that fires inside that
    // nested loop — the test thread is blocked in it until the first command
    // times out. With no session, neither `cmd` ever gets a response.
    QTimer::singleShot(300, &other, [&other] { other.write("cmd r\n"); });
    s.client.write("cmd info mfp\n");
    QTest::qWait(10600);

    // The second client is refused rather than waiting on the same signals as
    // the first and being answered with its result.
    const QString raw = QString::fromUtf8(other.readAll());
    const QString reply = blockBody(raw);
    QVERIFY2(reply.startsWith(QStringLiteral("error busy")),
             qPrintable(QStringLiteral("second client got: %1").arg(raw)));
}
void TstRemoteControl::profileStopIsBusyGatedAgainstOtherConnections()
{
    ProfileWindow window;
    RemoteControl control(&window);
    QString error;
    QVERIFY2(control.listen(0, &error), qPrintable(error));

    QTcpSocket first;
    QVERIFY(connectAndAuth(control, first));

    // A second, authenticated client waiting to get a word in.
    QTcpSocket second;
    QVERIFY(connectAndAuth(control, second));

    // `profile stop` blocks on a nested loop until the save has been parsed, and
    // that loop services every connection: a second client's blocking verb must
    // therefore be refused, not run inside it (which stacks loops, and lets a
    // `run` restart the emulator session while the save is still being parsed),
    // and the window's single-slot debug-command routing depends on it. `cmd` is
    // used rather than `build` because a build with no source open raises a
    // modal dialog.
    //
    // The profile wait is released only once the second client has been
    // answered, which is the order the gate guarantees: while `profile stop` is
    // waiting, nothing else runs, so the refusal is what ends the wait. The
    // long-stop release exists so a missing refusal fails this test rather than
    // hanging it (and pre-fix, when the second `cmd` runs inside the wait, the
    // wait is held past the client's own timeout and no refusal ever arrives).
    QByteArray secondReply;
    QObject::connect(&second, &QTcpSocket::readyRead, &second, [&] {
        secondReply += second.readAll();
        if (!secondReply.isEmpty())
            window.profileResultsReady(true);
    });
    QTimer::singleShot(8000, &window, [&window] { window.profileResultsReady(true); });
    QTimer::singleShot(300, &second, [&second] { second.write("cmd r\n"); });

    // Generous, because pre-fix this reply does not come until the second
    // client's `cmd` has finished running inside the wait (its own 10 s
    // timeout): the assertion that then fails is the one below, naming what the
    // second client actually got.
    QCOMPARE(exchange(first, "profile stop", false, 20000).trimmed(), QStringLiteral("ok"));
    QVERIFY2(blockBody(QString::fromUtf8(secondReply)).startsWith("error busy:"),
             qPrintable(QStringLiteral("second client got: %1")
                            .arg(QString::fromUtf8(secondReply))));

    // The single-client path is unchanged: a lone `profile stop` still waits for
    // the parse and answers "ok".
    QTimer::singleShot(200, &window, [&window] { window.profileResultsReady(true); });
    QCOMPARE(roundTrip(first, "profile stop"), QStringLiteral("ok"));
}

// See tst_gui's main() for why both QSettings calls are needed and why
// QStandardPaths::setTestModeEnabled is not used here.
int main(int argc, char *argv[])
{
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settings;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    // Invariant 7: a GUI suite must skip rather than qFatal when there is no
    // display and no platform was chosen (CI sets QT_QPA_PLATFORM; a bare
    // container does not) — MAJ-50.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")
        && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    TstRemoteControl testCase;
    return QTest::qExec(&testCase, argc, argv);
}
#include "tst_remotecontrol.moc"
