/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          OCR for the MCP server: reads the text off a captured screen, so
 *          an agent that cannot look at images still learns what the emulated
 *          machine is showing.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_OCR_HPP
#define QT_MCP_OCR_HPP

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace McpOcr {
struct Line {
    QString text;
    double  confidence = -1.0; /* -1 when the engine does not report one */
    int     x = 0;             /* Box in captured pixels, top left origin. */
    int     y = 0;
    int     width  = 0;
    int     height = 0;
};

struct Result {
    QString       text;
    QVector<Line> lines;
    QString       engine;
    QString       alternative_text;
    QString       alternative_engine;
    QString       error;
    int           scale  = 1; /* Upscale factor the engines worked on */
    int           width  = 0; /* Dimensions of the capture */
    int           height = 0;

    [[nodiscard]] bool ok() const { return error.isEmpty(); }
};

/* Names of the engines this build and machine can use, best first. */
QStringList engines();

/* Reads the text of a captured screen. `engine` is one of engines(), or
   "auto" to run every available engine and keep the more convincing result.
   `scale` upscales the capture before recognition (0 picks a factor from the
   capture size); low resolution bitmap fonts need it. Runs synchronously. */
Result recognize(const QByteArray &png, const QString &engine, int scale);

#ifdef Q_OS_MACOS
/* Backed by the system's Vision framework; see qt_mcp_ocr_vision.mm. */
bool   visionAvailable();
Result visionRecognize(const QImage &image);
#endif
}

#endif // QT_MCP_OCR_HPP
