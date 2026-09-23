/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Screen capture for the MCP server: turns the frame the emulated
 *          display currently holds into a PNG an agent can look at, without
 *          writing anything to disk.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_SCREEN_HPP
#define QT_MCP_SCREEN_HPP

#include <QByteArray>
#include <QString>

namespace McpScreen {
struct Capture {
    QByteArray png;      /* Encoded PNG, empty on failure */
    int        width  = 0;
    int        height = 0;
    QString    error;    /* Reason the capture failed, empty on success */
};

/* Captures the frame of one monitor (0-based). Anything the emulated display
   has drawn can be captured, including while the machine is paused. */
Capture capture(int monitor_index);
}

#endif // QT_MCP_SCREEN_HPP
