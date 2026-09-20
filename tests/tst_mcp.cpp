// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Tests for the MCP shim (src/control/mcp/): the JSON-RPC server an agent
// talks to. McpServer is driven in-process through handleLine/sendLine; tool
// calls go over real loopback TCP to a scripted fake IDE, because the
// line-vs-block reply table and the watch-subscription failure paths are
// exactly where a regression would silently hang or mis-frame an agent.

#include "control/mcp/McpServer.h"

#include <algorithm>
#include <memory>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace pist::mcp;

namespace {

/// A minimal stand-in for PiST's remote-control socket: accepts one
/// connection, exposes complete received lines, and writes raw replies.
class FakeIde : public QObject
{
public:
    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            m_client = m_server.nextPendingConnection();
            connect(m_client, &QTcpSocket::readyRead, this, [this] {
                m_buffer += m_client->readAll();
                int nl = 0;
                while ((nl = m_buffer.indexOf('\n')) >= 0) {
                    m_lines.append(QString::fromUtf8(m_buffer.left(nl)));
                    m_buffer.remove(0, nl + 1);
                }
            });
        });
        return true;
    }

    quint16 port() const { return m_server.serverPort(); }

    /// The next complete command line, waiting for one to arrive. Empty on
    /// timeout — the caller's QCOMPARE then fails with a readable diff.
    QString nextLine(int timeoutMs = 5000)
    {
        if (!QTest::qWaitFor([this] { return !m_lines.isEmpty(); }, timeoutMs))
            return QString();
        return m_lines.takeFirst();
    }

    void send(const QByteArray &raw)
    {
        QVERIFY(m_client);
        m_client->write(raw);
        m_client->flush();
    }

private:
    QTcpServer m_server;
    QTcpSocket *m_client = nullptr;
    QByteArray m_buffer;
    QStringList m_lines;
};

} // namespace

class TstMcp : public QObject
{
    Q_OBJECT

    /// Everything the server has emitted, oldest first.
    QList<QJsonObject> m_out;

    std::unique_ptr<McpServer> makeServer(quint16 port)
    {
        auto server = std::make_unique<McpServer>(QStringLiteral("127.0.0.1"), port);
        connect(server.get(), &McpServer::sendLine, this, [this](const QByteArray &line) {
            m_out.append(QJsonDocument::fromJson(line).object());
        });
        return server;
    }

    void send(McpServer *server, int id, const QString &method, const QJsonObject &params = {})
    {
        QJsonObject message;
        message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        message.insert(QStringLiteral("id"), id);
        message.insert(QStringLiteral("method"), method);
        if (!params.isEmpty())
            message.insert(QStringLiteral("params"), params);
        server->handleLine(QJsonDocument(message).toJson(QJsonDocument::Compact));
    }

    /// The reply carrying `id`, waiting for it to appear. A null object on
    /// timeout, which fails the caller's assertions.
    QJsonObject waitReply(int id, int timeoutMs = 10000)
    {
        const auto hasReply = [this, id] {
            return std::any_of(m_out.begin(), m_out.end(),
                               [id](const QJsonObject &m) {
                                   return m.value(QStringLiteral("id")).toInt(-1) == id;
                               });
        };
        if (!QTest::qWaitFor(hasReply, timeoutMs))
            return QJsonObject();
        for (int i = 0; i < m_out.size(); ++i) {
            if (m_out[i].value(QStringLiteral("id")).toInt(-1) == id) {
                const QJsonObject found = m_out[i];
                m_out.removeAt(i);
                return found;
            }
        }
        return QJsonObject();
    }
    static QString resultText(const QJsonObject &reply)
    {
        const QJsonArray content = reply.value(QStringLiteral("result"))
                                       .toObject()
                                       .value(QStringLiteral("content"))
                                       .toArray();
        return content.isEmpty()
                   ? QString()
                   : content.first().toObject().value(QStringLiteral("text")).toString();
    }

    static bool resultIsError(const QJsonObject &reply)
    {
        return reply.value(QStringLiteral("result")).toObject()
            .value(QStringLiteral("isError"))
            .toBool();
    }

    static QJsonObject callParams(const QString &tool, const QJsonObject &args = {})
    {
        QJsonObject params;
        params.insert(QStringLiteral("name"), tool);
        params.insert(QStringLiteral("arguments"), args);
        return params;
    }

private slots:
    void initializeReportsTheBuildVersion()
    {
        auto server = makeServer(0);
        QJsonObject params;
        params.insert(QStringLiteral("protocolVersion"), QStringLiteral("2025-06-18"));
        send(server.get(), 1, QStringLiteral("initialize"), params);

        const QJsonObject reply = waitReply(1);
        const QJsonObject info = reply.value(QStringLiteral("result"))
                                     .toObject()
                                     .value(QStringLiteral("serverInfo"))
                                     .toObject();
        // A literal here already shipped one release stale (0.6.2 introducing
        // itself as 0.6.1); the version must come from the build.
        QCOMPARE(info.value(QStringLiteral("version")).toString(),
                 QStringLiteral(PIST_VERSION));
        QVERIFY(!reply.value(QStringLiteral("result"))
                      .toObject()
                      .value(QStringLiteral("protocolVersion"))
                      .toString()
                      .isEmpty());
    }

    void toolsListCoversTheCatalog()
    {
        auto server = makeServer(0);
        send(server.get(), 1, QStringLiteral("tools/list"));

        const QJsonObject reply = waitReply(1);
        const QJsonArray tools = reply.value(QStringLiteral("result"))
                                     .toObject()
                                     .value(QStringLiteral("tools"))
                                     .toArray();
        QStringList names;
        for (const QJsonValue &tool : tools)
            names.append(tool.toObject().value(QStringLiteral("name")).toString());
        names.sort();
        // Pin the whole set: a renamed or dropped tool is how an agent's
        // workflow breaks without anything here noticing.
        QCOMPARE(names, QStringList({"pist_breakpoint", "pist_breakpoints", "pist_build",
                                     "pist_cmd", "pist_console", "pist_continue",
                                     "pist_open", "pist_problems", "pist_profile_results",
                                     "pist_profile_start", "pist_profile_stop",
                                     "pist_read", "pist_run", "pist_screenshot",
                                     "pist_setmem", "pist_setreg", "pist_state",
                                     "pist_step", "pist_stepover", "pist_stop",
                                     "pist_watch", "pist_watchpoint"}));
    }

    void lineAndBlockRepliesAreFramedCorrectly()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        // A line reply: `build` answers with a bare `ok`.
        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_build")));
        QCOMPARE(ide.nextLine(), QStringLiteral("build"));
        ide.send("ok\n");
        QJsonObject reply = waitReply(1);
        QCOMPARE(resultText(reply), QStringLiteral("ok"));
        QVERIFY(!resultIsError(reply));

        // pist_state goes to statejson and comes back in both shapes: parsed
        // structuredContent and the same JSON pretty-printed as the text block.
        send(server.get(), 2, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_state")));
        QCOMPARE(ide.nextLine(), QStringLiteral("statejson"));
        ide.send("{\"running\":false,\"stopped\":true,\"pc\":\"0x00012596\"}\n.\n");
        reply = waitReply(2);
        QVERIFY(resultText(reply).contains(QLatin1String("0x00012596")));
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("pc")).toString(),
                 QStringLiteral("0x00012596"));
        QVERIFY(!resultIsError(reply));
        // An IDE-side `error …` is a tool error (isError), not a protocol error.
        send(server.get(), 3, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_step")));
        QCOMPARE(ide.nextLine(), QStringLiteral("step"));
        ide.send("error not stopped\n");
        reply = waitReply(3);
        QCOMPARE(resultText(reply), QStringLiteral("error not stopped"));
        QVERIFY(resultIsError(reply));
    }

    void unknownToolAndMethodGetProtocolErrors()
    {
        auto server = makeServer(0);
        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_nope")));
        QJsonObject reply = waitReply(1);
        QCOMPARE(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("code"))
                     .toInt(),
                 -32602);

        send(server.get(), 2, QStringLiteral("bogus/method"));
        reply = waitReply(2);
        QCOMPARE(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("code"))
                     .toInt(),
                 -32601);
    }

    void watchFlowDeliversEventsAsNotifications()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        QCOMPARE(ide.nextLine(), QStringLiteral("watch"));
        ide.send("ok\n");
        QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));

        // An event pushed on the subscription arrives as an MCP log
        // notification, so an agent waiting on a stop is woken.
        ide.send("event stopped pc=0x00012396\n");
        QJsonObject notification;
        QTRY_VERIFY_WITH_TIMEOUT(
            ([this, &notification] {
                 for (const QJsonObject &m : m_out) {
                     if (m.value(QStringLiteral("method")).toString()
                         == QLatin1String("notifications/message")) {
                         notification = m;
                         return true;
                     }
                 }
                 return false;
             }()),
            5000);
        const QJsonObject data = notification.value(QStringLiteral("params"))
                                     .toObject()
                                     .value(QStringLiteral("data"))
                                     .toObject();
        QCOMPARE(data.value(QStringLiteral("event")).toString(), QStringLiteral("stopped"));
        QCOMPARE(data.value(QStringLiteral("detail")).toString(),
                 QStringLiteral("pc=0x00012396"));

        // And a repeated pist_watch drains what has accumulated.
        send(server.get(), 2, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        reply = waitReply(2);
        QVERIFY(resultText(reply).contains(QLatin1String("event stopped pc=0x00012396")));
        QVERIFY(!resultIsError(reply));
    }

    void watchAgainstAnUnreachableIdeAnswersInsteadOfHanging()
    {
        // A port nothing listens on: connect fails, and the pending watch must
        // be answered with the failure — it used to hang, because the
        // subscription is neither in the request queue nor in flight.
        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 deadPort = probe.serverPort();
        probe.close();

        auto server = makeServer(deadPort);
        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        const QJsonObject reply = waitReply(1, 15000);
        QVERIFY(resultIsError(reply));
        QVERIFY(resultText(reply).startsWith(QLatin1String("watch failed:")));
    }

    void noEndpointFailsTheCallWithTheRemedy()
    {
        auto server = makeServer(0); // port 0: not configured
        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_build")));
        const QJsonObject reply = waitReply(1);
        QVERIFY(resultIsError(reply));
        QVERIFY(resultText(reply).contains(QLatin1String("not configured")));
    }
};

QTEST_GUILESS_MAIN(TstMcp)
#include "tst_mcp.moc"
