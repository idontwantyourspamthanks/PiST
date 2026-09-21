// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

struct OsCallInfo;

/// The canonical assembler binding for `info`, ready to insert into a source
/// file: the argument pushes in reverse declaration order, the function
/// number as a word, the trap, and the caller-side stack cleanup — tab
/// indented, one instruction per line, with a trailing newline.
///
/// Arguments are written as placeholders named after the prototype's
/// parameters (`pea buf`, `move.w #handle,-(sp)`): the template is meant to
/// be edited into shape, so it asks for attention at exactly the spots that
/// need real values. The irregular calls get their documented shapes: Pexec's
/// fixed env/cmdline/name/mode layout, the reserved zero word of Mshrink and
/// Frename, and Dbmsg's reserved word as the literal 5 it must be.
QString osCallBinding(const OsCallInfo &info);

} // namespace pist
