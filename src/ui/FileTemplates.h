// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace pist {

/// The starting points File ▸ New File offers (docs/untitled-plan.md §4).
///
/// There is deliberately no "empty unnamed buffer" entry: an assembly source
/// exists to be assembled, and everything PiST anchors to it — the build's
/// output paths, the line map, breakpoints, the blame lane, the floppy
/// write-back — is anchored to a real path. A new document is therefore a file
/// on disk before it can be edited, and this enum is the choice of *contents*
/// that file starts with. *Blank* is the empty file, not an untitled one.
enum class SourceTemplate {
    Blank,         ///< An empty file — the purist's start.
    GemdosHello,   ///< Print a line, wait for a key, exit.
    KeyboardInput, ///< Read keys with GEMDOS Cconin until ESC.
    ImageInclude,  ///< Draw a sprite out of the bitplanes PiST exports.
};

/// The suffix a template's name suggests when the user types none. One value
/// for every template: the extension belongs to the assembler, not to the
/// choice of contents, and the name field stays free-form over it (`.asm`,
/// `.x68`, …).
inline constexpr const char *kSourceSuffix = ".s";

/// The picker's entries, in the order the plan lists them.
QList<SourceTemplate> allSourceTemplates();

/// The template's name in the picker.
QString templateLabel(SourceTemplate kind);

/// One line saying what the template gives you, for the picker's tooltip.
QString templateDescription(SourceTemplate kind);

/// The bytes a new file gets from this template. Empty for Blank, and never
/// anything a fresh file cannot be: these are the shipped starting points, and
/// `tst_gui` assembles them so one that drifted into a non-building state
/// fails CI rather than its first user (docs/untitled-plan.md §9).
QByteArray templateBytes(SourceTemplate kind);

} // namespace pist
