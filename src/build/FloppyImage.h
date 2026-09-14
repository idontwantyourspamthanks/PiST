// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// FAT12 floppy image authoring for the AUTO-folder boot path.
///
/// TOS 1.00/1.02 have no GEMDOS-HD autostart (Hatari refuses GEMDOS HD
/// outright below 1.04 — docs/PLAN.md §5 rule 3), but every TOS executes
/// AUTO/*.PRG from the boot floppy. Writing the image ourselves keeps the
/// fallback free of an mtools/dosfstools dependency, and the layout is pinned
/// against a real mkfs.vfat image, not guesswork.
namespace floppy {

/// Write a 720 KiB FAT12 image containing AUTO/PROG.PRG (the program TOS will
/// execute on boot) and an empty EMUDESK.INF at the root. The INF matters
/// beyond cosmetics: Hatari's `--debug-except autostart,...` mask arms when
/// TOS loads the INF, and without one the exception breakpoints never arm at
/// all on this boot path.
bool writeAutoFolderImage(const QString &imagePath, const QString &programPath,
                          QString *error);

} // namespace floppy

} // namespace pist
