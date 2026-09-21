// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QStringList>

namespace pist {

struct OsCallInfo;

/// The OS call a source line belongs to, when it belongs to one.
///
/// `trapContext` is true when the line is a `trap #1`/`#13`/`#14` instruction
/// or a push line feeding one. `call` is the resolved function, or nullptr
/// when the function number is not statically known (`move.w d0,-(sp)`) or
/// not a documented call. `args` holds the operand texts of the argument
/// pushes found between the function-number push and the trap, in push order
/// (`pea msg` → "msg"), so the panel can say what the call is being made
/// *with*, not just which call it is.
struct OsCallMatch
{
    const OsCallInfo *call = nullptr;
    bool trapContext = false;
    QStringList args;
};

/// Resolve the OS-call context of `lines[lineIndex]` (0-based).
///
/// Two shapes are recognised:
///  - the line is a trap instruction: the function number comes from the most
///    recent word push above it (`move.w #9,-(sp)`, any radix or a symbolic
///    name, `clr.w -(sp)` = 0), found by a bounded backward scan;
///  - the line is such a push: the trap is found by a bounded forward scan,
///    then resolved the same way.
///
/// Scans skip blank and comment lines, stop at labels and at the file's edge,
/// and never look further than a handful of lines: OS calls are a local
/// idiom, and a push far above its trap is not one. Lines outside any trap
/// sequence return a default OsCallMatch.
OsCallMatch osCallAt(const QStringList &lines, int lineIndex);

} // namespace pist
