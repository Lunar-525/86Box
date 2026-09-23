/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Model Context Protocol (MCP) server over Streamable HTTP.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_server.hpp"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

extern "C" {
#include <86box/version.h>
}

/* Tool calls are expected to be quick (file work or one sampling window); a
   handler that never completes must not leave the client hanging forever. */
static const int MCP_TOOL_CALL_TIMEOUT_MS = 20000;

/* Protocol revisions this server understands. The revision requested by the
   client is echoed back when known, which keeps newer clients happy. */
static const QStringList MCP_PROTOCOL_VERSIONS = {
    "2025-11-25",
    "2025-06-18",
    "2025-03-26",
    "2024-11-05",
    "2024-10-07",
};

static const QString MCP_DEFAULT_PROTOCOL_VERSION = QStringLiteral("2025-06-18");

static const QString MCP_INSTRUCTIONS = QStringLiteral(
    "86Box MCP server. Use vm_list to enumerate the virtual machines known to "
    "the VM manager, vm_create to add one, vm_config_get/vm_config_set to read "
    "and change a machine's configuration (86box.cfg), and vm_performance to "
    "read the live CPU and cache statistics shown by Tools > Performance for a "
    "running machine.");

/* ------------------------------------------------------------------ */
/* McpToolResult                                                       */
/* ------------------------------------------------------------------ */

McpToolResult
McpToolResult::ok(const QJsonObject &data)
{
    McpToolResult result;
    result.data     = data;
    result.is_error = false;
    return result;
}

McpToolResult
McpToolResult::failure(const QString &message)
{
    McpToolResult result;
    result.data     = QJsonObject { { "error", message } };
    result.is_error = true;
    return result;
}

McpToolResult
McpToolResult::withImage(const QByteArray &image, const QString &media_type, const QJsonObject &data)
{
    McpToolResult result;
    result.data       = data;
    result.image      = image;
    result.image_mime = media_type;
    result.is_error   = false;
    return result;
}

/* ------------------------------------------------------------------ */
/* McpServer                                                           */
/* ------------------------------------------------------------------ */

McpServer::McpServer(QObject *parent)
    : QObject(parent)
{
}

McpServer::~McpServer()
{
    stop();
}

bool
McpServer::start(quint16 port, const QHostAddress &address)
{
    if (server != nullptr)
        stop();

    server = new QTcpServer(this);
    if (!server->listen(address, port)) {
        last_error = server->errorString();
        qWarning("MCP: cannot listen on %s:%u: %s", qPrintable(address.toString()),
                 static_cast<unsigned int>(port), qPrintable(last_error));
        delete server;
        server = nullptr;
        return false;
    }

    last_error  = QString();
    listen_port = server->serverPort();
    connect(server, &QTcpServer::newConnection, this, &McpServer::onNewConnection);
    qInfo("MCP: listening on %s (%d tool(s))", qPrintable(url()), tools.size());
    return true;
}

void
McpServer::stop()
{
    for (auto it = connections.begin(); it != connections.end(); ++it) {
        if (it.key() != nullptr)
            it.key()->abort();
    }
    connections.clear();

    if (server != nullptr) {
        server->close();
        delete server;
        server = nullptr;
    }
    listen_port = 0;
}

bool
McpServer::isRunning() const
{
    return (server != nullptr) && server->isListening();
}

quint16
McpServer::port() const
{
    return listen_port;
}

QString
McpServer::url() const
{
    return QStringLiteral("http://127.0.0.1:%1/mcp").arg(listen_port);
}

QString
McpServer::lastError() const
{
    return last_error;
}

void
McpServer::addTool(const McpTool &tool)
{
    tools.insert(tool.name, tool);
}

int
McpServer::toolCount() const
{
    return tools.size();
}

void
McpServer::onNewConnection()
{
    while (server->hasPendingConnections()) {
        QTcpSocket *socket = server->nextPendingConnection();
        if (socket == nullptr)
            continue;

        connections.insert(socket, ConnectionState {});

        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            onReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            onDisconnected(socket);
        });
    }
}

void
McpServer::onDisconnected(QTcpSocket *socket)
{
    connections.remove(socket);
    socket->deleteLater();
}

/* Collects bytes until one complete HTTP request (headers plus body) is
   buffered. Returns true once `body` holds the request body. */
bool
McpServer::parseRequest(QTcpSocket *socket, ConnectionState &state, QByteArray &body)
{
    if (!state.parsed_headers) {
        const int header_end = state.buffer.indexOf("\r\n\r\n");
        if (header_end < 0)
            return false;

        const QByteArray head = state.buffer.left(header_end);
        const QList<QByteArray> lines = head.split('\n');
        const QList<QByteArray> request_line = lines.value(0).trimmed().split(' ');
        if (request_line.size() >= 2) {
            state.method = QString::fromLatin1(request_line.at(0));
            const QString target = QString::fromLatin1(request_line.at(1));
            /* Tolerate a trailing slash or a custom path prefix. */
            if (!target.startsWith(QStringLiteral("/mcp")) && (target != QStringLiteral("/"))) {
                respond(socket, 404, "application/json",
                        QByteArray("{\"error\":\"unknown endpoint\"}"));
                state.answered = true;
                return false;
            }
        }

        for (int i = 1; i < lines.size(); i++) {
            const QByteArray line = lines.at(i).trimmed();
            const int colon = line.indexOf(':');
            if (colon <= 0)
                continue;
            const QByteArray name  = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "content-length")
                state.content_length = value.toInt();
        }

        state.parsed_headers = true;
        state.buffer.remove(0, header_end + 4);
    }

    if (state.buffer.size() < state.content_length)
        return false;

    body = state.buffer.left(state.content_length);
    state.buffer.clear();
    return true;
}

void
McpServer::respond(QTcpSocket *socket, int status, const QByteArray &content_type, const QByteArray &body)
{
    if ((socket == nullptr) || (socket->state() != QAbstractSocket::ConnectedState))
        return;

    const char *reason = "OK";
    switch (status) {
        case 202: reason = "Accepted"; break;
        case 204: reason = "No Content"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        default:  break;
    }

    QByteArray response;
    response.append(QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(status).arg(QString::fromLatin1(reason)).toUtf8());
    if (!content_type.isEmpty())
        response.append("Content-Type: ").append(content_type).append("\r\n");
    response.append("Content-Length: ").append(QByteArray::number(body.size())).append("\r\n");
    if (status == 405)
        response.append("Allow: POST\r\n");
    /* One request per connection keeps the state machine trivial, and the
       client simply opens a new connection for the next message. */
    response.append("Connection: close\r\n\r\n");
    response.append(body);

    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
}

void
McpServer::onReadyRead(QTcpSocket *socket)
{
    auto it = connections.find(socket);
    if (it == connections.end())
        return;

    ConnectionState &state = it.value();
    if (state.answered)
        return;

    state.buffer.append(socket->readAll());

    QByteArray body;
    if (!parseRequest(socket, state, body)) {
        if (state.answered) {
            /* An unsupported endpoint was already answered; the socket closes
               itself and the disconnected handler releases the state. */
            connections.remove(socket);
            socket->deleteLater();
        }
        return;
    }

    state.answered = true;

    /* Everything but POST is transport bookkeeping. */
    if (state.method == "GET") {
        /* 405 tells the client that this server offers no server-initiated
           SSE stream, which the Streamable HTTP specification allows. */
        respond(socket, 405, {}, {});
        return;
    }
    if (state.method == "DELETE") {
        respond(socket, 204, {}, {});
        return;
    }
    if (state.method != "POST") {
        respond(socket, 405, {}, {});
        return;
    }

    QJsonParseError parse_error {};
    const QJsonDocument document = QJsonDocument::fromJson(body, &parse_error);
    if ((parse_error.error != QJsonParseError::NoError) || !document.isObject()) {
        respond(socket, 400, "application/json",
                QByteArray("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}"));
        return;
    }

    handleMessage(socket, state, document.object());
}

void
McpServer::respondJson(QTcpSocket *socket, const QJsonObject &message)
{
    respond(socket, 200, "application/json", QJsonDocument(message).toJson(QJsonDocument::Compact));
}

void
McpServer::respondMethodNotFound(QTcpSocket *socket, const QJsonValue &id, const QString &method)
{
    QJsonObject error;
    error["code"]    = -32601;
    error["message"] = QStringLiteral("Method not found: %1").arg(method);

    QJsonObject response;
    response["jsonrpc"] = QStringLiteral("2.0");
    response["id"]      = id;
    response["error"]   = error;
    respondJson(socket, response);
}

void
McpServer::handleMessage(QTcpSocket *socket, ConnectionState &state, const QJsonObject &message)
{
    Q_UNUSED(state);
    const QString method = message.value("method").toString();

    /* A message without an id is a notification: it has no reply. */
    if (!message.contains("id")) {
        qDebug("MCP: notification \"%s\" received", qPrintable(method));
        respond(socket, 202, {}, {});
        return;
    }

    const QJsonValue id = message.value("id");

    if (method == QStringLiteral("initialize")) {
        handleInitialize(socket, message);
    } else if (method == QStringLiteral("tools/list")) {
        handleToolsList(socket, message);
    } else if (method == QStringLiteral("tools/call")) {
        handleToolsCall(socket, message);
    } else if (method == QStringLiteral("ping")) {
        QJsonObject response;
        response["jsonrpc"] = QStringLiteral("2.0");
        response["id"]      = id;
        response["result"]  = QJsonObject {};
        respondJson(socket, response);
    } else {
        respondMethodNotFound(socket, id, method);
    }
}

void
McpServer::handleInitialize(QTcpSocket *socket, const QJsonObject &message)
{
    const QJsonObject params          = message.value("params").toObject();
    const QString     client_version  = params.value("protocolVersion").toString();
    const QString     protocol_version = MCP_PROTOCOL_VERSIONS.contains(client_version)
                                             ? client_version
                                             : MCP_DEFAULT_PROTOCOL_VERSION;

    QJsonObject tools_capability;
    tools_capability["listChanged"] = false;

    QJsonObject capabilities;
    capabilities["tools"] = tools_capability;

    QJsonObject server_info;
    server_info["name"]    = QString::fromLatin1(EMU_NAME);
    server_info["title"]   = QStringLiteral("86Box");
    server_info["version"] = QString::fromLatin1(EMU_VERSION);

    QJsonObject result;
    result["protocolVersion"] = protocol_version;
    result["capabilities"]    = capabilities;
    result["serverInfo"]      = server_info;
    result["instructions"]    = MCP_INSTRUCTIONS;

    QJsonObject response;
    response["jsonrpc"] = QStringLiteral("2.0");
    response["id"]      = message.value("id");
    response["result"]  = result;

    qInfo("MCP: client initialized (protocol %s, client %s %s)",
          qPrintable(protocol_version),
          qPrintable(params.value("clientInfo").toObject().value("name").toString()),
          qPrintable(params.value("clientInfo").toObject().value("version").toString()));

    respondJson(socket, response);
}

QJsonObject
McpServer::toolListObject() const
{
    QJsonArray array;
    for (const McpTool &tool : tools) {
        QJsonObject entry;
        entry["name"]        = tool.name;
        entry["description"] = tool.description;
        entry["inputSchema"] = tool.input_schema;
        if (!tool.title.isEmpty())
            entry["title"] = tool.title;
        array.append(entry);
    }

    QJsonObject result;
    result["tools"] = array;
    return result;
}

void
McpServer::handleToolsList(QTcpSocket *socket, const QJsonObject &message)
{
    QJsonObject response;
    response["jsonrpc"] = QStringLiteral("2.0");
    response["id"]      = message.value("id");
    response["result"]  = toolListObject();
    respondJson(socket, response);
}

void
McpServer::handleToolsCall(QTcpSocket *socket, const QJsonObject &message)
{
    const QJsonObject params = message.value("params").toObject();
    const QString     name   = params.value("name").toString();
    const QJsonObject args   = params.value("arguments").toObject();
    const QJsonValue  id     = message.value("id");

    const auto finish = [](const QPointer<QTcpSocket> &guard, const QJsonValue &id, const McpToolResult &tool_result) {
        QTcpSocket *socket = guard.data();
        if ((socket == nullptr) || (socket->state() != QAbstractSocket::ConnectedState))
            return;

        QJsonArray content;

        /* Images come first: this is what makes a capture useful to a model
           that can see, while the JSON below stays the machine readable form. */
        if (!tool_result.image.isEmpty()) {
            QJsonObject image_entry;
            image_entry["type"]     = QStringLiteral("image");
            image_entry["data"]     = QString::fromLatin1(tool_result.image.toBase64());
            image_entry["mimeType"] = tool_result.image_mime.isEmpty()
                                          ? QStringLiteral("application/octet-stream")
                                          : tool_result.image_mime;
            content.append(image_entry);
        }

        QJsonObject content_entry;
        content_entry["type"] = QStringLiteral("text");
        if (tool_result.is_error) {
            const QString text = tool_result.data.value("error").toString();
            content_entry["text"] = text.isEmpty()
                                        ? QStringLiteral("The tool call failed.")
                                        : text;
        } else {
            content_entry["text"] = QString::fromUtf8(QJsonDocument(tool_result.data).toJson(QJsonDocument::Indented));
        }
        content.append(content_entry);

        QJsonObject result;
        result["content"] = content;
        if (tool_result.is_error)
            result["isError"] = true;

        QJsonObject response;
        response["jsonrpc"] = QStringLiteral("2.0");
        response["id"]      = id;
        response["result"]  = result;

        respond(socket, 200, "application/json", QJsonDocument(response).toJson(QJsonDocument::Compact));
    };

    const auto tool_it = tools.constFind(name);
    if (tool_it == tools.constEnd()) {
        finish(QPointer<QTcpSocket>(socket), id, McpToolResult::failure(QStringLiteral("Unknown tool: %1").arg(name)));
        return;
    }

    /* A tool may finish later from the event loop, and the completion may run
       after this function returned: share the completion state with the heap. */
    const auto guard     = std::make_shared<QPointer<QTcpSocket>>(socket);
    const auto completed = std::make_shared<bool>(false);

    const auto done = [guard, completed, id, finish](const McpToolResult &tool_result) {
        if (*completed)
            return;
        *completed = true;
        finish(*guard, id, tool_result);
    };

    QTimer::singleShot(MCP_TOOL_CALL_TIMEOUT_MS, socket, [done, name] {
        qWarning("MCP: tool \"%s\" timed out", qPrintable(name));
        done(McpToolResult::failure(QStringLiteral("Tool \"%1\" timed out.").arg(name)));
    });

    qInfo("MCP: tools/call %s", qPrintable(name));

    /* Arguments are optional for tools without required parameters. */
    tool_it->handler(args, done);
}
