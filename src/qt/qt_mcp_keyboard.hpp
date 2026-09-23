/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Keyboard injection for the MCP server: types text or presses keys
 *          in the emulated machine, so an agent can drive a guest it can
 *          already see.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#ifndef QT_MCP_KEYBOARD_HPP
#define QT_MCP_KEYBOARD_HPP

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>

namespace McpKeyboard {
/* Scancodes are the set 1 codes the emulator's keyboard core takes; extended
   keys carry 0x100 (for example 0x148 for the up arrow). */
struct Stroke {
    QVector<uint16_t> codes; /* Pressed in order, released in reverse */
};

constexpr int DEFAULT_DELAY_MS = 30; /* Between press and release, and between keystrokes */
constexpr int MAX_TEXT_CHARS   = 160;
constexpr int MAX_KEYS         = 32;

/* One keystroke takes two delays, so the caller must keep the total bounded. */
constexpr int MAX_DURATION_MS = 20000;

/* Turns a request into keystrokes. Accepts either "text" (typed literally) or
   "keys" (names such as "enter" or combinations such as "ctrl+alt+del").
   Returns false and fills `error` when the request cannot be carried out. */
bool build(const QJsonObject &request, QVector<Stroke> &strokes, int &delay_ms, QString &error);

/* Injects the keystrokes, spacing them out so a slow guest does not drop any,
   and reports the number of keystrokes and the time they took. */
void play(QObject *context, const QVector<Stroke> &strokes, int delay_ms,
          std::function<void(int keystrokes, int duration_ms)> done);

/* Names accepted in `keys`, for the tool description and error messages. */
QStringList keyNames();
}

#endif // QT_MCP_KEYBOARD_HPP
