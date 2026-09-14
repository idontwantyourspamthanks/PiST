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


    /// Hard disk image, attached as ACSI. ACSI is used rather than IDE because it
    /// exists on every ST-family machine, whereas IDE is STe and later.
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

    /// AUTO-folder floppy image for TOS 1.00/1.02. When set, the session
    /// boots from this image instead of the GEMDOS HD: no positional program
    /// argument and no `-d` (GEMDOS HD does not exist below TOS 1.04 — Hatari
    /// refuses it outright). The image is generated per session by
    /// floppy::writeAutoFolderImage (docs/PLAN.md §5 rule 3's fallback).
    QString bootFloppyPath;

    /// Pass `--debug` to arm the exception mask at startup. Required on the
    /// AUTO-folder path: the `autostart` deferral in `debugExceptions` arms
    /// only when TOS loads the virtual EMUDESK.INF from a GEMDOS HD
    /// (src/inffile.c), which a floppy boot never provides, so without this
    /// the mask stays zero and faulting programs never break in. Not used on
    /// the GEMDOS-HD path, where arming at startup would risk boot-time
    /// faults breaking in before the program runs.
    bool debugToggle = false;

    /// Embedded display: the X11 window ID (as text) of the container PiST
    /// provides. Exported to the child as `PARENT_WIN_ID`, and Hatari reparents
    /// its SDL window into it (src/control.c). Empty means the emulator runs as
    /// its own top-level window, which is the default and the only mode that
    /// works off X11. Setting it also pins the child to SDL_VIDEODRIVER=x11, so
    /// both ends are clients of the same X display.
    QString parentWindowId;

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
