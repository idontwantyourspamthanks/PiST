// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/SessionConfig.h"

namespace pist {

QStringList SessionConfig::toArgv() const
{
    QStringList argv;
    argv << hatariPath;

    if (!tosPath.isEmpty())
        argv << QStringLiteral("--tos") << tosPath;

    argv << QStringLiteral("--machine") << machine;

    if (memSizeMiB > 0)
        argv << QStringLiteral("--memsize") << QString::number(memSizeMiB);

    argv << QStringLiteral("--monitor") << monitor;

    if (!acsiImage.isEmpty()) {
        argv << QStringLiteral("--acsi")
             << QStringLiteral("%1=%2").arg(acsiId).arg(acsiImage);
    }

    // Floppy drives are individually addressed; there is no generic --disk.
    static const char *const floppyOpts[] = {"--disk-a", "--disk-b"};
    for (int i = 0; i < floppyImages.size() && i < 2; ++i) {
        if (!floppyImages.at(i).isEmpty())
            argv << QString::fromLatin1(floppyOpts[i]) << floppyImages.at(i);
    }

    // A reset-requiring option change raises a modal dialog inside the emulator
    // window, which would hang a headless IDE. Reset-requiring changes are
    // applied by relaunching instead (docs/PLAN.md §5 rule 8).
    argv << QStringLiteral("--alert-level") << QStringLiteral("fatal");
    argv << QStringLiteral("--confirm-quit") << QStringLiteral("off");
    argv << QStringLiteral("--sound") << QStringLiteral("off");

    if (fastForward)
        argv << QStringLiteral("--fast-forward") << QStringLiteral("yes");

    // `-d <dir>` sets the GEMDOS HD directory. Note that `--gemdos-drive` only
    // assigns a drive *letter* and is not needed for the default (C:); the
    // directory normally comes from the program positional below.
    if (!gemdosDir.isEmpty())
        argv << QStringLiteral("-d") << gemdosDir;

    if (!bootstrapScriptPath.isEmpty())
        argv << QStringLiteral("--parse") << bootstrapScriptPath;

    // Break in on the faults that mean the program under test is broken.
    // `hasDebugExcept` is checked by the caller, which leaves this empty on a
    // build that would reject the option.
    if (!debugExceptions.isEmpty())
        argv << QStringLiteral("--debug-except") << debugExceptions;

    // Omitted entirely when the emulator has no control-socket support; see the
    // field comment.
    if (!controlSocketPath.isEmpty())
        argv << QStringLiteral("--control-socket") << controlSocketPath;

    argv << extraArgs;

    // Exactly one positional argument, and it must be the program: this is the
    // only form that both mounts the GEMDOS HD and autostarts the program, which
    // in turn is what makes Hatari load its symbols (docs/PLAN.md §5 rule 1).
    if (!programPath.isEmpty())
        argv << programPath;

    return argv;
}

} // namespace pist
