// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

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

    /// Detected as EmuTOS via the `ETOS` magic at offset 0x2c (the same test
    /// Hatari performs in `src/tos.c`).
    bool isEmuTos = false;

    /// Whether the version came from the image header rather than its filename.
    /// Only header-derived versions are trustworthy for the autostart check.
    bool versionFromHeader = false;

    /// Whether the ROM can autostart a program passed as the Hatari positional
    /// argument. Autostart is implemented through `C:\EMUDESK.INF`, and Hatari
    /// refuses it below TOS 1.04 (src/inffile.c:1042). Without autostart there
    /// is no Pexec, so no symbols are loaded and the entry breakpoint never
    /// fires — the session appears to hang (docs/PLAN.md §5 rule 3).
    ///
    /// Two things make this prediction rather than a guess:
    ///
    ///  - it uses the same field Hatari reads, the version at image offset 2;
    ///  - it requires that value to have come from the **header**, because
    ///    Hatari never looks at filenames. A version inferred from a name is
    ///    not evidence about what the emulator will do.
    ///
    /// So an image whose header cannot be read reports `false` here even if its
    /// filename claims a new version, and the caller should treat that as
    /// "unknown" rather than "too old" — see `versionFromHeader`.
    bool supportsAutostart() const
    {
        return versionKnown && versionFromHeader && versionCode >= 0x0104;
    }

    /// Whether the image is known to be too old for the autostart run path.
    /// Distinct from `!supportsAutostart()`, which also covers "unknown".
    bool knownTooOldForAutostart() const
    {
        return versionKnown && versionFromHeader && versionCode < 0x0104;
    }

    QString versionText() const
    {
        if (!versionKnown)
            return QStringLiteral("unknown");
        // Both bytes are rendered in hex, matching Hatari's own `%x.%02x`
        // (src/tos.c:874). So 0x0162 displays as "1.62", not "1.98".
        const QString number =
            QStringLiteral("%1.%2")
                .arg((versionCode >> 8) & 0xff, 1, 16)
                .arg(versionCode & 0xff, 2, 16, QLatin1Char('0'));
        return isEmuTos ? QStringLiteral("EmuTOS (TOS %1)").arg(number) : number;
    }
};

/// Read the TOS version and EmuTOS marker from a ROM image's header.
///
/// Mirrors Hatari's own reading in `src/tos.c:TOS_LoadImage`:
///
///   - version  = big-endian 16-bit at offset 2
///   - versions reported as 0x0206 with 0x186A at offset 30 are TOS 2.08
///   - EmuTOS   = big-endian 32-bit `ETOS` (0x45544F53) at offset 0x2c
///
/// Returns false if the file is too small to hold a header or cannot be read,
/// in which case the caller may fall back to a filename heuristic.
bool readTosHeader(const QString &path, TosRom *rom);

/// Scan a directory for ROM images (`.img`, `.rom`). Versions come from each
/// image's header; a filename such as "TOS v1.04 (1989)(...)" is used only when
/// the header cannot be read.
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
