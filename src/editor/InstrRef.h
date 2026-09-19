// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>

namespace pist {

/// One mnemonic of the 68000 reference.
///
/// `flags` is a short phrase describing the condition codes the instruction
/// touches, or empty when that is not known — the reference would rather say
/// nothing than state a flag effect that is wrong.
struct InstructionInfo
{
    QString mnemonic;
    QString summary;
    QString flags;
};

/// The whole reference, in the order it should be listed: grouped by what the
/// instructions do, not alphabetically, so the panel reads like a manual.
const QList<InstructionInfo> &instructionTable();

/// The entry for `word`, or nullptr when it is not a 68000 mnemonic.
///
/// The lookup is case-insensitive and ignores a trailing size suffix, so
/// `addq.l`, `ADDQ` and `Move.w` all resolve. Directives (`dc.b`, `include`),
/// labels and anything else the editor might have under the cursor resolve to
/// nullptr rather than to a guessed entry.
const InstructionInfo *instructionRef(const QString &word);

} // namespace pist
