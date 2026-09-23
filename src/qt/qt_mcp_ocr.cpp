/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          OCR for the MCP server.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_ocr.hpp"

#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>

namespace McpOcr {
namespace {
/* Engines other than Vision shell out to an OCR binary. */
QString
tesseractBinary()
{
    static const QString binary = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    return binary;
}

bool
haveVision()
{
#ifdef Q_OS_MACOS
    return visionAvailable();
#else
    return false;
#endif
}

/* Neither engine copes with the bitmap fonts of a text mode screen at their
   native size, while a machine that already draws many pixels needs no help:
   pick a factor that keeps the longest side near 2048. */
int
automaticScale(int width, int height)
{
    const int longest = std::max(width, height);
    if (longest <= 0)
        return 1;

    return std::clamp(2048 / longest, 1, 4);
}

QImage
upscale(const QImage &image, int scale)
{
    if (scale <= 1)
        return image;

    return image.scaled(image.width() * scale, image.height() * scale,
                        Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

/* Tesseract reports words; they are grouped back into the lines a reader sees,
   and the boxes come back in capture coordinates. */
Result
runTesseract(const QImage &image, int scale)
{
    Result result;
    result.engine = QStringLiteral("tesseract");
    result.scale  = scale;

    const QString binary = tesseractBinary();
    if (binary.isEmpty()) {
        result.error = QStringLiteral("The tesseract OCR program is not installed.");
        return result;
    }

    QTemporaryDir directory;
    if (!directory.isValid()) {
        result.error = QStringLiteral("No temporary directory is available for the OCR input.");
        return result;
    }

    /* Tesseract's PNG reader rejects images with an alpha channel, and the
       capture carries one, so hand it plain RGB. */
    const QString input = directory.filePath(QStringLiteral("screen.png"));
    if (!image.convertToFormat(QImage::Format_RGB888).save(input, "PNG")) {
        result.error = QStringLiteral("The captured screen could not be prepared for OCR.");
        return result;
    }

    QProcess process;
    process.start(binary, QStringList { input, QStringLiteral("stdout"),
                                        QStringLiteral("-l"), QStringLiteral("eng"),
                                        QStringLiteral("--psm"), QStringLiteral("6"),
                                        QStringLiteral("tsv") });
    if (!process.waitForStarted(5000)) {
        result.error = QStringLiteral("The tesseract OCR program could not be started.");
        return result;
    }
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(1000);
        result.error = QStringLiteral("Tesseract did not finish in time.");
        return result;
    }
    if (process.exitStatus() != QProcess::NormalExit) {
        result.error = QStringLiteral("Tesseract was terminated unexpectedly.");
        return result;
    }

    /* TSV rows: level page block paragraph line word left top width height conf text.
       Level 5 is a word; consecutive words of one line are joined. */
    const QString output = QString::fromUtf8(process.readAllStandardOutput());
    QStringList   words;
    Line          line;
    int           block         = -1;
    int           line_number   = -1;
    double        confidence    = 0.0;
    int           confidence_n  = 0;

    const auto flush = [&]() {
        if (!words.isEmpty()) {
            Line finished = line;
            finished.text       = words.join(QLatin1Char(' '));
            finished.confidence = (confidence_n > 0) ? (confidence / confidence_n) : -1.0;
            finished.x          = finished.x / scale;
            finished.y          = finished.y / scale;
            finished.width      = std::max(1, finished.width / scale);
            finished.height     = std::max(1, finished.height / scale);
            result.lines.append(finished);
        }

        words.clear();
        line          = Line {};
        block         = -1;
        line_number   = -1;
        confidence    = 0.0;
        confidence_n  = 0;
    };

    const QStringList rows = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = 1; i < rows.size(); i++) { /* Row 0 is the header */
        const QStringList fields = rows.at(i).split(QLatin1Char('\t'));
        if ((fields.size() < 12) || (fields.at(0).toInt() != 5))
            continue;

        const QString word = fields.at(11);
        if (word.isEmpty())
            continue;

        const int word_block = fields.at(2).toInt();
        const int word_line  = fields.at(4).toInt();
        if (!words.isEmpty() && ((word_block != block) || (word_line != line_number)))
            flush();

        const int left   = fields.at(6).toInt();
        const int top    = fields.at(7).toInt();
        const int width  = fields.at(8).toInt();
        const int height = fields.at(9).toInt();

        if (words.isEmpty()) {
            block       = word_block;
            line_number = word_line;
            line.x      = left;
            line.y      = top;
            line.width  = width;
            line.height = height;
        } else {
            /* Grow the box so it covers this word as well. */
            const int right  = std::max(line.x + line.width, left + width);
            const int bottom = std::max(line.y + line.height, top + height);
            line.x           = std::min(line.x, left);
            line.y           = std::min(line.y, top);
            line.width       = right - line.x;
            line.height      = bottom - line.y;
        }

        words.append(word);
        const double word_confidence = fields.at(10).toDouble();
        if (word_confidence > 0.0) {
            confidence += word_confidence;
            confidence_n++;
        }
    }
    flush();

    QStringList texts;
    for (const Line &each : result.lines)
        texts.append(each.text);
    result.text = texts.join(QLatin1Char('\n'));

    return result;
}

Result
runEngine(const QString &name, const QImage &image, int scale)
{
    if (name == QStringLiteral("apple-vision")) {
#ifdef Q_OS_MACOS
        Result result = visionRecognize(image);
        result.scale  = scale;
        return result;
#else
        Result result;
        result.engine = QStringLiteral("apple-vision");
        result.error  = QStringLiteral("The Vision OCR engine is only available in macOS builds.");
        return result;
#endif
    }

    return runTesseract(image, scale);
}

/* How much text a result actually carries; used to pick between engines. */
int
weight(const Result &candidate)
{
    int count = 0;
    for (const QChar &character : candidate.text) {
        if (!character.isSpace())
            count++;
    }
    return count;
}
} /* namespace */

QStringList
engines()
{
    QStringList available;

    if (haveVision())
        available.append(QStringLiteral("apple-vision"));
    if (!tesseractBinary().isEmpty())
        available.append(QStringLiteral("tesseract"));

    return available;
}

Result
recognize(const QByteArray &png, const QString &engine, int scale)
{
    Result result;

    const QImage capture = QImage::fromData(png, "PNG");
    if (capture.isNull()) {
        result.error = QStringLiteral("The captured screen could not be decoded.");
        return result;
    }

    result.width  = capture.width();
    result.height = capture.height();

    const int    factor    = (scale > 0) ? std::clamp(scale, 1, 6) : automaticScale(capture.width(), capture.height());
    const QImage prepared  = upscale(capture, factor);
    const QStringList available = engines();

    if (available.isEmpty()) {
        result.error = QStringLiteral("No OCR engine is available on this system: install tesseract, "
                                      "or use a macOS build with Vision support.");
        return result;
    }

    const QString wanted = engine.trimmed().toLower();

    QVector<Result> attempts;
    if (wanted.isEmpty() || (wanted == QStringLiteral("auto"))) {
        for (const QString &name : available)
            attempts.append(runEngine(name, prepared, factor));
    } else if (!available.contains(wanted)) {
        result.error = QStringLiteral("The OCR engine \"%1\" is not available; this build has %2.")
                           .arg(wanted, available.join(QStringLiteral(", ")));
        return result;
    } else {
        attempts.append(runEngine(wanted, prepared, factor));
    }

    /* Keep the attempt that read the most and offer the runner up as an
       alternative: the engines misread different characters. */
    int best = -1;
    for (int i = 0; i < attempts.size(); i++) {
        if (!attempts.at(i).ok())
            continue;
        if ((best < 0) || (weight(attempts.at(i)) > weight(attempts.at(best))))
            best = i;
    }

    if (best < 0) {
        result.error = attempts.isEmpty() ? QStringLiteral("No OCR engine produced a result.")
                                          : attempts.first().error;
        return result;
    }

    result        = attempts.at(best);
    result.width  = capture.width();
    result.height = capture.height();
    result.scale  = factor;

    for (int i = 0; i < attempts.size(); i++) {
        if ((i == best) || !attempts.at(i).ok() || attempts.at(i).text.isEmpty())
            continue;
        if (attempts.at(i).text == result.text)
            continue;
        result.alternative_text   = attempts.at(i).text;
        result.alternative_engine = attempts.at(i).engine;
        break;
    }

    return result;
}
}
