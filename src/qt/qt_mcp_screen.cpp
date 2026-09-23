/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Screen capture for the MCP server.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_screen.hpp"

#include <QBuffer>
#include <QImage>
#include <QVector>

extern "C" {
#include <86box/plat.h>
#include <86box/video.h>
}

namespace McpScreen {
Capture
capture(int monitor_index)
{
    Capture result;

    if ((monitor_index < 0) || (monitor_index >= MONITORS_NUM)) {
        result.error = QStringLiteral("No such monitor: %1").arg(monitor_index + 1);
        return result;
    }

    /* Taking the blit lock keeps the emulation thread from redrawing the frame
       while it is being copied, exactly like the screenshot menu entries do. */
    startblit();

    int width  = 0;
    int height = 0;
    if (video_capture_frame_monitor(nullptr, &width, &height, monitor_index) != 0) {
        endblit();
        result.error = QStringLiteral("The monitor has not drawn a frame yet.");
        return result;
    }

    QVector<uint32_t> pixels(width * height);
    if (video_capture_frame_monitor(pixels.data(), &width, &height, monitor_index) != 0) {
        endblit();
        result.error = QStringLiteral("The frame buffer could not be read.");
        return result;
    }

    endblit();

    /* Frame buffers store 0xXXRRGGBB words, which is what Format_RGB32 holds on
       a little endian host. */
    const QImage frame(reinterpret_cast<const uchar *>(pixels.constData()), width, height,
                       width * static_cast<int>(sizeof(uint32_t)), QImage::Format_RGB32);

    QBuffer buffer(&result.png);
    if (!buffer.open(QIODevice::WriteOnly) || !frame.save(&buffer, "PNG")) {
        result.png.clear();
        result.error = QStringLiteral("The captured frame could not be encoded as PNG.");
        return result;
    }

    result.width  = width;
    result.height = height;
    return result;
}
}
