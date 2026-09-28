// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/StKeyboard.h"

#include <Qt>

namespace pist::stkbd {

namespace {

struct Mapping
{
    int qtKey;
    int scancode;
};

// Atari ST scancodes (the IKBD set TOS reads), keyed by Qt::Key. Letters and
// digits by identity; punctuation follows the standard layout — the table is
// what Hatari's own symbolic mapping uses, so a guest sees the same key under
// both display paths.
constexpr Mapping kMappings[] = {
    { Qt::Key_Escape, 0x01 },
    { Qt::Key_1, 0x02 }, { Qt::Key_2, 0x03 }, { Qt::Key_3, 0x04 },
    { Qt::Key_4, 0x05 }, { Qt::Key_5, 0x06 }, { Qt::Key_6, 0x07 },
    { Qt::Key_7, 0x08 }, { Qt::Key_8, 0x09 }, { Qt::Key_9, 0x0A },
    { Qt::Key_0, 0x0B },
    { Qt::Key_Minus, 0x0C }, { Qt::Key_Equal, 0x0D },
    { Qt::Key_Backspace, 0x0E }, { Qt::Key_Tab, 0x0F },
    { Qt::Key_Q, 0x10 }, { Qt::Key_W, 0x11 }, { Qt::Key_E, 0x12 },
    { Qt::Key_R, 0x13 }, { Qt::Key_T, 0x14 }, { Qt::Key_Y, 0x15 },
    { Qt::Key_U, 0x16 }, { Qt::Key_I, 0x17 }, { Qt::Key_O, 0x18 },
    { Qt::Key_P, 0x19 },
    { Qt::Key_BracketLeft, 0x1A }, { Qt::Key_BracketRight, 0x1B },
    { Qt::Key_Return, 0x1C }, { Qt::Key_Enter, 0x72 }, // keypad Enter
    { Qt::Key_Control, 0x1D },
    { Qt::Key_A, 0x1E }, { Qt::Key_S, 0x1F }, { Qt::Key_D, 0x20 },
    { Qt::Key_F, 0x21 }, { Qt::Key_G, 0x22 }, { Qt::Key_H, 0x23 },
    { Qt::Key_J, 0x24 }, { Qt::Key_K, 0x25 }, { Qt::Key_L, 0x26 },
    { Qt::Key_Semicolon, 0x27 }, { Qt::Key_Apostrophe, 0x28 },
    { Qt::Key_QuoteLeft, 0x29 },
    { Qt::Key_Shift, 0x2A }, // left Shift; Qt cannot report handedness
    { Qt::Key_Backslash, 0x2B },
    { Qt::Key_Z, 0x2C }, { Qt::Key_X, 0x2D }, { Qt::Key_C, 0x2E },
    { Qt::Key_V, 0x2F }, { Qt::Key_B, 0x30 }, { Qt::Key_N, 0x31 },
    { Qt::Key_M, 0x32 },
    { Qt::Key_Comma, 0x33 }, { Qt::Key_Period, 0x34 }, { Qt::Key_Slash, 0x35 },
    { Qt::Key_Alt, 0x38 },
    { Qt::Key_Space, 0x39 },
    { Qt::Key_CapsLock, 0x3A },
    { Qt::Key_F1, 0x3B }, { Qt::Key_F2, 0x3C }, { Qt::Key_F3, 0x3D },
    { Qt::Key_F4, 0x3E }, { Qt::Key_F5, 0x3F }, { Qt::Key_F6, 0x40 },
    { Qt::Key_F7, 0x41 }, { Qt::Key_F8, 0x42 }, { Qt::Key_F9, 0x43 },
    { Qt::Key_F10, 0x44 },
    { Qt::Key_Home, 0x47 },
    { Qt::Key_Up, 0x48 },
    { Qt::Key_PageUp, 0x49 },
    { Qt::Key_Left, 0x4B },
    { Qt::Key_Right, 0x4D },
    { Qt::Key_End, 0x4F },
    { Qt::Key_Down, 0x50 },
    { Qt::Key_PageDown, 0x51 },
    { Qt::Key_Insert, 0x52 },
    { Qt::Key_Delete, 0x53 },
};

} // namespace

int scancodeFromQtKey(int qtKey)
{
    for (const Mapping &m : kMappings)
        if (m.qtKey == qtKey)
            return m.scancode;
    return -1;
}

} // namespace pist::stkbd
