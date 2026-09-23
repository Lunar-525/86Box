/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Floppy and optical drive control for the MCP server.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_media.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include "qt_mediamenu.hpp"

extern "C" {
#include <86box/timer.h>
#include <86box/cdrom.h>
#include <86box/fdd.h>
#include <86box/fdd_tape.h>
#include <86box/plat.h>
}

namespace McpMedia {
namespace {
/* MediaMenu, which does the actual work, only exists once the machine's window
   is up; until then there is nothing to load media into. */
bool
mediaMenuReady()
{
    return MediaMenu::ptr != nullptr;
}

bool
floppyPresent(int index)
{
    if ((index < 0) || (index >= FDD_NUM))
        return false;

    /* Type 0 is "no drive"; the tape drive shares the select line but takes no
       floppy images. */
    return (fdd_get_type(index) != 0) && !fdd_tape_present(index);
}

bool
opticalPresent(int index)
{
    if ((index < 0) || (index >= CDROM_NUM))
        return false;

    return cdrom[index].priv != nullptr;
}
} /* namespace */

bool
parseTarget(const QString &drive, Target &target, QString &error)
{
    const QString name = drive.trimmed().toLower();
    if (name.isEmpty()) {
        error = QStringLiteral("No drive was named: pass \"a\", \"b\" for a floppy drive, "
                               "or \"cd1\"..\"cd8\" for an optical drive.");
        return false;
    }

    static const QRegularExpression floppy(QStringLiteral("^(?:([ab])|(?:floppy|fdd|f)(\\d))$"));
    static const QRegularExpression optical(QStringLiteral("^(?:cd|cdrom|dvd)(\\d)$"));

    if (const auto match = floppy.match(name); match.hasMatch()) {
        int index = 0;
        if (!match.captured(1).isEmpty())
            index = match.captured(1) == QStringLiteral("a") ? 0 : 1;
        else
            index = match.captured(2).toInt() - 1;

        target.index   = index;
        target.optical = false;
        target.name    = (index < 2) ? QString(QChar('a' + index)) : QStringLiteral("floppy%1").arg(index + 1);
        return true;
    }

    if (const auto match = optical.match(name); match.hasMatch()) {
        target.index   = match.captured(1).toInt() - 1;
        target.optical = true;
        target.name    = QStringLiteral("cd%1").arg(target.index + 1);
        return true;
    }

    error = QStringLiteral("Unknown drive \"%1\": use \"a\" or \"b\" for a floppy drive, "
                           "or \"cd1\"..\"cd8\" for an optical drive.")
                .arg(drive);
    return false;
}

QJsonObject
state(const Target &target)
{
    QJsonObject object;
    object["drive"] = target.name;

    if (target.optical) {
        object["kind"] = QStringLiteral("optical");

        if (!opticalPresent(target.index)) {
            object["present"] = false;
            return object;
        }

        object["present"] = true;
        const bool empty  = cdrom_is_empty(target.index) != 0;
        object["empty"]   = empty;
        object["path"]    = empty ? QString() : QString::fromUtf8(cdrom[target.index].image_path);
        object["playing"] = cdrom_is_playing(target.index) != 0;
        object["paused"]  = cdrom_is_paused(target.index) != 0;
        return object;
    }

    object["kind"] = QStringLiteral("floppy");

    if (!floppyPresent(target.index)) {
        object["present"] = false;
        return object;
    }

    object["present"] = true;
    const bool empty  = drive_empty[target.index] || (floppyfns[target.index][0] == '\0');
    object["empty"]   = empty;
    object["path"]    = empty ? QString() : QString::fromUtf8(floppyfns[target.index]);
    object["write_protected"] = writeprot[target.index] != 0;
    object["type"]            = QString::fromUtf8(fdd_getname(fdd_get_type(target.index)));
    return object;
}

QJsonObject
drivesState()
{
    QJsonObject result;

    QJsonArray floppies;
    for (int i = 0; i < FDD_NUM; i++) {
        if (!floppyPresent(i))
            continue;

        Target target;
        target.index   = i;
        target.optical = false;
        target.name    = (i < 2) ? QString(QChar('a' + i)) : QStringLiteral("floppy%1").arg(i + 1);
        floppies.append(state(target));
    }

    QJsonArray opticals;
    for (int i = 0; i < CDROM_NUM; i++) {
        if (!opticalPresent(i))
            continue;

        Target target;
        target.index   = i;
        target.optical = true;
        target.name    = QStringLiteral("cd%1").arg(i + 1);
        opticals.append(state(target));
    }

    result["floppy"]  = floppies;
    result["optical"] = opticals;
    return result;
}

bool
load(const Target &target, const QString &path, bool write_protected, QJsonObject &state_out, QString &error)
{
    if (path.isEmpty()) {
        error = QStringLiteral("A \"path\" to an image (or, for an optical drive, a folder) is required.");
        return false;
    }

    if (!mediaMenuReady()) {
        error = QStringLiteral("The media menu is not available in this process.");
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        error = QStringLiteral("There is nothing at %1.").arg(path);
        return false;
    }

    const QByteArray utf8 = path.toUtf8();

    if (target.optical) {
        if (!opticalPresent(target.index)) {
            error = QStringLiteral("The machine has no optical drive %1.").arg(target.name);
            return false;
        }
        /* Both an image and a folder go through the same entry point: a
           folder is mounted as a virtual ISO, which is what the media menu's
           "load folder" entry does as well. */
        cdrom_mount(static_cast<uint8_t>(target.index), const_cast<char *>(utf8.constData()));

        state_out = state(target);
        if (state_out.value(QStringLiteral("empty")).toBool()) {
            error = QStringLiteral("The image %1 could not be mounted; the optical drive is still empty.")
                        .arg(path);
            return false;
        }
        return true;
    }

    if (!floppyPresent(target.index)) {
        error = QStringLiteral("The machine has no floppy drive %1.").arg(target.name);
        return false;
    }
    if (info.isDir()) {
        error = QStringLiteral("A floppy drive takes an image file, not a folder.");
        return false;
    }

    floppy_mount(static_cast<uint8_t>(target.index), const_cast<char *>(utf8.constData()),
                 write_protected ? 1 : 0);

    state_out = state(target);
    if (state_out.value(QStringLiteral("empty")).toBool()) {
        error = QStringLiteral("The image %1 could not be mounted; check that it fits drive %2.")
                    .arg(path, target.name);
        return false;
    }
    return true;
}

bool
eject(const Target &target, QJsonObject &state_out, QString &error)
{
    if (target.optical) {
        if (!opticalPresent(target.index)) {
            error = QStringLiteral("The machine has no optical drive %1.").arg(target.name);
            return false;
        }
        cdrom_eject(static_cast<uint8_t>(target.index));
    } else {
        if (!floppyPresent(target.index)) {
            error = QStringLiteral("The machine has no floppy drive %1.").arg(target.name);
            return false;
        }
        if (drive_empty[target.index]) {
            error = QStringLiteral("Drive %1 is already empty.").arg(target.name);
            return false;
        }
        floppy_eject(static_cast<uint8_t>(target.index));
    }

    state_out = state(target);
    return true;
}
}
