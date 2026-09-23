/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Floppy and optical drive control for the MCP server: loading and
 *          ejecting media in a running machine, exactly like the media menu.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_MEDIA_HPP
#define QT_MCP_MEDIA_HPP

#include <QJsonObject>
#include <QString>

namespace McpMedia {
/* One drive of the emulated machine: "a", "b", "floppy1".."floppy4",
   "cd1".."cd8", or "cdrom1".."cdrom8". */
struct Target {
    int     index   = 0;
    bool    optical = false;
    QString name; /* Normalised drive name, such as "a" or "cd1" */
};

bool parseTarget(const QString &drive, Target &target, QString &error);

/* Both return false and fill `error` when the drive is absent or the media
   cannot be used; on success the drive's new state is in `state`. */
bool load(const Target &target, const QString &path, bool write_protected,
          QJsonObject &state, QString &error);
bool eject(const Target &target, QJsonObject &state, QString &error);

/* What is in the drive right now. */
QJsonObject state(const Target &target);

/* Every drive the machine really has, with the media in it. This is what the
   configuration file cannot tell: it leaves out drive types that equal the
   built-in default, and it lists optical drives that are not attached. */
QJsonObject drivesState();
}

#endif // QT_MCP_MEDIA_HPP
