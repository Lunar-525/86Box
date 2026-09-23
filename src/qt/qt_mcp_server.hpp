/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Model Context Protocol (MCP) server: exposes 86Box functionality
 *          to MCP clients (AI agents) over the Streamable HTTP transport on
 *          the loopback interface.
 *
 *          The server itself is transport and protocol only. Every capability
 *          is registered as an McpTool by a feature module, so nothing in
 *          this file knows about virtual machines.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_SERVER_HPP
#define QT_MCP_SERVER_HPP

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTcpServer>

#include <functional>

class QTcpSocket;

/* Loopback port used unless "mcp_port" is set in the VM manager's settings. */
constexpr quint16 MCP_DEFAULT_PORT = 8674;
/* Ports below this one are reserved for privileged services. */
constexpr int MCP_MIN_PORT = 1024;

/* Result of a single MCP tool call. `data` is the machine readable payload;
   it is rendered into the tool result content as pretty-printed JSON. A result
   may carry one image instead of (or next to) that JSON, which is how an agent
   gets to look at an emulated screen. */
class McpToolResult {
public:
    QJsonObject data;
    QByteArray  image;      /* Encoded image bytes, empty when there is none */
    QString     image_mime; /* Media type of `image` */
    bool        is_error = false;

    static McpToolResult ok(const QJsonObject &data);
    static McpToolResult failure(const QString &message);
    static McpToolResult withImage(const QByteArray &image, const QString &media_type,
                                   const QJsonObject &data);
};

using McpDone        = std::function<void(const McpToolResult &)>;
using McpToolHandler = std::function<void(const QJsonObject &arguments, McpDone done)>;

/* One callable capability advertised through tools/list. */
struct McpTool {
    QString        name;
    QString        title;
    QString        description;
    QJsonObject    input_schema;
    McpToolHandler handler;
};

/* Minimal, self-contained MCP server speaking the Streamable HTTP transport:
   POST carries one JSON-RPC message and the reply is returned as
   "application/json", GET answers 405 (no server-initiated SSE stream) and
   DELETE terminates a session. Requests are handled asynchronously, so a tool
   is free to complete its McpDone later from the Qt event loop. */
class McpServer : public QObject {
    Q_OBJECT

public:
    explicit McpServer(QObject *parent = nullptr);
    ~McpServer() override;

    /* Binds to `address`: `port`; port 0 picks a free port. */
    bool start(quint16 port, const QHostAddress &address = QHostAddress::LocalHost);
    void stop();

    [[nodiscard]] bool    isRunning() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] QString url() const;
    /* Reason the last start() failed, empty when it succeeded. */
    [[nodiscard]] QString lastError() const;

    void addTool(const McpTool &tool);
    [[nodiscard]] int toolCount() const;

private:
    struct ConnectionState {
        QByteArray buffer;
        bool       parsed_headers = false;
        bool       answered       = false;
        QString    method;
        int        content_length = 0;
    };

    QTcpServer                   *server;
    quint16                       listen_port = 0;
    QString                       last_error;
    QHash<QString, McpTool>       tools;
    QHash<QTcpSocket *, ConnectionState> connections;

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void onDisconnected(QTcpSocket *socket);

    static bool parseRequest(QTcpSocket *socket, ConnectionState &state, QByteArray &body);
    static void respond(QTcpSocket *socket, int status, const QByteArray &content_type, const QByteArray &body);

    void handleMessage(QTcpSocket *socket, ConnectionState &state, const QJsonObject &message);
    void handleInitialize(QTcpSocket *socket, const QJsonObject &message);
    void handleToolsList(QTcpSocket *socket, const QJsonObject &message);
    void handleToolsCall(QTcpSocket *socket, const QJsonObject &message);
    void respondJson(QTcpSocket *socket, const QJsonObject &message);
    void respondMethodNotFound(QTcpSocket *socket, const QJsonValue &id, const QString &method);

    [[nodiscard]] QJsonObject toolListObject() const;
};

#endif // QT_MCP_SERVER_HPP
