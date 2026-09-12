// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/TosRom.h"

#include "emu/Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

namespace pist {

namespace {

/// TOS version lives at offset 2 of the image, big-endian.
constexpr qint64 kVersionOffset = 2;
constexpr qint64 kEmuTosMagicOffset = 0x2c;
constexpr quint32 kEmuTosMagic = 0x45544F53; // 'ETOS'
constexpr qint64 kMinHeaderSize = 0x40;

/// TOS 2.06 images that are really 2.08 carry 0x186A at offset 30.
constexpr qint64 kTos208MarkerOffset = 30;
constexpr quint16 kTos206Version = 0x0206;
constexpr quint16 kTos208Marker = 0x186A;
constexpr quint16 kTos208Version = 0x0208;

quint16 bigEndian16(const QByteArray &data, qint64 offset)
{
    return static_cast<quint16>((static_cast<quint8>(data.at(offset)) << 8)
                                | static_cast<quint8>(data.at(offset + 1)));
}

quint32 bigEndian32(const QByteArray &data, qint64 offset)
{
    return (static_cast<quint32>(static_cast<quint8>(data.at(offset))) << 24)
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 1))) << 16)
         | (static_cast<quint32>(static_cast<quint8>(data.at(offset + 2))) << 8)
         | static_cast<quint32>(static_cast<quint8>(data.at(offset + 3)));
}

/// "TOS v1.04 (1989)(Atari Corp)(Mega ST)(GB)[Rainbow TOS].img"
/// Only a fallback: filenames are user-controlled and often say nothing.
const QRegularExpression &nameVersionRe()
{
    static const QRegularExpression re(QStringLiteral(R"(TOS\s+v(\d+)\.(\d+))"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

void applyFilenameHeuristic(TosRom *rom)
{
    auto m = nameVersionRe().match(rom->fileName);
    if (!m.hasMatch())
        return;
    // The digits are version *hex* bytes by convention, so "TOS v1.62" means
    // version code 0x0162 and "TOS v2.06" means 0x0206. Parsing them as decimal
    // would turn 1.62 into 0x013E.
    const int major = m.captured(1).toInt(nullptr, 16);
    const int minor = m.captured(2).toInt(nullptr, 16);
    rom->versionCode = (major << 8) | (minor & 0xff);
    rom->versionKnown = true;
    rom->versionFromHeader = false;
}

} // namespace

bool readTosHeader(const QString &path, TosRom *rom)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    const QByteArray header = file.read(kMinHeaderSize);
    if (header.size() < kMinHeaderSize)
        return false;

    quint16 version = bigEndian16(header, kVersionOffset);

    // TOS 2.06 images that are actually 2.08, as Hatari itself adjusts.
    if (version == kTos206Version
        && bigEndian16(header, kTos208MarkerOffset) == kTos208Marker) {
        version = kTos208Version;
    }

    rom->versionCode = version;
    rom->versionKnown = true;
    rom->versionFromHeader = true;
    rom->isEmuTos = (bigEndian32(header, kEmuTosMagicOffset) == kEmuTosMagic);
    return true;
}

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

        // The header is authoritative; the filename is only a fallback for
        // images whose header cannot be read. Guessing from a filename would
        // let an old ROM be misclassified as "unknown" and slip past the
        // autostart check, which is exactly the silent hang it exists to
        // prevent.
        if (!readTosHeader(rom.path, &rom))
            applyFilenameHeuristic(&rom);

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
