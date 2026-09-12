// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// What the installed Hatari build supports.
///
/// The IDE must never assume a feature exists: upstream options have appeared
/// and disappeared between releases, and a distro build may lag git `main`
/// (docs/PLAN.md §2.4). Everything the debug client depends on is probed once at
/// startup.
struct HatariCapabilities
{
    bool valid = false;
    QString version;        // e.g. "2.6.1"
    int versionMajor = 0;
    int versionMinor = 0;
    int versionPatch = 0;

    bool hasControlSocket = false;
    bool hasCmdFifo = false;
    bool hasSymbolAutoloadOption = false; // --symload (git main; absent in 2.6.1)
    bool hasBacktraceCommand = false;     // `bt` (git main; absent in 2.6.1)
    bool hasConout = false;

    /// True when the debugger command is `symbols autoload <mode>` (main) rather
    /// than the boolean `symbols autoload on|off` (2.6.1).
    bool hasThreeModeSymbolAutoload() const { return hasSymbolAutoloadOption; }

    QString summary() const;
};

/// Probe a Hatari binary. Runs `--version` and `-h`; safe to call before any
/// session exists.
HatariCapabilities probeHatari(const QString &hatariPath);

} // namespace pist
