// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#include "emu/TosRom.h"

#include "emu/Paths.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace pist {

namespace {

/// "TOS v1.04 (1989)(Atari Corp)(Mega ST)(GB)[Rainbow TOS].img"
const QRegularExpression &versionRe()
{
    static const QRegularExpression re(QStringLiteral(R"(TOS\s+v(\d+)\.(\d+))"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

} // namespace

QList<TosRom> scanTosRoms(const QString &directory)
{
    QList<TosRom> roms;
    const QDir dir(directory);
    const QStringList entries =
        dir.entryList({QStringLiteral("*.img"), QStringLiteral("*.rom")}, QDir::Files, QDir::Name);

    for (const QString &entry : entries) {
        TosRom rom;
        rom.fileName = entry;
        rom.path = dir.absoluteFilePath(entry);

        auto m = versionRe().match(entry);
        if (m.hasMatch()) {
            const int major = m.captured(1).toInt();
            const int minor = m.captured(2).toInt();
            rom.versionCode = (major << 8) | minor;
            rom.versionKnown = true;
        }
        // EmuTOS images (etos*.img) do not carry a TOS version in their name.
        // Their reported version is not parsed here, so they stay "unknown" and
        // are preferred only over a known-incompatible ROM.

        roms.append(rom);
    }

    return roms;
}

QList<TosRom> findTosRoms()
{
    QList<TosRom> all;
    QSet<QString> seen;

    for (const QString &dir : paths::tosSearchPaths()) {
        for (const TosRom &rom : scanTosRoms(dir)) {
            if (seen.contains(rom.path))
                continue;
            seen.insert(rom.path);
            all.append(rom);
        }
    }

    return all;
}

TosRom selectPreferredRom(const QList<TosRom> &roms)
{
    // Three passes, best first.
    for (const TosRom &rom : roms) {
        if (rom.supportsAutostart())
            return rom;
    }
    for (const TosRom &rom : roms) {
        if (!rom.versionKnown)
            return rom;
    }
    if (!roms.isEmpty())
        return roms.first();
    return {};
}

} // namespace pist
