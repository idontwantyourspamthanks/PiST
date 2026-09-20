// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QStringList>

namespace pist {

/// Navigation over assembly text: which file an `include` names, where that
/// file lives on disk, which line defines a symbol, and which symbol sits under
/// the mouse.
///
/// These are plain functions over text rather than members of a widget because
/// the same questions are asked from several places — Ctrl+click and hover in
/// the editor, "go to label" from a menu or the remote-control console — and
/// each wants the same answer without owning an editor. They are also the part
/// of navigation that can be reasoned about (and tested) without a window.
///
/// The token rules follow the editor's syntax highlighter, AsmHighlighter:
/// Motorola syntax has `;` starting a comment anywhere, `*` only in the first
/// column, and labels are identifiers that start a line.

/// The file name an `include` directive names, or empty when the line is not an
/// include.
///
/// Both quote styles are accepted (`include "lib.s"`, `include 'lib.s'`), the
/// directive may be indented and of any case, and a trailing `;` comment is
/// ignored.
///
/// The directive must stand at the start of the code field, which is
/// what keeps a comment that merely mentions an include (`; include "lib.s"`)
/// from naming a file. The quotes must pair up: `include 'lib.s` names nothing.
///
/// vasm also accepts a label in front of the directive (`loop: include "lib.s"`);
/// that line is treated as a label and not as an include, so a caller wanting to
/// navigate it must look at the line's label first.
QString includeTargetAt(const QString &lineText);

/// Locate `name` on disk the way the assembler does: the directory of the file
/// doing the including first, then `includePaths` in the order given (`-I`).
///
/// Returns the absolute path of the first existing regular file, or empty when
/// none exists. Relative include paths are read against the current working
/// directory, which is where the assembler runs from too. Nothing is searched
/// on a path outside those, so a name that resolves during the build resolves
/// here, and one that does not resolves nowhere.
QString resolveInclude(const QString &name, const QString &currentFileDir,
                       const QStringList &includePaths);

/// The 1-based line defining label `word`, or 0 when no line does.
///
/// Recognises a symbol defined at the start of the code field: `foo:`, `foo:`
/// with an instruction after it, and the assignments the assembler treats as
/// definitions — `foo equ 5`, `foo set 5` and `foo = 5`. The comparison is
/// case-insensitive, because assembly symbols are. Text after a `;` and a line
/// beginning with `*` are comments, so a symbol mentioned in a comment is not a
/// definition.
int labelLine(const QString &documentText, const QString &word);

/// The symbol or file-name token under `column` (0-based) of `lineText`, or
/// empty when there is none.
///
/// A token is a run of letters, digits, `_`, `.` and `$` — the same alphabet the
/// highlighter's label pattern uses — which covers an operand (`d0`), a symbol
/// reference (`kMaxX` in `kMaxX+1`) and the file name inside an include's quotes
/// (`lib.s` in `include "lib.s"`). When the character under `column` is one of
/// those, its whole token is returned. Otherwise punctuation and whitespace
/// carry no token of their own and the token immediately to the left wins, so a
/// click on the `+` of `kMaxX+1` or on the `,` of `1,d4` finds the token being
/// left; with nothing to the left (column 0, an indented line's leading tab, the
/// `#` of `#kMaxX`) the token ahead serves instead. A column at or past the end
/// of the line is clamped to its last character, so clicking just past a word
/// still finds that word.
QString wordAtCursor(const QString &lineText, int column);

} // namespace pist
