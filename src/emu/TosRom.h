// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>

namespace pist {

/// A TOS-compatible ROM image found on disk.
///
/// pist never ships an original TOS image: those remain proprietary, so ROMs
/// are always user-supplied (docs/PLAN.md §7). EmuTOS may be shipped separately.
struct TosRom
{
    QString path;        ///< absolute path to the image
    QString fileName;
    int versionCode = 0; ///< 0x0104 style, or 0 when unknown
    bool versionKnown = false;

    /// Whether the ROM can autostart a program passed as the Hatari positional
    /// argument. Autostart is implemented through `C:\EMUDESK.INF`, and Hatari
    /// refuses it below TOS 1.04 (src/inffile.c:1042). Without autostart there
    /// is no Pexec, so no symbols are loaded and the entry breakpoint never
    /// fires — the session appears to hang (docs/PLAN.md §5 rule 3).
    bool supportsAutostart() const { return versionKnown && versionCode >= 0x0104; }

    QString versionText() const
    {
        if (!versionKnown)
            return QStringLiteral("unknown");
        return QStringLiteral("%1.%2")
            .arg((versionCode >> 8) & 0xff)
            .arg(versionCode & 0xff, 2, 10, QLatin1Char('0'));
    }
};

/// Scan a directory for ROM images (`.img`), parsing the TOS version from the
/// usual "TOS v1.04 (1989)(...)" naming. Files whose version cannot be
/// determined are still returned, with `versionKnown` false.
QList<TosRom> scanTosRoms(const QString &directory);

/// Scan every directory reported by `paths::tosSearchPaths()`, in priority
/// order, de-duplicating by absolute path. This is what the application uses;
/// there is no single directory that works on all three platforms.
QList<TosRom> findTosRoms();

/// Pick the best ROM for running a program: prefer a known autostart-capable
/// version, then an unknown one, and only then a known-incompatible version.
/// Returns an invalid entry (empty path) if the list is empty.
TosRom selectPreferredRom(const QList<TosRom> &roms);

} // namespace pist
