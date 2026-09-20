// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "control/mcp/McpServer.h"

#include "control/mcp/ControlClient.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace pist::mcp {

namespace {

/// The MCP revision this server implements. 2025-06-18 is the newest revision
/// with a stable, finalized stdio transport and the tool-call shape used here;
/// its stdio framing (newline-delimited JSON) is unchanged in the later
/// revisions. A client asking for a different version is answered with this
/// one, which the spec allows — the client then decides whether it can continue.
constexpr char kProtocolVersion[] = "2025-06-18";

/// JSON-RPC 2.0 standard error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;

QJsonObject toolObject(const QString &name, const QString &title, const QString &description,
                       const QJsonObject &properties, const QJsonArray &required)
{
    QJsonObject schema;
    schema.insert(QStringLiteral("type"), QStringLiteral("object"));
    schema.insert(QStringLiteral("properties"), properties);
    // Always present: an empty `required` is what tells a model the call takes
    // no arguments, and some clients reject a schema that omits it.
    schema.insert(QStringLiteral("required"), required);
    QJsonObject tool;
    tool.insert(QStringLiteral("name"), name);
    tool.insert(QStringLiteral("title"), title);
    tool.insert(QStringLiteral("description"), description);
    tool.insert(QStringLiteral("inputSchema"), schema);
    return tool;
}

/// A `content` array holding one text block — the shape every MCP client
/// renders. `isError` is set by the caller in the enclosing result.
QJsonArray textContent(const QString &text)
{
    QJsonObject block;
    block.insert(QStringLiteral("type"), QStringLiteral("text"));
    block.insert(QStringLiteral("text"), text);
    return QJsonArray{block};
}

} // namespace

McpServer::McpServer(const QString &controlHost, quint16 controlPort, QObject *parent)
    : QObject(parent)
    , m_host(controlHost)
    , m_port(controlPort)
{
    m_control = new ControlClient(this);
    m_control->setEndpoint(controlHost, controlPort);
    connect(m_control, &ControlClient::replied, this, &McpServer::deliverReply);
    connect(m_control, &ControlClient::failed, this, &McpServer::deliverFailure);

    m_events = new ControlClient(this);
    m_events->setEndpoint(controlHost, controlPort);
    connect(m_events, &ControlClient::event, this, [this](const QString &name, const QString &detail) {
        QString line = QStringLiteral("event ") + name;
        if (!detail.isEmpty())
            line += QLatin1Char(' ') + detail;
        m_recentEvents.append(line);
        emit logLine(QStringLiteral("event: ") + line);

        // Push it to the client as well, so an agent waiting on a stop is woken
        // rather than having to call pist_watch in a loop. `notifications/message`
        // is the spec's channel for unstructured server-side information; the
        // `logging` capability this server declares is what makes it legal.
        QJsonObject data;
        data.insert(QStringLiteral("event"), name);
        data.insert(QStringLiteral("detail"), detail);
        QJsonObject params;
        params.insert(QStringLiteral("level"), QStringLiteral("notice"));
        params.insert(QStringLiteral("logger"), QStringLiteral("pist.session"));
        params.insert(QStringLiteral("data"), data);
        QJsonObject notification;
        notification.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
        notification.insert(QStringLiteral("method"), QStringLiteral("notifications/message"));
        notification.insert(QStringLiteral("params"), params);
        sendRaw(notification);
    });
    connect(m_events, &ControlClient::subscribeReplied, this, [this](bool ok, const QString &error) {
        if (!m_havePendingWatch)
            return;
        const QJsonValue id = m_pendingWatchId;
        m_havePendingWatch = false;
        m_pendingWatchId = QJsonValue();

        if (!ok) {
            // The subscription failed, so the tool reports the IDE's error
            // rather than pretending events will arrive.
            QJsonObject result;
            result.insert(QStringLiteral("content"), textContent(
                QStringLiteral("watch failed: ") + error));
            result.insert(QStringLiteral("isError"), true);
            sendResult(id, result);
            return;
        }
        QJsonObject result;
        result.insert(QStringLiteral("content"), textContent(
            QStringLiteral("watching for events; call pist_watch again to collect "
                           "events that have happened since")));
        result.insert(QStringLiteral("isError"), false);
        sendResult(id, result);
    });
    connect(m_events, &ControlClient::connectionChanged, this, [this](bool connected) {
        emit logLine(connected ? QStringLiteral("event connection up")
                               : QStringLiteral("event connection down"));
    });
}

void McpServer::ensureSubscribed()
{
    if (m_events->isSubscribed())
        return;
    m_events->subscribe();
}

void McpServer::handleLine(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;

    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        // A parse error has no id to answer to, so the reply carries null, as
        // JSON-RPC 2.0 specifies.
        sendError(QJsonValue(), kParseError,
                  QStringLiteral("Parse error: ") + error.errorString());
        return;
    }
    dispatch(doc.object());
}

void McpServer::dispatch(const QJsonObject &message)
{
    const QJsonValue methodValue = message.value(QStringLiteral("method"));
    if (!methodValue.isString()) {
        sendError(message.value(QStringLiteral("id")), kInvalidRequest,
                  QStringLiteral("Missing method"));
        return;
    }
    const QString method = methodValue.toString();
    const QJsonValue id = message.value(QStringLiteral("id"));
    const QJsonObject params = message.value(QStringLiteral("params")).toObject();

    // A notification (no id) is a message the client does not expect an answer
    // to; answering one is a protocol violation.
    const bool isNotification = !message.contains(QStringLiteral("id"));

    if (method == QLatin1String("initialize")) {
        if (!isNotification)
            handleInitialize(id, params);
        return;
    }
    if (method == QLatin1String("notifications/initialized")
        || method == QLatin1String("initialized")) {
        // The client is ready. Nothing to answer, but the state is recorded so
        // a strict client sees the expected silence.
        m_initialized = true;
        emit logLine(QStringLiteral("client initialized"));
        return;
    }
    if (method == QLatin1String("notifications/cancelled")) {
        return;
    }
    if (method == QLatin1String("ping")) {
        if (!isNotification) {
            QJsonObject result;
            sendResult(id, result);
        }
        return;
    }
    if (method == QLatin1String("tools/list")) {
        if (!isNotification)
            handleToolsList(id);
        return;
    }
    if (method == QLatin1String("tools/call")) {
        if (!isNotification)
            handleToolsCall(id, params);
        return;
    }
    if (method.startsWith(QLatin1String("notifications/")))
        return; // Unknown notification: ignore, per JSON-RPC.

    if (!isNotification)
        sendError(id, kMethodNotFound, QStringLiteral("Method not found: ") + method);
}

void McpServer::handleInitialize(const QJsonValue &id, const QJsonObject &params)
{
    // Version negotiation: answer with this server's version whether or not it
    // matches the request; the client decides whether it can proceed. The spec
    // requires the server to reply with a version it supports.
    const QString requested = params.value(QStringLiteral("protocolVersion")).toString();
    emit logLine(QStringLiteral("initialize from client, requested protocol %1")
                     .arg(requested.isEmpty() ? QStringLiteral("(none)") : requested));

    QJsonObject tools;
    tools.insert(QStringLiteral("listChanged"), false);

    QJsonObject capabilities;
    capabilities.insert(QStringLiteral("tools"), tools);
    // Declared because the shim can emit notifications/message; unused today,
    // but a client that gates its handling on the capability needs it present
    // for the event stream to be meaningful.
    capabilities.insert(QStringLiteral("logging"), QJsonObject());

    QJsonObject serverInfo;
    serverInfo.insert(QStringLiteral("name"), QStringLiteral("pist"));
    serverInfo.insert(QStringLiteral("title"), QStringLiteral("PiST Atari ST IDE"));
    // From the build, not a literal: a hardcoded version here already shipped
    // one release stale (a 0.6.2 binary introducing itself as 0.6.1).
    serverInfo.insert(QStringLiteral("version"), QStringLiteral(PIST_VERSION));

    QJsonObject result;
    result.insert(QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion));
    result.insert(QStringLiteral("capabilities"), capabilities);
    result.insert(QStringLiteral("serverInfo"), serverInfo);
    result.insert(QStringLiteral("instructions"),
                  QStringLiteral("Drive a running PiST IDE. Tools map onto the IDE's "
                                 "remote-control verbs; run/build answer only when the "
                                 "work has finished. Use pist_watch to be told when the "
                                 "debugger stops instead of polling pist_state."));
    sendResult(id, result);
}

void McpServer::handleToolsList(const QJsonValue &id)
{
    QJsonObject result;
    result.insert(QStringLiteral("tools"), tools());
    sendResult(id, result);
}

void McpServer::handleToolsCall(const QJsonValue &id, const QJsonObject &params)
{
    const QString name = params.value(QStringLiteral("name")).toString();
    const QJsonObject args = params.value(QStringLiteral("arguments")).toObject();

    if (name.isEmpty()) {
        sendError(id, kInvalidParams, QStringLiteral("Missing tool name"));
        return;
    }

    // pist_watch is the one tool that is not a straight pass-through: it
    // establishes (or re-reads) the event subscription.
    if (name == QLatin1String("pist_watch")) {
        if (!m_events->isSubscribed()) {
            m_pendingWatchId = id;
            m_havePendingWatch = true;
            ensureSubscribed();
            return;
        }
        // Already watching: answer at once with whatever has accumulated, so a
        // repeated call is useful rather than a no-op.
        const QStringList events = m_recentEvents;
        m_recentEvents.clear();
        QJsonObject result;
        const QString text = events.isEmpty()
            ? QStringLiteral("watching; no events since the last call")
            : events.join(QLatin1Char('\n'));
        result.insert(QStringLiteral("content"), textContent(text));
        result.insert(QStringLiteral("isError"), false);
        sendResult(id, result);
        return;
    }

    // Every other tool is a remote-control verb with zero or more text
    // arguments. `block` must match the verb's framing (see RemoteControl's
    // help): a mismatch would misparse the reply.
    QString command;
    bool block = false;
    if (name == QLatin1String("pist_state")) {
        // statejson, not state: the JSON is served as structured content (see
        // deliverReply), which is what an agent should consume.
        command = QStringLiteral("statejson");
        block = true;
    } else if (name == QLatin1String("pist_problems")) {
        command = QStringLiteral("problems");
        block = true;
    } else if (name == QLatin1String("pist_read")) {
        command = QStringLiteral("read");
        block = true;
    } else if (name == QLatin1String("pist_console")) {
        command = QStringLiteral("console");
        block = true;
    } else if (name == QLatin1String("pist_breakpoints")) {
        // There is no dedicated breakpoint-listing verb; the debugger's own
        // breakpoint listing is the honest answer, and `b` with no arguments
        // lists them. It is a block reply.
        command = QStringLiteral("cmd b");
        block = true;
    } else if (name == QLatin1String("pist_run")) {
        command = QStringLiteral("run");
    } else if (name == QLatin1String("pist_build")) {
        command = QStringLiteral("build");
    } else if (name == QLatin1String("pist_continue")) {
        command = QStringLiteral("continue");
    } else if (name == QLatin1String("pist_open")) {
        command = QStringLiteral("open %1").arg(args.value(QStringLiteral("path")).toString());
    } else if (name == QLatin1String("pist_profile_start")) {
        command = QStringLiteral("profile start");
    } else if (name == QLatin1String("pist_profile_stop")) {
        command = QStringLiteral("profile stop");
    } else if (name == QLatin1String("pist_profile_results")) {
        command = QStringLiteral("profile results");
        block = true;
    } else if (name == QLatin1String("pist_symbols")) {
        const QString filter = args.value(QStringLiteral("filter")).toString();
        command = filter.isEmpty() ? QStringLiteral("symbols")
                                   : QStringLiteral("symbols ") + filter;
        block = true;
    } else if (name == QLatin1String("pist_readmem")) {
        command = QStringLiteral("readmem %1 %2")
                      .arg(args.value(QStringLiteral("address")).toString(),
                           args.value(QStringLiteral("length")).toString());
        block = true;
    } else if (name == QLatin1String("pist_disasm")) {
        const QString address = args.value(QStringLiteral("address")).toString();
        command = address.isEmpty() ? QStringLiteral("disasm")
                                    : QStringLiteral("disasm ") + address;
        block = true;
    } else if (name == QLatin1String("pist_stop")) {
        command = QStringLiteral("stop");
    } else if (name == QLatin1String("pist_step")) {
        command = QStringLiteral("step");
    } else if (name == QLatin1String("pist_stepover")) {
        command = QStringLiteral("stepover");
    } else if (name == QLatin1String("pist_setreg")) {
        const QString reg = args.value(QStringLiteral("register")).toString();
        const QString value = args.value(QStringLiteral("value")).toString();
        command = QStringLiteral("setreg %1 %2").arg(reg, value);
    } else if (name == QLatin1String("pist_setmem")) {
        const QString address = args.value(QStringLiteral("address")).toString();
        const QString value = args.value(QStringLiteral("value")).toString();
        command = QStringLiteral("setmem %1 %2").arg(address, value);
    } else if (name == QLatin1String("pist_breakpoint")) {
        const QString label = args.value(QStringLiteral("label")).toString();
        command = label.isEmpty()
                      ? QStringLiteral("breakpoint %1")
                            .arg(args.value(QStringLiteral("line")).toInt())
                      : QStringLiteral("breakpoint ") + label;
    } else if (name == QLatin1String("pist_watchpoint")) {
        const QString address = args.value(QStringLiteral("address")).toString();
        command = QStringLiteral("watchpoint %1").arg(address);
    } else if (name == QLatin1String("pist_cmd")) {
        const QString text = args.value(QStringLiteral("command")).toString();
        command = QStringLiteral("cmd %1").arg(text);
        block = true;
    } else if (name == QLatin1String("pist_screenshot")) {
        const QString path = args.value(QStringLiteral("path")).toString();
        command = path.isEmpty() ? QStringLiteral("screenshot")
                                 : QStringLiteral("screenshot %1").arg(path);
    } else {
        sendError(id, kInvalidParams, QStringLiteral("Unknown tool: ") + name);
        return;
    }

    const quint64 token = m_nextToken++;
    m_outstanding.insert(token, Outstanding{id, name});
    m_control->request(command, block, token);
}

void McpServer::deliverReply(quint64 token, const QString &text)
{
    const auto it = m_outstanding.find(token);
    if (it == m_outstanding.end())
        return;
    const Outstanding outstanding = it.value();
    m_outstanding.erase(it);

    // `error …` from the IDE is a *tool execution* error, not a protocol error:
    // the call was well-formed and the IDE answered, so it goes back in a
    // result with isError, which is what the spec prescribes for API failures.
    const bool isError = text.startsWith(QLatin1String("error"));

    // Tools whose answer is a JSON document get it in both shapes: parsed as
    // structuredContent for clients that consume fields, and pretty-printed
    if (!isError && (outstanding.tool == QLatin1String("pist_state")
                     || outstanding.tool == QLatin1String("pist_problems")
                     || outstanding.tool == QLatin1String("pist_profile_results")
                     || outstanding.tool == QLatin1String("pist_symbols")
                     || outstanding.tool == QLatin1String("pist_read")
                     || outstanding.tool == QLatin1String("pist_readmem")
                     || outstanding.tool == QLatin1String("pist_disasm"))) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
        if (parseError.error == QJsonParseError::NoError) {
            QJsonObject structured;
            if (doc.isObject())
                structured = doc.object();
            else if (doc.isArray())
                structured.insert(QStringLiteral("items"), doc.array());
            QJsonObject result;
            result.insert(QStringLiteral("structuredContent"), structured);
            result.insert(QStringLiteral("content"),
                          textContent(QString::fromUtf8(doc.toJson(QJsonDocument::Indented))));
            result.insert(QStringLiteral("isError"), false);
            sendResult(outstanding.id, result);
            return;
        }
    }

    QJsonObject result;
    result.insert(QStringLiteral("content"), textContent(text));
    result.insert(QStringLiteral("isError"), isError);
    sendResult(outstanding.id, result);
}

void McpServer::deliverFailure(quint64 token, const QString &message)
{
    const auto it = m_outstanding.find(token);
    if (it == m_outstanding.end())
        return;
    const QJsonValue id = it.value().id;
    m_outstanding.erase(it);

    QJsonObject result;
    result.insert(QStringLiteral("content"), textContent(message));
    result.insert(QStringLiteral("isError"), true);
    sendResult(id, result);
}

void McpServer::sendResult(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject message;
    message.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    message.insert(QStringLiteral("id"), id);
    message.insert(QStringLiteral("result"), result);
    sendRaw(message);
}

void McpServer::sendError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject error;
    error.insert(QStringLiteral("code"), code);
    error.insert(QStringLiteral("message"), message);
    QJsonObject response;
    response.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    response.insert(QStringLiteral("id"), id);
    response.insert(QStringLiteral("error"), error);
    sendRaw(response);
}

void McpServer::sendRaw(const QJsonObject &message)
{
    // Compact, and with no embedded newline: it is one line of the transport.
    emit sendLine(QJsonDocument(message).toJson(QJsonDocument::Compact));
}

QJsonArray McpServer::tools()
{
    QJsonArray list;

    const QJsonObject empty;

    list.append(toolObject(
        QStringLiteral("pist_run"), QStringLiteral("Run the program"),
        QStringLiteral("Build the current source and start the emulator. Answers only "
                       "once the session is running, so no polling is needed."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_build"), QStringLiteral("Build"),
        QStringLiteral("Assemble and link the current source. Answers only when the "
                       "build has finished. Call pist_problems for the diagnostics "
                       "when it fails."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_state"), QStringLiteral("Machine state"),
        QStringLiteral("Registers and program counter of the stopped machine, as a "
                       "JSON object (also in structuredContent). While running it "
                       "carries only the running/stopped flags."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_stop"), QStringLiteral("Stop the session"),
        QStringLiteral("Stop the emulator session."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_step"), QStringLiteral("Step one instruction"),
        QStringLiteral("Step one 68000 instruction (debugger must be stopped)."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_stepover"), QStringLiteral("Step over"),
        QStringLiteral("Step over a subroutine call."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_continue"), QStringLiteral("Continue"),
        QStringLiteral("Resume execution until a breakpoint or watchpoint is hit."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_console"), QStringLiteral("Console text"),
        QStringLiteral("The IDE's build and debug console text."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_breakpoints"), QStringLiteral("List breakpoints"),
        QStringLiteral("List the debugger's breakpoints and watchpoints. Requires a "
                       "running emulator session (answers '(no response)' otherwise)."),
        empty, {}));

    {
        QJsonObject props;
        QJsonObject line;
        line.insert(QStringLiteral("type"), QStringLiteral("integer"));
        line.insert(QStringLiteral("description"),
                    QStringLiteral("Source line number to toggle a breakpoint on"));
        QJsonObject label;
        label.insert(QStringLiteral("type"), QStringLiteral("string"));
        label.insert(QStringLiteral("description"),
                     QStringLiteral("Symbol to break at instead of a line number — "
                                    "resolved to its definition's file:line; the reply "
                                    "names where it landed"));
        props.insert(QStringLiteral("line"), line);
        props.insert(QStringLiteral("label"), label);
        list.append(toolObject(
            QStringLiteral("pist_breakpoint"), QStringLiteral("Toggle breakpoint"),
            QStringLiteral("Toggle a breakpoint at a source line or at a symbol's "
                           "definition (pass label). Only lines holding an "
                           "instruction can fire; the reply says when a line has none."),
            props, {}));
    }

    {
        QJsonObject props;
        QJsonObject filter;
        filter.insert(QStringLiteral("type"), QStringLiteral("string"));
        filter.insert(QStringLiteral("description"),
                      QStringLiteral("Optional case-insensitive name filter"));
        props.insert(QStringLiteral("filter"), filter);
        list.append(toolObject(
            QStringLiteral("pist_symbols"), QStringLiteral("List symbols"),
            QStringLiteral("The build's symbol table as a JSON array (also in "
                           "structuredContent): name, file and line, and the address "
                           "once the program is loaded."),
            props, {}));
    }

    {
        QJsonObject props;
        QJsonObject address;
        address.insert(QStringLiteral("type"), QStringLiteral("string"));
        address.insert(QStringLiteral("description"),
                       QStringLiteral("Address to read from, e.g. $12596 or 0x12596"));
        QJsonObject length;
        length.insert(QStringLiteral("type"), QStringLiteral("string"));
        length.insert(QStringLiteral("description"),
                      QStringLiteral("How many bytes to read, decimal"));
        props.insert(QStringLiteral("address"), address);
        props.insert(QStringLiteral("length"), length);
        list.append(toolObject(
            QStringLiteral("pist_readmem"), QStringLiteral("Read memory"),
            QStringLiteral("Read memory as JSON rows of hex bytes (also in "
                           "structuredContent). Requires a stopped emulator session."),
            props, QJsonArray{QStringLiteral("address"), QStringLiteral("length")}));
    }

    {
        QJsonObject props;
        QJsonObject address;
        address.insert(QStringLiteral("type"), QStringLiteral("string"));
        address.insert(QStringLiteral("description"),
                       QStringLiteral("Address to disassemble from; the PC when omitted"));
        props.insert(QStringLiteral("address"), address);
        list.append(toolObject(
            QStringLiteral("pist_disasm"), QStringLiteral("Disassemble"),
            QStringLiteral("Disassembly as JSON rows {address, bytes, text} (also in "
                           "structuredContent). Requires a stopped emulator session."),
            props, {}));
    }

    {
        QJsonObject props;
        QJsonObject reg;
        reg.insert(QStringLiteral("type"), QStringLiteral("string"));
        reg.insert(QStringLiteral("description"),
                   QStringLiteral("Register name, e.g. d0, a7, pc, sr"));
        QJsonObject value;
        value.insert(QStringLiteral("type"), QStringLiteral("string"));
        value.insert(QStringLiteral("description"),
                     QStringLiteral("New value, hex with $ or 0x, or plain hex"));
        props.insert(QStringLiteral("register"), reg);
        props.insert(QStringLiteral("value"), value);
        list.append(toolObject(
            QStringLiteral("pist_setreg"), QStringLiteral("Set register"),
            QStringLiteral("Write a register while the machine is stopped."),
            props, QJsonArray{QStringLiteral("register"), QStringLiteral("value")}));
    }

    {
        QJsonObject props;
        QJsonObject address;
        address.insert(QStringLiteral("type"), QStringLiteral("string"));
        address.insert(QStringLiteral("description"),
                       QStringLiteral("Address, e.g. $12345 or 0x12345"));
        QJsonObject value;
        value.insert(QStringLiteral("type"), QStringLiteral("string"));
        value.insert(QStringLiteral("description"), QStringLiteral("Byte value to write"));
        props.insert(QStringLiteral("address"), address);
        props.insert(QStringLiteral("value"), value);
        list.append(toolObject(
            QStringLiteral("pist_setmem"), QStringLiteral("Write memory byte"),
            QStringLiteral("Write one byte to memory while the machine is stopped."),
            props, QJsonArray{QStringLiteral("address"), QStringLiteral("value")}));
    }

    {
        QJsonObject props;
        QJsonObject address;
        address.insert(QStringLiteral("type"), QStringLiteral("string"));
        address.insert(QStringLiteral("description"),
                       QStringLiteral("Address to watch, with optional size suffix "
                                      "(.b/.w/.l), e.g. $12345.l"));
        props.insert(QStringLiteral("address"), address);
        list.append(toolObject(
            QStringLiteral("pist_watchpoint"), QStringLiteral("Add watchpoint"),
            QStringLiteral("Break when the value at an address changes."),
            props, QJsonArray{QStringLiteral("address")}));
    }

    {
        QJsonObject props;
        QJsonObject text;
        text.insert(QStringLiteral("type"), QStringLiteral("string"));
        text.insert(QStringLiteral("description"),
                    QStringLiteral("Arbitrary Hatari debugger command, e.g. 'info mfp'"));
        props.insert(QStringLiteral("command"), text);
        list.append(toolObject(
            QStringLiteral("pist_cmd"), QStringLiteral("Debugger command"),
            QStringLiteral("Run an arbitrary Hatari debugger command and return its "
                           "output. Requires a running emulator session, and most "
                           "commands a stopped machine."),
            props, QJsonArray{QStringLiteral("command")}));
    }

    {
        QJsonObject props;
        QJsonObject path;
        path.insert(QStringLiteral("type"), QStringLiteral("string"));
        path.insert(QStringLiteral("description"),
                    QStringLiteral("Where to save the PNG (default /tmp/pist-screenshot.png)"));
        props.insert(QStringLiteral("path"), path);
        list.append(toolObject(
            QStringLiteral("pist_screenshot"), QStringLiteral("Screenshot"),
            QStringLiteral("Save the IDE window, including the embedded emulator "
                           "display, as a PNG. Needs a visible window; it fails "
                           "against a headless (offscreen) IDE."),
            props, {}));
    }

    {
        QJsonObject props;
        QJsonObject path;
        path.insert(QStringLiteral("type"), QStringLiteral("string"));
        path.insert(QStringLiteral("description"),
                    QStringLiteral("Source file to open in the IDE"));
        props.insert(QStringLiteral("path"), path);
        list.append(toolObject(
            QStringLiteral("pist_open"), QStringLiteral("Open a source file"),
            QStringLiteral("Open a source file in the IDE, making it the current "
                           "document (what build/run/breakpoint act on)."),
            props, QJsonArray{QStringLiteral("path")}));
    }

    list.append(toolObject(
        QStringLiteral("pist_read"), QStringLiteral("Read the current document"),
        QStringLiteral("The document the IDE is showing, as a JSON object {path, "
                       "text} (also in structuredContent)."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_problems"), QStringLiteral("Build problems"),
        QStringLiteral("The Problems pane — file, line and message for every "
                       "diagnostic from the last build, as a JSON array (also in "
                       "structuredContent). Call this after pist_build instead of "
                       "parsing pist_console output."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_profile_start"), QStringLiteral("Start profiling"),
        QStringLiteral("Start collecting per-instruction execution counts "
                       "(debugger must be stopped; resume after starting)."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_profile_stop"), QStringLiteral("Stop profiling"),
        QStringLiteral("Stop collecting. Answers once the results are parsed and "
                       "ready, so pist_profile_results immediately after is this "
                       "run's data, never the previous run's."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_profile_results"), QStringLiteral("Profile results"),
        QStringLiteral("Per-source-line execution counts from the last profile, "
                       "sorted hottest first, as a JSON array (also in "
                       "structuredContent). Empty until a profile is collected."),
        empty, {}));

    list.append(toolObject(
        QStringLiteral("pist_watch"), QStringLiteral("Watch for events"),
        QStringLiteral("Subscribe to debug-session events (stopped/running) and "
                       "return any that have happened since the last call. Call this "
                       "instead of polling pist_state when waiting for a breakpoint "
                       "to be hit. Events also arrive as MCP log notifications."),
        empty, {}));

    return list;
}

} // namespace pist::mcp
