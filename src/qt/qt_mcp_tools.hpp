/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          MCP tools exposing the VM manager: the virtual machine list,
 *          virtual machine creation, per-machine configuration (86box.cfg)
 *          and the live performance counters of a running machine.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_TOOLS_HPP
#define QT_MCP_TOOLS_HPP

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>

#include "qt_mcp_server.hpp"

class VMManagerMain;
class VMManagerSystem;

/* A screen capture in flight: which machine and monitor it belongs to. */
struct ScreenRequest {
    QString machine;
    QString display_name;
    int     monitor = 1;
};

/* Registers the 86Box tools on `server`. The manager is used for live state
   (which machine is running) and to refresh the list after changes. */
class McpTools86Box final : public QObject {
    Q_OBJECT

public:
    explicit McpTools86Box(VMManagerMain *manager, QObject *parent = nullptr);

    void registerTools(McpServer &server);

private:
    VMManagerMain *_manager;
    quint64        _next_request_id = 0;

    McpToolResult toolList(const QJsonObject &arguments);
    McpToolResult toolCreate(const QJsonObject &arguments);
    McpToolResult toolConfigGet(const QJsonObject &arguments);
    McpToolResult toolConfigSet(const QJsonObject &arguments);
    McpToolResult toolPower(const QJsonObject &arguments);
    void          toolPerformance(const QJsonObject &arguments, McpDone done);
    void          toolScreenshot(const QJsonObject &arguments, McpDone done);
    void          toolScreenText(const QJsonObject &arguments, McpDone done);
    void          toolKeyboard(const QJsonObject &arguments, McpDone done);
    void          toolMedia(const QJsonObject &arguments, McpDone done);

    /* Asks a running machine for its screen; `done` receives the PNG capture. */
    void requestCapture(const QJsonObject &arguments,
                        std::function<void(const ScreenRequest &request, const QByteArray &png,
                                           const QString &error)> done);

    /* Finds the machine a request refers to, which has to be running. */
    VMManagerSystem *findRunningMachine(const QString &requested, QString &error);
};

#endif // QT_MCP_TOOLS_HPP
