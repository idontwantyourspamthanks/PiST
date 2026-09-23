// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// pist-mcp: an MCP (Model Context Protocol) server on stdio that forwards to a
// running PiST over its remote-control socket. It is what makes the IDE
// discoverable as a native tool set by Claude, Cursor and other MCP clients.
//
// Transport: MCP's stdio binding — one JSON-RPC 2.0 message per line on stdin
// and stdout, UTF-8, no embedded newlines and no Content-Length headers. See
// McpServer.h for the spec discussion; stdout carries MCP messages only, so all
// logging goes to stderr.

#include "control/mcp/McpServer.h"

#include "control/mcp/ControlClient.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QMetaObject>
#include <QThread>

#include <cstdio>

using namespace pist::mcp;

namespace {

/// Where PiST is listening. `--port` wins over `PIST_CONTROL_PORT`, mirroring
/// how pist itself resolves the same two (src/main.cpp); with neither, the
/// discovery file a listening IDE publishes (RemoteControl::discoveryFilePath)
/// is the zero-configuration answer.
quint16 controlPort(const QCommandLineParser &parser, const QCommandLineOption &option,
                    QString *discoveryToken, QString *error)
{
    QString text = parser.value(option);
    if (text.isEmpty())
        text = qEnvironmentVariable("PIST_CONTROL_PORT");
    // The file is read whenever it exists — its token is needed even when the
    // port came from --port or the environment, or that connection has no
    // auth line and the server drops it.
    const ControlClient::Discovery found = ControlClient::readDiscoveryFile();
    if (text.isEmpty() && found.port != 0)
        text = QString::number(found.port);
    *discoveryToken = found.token;
    if (text.isEmpty()) {
        // Not fatal: the server still completes the MCP handshake, and every
        // tool call reports exactly what to do about it. Refusing to start would
        // make the failure invisible to the agent (the client would just see a
        // dead server).
        *error = QStringLiteral(
            "PiST control port is not set: pass --port <n>, set "
            "PIST_CONTROL_PORT, or start pist with --control-port <n> (it writes "
            "a discovery file pist-mcp reads)");
        return 0;
    }
    bool ok = false;
    const uint port = text.toUInt(&ok);
    if (!ok || port == 0 || port > 65535) {
        *error = QStringLiteral("invalid control port '%1'").arg(text);
        return 0;
    }
    return quint16(port);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("pist-mcp"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PIST_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("MCP server that drives a running PiST over its "
                       "remote-control socket."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption(
        QStringLiteral("port"),
        QStringLiteral("Port PiST's remote control listens on (or set "
                       "PIST_CONTROL_PORT)."),
        QStringLiteral("port"));
    parser.addOption(portOption);
    QCommandLineOption hostOption(
        QStringLiteral("host"),
        QStringLiteral("Host to reach PiST on (default 127.0.0.1)."),
        QStringLiteral("host"), QStringLiteral("127.0.0.1"));
    parser.addOption(hostOption);
    QCommandLineOption tokenOption(
        QStringLiteral("token"),
        QStringLiteral("Session token the IDE expects (or set PIST_CONTROL_TOKEN; "
                       "the discovery file carries it)."),
        QStringLiteral("token"));
    parser.addOption(tokenOption);
    parser.process(app);

    QString discoveryToken;
    QString configError;
    const quint16 port = controlPort(parser, portOption, &discoveryToken, &configError);
    if (!configError.isEmpty())
        fprintf(stderr, "pist-mcp: %s\n", qPrintable(configError));

    const QString host = parser.value(hostOption);
    QString token = parser.value(tokenOption);
    if (token.isEmpty())
        token = qEnvironmentVariable("PIST_CONTROL_TOKEN");
    if (token.isEmpty())
        token = discoveryToken;

    McpServer server(host, port);
    server.setToken(token);

    // The address resolved above is only good until the IDE restarts: every
    // start mints a new session token, and the control port is whatever the
    // user passed to that instance. A shim that keeps dialing the old pair is
    // told "error auth required" once and then goes silent for the rest of the
    // agent's session, which is indistinguishable from a dead IDE. So the
    // address is resolved afresh from the discovery file before every dial —
    // and again when the IDE refuses the token.
    //
    // A pinned --port/--host is where the user said the IDE is and is never
    // overridden; the token is per-session, so the one the file publishes is
    // taken whenever there is one (--token and PIST_CONTROL_TOKEN are the
    // fallback for an IDE that publishes no file, not a claim about a session
    // that has since been replaced).
    const bool pinned = parser.isSet(portOption) || parser.isSet(hostOption)
                        || !qEnvironmentVariable("PIST_CONTROL_PORT").isEmpty();
    server.setDiscoveryResolver([pinned, host, port, token] {
        ControlClient::Discovery found = ControlClient::readDiscoveryFile();
        if (found.port == 0)
            return found; // Nothing published: keep the address in force.
        if (found.token.isEmpty())
            found.token = token;
        if (!pinned)
            return found;
        found.host = host;
        found.port = port;
        return found;
    });

    // stdout is the MCP transport: exactly one JSON object per line, nothing
    // else. Written through stdio rather than QTextStream so the flush is
    // immediate — a buffered reply would deadlock the client waiting on it.
    QObject::connect(&server, &McpServer::sendLine, &app, [](const QByteArray &line) {
        fwrite(line.constData(), 1, size_t(line.size()), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    });
    // stderr is free-form, and the spec explicitly allows logging there.
    QObject::connect(&server, &McpServer::logLine, &app, [](const QString &line) {
        fprintf(stderr, "pist-mcp: %s\n", qPrintable(line));
    });

    // Read stdin on its own thread and hand each whole line to the server on
    // the main loop. A blocking read is required (QSocketNotifier cannot watch
    // fd 0 on Windows, where this tool also ships), but the main loop must stay
    // free: a `run` answers asynchronously, and blocking the loop on stdin would
    // starve that reply. The thread is what keeps "blocking read" and
    // "responsive loop" from being a choice.
    auto *reader = QThread::create([&server, &app] {
        QFile in;
        if (!in.open(stdin, QIODevice::ReadOnly)) {
            fprintf(stderr, "pist-mcp: cannot read stdin\n");
            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
            return;
        }
        QByteArray buffer;
        forever {
            const QByteArray chunk = in.readLine();
            if (chunk.isEmpty())
                break; // End of input: the client closed the stream, which the
                       // spec names as the graceful-shutdown signal.
            buffer += chunk;
            int nl = 0;
            while ((nl = buffer.indexOf('\n')) >= 0) {
                QByteArray line = buffer.left(nl);
                buffer.remove(0, nl + 1);
                // handleLine is a plain member; invoking it queued on the
                // server's thread keeps every JSON reply and socket write on
                // the one loop that owns them.
                QMetaObject::invokeMethod(
                    &server, [&server, line] { server.handleLine(line); },
                    Qt::QueuedConnection);
            }
        }
        if (!buffer.trimmed().isEmpty()) {
            const QByteArray line = buffer.trimmed();
            QMetaObject::invokeMethod(&server, [&server, line] { server.handleLine(line); },
                                      Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
    });
    reader->start();

    const int code = app.exec();
    reader->wait();
    return code;
}
