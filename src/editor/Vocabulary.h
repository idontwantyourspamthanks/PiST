// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The assembler's vocabulary: the instruction and directive words the editor
// both *paints* and *offers*.
//
// One list, deliberately. The highlighter builds its two patterns from these and
// completion offers them as candidates, so the editor cannot suggest something it
// would not colour, or colour something it would never suggest — a mismatch a
// second hand-written copy would find eventually, which is exactly how the
// mnemonic list drifted by nineteen entries before.

#pragma once

#include <QStringList>

namespace pist {

/// The mnemonics: the 68000 reference (`editor/InstrRef`) plus the 68020+ and FPU
/// names vasm's mot module accepts and the reference deliberately leaves out —
/// the reference is a *68000* reference, which `InstrRef.h` explains.
///
/// Lowercase, because vasm is case-insensitive and a source is written lowercase
/// as a rule: completion that inserted a shouted `ADDQ` into one would look like a
/// suggestion from another assembler.
const QStringList &mnemonicWords();

/// The directives and assembler controls: `dc.b` and its family, `equ`, `set`,
/// `even`, `section`, `include`, `macro`, `if`/`endif`, `opt`, …
///
/// Lowercase for the same reason as the mnemonics.
const QStringList &directiveWords();

} // namespace pist
