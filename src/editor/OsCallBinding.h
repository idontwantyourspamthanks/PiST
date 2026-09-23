// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

struct OsCallInfo;

/// The bytes the caller pops after `info`'s trap: the function number's word,
/// the arguments the prototype declares, the reserved words the binding adds
/// (Mshrink, Frename) and Pexec's fixed argument block.
///
/// Derived from the entry itself, so the generated cleanup and the number the
/// reference shows (`OsCallInfo::stackBytes`, which the suite pins to this
/// value) cannot drift apart.
int osCallStackBytes(const OsCallInfo &info);

/// The canonical assembler binding for `info`, ready to insert into a source
/// file: the argument pushes in reverse declaration order, the function
/// number as a word, the trap, and the caller-side stack cleanup — tab
/// indented, one instruction per line, with a trailing newline.
///
/// Arguments are written as placeholders named after the prototype's
/// parameters (`pea buf`, `move.w #handle,-(sp)`): the template is meant to
/// be edited into shape, so it asks for attention at exactly the spots that
/// need real values. The irregular calls get their documented shapes, which
/// come from the entry's layout members rather than from its trap number or a
/// parameter's name: Pexec's fixed env/cmdline/name block, the reserved zero
/// word of Mshrink and Frename, and Dbmsg's reserved word as the literal 5 it
/// must be.
QString osCallBinding(const OsCallInfo &info);

} // namespace pist
