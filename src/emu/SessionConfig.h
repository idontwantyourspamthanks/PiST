// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace pist {

/// Everything needed to launch one emulator session.
///
/// This is deliberately a plain value type: the IDE expresses emulator state as
/// command-line arguments and never writes or mutates a user's `hatari.cfg`
/// (PLAN.md §1, §5).
struct SessionConfig
{
    QString hatariPath = QStringLiteral("hatari");

    /// TOS ROM image. Never bundled with pist: original TOS images remain
    /// proprietary, so this is always user-supplied (PLAN.md §7).
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
    /// only when the program lives elsewhere (PLAN.md §5 rule 1).
    QString gemdosDir;

    /// The program to assemble and run.
    QString programPath;

    bool fastForward = true;

    /// Absolute path to the debugger command script generated for this session.
    QString bootstrapScriptPath;

    /// Absolute path for the control socket the IDE listens on.
    QString controlSocketPath;

    /// Per-session config directory; exported as HOME/XDG_CONFIG_HOME so that a
    /// user's real settings cannot leak into a session, and so an embedded
    /// emulator cannot overwrite them (PLAN.md §5 rule 7).
    QString sessionDir;

    QStringList extraArgs;

    QStringList toArgv() const;
};

} // namespace pist
