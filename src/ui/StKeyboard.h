// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

namespace pist::stkbd {

/// Map a Qt key (Qt::Key value) to an Atari ST keyboard scancode for the
/// media channel's KEY messages (docs/PLAN.md §12), or -1 when the key has
/// no ST equivalent. Layout-independent by construction: letters and digits
/// map by identity, not by what a shifted host layout would produce, because
/// TOS applies its own shift state to scancodes. Modifier handedness is not
/// portable in Qt, so Shift/Control/Alt always report the left variant.
int scancodeFromQtKey(int qtKey);

} // namespace pist::stkbd
