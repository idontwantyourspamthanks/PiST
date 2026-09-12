// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace pist {

/// Everything needed to launch one emulator session.
///
/// This is deliberately a plain value type: the IDE expresses emulator state as
/// command-line arguments and never writes or mutates a user's `hatari.cfg`
/// (docs/PLAN.md §1, §5).
struct SessionConfig
{
    QString hatariPath = QStringLiteral("hatari");

    /// TOS ROM image. Never bundled with PiST: original TOS images remain
    /// proprietary, so this is always user-supplied (docs/PLAN.md §7).
    QString tosPath;

    /// st / megast / ste / megaste / tt / falcon
    QString machine = QStringLiteral("st");

    /// Monitor: mono / rgb / vga / tv
    QString monitor = QStringLiteral("mono");

    /// ST RAM size in MiB (1..14). 0 means Hatari's 512 KiB default.
    int memSizeMiB = 1;

    QString tosResolution; // optional: --tos-res

    /// Hard disk images.
    QString ideMasterImage;
    QString acsiImage;
    int acsiId = 0;

    /// Floppy images, index 0 => drive A.
    QStringList floppyImages;

    /// GEMDOS HD directory. Note that the .PRG positional argument normally
    /// sets this implicitly to the program's own directory; set it explicitly
    /// only when the program lives elsewhere (docs/PLAN.md §5 rule 1).
    QString gemdosDir;

    /// The program to assemble and run.
    QString programPath;

    bool fastForward = true;

    /// Absolute path to the debugger command script generated for this session.
    QString bootstrapScriptPath;

    /// Absolute path for the control socket the IDE listens on.
    ///
    /// Left empty when the emulator build has no control-socket support (it is
    /// compiled only under `HAVE_UNIX_DOMAIN_SOCKETS`, i.e. not on Windows).
    /// Passing the option to a build that lacks it makes Hatari exit with
    /// "Unrecognized option", so the caller must gate it on the capability
    /// probe rather than setting this unconditionally.
    QString controlSocketPath;

    /// Exception flags for `--debug-except`.
    ///
    /// The `autostart` entry is load-bearing rather than a real exception class:
    /// it defers arming until the program has been loaded from the GEMDOS HD
    /// (`src/event.c`, on INF load). Without it, exceptions are live for the
    /// whole session including the TOS boot, so boot-time faults would break in
    /// before the program ever runs.
    ///
    /// `bus` is deliberately **excluded**. Verified against Hatari 2.6.1: arming
    /// it makes TOS raise a bus error at 0xfc0ee2 (inside the ROM) during
    /// startup, so the debugger breaks in before the program is even exec'd —
    /// which also leaves `symbols prg` with no program to load. The remaining
    /// three are the faults that indicate a bug in the program under test, and
    /// each was checked to leave a healthy program running untouched.
    ///
    /// `linea`/`linef`/`trace` are also excluded: Line-A and Line-F are normal
    /// parts of graphics calls, so arming them would break in constantly during
    /// ordinary TOS/VDI use.
    QString debugExceptions = QStringLiteral("autostart,illegal,address,zerodiv");

    /// Per-session config directory; exported as HOME/XDG_CONFIG_HOME so that a
    /// user's real settings cannot leak into a session, and so an embedded
    /// emulator cannot overwrite them (docs/PLAN.md §5 rule 7).
    QString sessionDir;

    QStringList extraArgs;

    QStringList toArgv() const;
};

} // namespace pist
