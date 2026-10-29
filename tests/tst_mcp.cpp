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

#include "control/mcp/ControlClient.h"

#include <algorithm>
#include <memory>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist::mcp;

namespace {

/// A minimal stand-in for PiST's remote-control socket: accepts connections
/// (the shim holds one for requests and one for events), exposes complete
/// received lines, and writes replies to whoever asked last.
class FakeIde : public QObject
{
public:
    /// Refuse any connection whose first line is not `auth <token>`, exactly as
    /// a listening IDE does (RemoteControl::onReadyRead). Empty — the default —
    /// means no token is checked, which is what the tests that predate tokens
    /// rely on.
    void setToken(const QString &token) { m_token = token; }

    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *client = m_server.nextPendingConnection();
            m_accepted.append(client);
            connect(client, &QTcpSocket::readyRead, this, [this, client] {
                const QByteArray chunk = client->readAll();
                m_wire += chunk;
                QByteArray &buffer = m_buffers[client];
                buffer += chunk;
                int nl = 0;
                while ((nl = buffer.indexOf('\n')) >= 0) {
                    const QString line = QString::fromUtf8(buffer.left(nl));
                    buffer.remove(0, nl + 1);
                    if (!m_token.isEmpty() && !m_authed.contains(client)) {
                        m_lines.append({client, line});
                        if (line == QLatin1String("auth ") + m_token) {
                            m_authed.insert(client);
                            sendTo(client, "ok\n");
                        } else {
                            sendTo(client, "error auth required\n");
                            client->disconnectFromHost();
                            // The real IDE stops reading here: the connection is
                            // going away and a line behind the refusal is not
                            // executed (pipelinedCommandAfterBadAuth).
                            break;
                        }
                        continue;
                    }
                    m_lines.append({client, line});
                }
            });
        });
        return true;
    }

    /// Shut the fake down the way an exiting IDE does: stop listening and close
    /// every accepted connection, so a shim holding one sees the disconnect.
    void stop()
    {
        m_server.close();
        for (QTcpSocket *client : std::as_const(m_accepted))
            client->disconnectFromHost();
    }

    quint16 port() const { return m_server.serverPort(); }

    /// Everything the shim has written, whatever it framed it as: an assertion
    /// about what reached the IDE rather than about how it was split.
    QByteArray wire() const { return m_wire; }

    /// The next complete command line from any connection, waiting for one.
    /// Empty on timeout — the caller's QCOMPARE then fails with a readable diff.
    QString nextLine(int timeoutMs = 5000)
    {
        if (!QTest::qWaitFor([this] { return !m_lines.isEmpty(); }, timeoutMs))
            return QString();
        const auto entry = m_lines.takeFirst();
        m_lastFrom = entry.first;
        return entry.second;
    }

    void send(const QByteArray &raw)
    {
        // Replies belong to whoever asked last: on the wrong socket the shim
        // would hang waiting for an answer that went to the other connection.
        QVERIFY(m_lastFrom);
        sendTo(m_lastFrom, raw);
    }

private:
    void sendTo(QTcpSocket *client, const QByteArray &raw)
    {
        m_lastFrom = client;
        client->write(raw);
        client->flush();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QByteArray m_wire;
    QList<QPair<QTcpSocket *, QString>> m_lines;
    QSet<QTcpSocket *> m_authed;
    QList<QTcpSocket *> m_accepted;
    QTcpSocket *m_lastFrom = nullptr;
    QString m_token;
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

    /// Publish an address the way a listening IDE does (its discovery file):
    /// `host port token [protocol-version]` on one line (the shape
    /// RemoteControl::listen writes). The real one is written owner-only and to
    /// a well-known path; a test points the shim at its own file instead. The
    /// version field is optional the way it is on the wire: a line without one
    /// is an IDE older than the handshake.
    void writeDiscovery(const QString &path, quint16 port, const QString &token,
                        int protocolVersion = -1)
    {
        QFile file(path);
        QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
        QString line = QStringLiteral("127.0.0.1 %1 %2").arg(port).arg(token);
        if (protocolVersion >= 0)
            line += QStringLiteral(" %1").arg(protocolVersion);
        file.write((line + QLatin1Char('\n')).toUtf8());
        file.close();
    }

    /// The first `notifications/message` emitted (an event pushed to the
    /// agent), or a null object when none has arrived yet.
    QJsonObject firstEventNotification() const
    {
        for (const QJsonObject &m : m_out) {
            if (m.value(QStringLiteral("method")).toString()
                == QLatin1String("notifications/message"))
                return m;
        }
        return QJsonObject();
    }

private slots:
    /// QtTest reuses one instance across methods (and only slots named init
    /// are invoked between them); earlier replies must not be matched later.
    void init() { m_out.clear(); }

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
        QCOMPARE(names, QStringList({"pist_asset", "pist_assets", "pist_breakpoint",
                                     "pist_breakpoints", "pist_build", "pist_build_release",
                                     "pist_builddiff", "pist_capturedma", "pist_capturescreen",
                                     "pist_clearwatchpoint", "pist_cmd", "pist_console",
                                     "pist_continue", "pist_depack", "pist_disasm", "pist_dumpmem",
                                     "pist_hook", "pist_memcompare", "pist_memmark", "pist_open",
                                     "pist_problems", "pist_profile_results", "pist_profile_start",
                                     "pist_profile_stop", "pist_read", "pist_readmem", "pist_ripavr",
                                     "pist_ripimage", "pist_ripsample", "pist_run", "pist_save",
                                     "pist_scandisk", "pist_scanmagic", "pist_screenshot",
                                     "pist_setmem", "pist_setreg", "pist_sndhinfo", "pist_state",
                                     "pist_step", "pist_stepover", "pist_stop", "pist_symbols",
                                     "pist_tabs", "pist_watch", "pist_watchpoint"}));
    }

    void ripimageMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("in.bin"));
        args.insert(QStringLiteral("offset"), QStringLiteral("0"));
        args.insert(QStringLiteral("width"), QStringLiteral("16"));
        args.insert(QStringLiteral("height"), QStringLiteral("16"));
        args.insert(QStringLiteral("out"), QStringLiteral("out.pim"));
        args.insert(QStringLiteral("paletteOffset"), QStringLiteral("20"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_ripimage"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("ripimage in.bin 0 16 16 out.pim 20"));
        ide.send("ok\n.\n");
        const QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));
    }

    void dumpmemMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        QJsonObject args;
        args.insert(QStringLiteral("address"), QStringLiteral("$100"));
        args.insert(QStringLiteral("length"), QStringLiteral("16"));
        args.insert(QStringLiteral("file"), QStringLiteral("out.bin"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_dumpmem"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("dumpmem $100 16 out.bin"));
        ide.send("ok\n.\n");
        const QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));
    }

    void capturescreenMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("out.pim"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_capturescreen"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("capturescreen out.pim"));
        ide.send("ok\n.\n");
        const QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));
    }

    void capturedmaMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("out.wav"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_capturedma"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("capturedma out.wav"));
        ide.send("ok\n.\n");
        const QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));
    }

    void ripavrMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());
        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("in.avr"));
        args.insert(QStringLiteral("offset"), QStringLiteral("0"));
        args.insert(QStringLiteral("out"), QStringLiteral("out.wav"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_ripavr"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("ripavr in.avr 0 out.wav"));
        ide.send("ok\n.\n");
        QVERIFY(!resultIsError(waitReply(1)));
    }

    void scanmagicMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());
        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("in.bin"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_scanmagic"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("scanmagic in.bin"));
        ide.send("ok\n.\n");
        QVERIFY(!resultIsError(waitReply(1)));
    }

    void depackMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());
        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("in.bin"));
        args.insert(QStringLiteral("offset"), QStringLiteral("0"));
        args.insert(QStringLiteral("out"), QStringLiteral("out.bin"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_depack"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("depack in.bin 0 out.bin"));
        ide.send("ok\n.\n");
        QVERIFY(!resultIsError(waitReply(1)));
    }

    void scandiskMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());
        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("disk.st"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_scandisk"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("scandisk disk.st"));
        ide.send("ok\n.\n");
        QVERIFY(!resultIsError(waitReply(1)));
    }

    void sndhinfoMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());
        QJsonObject args;
        args.insert(QStringLiteral("file"), QStringLiteral("tune.sndh"));
        args.insert(QStringLiteral("offset"), QStringLiteral("0"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_sndhinfo"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("sndhinfo tune.sndh 0"));
        ide.send("ok\n.\n");
        QVERIFY(!resultIsError(waitReply(1)));
    }

    void buildReleaseMapsToTheVerb()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        // The release tool must drive the release verb, not the debug one — the
        // whole point is producing the shipping PRG.
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build_release")));
        QCOMPARE(ide.nextLine(), QStringLiteral("buildrelease"));
        ide.send("ok\n");
        const QJsonObject reply = waitReply(1);
        QCOMPARE(resultText(reply), QStringLiteral("ok"));
        QVERIFY(!resultIsError(reply));
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
        ide.send("ok\n{\"running\":false,\"stopped\":true,\"pc\":\"0x00012596\"}\n.\n");
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

    void annotationsResourcesPromptsAndChains()
    {
        // Annotations come free with the catalog — no IDE needed for this part.
        {
            auto server = makeServer(0);
            send(server.get(), 1, QStringLiteral("tools/list"));
            const QJsonObject reply = waitReply(1);
            const QJsonArray tools = reply.value(QStringLiteral("result"))
                                         .toObject()
                                         .value(QStringLiteral("tools")).toArray();
            for (const QJsonValue &value : tools) {
                const QJsonObject tool = value.toObject();
                const QString name = tool.value(QStringLiteral("name")).toString();
                const QJsonObject annotations = tool.value(QStringLiteral("annotations")).toObject();
                if (name == QLatin1String("pist_state") || name == QLatin1String("pist_builddiff")
                    || name == QLatin1String("pist_assets"))
                    QCOMPARE(annotations.value(QStringLiteral("readOnlyHint")).toBool(), true);
                if (name == QLatin1String("pist_setmem"))
                    QCOMPARE(annotations.value(QStringLiteral("destructiveHint")).toBool(), true);
                // Clearing watchpoints rewrites live debugger state, so it
                // carries the same warning as the other mutations.
                if (name == QLatin1String("pist_clearwatchpoint"))
                    QCOMPARE(annotations.value(QStringLiteral("destructiveHint")).toBool(), true);
            }
        }

        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        // resources/list names the three resources; a read maps to its verb
        // and wraps the body as contents.
        send(server.get(), 1, QStringLiteral("resources/list"));
        QJsonObject reply = waitReply(1);
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("resources")).toArray().size(), 3);

        send(server.get(), 2, QStringLiteral("resources/read"),
             QJsonObject{{QStringLiteral("uri"), QStringLiteral("pist://state")}});
        QCOMPARE(ide.nextLine(), QStringLiteral("statejson"));
        ide.send("ok\n{\"running\":false,\"stopped\":true}\n.\n");
        reply = waitReply(2);
        const QJsonArray contents = reply.value(QStringLiteral("result")).toObject()
                                        .value(QStringLiteral("contents")).toArray();
        QCOMPARE(contents.size(), 1);
        QVERIFY(contents.first().toObject().value(QStringLiteral("text")).toString()
                    .contains(QLatin1String("stopped")));

        // Subscribe, then an event pushes notifications/resources/updated.
        send(server.get(), 3, QStringLiteral("resources/subscribe"),
             QJsonObject{{QStringLiteral("uri"), QStringLiteral("pist://state")}});
        reply = waitReply(3);
        QVERIFY(!reply.contains(QStringLiteral("error")));
        send(server.get(), 4, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_watch")));
        QCOMPARE(ide.nextLine(), QStringLiteral("watch"));
        ide.send("ok\n");
        reply = waitReply(4);
        ide.send("event stopped pc=0x00012596\n");
        QJsonObject updated;
        QTRY_VERIFY_WITH_TIMEOUT(
            ([this, &updated] {
                 for (const QJsonObject &m : m_out) {
                     if (m.value(QStringLiteral("method")).toString()
                         == QLatin1String("notifications/resources/updated")) {
                         updated = m;
                         return true;
                     }
                 }
                 return false;
             }()),
            5000);
        QCOMPARE(updated.value(QStringLiteral("params")).toObject()
                     .value(QStringLiteral("uri")).toString(),
                 QStringLiteral("pist://state"));

        // Prompts: the catalog and one expansion.
        send(server.get(), 5, QStringLiteral("prompts/list"));
        reply = waitReply(5);
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("prompts")).toArray().size(), 2);
        send(server.get(), 6, QStringLiteral("prompts/get"),
             QJsonObject{{QStringLiteral("name"), QStringLiteral("diagnose-build")}});
        reply = waitReply(6);
        QVERIFY(reply.value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("messages")).toArray().first().toObject()
                    .value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("text")).toString()
                    .contains(QLatin1String("pist_problems")));

        // tabs and save map to their verbs.
        send(server.get(), 7, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_tabs")));
        QCOMPARE(ide.nextLine(), QStringLiteral("tabs"));
        ide.send("ok\n[]\n.\n");
        reply = waitReply(7);
        QVERIFY(!resultIsError(reply));

        send(server.get(), 8, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_save")));
        QCOMPARE(ide.nextLine(), QStringLiteral("save"));
        ide.send("ok\n");
        reply = waitReply(8);
        QVERIFY(!resultIsError(reply));

        // A failed build chains the problems query into its error result.
        send(server.get(), 9, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build")));
        QCOMPARE(ide.nextLine(), QStringLiteral("build"));
        ide.send("error build failed\n");
        QCOMPARE(ide.nextLine(), QStringLiteral("problems"));
        ide.send("ok\n[{\"file\":\"a.s\",\"line\":3,\"message\":\"bad\",\"severity\":\"error\"}]\n.\n");
        reply = waitReply(9);
        QVERIFY(resultIsError(reply));
        const QJsonArray problems = reply.value(QStringLiteral("result")).toObject()
                                        .value(QStringLiteral("structuredContent")).toObject()
                                        .value(QStringLiteral("problems")).toArray();
        QCOMPARE(problems.size(), 1);
        QVERIFY(resultText(reply).contains(QLatin1String("error build failed")));
    }


    /// The stop hook an agent never sees for itself: `pist_hook` reports the
    /// script and what its last run did as fields, because parsing a translated
    /// sentence for a line number is not a contract anyone should rely on.
    void hookReportsTheScriptAndItsLastRun()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_hook")));
        QCOMPARE(ide.nextLine(), QStringLiteral("hook"));
        ide.send("ok\n{\"enabled\":true,\"path\":\"/p/demo/hello.hooks.lua\","
                 "\"autoContinue\":false,\"last\":{\"ran\":true,\"ok\":false,"
                 "\"timedOut\":false,\"pc\":\"0x00012596\",\"file\":\"hello.hooks.lua\","
                 "\"line\":4,\"logs\":0,\"writes\":2,"
                 "\"message\":\"attempt to index a nil value\"}}\n.\n");

        const QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));
        const QJsonObject hook = reply.value(QStringLiteral("result")).toObject()
                                     .value(QStringLiteral("structuredContent")).toObject();
        QCOMPARE(hook.value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(hook.value(QStringLiteral("path")).toString(),
                 QStringLiteral("/p/demo/hello.hooks.lua"));
        const QJsonObject last = hook.value(QStringLiteral("last")).toObject();
        QCOMPARE(last.value(QStringLiteral("ran")).toBool(), true);
        QCOMPARE(last.value(QStringLiteral("ok")).toBool(), false);
        QCOMPARE(last.value(QStringLiteral("line")).toInt(), 4);
        QCOMPARE(last.value(QStringLiteral("writes")).toInt(), 2);
        QCOMPARE(last.value(QStringLiteral("message")).toString(),
                 QStringLiteral("attempt to index a nil value"));
    }

    void newToolMappingsAndStructuredReplies()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        // symbols: a filtered query maps to the verb, and a JSON array answer
        // comes back wrapped in structuredContent.
        QJsonObject args;
        args.insert(QStringLiteral("filter"), QStringLiteral("cou"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_symbols"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("symbols cou"));
        ide.send("ok\n[{\"name\":\"count\",\"file\":\"hello.s\",\"line\":22}]\n.\n");
        QJsonObject reply = waitReply(1);
        const QJsonArray items = reply.value(QStringLiteral("result")).toObject()
                                     .value(QStringLiteral("structuredContent")).toObject()
                                     .value(QStringLiteral("items")).toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items.first().toObject().value(QStringLiteral("name")).toString(),
                 QStringLiteral("count"));

        // breakpoint by label goes to the label form of the verb.
        args = {};
        args.insert(QStringLiteral("label"), QStringLiteral("count"));
        send(server.get(), 2, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_breakpoint"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("breakpoint count"));
        ide.send("ok hello.s:22 = 0x000125a8\n");
        reply = waitReply(2);
        QVERIFY(!resultIsError(reply));

        // readmem and disasm map with their arguments in the debugger's shape.
        args = {};
        args.insert(QStringLiteral("address"), QStringLiteral("$12596"));
        args.insert(QStringLiteral("length"), QStringLiteral("16"));
        send(server.get(), 3, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_readmem"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("readmem $12596 16"));
        ide.send("ok\n[{\"address\":\"0x00012596\",\"bytes\":\"7000\"}]\n.\n");
        reply = waitReply(3);
        QVERIFY(resultText(reply).contains(QLatin1String("7000")));

        send(server.get(), 4, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_disasm")));
        QCOMPARE(ide.nextLine(), QStringLiteral("disasm"));
        ide.send("ok\n[{\"address\":\"0x00012596\",\"bytes\":\"7000\",\"text\":\"moveq #$00,d0\"}]\n.\n");
        reply = waitReply(4);
        QVERIFY(resultText(reply).contains(QLatin1String("moveq")));
        // Pre-existing tools: an edit-boundary slip once deleted these
        // mappings while the catalog (which pins only names) stayed green.
        send(server.get(), 5, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build")));
        QCOMPARE(ide.nextLine(), QStringLiteral("build"));
        ide.send("ok\n");
        reply = waitReply(5);
        QVERIFY(!resultIsError(reply));

        send(server.get(), 6, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_profile_stop")));
        QCOMPARE(ide.nextLine(), QStringLiteral("profile stop"));
        ide.send("ok\n");
        reply = waitReply(6);
        QVERIFY(!resultIsError(reply));

        send(server.get(), 7, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_stop")));
        QCOMPARE(ide.nextLine(), QStringLiteral("stop"));
        ide.send("ok\n");
        reply = waitReply(7);
        QVERIFY(!resultIsError(reply));
    }

    void compareAndAssetToolsMapTheirArguments()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        QJsonObject args;
        args.insert(QStringLiteral("address"), QStringLiteral("$1"));
        args.insert(QStringLiteral("length"), QStringLiteral("4"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_memmark"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("memmark $1 4"));
        ide.send("ok\n{\"base\":\"0x00000001\",\"length\":4}\n.\n");
        QJsonObject reply = waitReply(1);
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("base")).toString(),
                 QStringLiteral("0x00000001"));

        send(server.get(), 2, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_memcompare")));
        QCOMPARE(ide.nextLine(), QStringLiteral("memcompare"));
        ide.send("ok\n{\"differ\":0,\"advanced\":false}\n.\n");
        reply = waitReply(2);
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("advanced")).toBool(),
                 false);

        args = {};
        args.insert(QStringLiteral("advance"), true);
        send(server.get(), 3, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_memcompare"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("memcompare advance"));
        ide.send("ok\n{\"differ\":1,\"advanced\":true}\n.\n");
        reply = waitReply(3);
        QVERIFY(!resultIsError(reply));

        send(server.get(), 4, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_builddiff")));
        QCOMPARE(ide.nextLine(), QStringLiteral("builddiff"));
        ide.send("ok\n{\"ok\":true,\"written\":false}\n.\n");
        reply = waitReply(4);
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("written")).toBool(),
                 false);

        send(server.get(), 5, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_assets")));
        QCOMPARE(ide.nextLine(), QStringLiteral("assets"));
        ide.send("ok\n[{\"output\":\"hero.dat\"}]\n.\n");
        reply = waitReply(5);
        QCOMPARE(reply.value(QStringLiteral("result")).toObject()
                     .value(QStringLiteral("structuredContent")).toObject()
                     .value(QStringLiteral("items")).toArray().at(0).toObject()
                     .value(QStringLiteral("output")).toString(),
                 QStringLiteral("hero.dat"));

        args = {};
        args.insert(QStringLiteral("source"), QStringLiteral("hero.pim"));
        args.insert(QStringLiteral("phase"), QStringLiteral("walk"));
        args.insert(QStringLiteral("output"), QStringLiteral("hero.dat"));
        args.insert(QStringLiteral("masked"), true);
        args.insert(QStringLiteral("preShifts"), 8);
        args.insert(QStringLiteral("transparent"), -1);
        send(server.get(), 6, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_asset"), args));
        QCOMPARE(ide.nextLine(),
                 QStringLiteral("asset hero.pim walk hero.dat masked=1 preshifts=8 transparent=-1"));
        ide.send("ok\n");
        reply = waitReply(6);
        QCOMPARE(resultText(reply), QStringLiteral("ok"));

        args = {};
        args.insert(QStringLiteral("remove"), QStringLiteral("hero.dat"));
        send(server.get(), 7, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_asset"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("asset remove hero.dat"));
        ide.send("ok\n");
        reply = waitReply(7);
        QVERIFY(!resultIsError(reply));
    }

    /// The watchpoint pair, which is where a label survives GEMDOS relocating
    /// the program: `pist_watchpoint` takes a `label` as well as an `address`
    /// and sends whichever it was given, and `pist_clearwatchpoint` sends the
    /// bare verb when it was given neither.
    void watchpointToolsMapTheirArguments()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        // A label goes to the verb as a label — never as an address it resolved
        // itself, because only the IDE knows the live bases.
        QJsonObject args;
        args.insert(QStringLiteral("label"), QStringLiteral("counter.l"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_watchpoint"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("watchpoint counter.l"));
        ide.send("ok; watch counter.l at $12596\n");
        QJsonObject reply = waitReply(1);
        QVERIFY(!resultIsError(reply));

        // An address still works and is what is sent when only it was given.
        args = {};
        args.insert(QStringLiteral("address"), QStringLiteral("$1234.w"));
        send(server.get(), 2, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_watchpoint"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("watchpoint $1234.w"));
        ide.send("ok; watch $1234.w at $1234\n");
        reply = waitReply(2);
        QVERIFY(!resultIsError(reply));

        // Neither argument is refused before anything reaches the wire: the
        // bare verb would be sent with an empty argument, which the IDE answers
        // by refusing to name an address from nothing.
        send(server.get(), 3, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_watchpoint")));
        QVERIFY2(ide.nextLine(1000).isEmpty(),
                 qPrintable(QStringLiteral("a call with no argument reached the wire: %1")
                                .arg(QString::fromUtf8(ide.wire()))));
        reply = waitReply(3);
        QCOMPARE(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("code")).toInt(),
                 -32602);
        QVERIFY(resultText(reply).isEmpty());

        // Clearing with no argument clears every watchpoint: the verb goes out
        // with nothing after it.
        send(server.get(), 4, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_clearwatchpoint")));
        QCOMPARE(ide.nextLine(), QStringLiteral("clearwatchpoint"));
        ide.send("ok; cleared 2\n");
        reply = waitReply(4);
        QVERIFY(!resultIsError(reply));

        // Clearing one names it, the same way the add call did.
        args = {};
        args.insert(QStringLiteral("label"), QStringLiteral("counter.l"));
        send(server.get(), 5, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_clearwatchpoint"), args));
        QCOMPARE(ide.nextLine(), QStringLiteral("clearwatchpoint counter.l"));
        ide.send("ok; cleared 1\n");
        reply = waitReply(5);
        QVERIFY(!resultIsError(reply));
    }

    /// An argument is interpolated into a line-protocol command, and the IDE
    /// frames on '\n': without a check, an argument carrying one is executed
    /// there as a second verb — `quit`, withheld from the catalog on purpose,
    /// among them — and its extra reply line desyncs the one-in-flight
    /// correlation. The argument is attacker-influenceable without a malicious
    /// MCP client: source text an agent has just read (pist_read) fed back as a
    /// path or a command.
    void toolArgumentWithALineBreakIsRefusedAndNeverFramed()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        QJsonObject args;
        args.insert(QStringLiteral("path"), QStringLiteral("/tmp/evil.s\nquit"));
        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_open"), args));
        // Nothing reached the IDE at all — pre-fix the whole thing is framed,
        // so `open /tmp/evil.s` and the injected `quit` both arrive and the
        // verb withheld from this catalog runs on the IDE.
        QVERIFY2(ide.nextLine(1000).isEmpty(),
                 qPrintable(QStringLiteral("a refused call reached the wire: %1")
                                .arg(QString::fromUtf8(ide.wire()))));
        QJsonObject reply = waitReply(1);
        QCOMPARE(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("code")).toInt(),
                 -32602);
        QVERIFY2(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("message")).toString()
                     .contains(QLatin1String("path")),
                 qPrintable(QJsonDocument(reply).toJson(QJsonDocument::Compact)));

        // The other injection surface is free text an agent composes, so it
        // gets the same treatment; a bare carriage return is a break too.
        args = {};
        args.insert(QStringLiteral("command"), QStringLiteral("info mfp\rstop"));
        send(server.get(), 2, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_cmd"), args));
        QVERIFY2(ide.nextLine(1000).isEmpty(),
                 qPrintable(QStringLiteral("an unrunnable command reached the wire: %1")
                                .arg(QString::fromUtf8(ide.wire()))));
        reply = waitReply(2);
        QCOMPARE(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("code")).toInt(),
                 -32602);
        QVERIFY2(reply.value(QStringLiteral("error")).toObject()
                     .value(QStringLiteral("message")).toString()
                     .contains(QLatin1String("command")),
                 qPrintable(QJsonDocument(reply).toJson(QJsonDocument::Compact)));

        // The refusal is per-call: the next call is framed and answered, so a
        // rejected argument neither wedges the shim nor leaves the fake IDE's
        // first wire line something other than the command asked for.
        send(server.get(), 3, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build")));
        QCOMPARE(ide.nextLine(), QStringLiteral("build"));
        ide.send("ok\n");
        reply = waitReply(3);
        QVERIFY(!resultIsError(reply));
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

    /// A block's body is data: a line that is only '.' must not truncate it and
    /// a line beginning with "error" must not turn a successful call into a
    /// failure. The body travels with an explicit status header, and body lines
    /// starting with '.' are dot-stuffed (the sender prefixes one extra '.'; the
    /// client strips one). (Pre-fix the reply was cut at the first lone '.' and
    /// the header line leaked into the text.)
    void blockBodiesSurviveDotsAndErrorPrefixes()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_console")));
        QCOMPARE(ide.nextLine(), QStringLiteral("console"));
        // The lone '.' is stuffed as '..' so it cannot read as the terminator.
        ide.send("ok\nfirst line\n..\nerror 2 in line\nlast line\n.\n");

        const QJsonObject reply = waitReply(1);
        QVERIFY2(!resultIsError(reply),
                 qPrintable(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
        const QString text = resultText(reply);
        QVERIFY2(text.startsWith(QLatin1String("first line")), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("\n.\n")), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("error 2 in line")), qPrintable(text));
        QVERIFY2(text.contains(QLatin1String("last line")), qPrintable(text));
    }

    /// A second pist_watch while the first is still waiting for its
    /// acknowledgement must not overwrite the pending id — that left the first
    /// call unanswered forever. The second is refused, the first is answered.
    void aSecondWatchWhileOneIsPendingIsRefused()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        QCOMPARE(ide.nextLine(), QStringLiteral("watch"));

        // The acknowledgement has not arrived yet; the second call must be
        // answered now, not displace the first.
        send(server.get(), 2, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        const QJsonObject refused = waitReply(2);
        QVERIFY2(!refused.isEmpty(), "the superseding watch call was never answered");
        QVERIFY(resultIsError(refused));
        QVERIFY(resultText(refused).contains(QLatin1String("in flight")));

        // The acknowledgement answers the *first* call.
        ide.send("ok\n");
        const QJsonObject first = waitReply(1);
        QVERIFY2(!first.isEmpty(), "the first watch call was never answered");
        QVERIFY(!resultIsError(first));
    }

    /// A watch the IDE acknowledges never leaves the call hanging: the shim
    /// answers the waiting tool with a failure once its ack timer expires.
    /// (Pre-fix subscribe() armed no timer, so the tool waited forever.)
    void aWatchAckThatNeverArrivesIsAnswered()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        QCOMPARE(ide.nextLine(), QStringLiteral("watch"));
        // No acknowledgement is ever sent.

        const QJsonObject reply = waitReply(1, 15000);
        QVERIFY2(!reply.isEmpty(), "the watch call was left waiting on an ack that never came");
        QVERIFY(resultIsError(reply));
        QVERIFY(resultText(reply).contains(QLatin1String("did not answer")));
    }

    /// The shim's reply buffer is capped: a runaway reply fails the flight with
    /// an error instead of growing memory without limit. (Guard, not a
    /// regression demo: pre-fix the same input waits out the 180 s reply
    /// timeout, so the RED is the timeout itself, not a fast failure.)
    void anOversizedReplyFailsTheFlightInsteadOfGrowing()
    {
        FakeIde ide;
        QVERIFY(ide.start());
        auto server = makeServer(ide.port());

        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_console")));
        QCOMPARE(ide.nextLine(), QStringLiteral("console"));
        // 17 MiB with no newline: over the shim's 16 MiB cap.
        ide.send(QByteArray(17 * 1024 * 1024, 'x'));

        const QJsonObject reply = waitReply(1, 30000);
        QVERIFY2(!reply.isEmpty(), "the oversized reply never resulted in an answer");
        QVERIFY(resultIsError(reply));
        QVERIFY2(resultText(reply).contains(QLatin1String("buffer limit")),
                 qPrintable(resultText(reply)));
    }

    void noEndpointFailsTheCallWithTheRemedy()
    {
        auto server = makeServer(0); // port 0: not configured
        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_build")));
        const QJsonObject reply = waitReply(1);
        QVERIFY(resultIsError(reply));
        QVERIFY(resultText(reply).contains(QLatin1String("not configured")));
    }

    /// A token the IDE refuses is a session that has ended, and the shim must
    /// not treat it as the end of the road: the discovery file names the session
    /// that is running now, so re-reading it and dialing once more is what keeps
    /// an agent working across an IDE restart. (Pre-fix: "authentication
    /// rejected by PiST: error auth required", and the request fails.)
    void aRefusedTokenIsRetriedWithTheOneTheFilePublishes()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString discovery = dir.filePath(QStringLiteral("control-port"));

        FakeIde ide;
        QVERIFY(ide.start());
        ide.setToken(QStringLiteral("live-token"));
        writeDiscovery(discovery, ide.port(), QStringLiteral("live-token"));

        // The shim dials holding a token no live session issued: its read of the
        // file raced the restart, so the entry it resolved (and --token or
        // PIST_CONTROL_TOKEN pinned, the same way) is the previous session's
        // while the file now carries this one's.
        auto server = makeServer(ide.port());
        server->setToken(QStringLiteral("stale-token"));
        bool handedStaleOnce = false;
        server->setDiscoveryResolver([&] {
            if (!handedStaleOnce) {
                handedStaleOnce = true;
                return ControlClient::Discovery{QStringLiteral("127.0.0.1"), ide.port(),
                                                QStringLiteral("stale-token")};
            }
            return ControlClient::readDiscoveryFile(discovery);
        });

        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build")));

        // The stale token is refused, the file is re-read, and the retried
        // connection carries both the live token and the request that was
        // waiting — which is why the refused connection never saw `build`.
        QCOMPARE(ide.nextLine(), QStringLiteral("auth stale-token"));
        QCOMPARE(ide.nextLine(), QStringLiteral("auth live-token"));
        QCOMPARE(ide.nextLine(), QStringLiteral("build"));
        ide.send("ok\n");

        const QJsonObject reply = waitReply(1);
        QVERIFY2(!resultIsError(reply),
                 qPrintable(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
        QCOMPARE(resultText(reply), QStringLiteral("ok"));
    }

    /// A discovery line publishing a control-protocol version this shim does
    /// not speak must be refused before it is dialed (MIN-52). Negotiating
    /// first and then failing verb-by-verb hides the real problem: the two
    /// halves are from different releases. An absent version still connects —
    /// that is an IDE older than the handshake, covered by every other case.
    void anIncompatibleProtocolVersionIsRefusedBeforeDialing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString discovery = dir.filePath(QStringLiteral("control-port"));

        FakeIde ide;
        QVERIFY(ide.start());
        ide.setToken(QStringLiteral("live-token"));
        writeDiscovery(discovery, ide.port(), QStringLiteral("live-token"), 99);

        auto server = makeServer(ide.port());
        server->setDiscoveryResolver(
            [&] { return ControlClient::readDiscoveryFile(discovery); });

        send(server.get(), 1, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build")));

        const QJsonObject reply = waitReply(1);
        QVERIFY(resultIsError(reply));
        QVERIFY2(resultText(reply).contains(QLatin1String("control protocol 99")),
                 qPrintable(resultText(reply)));
        QVERIFY2(resultText(reply).contains(QLatin1String("upgrade the half that is older")),
                 qPrintable(resultText(reply)));
        // Nothing was dialed: the fake IDE never saw an auth attempt.
        QVERIFY(ide.nextLine().isEmpty());
    }

    /// An IDE restart is a new session — a new token, and a control port that
    /// may have moved — and the shim, which lives as long as the agent's client
    /// does, has to follow it without being restarted itself. (Pre-fix the one
    /// reconnect presents the old token to the old port, is refused, and the
    /// subscription goes silent for good: the failure is indistinguishable from
    /// an IDE that never came back.)
    void anIdeRestartIsFollowedWithoutRestartingTheShim()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString discovery = dir.filePath(QStringLiteral("control-port"));

        FakeIde first;
        QVERIFY(first.start());
        first.setToken(QStringLiteral("token-one"));
        writeDiscovery(discovery, first.port(), QStringLiteral("token-one"));

        auto server = makeServer(first.port());
        server->setToken(QStringLiteral("token-one"));
        server->setDiscoveryResolver(
            [discovery] { return ControlClient::readDiscoveryFile(discovery); });

        // Subscribed and watching.
        send(server.get(), 1, QStringLiteral("tools/call"), callParams(QStringLiteral("pist_watch")));
        QCOMPARE(first.nextLine(), QStringLiteral("auth token-one"));
        QCOMPARE(first.nextLine(), QStringLiteral("watch"));
        first.send("ok\n");
        QVERIFY(!resultIsError(waitReply(1)));

        // The IDE exits — and stays down past the first retry, so the shim has
        // to keep the subscription alive across a dial that fails.
        first.stop();
        QTest::qWait(1200);

        // It comes back as a new session: another port, another token, published
        // to the same discovery file.
        FakeIde second;
        QVERIFY(second.start());
        second.setToken(QStringLiteral("token-two"));
        writeDiscovery(discovery, second.port(), QStringLiteral("token-two"));

        // Without anything else driving it, the shim re-resolves, re-authenticates
        // and re-subscribes.
        QCOMPARE(second.nextLine(15000), QStringLiteral("auth token-two"));
        QCOMPARE(second.nextLine(), QStringLiteral("watch"));
        second.send("ok\n");

        // Events flow again on the new session...
        second.send("event stopped pc=0x00012596\n");
        QTRY_VERIFY_WITH_TIMEOUT(!firstEventNotification().isEmpty(), 5000);

        // ...and a tool call is answered by it: the second connection resolves
        // the moved port too, and waits for the new token's ack before sending.
        send(server.get(), 2, QStringLiteral("tools/call"),
             callParams(QStringLiteral("pist_build")));
        QCOMPARE(second.nextLine(), QStringLiteral("auth token-two"));
        QCOMPARE(second.nextLine(), QStringLiteral("build"));
        second.send("ok\n");
        const QJsonObject reply = waitReply(2);
        QVERIFY2(!resultIsError(reply),
                 qPrintable(QJsonDocument(reply).toJson(QJsonDocument::Compact)));
    }
};

QTEST_GUILESS_MAIN(TstMcp)
#include "tst_mcp.moc"
