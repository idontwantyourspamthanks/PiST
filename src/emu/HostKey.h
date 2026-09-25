/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * PiST - an IDE for Atari ST assembly development
 *
 * Qt key codes to the SDL_Keycode values Hatari's keymap switches on.
 * Shifted punctuation comes back from Qt as the character, and from SDL as
 * the unshifted key plus a separate shift press, so those map back to the
 * unshifted key. The shift key itself arrives as its own event.
 */

#pragma once

#include <QtGlobal>

namespace pist {

/// 0 when this key is not one the ST keyboard has.
inline int qtKeyToSdlSym(int qtKey)
{
    if (qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z)
        return qtKey - Qt::Key_A + 97; // SDLK_a
    if (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)
        return qtKey; // SDLK_0 is the same code
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12)
        return 1073741882 + (qtKey - Qt::Key_F1); // SDLK_F1

    switch (qtKey) {
    case Qt::Key_Escape: return 27;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return 9;
    case Qt::Key_Backspace: return 8;
    case Qt::Key_Return: return 13;
    case Qt::Key_Enter: return 1073741912; // SDLK_KP_ENTER
    case Qt::Key_Space: return 32;
    case Qt::Key_Delete: return 127;
    case Qt::Key_Insert: return 1073741898;
    case Qt::Key_Home: return 1073741899;
    case Qt::Key_End: return 1073741901;
    case Qt::Key_Left: return 1073741904;
    case Qt::Key_Right: return 1073741903;
    case Qt::Key_Down: return 1073741905;
    case Qt::Key_Up: return 1073741906;
    case Qt::Key_PageUp: return 1073741900;
    case Qt::Key_PageDown: return 1073741902;
    case Qt::Key_CapsLock: return 1073741881;
    case Qt::Key_Shift: return 1073742049; // SDLK_LSHIFT
    case Qt::Key_Control: return 1073742048; // SDLK_LCTRL
    case Qt::Key_Alt: return 1073742050; // SDLK_LALT
    case Qt::Key_Minus: return 45;
    case Qt::Key_Equal: return 61;
    case Qt::Key_BracketLeft: return 91;
    case Qt::Key_BracketRight: return 93;
    case Qt::Key_Backslash: return 92;
    case Qt::Key_Semicolon: return 59;
    case Qt::Key_Apostrophe: return 39;
    case Qt::Key_Comma: return 44;
    case Qt::Key_Period: return 46;
    case Qt::Key_Slash: return 47;
    case Qt::Key_QuoteLeft: return 96;
    // Qt reports the shifted character. Hatari wants the key underneath.
    case Qt::Key_Exclam: return 49;
    case Qt::Key_At: return 50;
    case Qt::Key_NumberSign: return 51;
    case Qt::Key_Dollar: return 52;
    case Qt::Key_Percent: return 53;
    case Qt::Key_AsciiCircum: return 54;
    case Qt::Key_Ampersand: return 55;
    case Qt::Key_Asterisk: return 56;
    case Qt::Key_ParenLeft: return 57;
    case Qt::Key_ParenRight: return 48;
    case Qt::Key_Underscore: return 45;
    case Qt::Key_Plus: return 61;
    case Qt::Key_BraceLeft: return 91;
    case Qt::Key_BraceRight: return 93;
    case Qt::Key_Bar: return 92;
    case Qt::Key_Colon: return 59;
    case Qt::Key_QuoteDbl: return 39;
    case Qt::Key_Less: return 44;
    case Qt::Key_Greater: return 46;
    case Qt::Key_Question: return 47;
    case Qt::Key_AsciiTilde: return 96;
    default: return 0;
    }
}

/// SDL_Keymod bits. These match SDL2 and the stub the release dylib compiles.
inline int qtModifiersToSdlMod(int qtModifiers)
{
    int mod = 0;
    if (qtModifiers & Qt::ShiftModifier)
        mod |= 0x0001; // KMOD_LSHIFT
    if (qtModifiers & Qt::ControlModifier)
        mod |= 0x0040; // KMOD_LCTRL
    if (qtModifiers & Qt::AltModifier)
        mod |= 0x0100; // KMOD_LALT
    if (qtModifiers & Qt::MetaModifier)
        mod |= 0x0400; // KMOD_LGUI
    return mod;
}

/// Bit 0 is the left button, bit 1 the right. Those are also Qt::LeftButton
/// and Qt::RightButton, and they are what pist_hatari_mouse reads.
inline int qtButtonsToHost(int qtButtons)
{
    int buttons = 0;
    if (qtButtons & Qt::LeftButton)
        buttons |= 1;
    if (qtButtons & Qt::RightButton)
        buttons |= 2;
    return buttons;
}

/// Widget pixel to 256ths of a video pixel, through the letterboxed rect.
inline void widgetToVideo256(int widgetX, int widgetY, int fitX, int fitY, int fitW, int fitH,
                             int videoW, int videoH, int *x256, int *y256)
{
    *x256 = fitW > 0 ? int(qint64(widgetX - fitX) * videoW * 256 / fitW) : 0;
    *y256 = fitH > 0 ? int(qint64(widgetY - fitY) * videoH * 256 / fitH) : 0;
}

/// Whole ST pixels of motion since the last sample. The first sample records
/// the position and returns no motion, so entering the panel does not fling
/// the ST pointer. The leftover fraction stays in the origin.
inline void takePointerDelta(int videoX256, int videoY256, int *originX256, int *originY256,
                             bool *have, int *dx, int *dy)
{
    if (!*have) {
        *originX256 = videoX256;
        *originY256 = videoY256;
        *have = true;
        *dx = 0;
        *dy = 0;
        return;
    }
    *dx = (videoX256 - *originX256) / 256;
    *dy = (videoY256 - *originY256) / 256;
    *originX256 += *dx * 256;
    *originY256 += *dy * 256;
}

} // namespace pist
