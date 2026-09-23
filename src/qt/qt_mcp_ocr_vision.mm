/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          OCR backend for the MCP server that uses the text recognition of
 *          the system's Vision framework (macOS 10.15 and newer). The
 *          framework is only weak-linked, so builds still run on older system
 *          versions, where this backend simply reports itself unavailable.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_ocr.hpp"

#ifdef Q_OS_MACOS

#include <QImage>

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <algorithm>
#include <vector>

namespace McpOcr {
bool
visionAvailable()
{
    if (@available(macOS 10.15, *))
        return true;

    return false;
}

Result
visionRecognize(const QImage &image)
{
    Result result;
    result.engine = QStringLiteral("apple-vision");

    if (!visionAvailable()) {
        result.error = QStringLiteral("The Vision framework needs macOS 10.15 or newer.");
        return result;
    }

    if (image.isNull() || (image.width() <= 0) || (image.height() <= 0)) {
        result.error = QStringLiteral("The captured screen is empty.");
        return result;
    }

    /* The frame buffers hold 0xXXRRGGBB words, which is the byte order Vision
       wants when the alpha channel is skipped. */
    const QImage source = image.convertToFormat(QImage::Format_RGB32);
    const int    width  = source.width();
    const int    height = source.height();

    const size_t    size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    CFDataRef       data = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(source.constBits()),
                                        static_cast<CFIndex>(size));
    if (data == nullptr) {
        result.error = QStringLiteral("The captured screen could not be handed to Vision.");
        return result;
    }

    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef   space    = CGColorSpaceCreateDeviceRGB();
    CGImageRef        cg_image = nullptr;

    if ((provider != nullptr) && (space != nullptr)) {
        cg_image = CGImageCreate(width, height, 8, 32, width * 4, space,
                                 static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst)
                                     | kCGBitmapByteOrder32Little,
                                 provider, nullptr, false, kCGRenderingIntentDefault);
    }

    if (cg_image == nullptr) {
        if (space != nullptr)
            CGColorSpaceRelease(space);
        if (provider != nullptr)
            CGDataProviderRelease(provider);
        CFRelease(data);
        result.error = QStringLiteral("The captured screen could not be handed to Vision.");
        return result;
    }

    @autoreleasepool {
        VNRecognizeTextRequest *request = [[VNRecognizeTextRequest alloc] init];
        request.recognitionLevel        = VNRequestTextRecognitionLevelAccurate;
        request.usesLanguageCorrection  = YES;

        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:cg_image
                                                                               options:@{}];
        NSError *error = nil;
        if (![handler performRequests:@[ request ] error:&error] || (error != nil)) {
            result.error = QStringLiteral("Vision could not read the captured screen: %1")
                               .arg(QString::fromNSString(error.localizedDescription ?: @"unknown error"));
        } else {
            /* Vision reports normalized boxes with a bottom left origin; sort
               them the way a reader goes through the screen. */
            NSMutableArray<VNRecognizedTextObservation *> *observations =
                [NSMutableArray arrayWithArray:(request.results ?: @[])];
            [observations sortUsingComparator:^NSComparisonResult(VNRecognizedTextObservation *a,
                                                                  VNRecognizedTextObservation *b) {
                const CGFloat row_a = a.boundingBox.origin.y;
                const CGFloat row_b = b.boundingBox.origin.y;
                if (fabs(row_a - row_b) > 0.01)
                    return (row_a > row_b) ? NSOrderedAscending : NSOrderedDescending;
                if (a.boundingBox.origin.x < b.boundingBox.origin.x)
                    return NSOrderedAscending;
                if (a.boundingBox.origin.x > b.boundingBox.origin.x)
                    return NSOrderedDescending;
                return NSOrderedSame;
            }];

            QStringList texts;
            for (VNRecognizedTextObservation *observation in observations) {
                VNRecognizedText *candidate = [observation topCandidates:1].firstObject;
                if (candidate == nil)
                    continue;

                Line line;
                line.text       = QString::fromNSString(candidate.string);
                line.confidence = candidate.confidence;

                const CGRect box = observation.boundingBox;
                line.x           = static_cast<int>(box.origin.x * width);
                line.y           = static_cast<int>((1.0 - box.origin.y - box.size.height) * height);
                line.width       = static_cast<int>(box.size.width * width);
                line.height      = static_cast<int>(box.size.height * height);

                result.lines.append(line);
                texts.append(line.text);
            }

            result.text = texts.join(QLatin1Char('\n'));
            if (result.text.isEmpty())
                result.error = QStringLiteral("Vision found no text on the captured screen.");
        }
    }

    CGImageRelease(cg_image);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);

    return result;
}
}

#endif /* Q_OS_MACOS */
