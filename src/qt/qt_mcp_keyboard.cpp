/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Keyboard injection for the MCP server.
 *
 * Authors: 86Box contributors
 *
 *          Copyright 2025 86Box contributors
 */
#include "qt_mcp_keyboard.hpp"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>
#include <QTimer>

#include <memory>

extern "C" {
#include <86box/keyboard.h>
#include <86box/plat.h>
}

namespace McpKeyboard {
namespace {
constexpr uint16_t SCAN_LEFT_SHIFT = 0x2A;

/* Characters of a US layout: the key to press and whether Shift is needed. */
struct CharacterKey {
    uint16_t code;
    bool     shifted;
};

const QHash<QChar, CharacterKey> &
characterTable()
{
    static const QHash<QChar, CharacterKey> table = [] {
        QHash<QChar, CharacterKey> keys;

        /* Letters, in keyboard order. */
        const char *rows[] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
        const uint16_t row_codes[][10] = {
            { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19 },
            { 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x00 },
            { 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x00, 0x00, 0x00 },
        };

        for (int row = 0; row < 3; row++) {
            for (int i = 0; rows[row][i] != '\0'; i++) {
                if (row_codes[row][i] == 0x00)
                    continue;
                const QChar lower = QLatin1Char(rows[row][i]);
                const QChar upper = lower.toUpper();
                keys.insert(lower, CharacterKey { row_codes[row][i], false });
                keys.insert(upper, CharacterKey { row_codes[row][i], true });
            }
        }

        /* Digits and the punctuation of the US layout. */
        const struct {
            QChar    plain;
            QChar    shifted;
            uint16_t code;
        } punctuation[] = {
            { QLatin1Char('1'), QLatin1Char('!'), 0x02 },
            { QLatin1Char('2'), QLatin1Char('@'), 0x03 },
            { QLatin1Char('3'), QLatin1Char('#'), 0x04 },
            { QLatin1Char('4'), QLatin1Char('$'), 0x05 },
            { QLatin1Char('5'), QLatin1Char('%'), 0x06 },
            { QLatin1Char('6'), QLatin1Char('^'), 0x07 },
            { QLatin1Char('7'), QLatin1Char('&'), 0x08 },
            { QLatin1Char('8'), QLatin1Char('*'), 0x09 },
            { QLatin1Char('9'), QLatin1Char('('), 0x0A },
            { QLatin1Char('0'), QLatin1Char(')'), 0x0B },
            { QLatin1Char('-'), QLatin1Char('_'), 0x0C },
            { QLatin1Char('='), QLatin1Char('+'), 0x0D },
            { QLatin1Char('['), QLatin1Char('{'), 0x1A },
            { QLatin1Char(']'), QLatin1Char('}'), 0x1B },
            { QLatin1Char(';'), QLatin1Char(':'), 0x27 },
            { QLatin1Char('\''), QLatin1Char('"'), 0x28 },
            { QLatin1Char('`'), QLatin1Char('~'), 0x29 },
            { QLatin1Char('\\'), QLatin1Char('|'), 0x2B },
            { QLatin1Char(','), QLatin1Char('<'), 0x33 },
            { QLatin1Char('.'), QLatin1Char('>'), 0x34 },
            { QLatin1Char('/'), QLatin1Char('?'), 0x35 },
            { QLatin1Char(' '), QChar(0x00), 0x39 },
            { QLatin1Char('\t'), QChar(0x00), 0x0F },
            { QLatin1Char('\n'), QChar(0x00), 0x1C },
            { QLatin1Char('\r'), QChar(0x00), 0x1C },
            { QLatin1Char('\b'), QChar(0x00), 0x0E },
        };

        for (const auto &entry : punctuation) {
            keys.insert(entry.plain, CharacterKey { entry.code, false });
            if (entry.shifted != QChar(0x00))
                keys.insert(entry.shifted, CharacterKey { entry.code, true });
        }

        return keys;
    }();

    return table;
}

/* Named keys, so an agent does not have to know scancodes. */
const QHash<QString, uint16_t> &
namedKeys()
{
    static const QHash<QString, uint16_t> keys = {
        { QStringLiteral("esc"),          0x01 },
        { QStringLiteral("escape"),       0x01 },
        { QStringLiteral("backspace"),    0x0E },
        { QStringLiteral("tab"),          0x0F },
        { QStringLiteral("enter"),        0x1C },
        { QStringLiteral("return"),       0x1C },
        { QStringLiteral("ctrl"),         0x1D },
        { QStringLiteral("lctrl"),        0x1D },
        { QStringLiteral("rctrl"),        0x11D },
        { QStringLiteral("shift"),        0x2A },
        { QStringLiteral("lshift"),       0x2A },
        { QStringLiteral("rshift"),       0x36 },
        { QStringLiteral("alt"),          0x38 },
        { QStringLiteral("lalt"),         0x38 },
        { QStringLiteral("ralt"),         0x138 },
        { QStringLiteral("space"),        0x39 },
        { QStringLiteral("capslock"),     0x3A },
        { QStringLiteral("caps"),         0x3A },
        { QStringLiteral("minus"),        0x0C },
        { QStringLiteral("plus"),         0x0D },
        { QStringLiteral("comma"),        0x33 },
        { QStringLiteral("period"),       0x34 },
        { QStringLiteral("slash"),        0x35 },
        { QStringLiteral("semicolon"),    0x27 },
        { QStringLiteral("quote"),        0x28 },
        { QStringLiteral("backslash"),    0x2B },
        { QStringLiteral("f1"),           0x3B },
        { QStringLiteral("f2"),           0x3C },
        { QStringLiteral("f3"),           0x3D },
        { QStringLiteral("f4"),           0x3E },
        { QStringLiteral("f5"),           0x3F },
        { QStringLiteral("f6"),           0x40 },
        { QStringLiteral("f7"),           0x41 },
        { QStringLiteral("f8"),           0x42 },
        { QStringLiteral("f9"),           0x43 },
        { QStringLiteral("f10"),          0x44 },
        { QStringLiteral("f11"),          0x57 },
        { QStringLiteral("f12"),          0x58 },
        { QStringLiteral("numlock"),      0x45 },
        { QStringLiteral("scrolllock"),   0x46 },
        { QStringLiteral("home"),         0x147 },
        { QStringLiteral("up"),           0x148 },
        { QStringLiteral("pageup"),       0x149 },
        { QStringLiteral("pgup"),         0x149 },
        { QStringLiteral("left"),         0x14B },
        { QStringLiteral("right"),        0x14D },
        { QStringLiteral("end"),          0x14F },
        { QStringLiteral("down"),         0x150 },
        { QStringLiteral("pagedown"),     0x151 },
        { QStringLiteral("pgdn"),         0x151 },
        { QStringLiteral("insert"),       0x152 },
        { QStringLiteral("ins"),          0x152 },
        { QStringLiteral("delete"),       0x153 },
        { QStringLiteral("del"),          0x153 },
        { QStringLiteral("win"),          0x15B },
        { QStringLiteral("lwin"),         0x15B },
        { QStringLiteral("rwin"),         0x15C },
        { QStringLiteral("menu"),         0x15D },
        { QStringLiteral("printscreen"),  0x137 },
        { QStringLiteral("prtsc"),        0x137 },
        { QStringLiteral("pause"),        0x145 },
        { QStringLiteral("kp0"),          0x52 },
        { QStringLiteral("kp1"),          0x4F },
        { QStringLiteral("kp2"),          0x50 },
        { QStringLiteral("kp3"),          0x51 },
        { QStringLiteral("kp4"),          0x4B },
        { QStringLiteral("kp5"),          0x4C },
        { QStringLiteral("kp6"),          0x4D },
        { QStringLiteral("kp7"),          0x47 },
        { QStringLiteral("kp8"),          0x48 },
        { QStringLiteral("kp9"),          0x49 },
        { QStringLiteral("kpenter"),      0x11C },
        { QStringLiteral("kpdivide"),     0x135 },
        { QStringLiteral("kpmultiply"),   0x37 },
        { QStringLiteral("kpminus"),      0x4A },
        { QStringLiteral("kpplus"),       0x4E },
        { QStringLiteral("kpperiod"),     0x53 },
    };

    return keys;
}

bool
characterStroke(QChar character, Stroke &stroke)
{
    const auto entry = characterTable().constFind(character);
    if (entry == characterTable().constEnd())
        return false;

    stroke.codes.clear();
    if (entry.value().shifted)
        stroke.codes.append(SCAN_LEFT_SHIFT);
    stroke.codes.append(entry.value().code);
    return true;
}

/* "ctrl+alt+del" and friends: every part is a named key or a single character. */
bool
combinationStroke(const QString &combination, Stroke &stroke, QString &error)
{
    stroke.codes.clear();

    const QStringList parts = combination.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        error = QStringLiteral("Empty key name.");
        return false;
    }

    for (const QString &part : parts) {
        const QString name = part.trimmed().toLower();

        const auto named = namedKeys().constFind(name);
        if (named != namedKeys().constEnd()) {
            stroke.codes.append(named.value());
            continue;
        }

        if (name.size() == 1) {
            Stroke single;
            if (characterStroke(name.at(0), single) && !single.codes.isEmpty()) {
                stroke.codes.append(single.codes);
                continue;
            }
        }

        error = QStringLiteral("Unknown key \"%1\". Use a single character or one of: %2")
                    .arg(part.trimmed(), keyNames().join(QStringLiteral(", ")));
        return false;
    }

    return !stroke.codes.isEmpty();
}
} /* namespace */

QStringList
keyNames()
{
    QStringList names = namedKeys().keys();
    names.sort();
    return names;
}

bool
build(const QJsonObject &request, QVector<Stroke> &strokes, int &delay_ms, QString &error)
{
    strokes.clear();
    error.clear();

    if (dopause) {
        error = QStringLiteral("The machine is paused; resume it before sending keys.");
        return false;
    }

    delay_ms = request.value(QStringLiteral("delay_ms")).toInt(DEFAULT_DELAY_MS);
    if ((delay_ms < 5) || (delay_ms > MAX_DURATION_MS))
        delay_ms = DEFAULT_DELAY_MS;

    const QString text = request.value(QStringLiteral("text")).toString();
    const QJsonArray keys = request.value(QStringLiteral("keys")).toArray();

    if (text.isEmpty() && keys.isEmpty()) {
        error = QStringLiteral("Nothing to send: pass \"text\" or \"keys\".");
        return false;
    }

    if (!text.isEmpty()) {
        if (text.size() > MAX_TEXT_CHARS) {
            error = QStringLiteral("Text is limited to %1 characters; send it in parts.")
                        .arg(MAX_TEXT_CHARS);
            return false;
        }

        for (const QChar &character : text) {
            Stroke stroke;
            if (!characterStroke(character, stroke)) {
                error = QStringLiteral("The character \"%1\" is not on a US keyboard; "
                                       "use \"keys\" for special keys.")
                            .arg(character);
                return false;
            }
            strokes.append(stroke);
        }
    }

    if (!keys.isEmpty()) {
        if (keys.size() > MAX_KEYS) {
            error = QStringLiteral("At most %1 keys can be pressed in one request.").arg(MAX_KEYS);
            return false;
        }

        for (const QJsonValue &value : keys) {
            Stroke stroke;
            if (!combinationStroke(value.toString(), stroke, error))
                return false;
            strokes.append(stroke);
        }
    }

    if ((strokes.size() * 2 * delay_ms) > MAX_DURATION_MS) {
        error = QStringLiteral("%1 keystrokes at %2 ms each would take longer than %3 ms; "
                               "lower \"delay_ms\" or send less.")
                    .arg(strokes.size())
                    .arg(delay_ms)
                    .arg(MAX_DURATION_MS);
        return false;
    }

    return true;
}

void
play(QObject *context, const QVector<Stroke> &strokes, int delay_ms,
     std::function<void(int keystrokes, int duration_ms)> done)
{
    /* The sequence outlives this call: the timer walks it while the event loop
       runs, which is also what keeps the emulated keyboard controller from
       being flooded with a make and break code in the same instant. */
    const auto index   = std::make_shared<int>(0);
    const auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();

    const auto step = std::make_shared<std::function<void()>>();

    /* One keystroke: press, wait, release, wait, then the next one. The delays
       are what keep a slow guest's keyboard controller from dropping codes. */
    *step = [context, strokes, delay_ms, done, index, elapsed, step]() {
        if (*index >= strokes.size()) {
            done(strokes.size(), static_cast<int>(elapsed->elapsed()));
            return;
        }

        const Stroke stroke = strokes.at(*index);
        (*index)++;

        for (const uint16_t code : stroke.codes)
            keyboard_input(1, code);

        QTimer::singleShot(delay_ms, context, [context, stroke, delay_ms, step]() {
            for (int i = stroke.codes.size() - 1; i >= 0; i--)
                keyboard_input(0, stroke.codes.at(i));

            QTimer::singleShot(delay_ms, context, [step]() {
                (*step)();
            });
        });
    };

    (*step)();
}
}
