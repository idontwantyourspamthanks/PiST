// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// What the cursor is on, resolved once for every surface that asks
// (docs/glm-features.md §4).

#pragma once

#include "editor/OsCallRef.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// What a position in an assembly source is, as far as the IDE can honestly say:
/// the OS call the line is making, the generic meaning of a trap whose function
/// number is not statically known, or the word under the cursor.
///
/// The reference dock shows the long form of this and the hover card the short
/// one, so the dispatch lives here rather than in either of them — two surfaces
/// that resolved it separately would eventually disagree about what a line does.
///
/// Resolving *nothing* is a real answer, and the important one: a label, a
/// directive or a typo has no OS call and may have no reference entry, and neither
/// caller may turn that into a guess (`InstrRef.h` refuses the same way).
struct CursorReference
{
    /// The TOS call this position is part of, when the trap sequence around it
    /// resolves one. The call wins over the word: a push above `trap #1` is part
    /// of that call, not an unrelated instruction to describe.
    const OsCallInfo *call = nullptr;
    /// The arguments it pushes, in source order.
    QList<QString> args;
    /// True on a trap line whose function number or arguments did not resolve:
    /// the line is still doing `trap`, which the reference can describe.
    bool trapContext = false;
    /// The word under the cursor, in `asmlex`'s token alphabet (letters, digits,
    /// `_`, `.` and `$`, so a label keeps its underscore and `move.w` its suffix).
    /// Empty when the position carries no word of its own.
    QString word;
};

/// Resolve `column` of `lines[lineIndex]`.
///
/// `lines` is a small window of the document around that line: the OS-call scanner
/// is bounded to a few lines by design (`OsCallScan` says why), so the caller hands
/// it a window rather than the whole file.
CursorReference referenceAt(const QStringList &lines, int lineIndex, int column);

} // namespace pist
