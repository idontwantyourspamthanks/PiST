// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

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
    /// `--symload` (git main; absent in 2.6.1). Present iff the debugger uses the
    /// three-mode `symbols autoload <exec|debugger|off>` form rather than the
    /// boolean one, so it also selects which bootstrap script line to write.
    bool hasSymbolAutoloadOption = false;

    /// `--debug-except`: breaks in on CPU exceptions.
    bool hasDebugExcept = false;

    /// `--parse`: run debugger commands from a file at startup.
    bool hasParse = false;


    /// The HRDB remote-debug protocol (the tattlemuss hrdb-main fork). The fork
    /// is version-identical to upstream and adds no CLI option, so neither a
    /// version nor an option probe can find it; the listener's banner string is
    /// in the binary, and upstream's never is.
    bool hasHrdb = false;
    QString summary() const;
};

/// Probe a Hatari binary. Runs `--version` and `-h`, and scans the binary's
/// content for the HRDB banner string; safe to call before any session exists.
HatariCapabilities probeHatari(const QString &hatariPath);

} // namespace pist
