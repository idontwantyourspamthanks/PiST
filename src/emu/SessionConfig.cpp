// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/SessionConfig.h"

#include <QDir>

namespace pist {

QString hatariHostPath(const QString &path)
{
    return QDir::toNativeSeparators(path);
}

QStringList SessionConfig::toArgv() const
{
    QStringList argv;
    argv << hatariHostPath(hatariPath);

    if (!tosPath.isEmpty())
        argv << QStringLiteral("--tos") << hatariHostPath(tosPath);

    argv << QStringLiteral("--machine") << machine;

    if (memSizeMiB > 0)
        argv << QStringLiteral("--memsize") << QString::number(memSizeMiB);

    argv << QStringLiteral("--monitor") << monitor;

    if (!acsiImage.isEmpty()) {
        argv << QStringLiteral("--acsi")
             << QStringLiteral("%1=%2").arg(acsiId).arg(hatariHostPath(acsiImage));
    }

    // Floppy drives are individually addressed; there is no generic --disk.
    static const char *const floppyOpts[] = {"--disk-a", "--disk-b"};
    bool userFloppy = false;
    const bool userDiskA = floppyImages.size() > 0 && !floppyImages.at(0).isEmpty();
    for (int i = 0; i < floppyImages.size() && i < 2; ++i) {
        if (!floppyImages.at(i).isEmpty()) {
            argv << QString::fromLatin1(floppyOpts[i]) << hatariHostPath(floppyImages.at(i));
            userFloppy = true;
        }
    }

    // A reset-requiring option change raises a modal dialog inside the emulator
    // window, which would hang a headless IDE. Reset-requiring changes are
    // applied by relaunching instead (docs/PLAN.md §5 rule 8).
    argv << QStringLiteral("--alert-level") << QStringLiteral("fatal");
    argv << QStringLiteral("--confirm-quit") << QStringLiteral("off");
    argv << QStringLiteral("--sound") << QStringLiteral("off");

    // Cover disks (and TOS floppy I/O in general) miss sectors under
    // fast-forward. Keep turbo only when no user image is mounted.
    if (fastForward && !userFloppy)
        argv << QStringLiteral("--fast-forward") << QStringLiteral("yes");

    // `-d <dir>` sets the GEMDOS HD directory. Note that `--gemdos-drive` only
    // assigns a drive *letter* and is not needed for the default (C:); the
    // directory normally comes from the program positional below. On the
    // AUTO-folder path GEMDOS HD does not exist (TOS < 1.04 rejects it), so
    // the floppy replaces both it and the positional.
    //
    // `--disk-a` is last-wins. A user image on A: must not be overwritten by
    // the AUTO-folder boot floppy; launchEmulator clears A: and shifts that
    // image to B: when TOS actually needs A: for autostart.
    const bool autoBoot = !bootFloppyPath.isEmpty() && !userDiskA;
    if (autoBoot) {
        argv << QStringLiteral("--disk-a") << hatariHostPath(bootFloppyPath);
        if (debugToggle)
            argv << QStringLiteral("--debug");
    } else if (!gemdosDir.isEmpty()) {
        argv << QStringLiteral("-d") << hatariHostPath(gemdosDir);
    }
    if (!bootstrapScriptPath.isEmpty())
        argv << QStringLiteral("--parse") << hatariHostPath(bootstrapScriptPath);

    // Break in on the faults that mean the program under test is broken.
    // `hasDebugExcept` is checked by the caller, which leaves this empty on a
    // build that would reject the option.
    if (!debugExceptions.isEmpty())
        argv << QStringLiteral("--debug-except") << debugExceptions;

    // Omitted entirely when the emulator has no control-socket support; see the
    // field comment.
    if (!controlSocketPath.isEmpty())
        argv << QStringLiteral("--control-socket") << hatariHostPath(controlSocketPath);

    argv << extraArgs;

    // Exactly one positional argument, and it must be the program: this is the
    // only form that both mounts the GEMDOS HD and autostarts the program, which
    // in turn is what makes Hatari load its symbols (docs/PLAN.md §5 rule 1).
    // On the AUTO-folder path there is no positional: the program boots from
    // the floppy (and a positional would wrongly also mount a GEMDOS HD).
    if (!programPath.isEmpty() && !autoBoot)
        argv << hatariHostPath(programPath);

    return argv;
}

} // namespace pist
