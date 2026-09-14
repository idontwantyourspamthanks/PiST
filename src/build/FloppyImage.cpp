// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/FloppyImage.h"

#include <QFile>
#include <QFileInfo>
#include <QVector>

namespace pist {
namespace floppy {

namespace {

// The canonical 720 KiB geometry, taken byte for byte from a real mkfs.vfat
// image (the reference in the test): 512-byte sectors, 2-sector clusters,
// two FATs of 3 sectors, 112 root-directory entries, 1440 sectors total.
// Data starts at sector 1 + 2*3 + 7 = 14, so cluster 2 lives at offset 7168.
constexpr int kSectorSize = 512;
constexpr int kSectorsPerCluster = 2;
constexpr int kClusterSize = kSectorSize * kSectorsPerCluster;
constexpr int kReservedSectors = 1;
constexpr int kFatCount = 2;
constexpr int kFatSectors = 3;
constexpr int kRootEntries = 112;
constexpr int kRootSectors = kRootEntries * 32 / kSectorSize;  // 7
constexpr int kTotalSectors = 1440;
constexpr int kFirstDataSector = kReservedSectors + kFatCount * kFatSectors + kRootSectors;
// Media descriptor 0xF0, matching mkfs.vfat's canonical 720K image (the Atari
// 720K convention is 0xF9; TOS accepts either, but the FAT's first entry
// already carries 0xF0, and consistency keeps the geometry testable against
// the reference).
constexpr quint8 kMediaByte = 0xF0;

void put16(QVector<quint8> &image, int offset, quint16 value)
{
    image[offset] = value & 0xff;
    image[offset + 1] = (value >> 8) & 0xff;
}

void put32(QVector<quint8> &image, int offset, quint32 value)
{
    for (int i = 0; i < 4; ++i)
        image[offset + i] = (value >> (i * 8)) & 0xff;
}

/// FAT12 entries are 12 bits, packed little-endian two entries per three
/// bytes: [ll HH | hh LL] with the entry number's parity choosing the halves.
void setFat12Entry(QVector<quint8> &fat, int index, quint16 value)
{
    const int at = index + index / 2;
    if (index & 1) {
        fat[at] = (fat[at] & 0x0f) | ((value << 4) & 0xf0);
        fat[at + 1] = (value >> 4) & 0xff;
    } else {
        fat[at] = value & 0xff;
        fat[at + 1] = (fat[at + 1] & 0xf0) | ((value >> 8) & 0x0f);
    }
}

/// An 8.3 name, space-padded.
void putName(QVector<quint8> &image, int offset, const char *name)
{
    for (int i = 0; i < 11; ++i)
        image[offset + i] = name[i] ? quint8(name[i]) : 0x20;
}

/// One 32-byte directory entry.
void putDirEntry(QVector<quint8> &image, int offset, const char *name, quint8 attr,
                 quint16 cluster, quint32 size)
{
    putName(image, offset, name);
    image[offset + 11] = attr;
    put16(image, offset + 26, cluster);
    put32(image, offset + 28, size);
}

int clusterSector(int cluster)
{
    return kFirstDataSector + (cluster - 2) * kSectorsPerCluster;
}

} // namespace

bool writeAutoFolderImage(const QString &imagePath, const QString &programPath,
                          QString *error)
{
    QFile program(programPath);
    if (!program.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(programPath, program.errorString());
        return false;
    }
    const QByteArray prg = program.readAll();

    QVector<quint8> image(kTotalSectors * kSectorSize, 0);

    // Boot sector, with the BPB byte-for-byte the mkfs.vfat reference's.
    image[0] = 0xeb;
    image[1] = 0x3c;
    image[2] = 0x90;
    for (int i = 0; i < 8; ++i)
        image[3 + i] = quint8("mkfs.fat"[i]);
    put16(image, 11, kSectorSize);
    image[13] = kSectorsPerCluster;
    put16(image, 14, kReservedSectors);
    image[16] = kFatCount;
    put16(image, 17, kRootEntries);
    put16(image, 19, kTotalSectors);
    image[21] = kMediaByte;
    put16(image, 22, kFatSectors);
    put16(image, 24, 9);   // sectors per track
    put16(image, 26, 2);   // heads
    put16(image, 510, 0xaa55);

    // Both FAT copies: the reserved entries (media + end-of-chain markers)
    // and our chain. AUTO's directory is one cluster (2); the program starts
    // at cluster 3 and runs contiguously to the end of its size.
    const int prgClusters = (int(prg.size()) + kClusterSize - 1) / kClusterSize;
    for (int f = 0; f < kFatCount; ++f) {
        const int fatStart = (kReservedSectors + f * kFatSectors) * kSectorSize;
        QVector<quint8> fat = image.mid(fatStart, kFatSectors * kSectorSize);
        fat[0] = kMediaByte;
        fat[1] = 0xff;
        fat[2] = 0xff;
        setFat12Entry(fat, 2, 0xfff);  // AUTO directory: last cluster of its chain
        for (int c = 0; c < prgClusters; ++c)
            setFat12Entry(fat, 3 + c, c + 1 == prgClusters ? 0xfff : quint16(4 + c));
        for (int i = 0; i < fat.size(); ++i)
            image[fatStart + i] = fat[i];
    }

    // Root directory: the AUTO subdirectory and an empty EMUDESK.INF. The INF
    // is what arms the --debug-except autostart mask: Hatari's deferral hook
    // fires when TOS loads it, and an empty file is enough to fire it.
    const int rootStart = (kReservedSectors + kFatCount * kFatSectors) * kSectorSize;
    putDirEntry(image, rootStart, "AUTO       ", 0x10, 2, 0);
    putDirEntry(image, rootStart + 32, "EMUDESK INF", 0x20, 0, 0);

    // AUTO's directory cluster: ".", "..", and the program.
    const int autoStart = clusterSector(2) * kSectorSize;
    putDirEntry(image, autoStart, ".          ", 0x10, 2, 0);
    putDirEntry(image, autoStart + 32, "..         ", 0x10, 0, 0);
    putDirEntry(image, autoStart + 64, "PROG    PRG", 0x20, 3, quint32(prg.size()));

    // The program's clusters, contiguous from cluster 3.
    const int dataStart = clusterSector(3) * kSectorSize;
    for (int i = 0; i < prg.size(); ++i)
        image[dataStart + i] = quint8(prg[i]);

    QFile out(imagePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("cannot write %1: %2").arg(imagePath, out.errorString());
        return false;
    }
    if (out.write(reinterpret_cast<const char *>(image.constData()), image.size()) != image.size()) {
        *error = QStringLiteral("short write to %1").arg(imagePath);
        return false;
    }
    return true;
}

} // namespace floppy

} // namespace pist
