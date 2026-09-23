// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/FloppyImage.h"

#include "support/FileWrite.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QVector>

#include <cstring>

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
constexpr int kMaxDataClusters = (kTotalSectors - kFirstDataSector) / kSectorsPerCluster;
// Media descriptor 0xF0, matching mkfs.vfat's canonical 720K image (the Atari
// 720K convention is 0xF9; TOS accepts either, but the FAT's first entry
// already carries 0xF0, and consistency keeps the geometry testable against
// the reference).
constexpr quint8 kMediaByte = 0xF0;

void setError(QString *error, const QString &text)
{
    if (error)
        *error = text;
}

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

void put16b(QByteArray &buf, int offset, quint16 value)
{
    buf[offset] = char((value >> 8) & 0xff);
    buf[offset + 1] = char(value & 0xff);
}

quint16 read16(const QByteArray &buf, int offset)
{
    return quint16(quint8(buf.at(offset)) | (quint16(quint8(buf.at(offset + 1))) << 8));
}

quint16 readBe16(const QByteArray &buf, int offset)
{
    return quint16((quint8(buf.at(offset)) << 8) | quint8(buf.at(offset + 1)));
}

quint32 read32(const QByteArray &buf, int offset)
{
    return quint32(quint8(buf.at(offset)))
        | (quint32(quint8(buf.at(offset + 1))) << 8)
        | (quint32(quint8(buf.at(offset + 2))) << 16)
        | (quint32(quint8(buf.at(offset + 3))) << 24);
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

quint16 fat12Get(const QByteArray &image, int fatOffset, int index)
{
    const int at = fatOffset + index + index / 2;
    if (at + 1 >= image.size())
        return 0;
    const quint16 val = read16(image, at);
    return index & 1 ? quint16(val >> 4) : quint16(val & 0x0fff);
}

/// An 8.3 name, space-padded.
///
/// A literal shorter than the 11-character field stops at its own terminator
/// rather than being read to the end of the field: the padding is what the
/// missing characters become, and the bytes past the NUL are not the caller's
/// to hand over (every current call site passes exactly 11, which is what hid
/// it — finding NIT-10).
void putName(QVector<quint8> &image, int offset, const char *name)
{
    int i = 0;
    for (; i < 11 && name[i]; ++i)
        image[offset + i] = quint8(name[i]);
    for (; i < 11; ++i)
        image[offset + i] = 0x20;
}

void putName11(QVector<quint8> &image, int offset, const QByteArray &name11)
{
    for (int i = 0; i < 11; ++i)
        image[offset + i] = i < name11.size() ? quint8(name11.at(i)) : 0x20;
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

void putDirEntry11(QVector<quint8> &image, int offset, const QByteArray &name11, quint8 attr,
                   quint16 cluster, quint32 size)
{
    putName11(image, offset, name11);
    image[offset + 11] = attr;
    put16(image, offset + 26, cluster);
    put32(image, offset + 28, size);
}

int clusterSector(int cluster)
{
    return kFirstDataSector + (cluster - 2) * kSectorsPerCluster;
}

int clusterOffset(int cluster)
{
    return clusterSector(cluster) * kSectorSize;
}

bool isMsa(const QByteArray &bytes)
{
    return bytes.size() >= 10 && readBe16(bytes, 0) == 0x0E0F;
}

/// The documented MSA run-length encoding (quoted in Hatari's
/// src/floppies/msa.c): literal bytes pass through, and a run is
/// `$E5 <value> <16-bit big-endian length>` — six $AA bytes are $E5 $AA $00
/// $06. Runs of four or more pay for themselves; shorter ones are written
/// literally, except $E5 itself, which always has to be escaped because the
/// marker is indistinguishable from data otherwise. The 16-bit length matters:
/// a zero-filled track is the normal case on a floppy, and an 8-bit count
/// cannot cover one.
QByteArray compressTrack(const QByteArray &src)
{
    QByteArray out;
    out.reserve(src.size());
    const auto *s = reinterpret_cast<const quint8 *>(src.constData());
    int i = 0;
    while (i < src.size()) {
        const quint8 b = s[i];
        int run = 1;
        while (i + run < src.size() && s[i + run] == b && run < 0xFFFF)
            ++run;
        if (b == 0xE5 || run >= 4) {
            out.append(char(0xE5));
            out.append(char(b));
            out.append(char((run >> 8) & 0xff));
            out.append(char(run & 0xff));
            i += run;
        } else {
            out.append(char(b));
            ++i;
        }
    }
    return out;
}

/// The documented inverse of the encoding above: literal bytes, and
/// `$E5 <value> <16-bit big-endian run>`.
///
/// Deliberately stricter than Hatari's decoder, which clamps an over-long run
/// and simply stops when the input runs out, leaving the rest of the track as
/// whatever the buffer held. Hatari can afford that — a partly-zero disk still
/// boots, and refusing a game disk is worse than loading it imperfectly. This
/// reader gates a *rewriting* file manager: `loadRaw` success is what lets
/// `updateImage` stage a new image over the user's file, so a damaged archive
/// that decoded into a partly-zero disk would be silently re-encoded and
/// written back. A malformed image therefore fails here with an error and the
/// original is never touched.
bool decompressTrack(const QByteArray &src, int trackSize, QByteArray *out)
{
    out->fill('\0', trackSize);
    auto *d = reinterpret_cast<quint8 *>(out->data());
    const auto *s = reinterpret_cast<const quint8 *>(src.constData());
    int si = 0;
    int di = 0;
    while (di < trackSize && si < src.size()) {
        if (s[si] != 0xE5) {
            d[di++] = s[si++];
            continue;
        }
        // Marker plus value plus a 16-bit length: four bytes in all.
        if (si + 4 > src.size())
            return false;   // truncated run header
        const quint8 val = s[si + 1];
        const int count = (int(quint8(s[si + 2])) << 8) | quint8(s[si + 3]);
        si += 4;
        // A zero-length run carries no meaning, and a run past the end of the
        // track can only come from damage.
        if (count <= 0 || di + count > trackSize)
            return false;
        std::memset(d + di, val, size_t(count));
        di += count;
    }
    return di == trackSize;
}

bool geometryFromSectors(int sectors, int *tracks, int *sides, int *spt)
{
    struct Guess { int tracks; int sides; int spt; };
    static const Guess guesses[] = {
        {80, 2, 9},
        {80, 1, 9},
        {80, 2, 10},
        {82, 2, 9},
        {80, 2, 11},
        {80, 2, 18},
        {81, 2, 9},
        {83, 2, 9},
    };
    for (const Guess &g : guesses) {
        if (g.tracks * g.sides * g.spt == sectors) {
            *tracks = g.tracks;
            *sides = g.sides;
            *spt = g.spt;
            return true;
        }
    }
    return false;
}

bool decodeMsa(const QByteArray &msa, QByteArray *raw, QString *error)
{
    if (!isMsa(msa)) {
        setError(error, QObject::tr("not an MSA image"));
        return false;
    }
    const int spt = readBe16(msa, 2);
    const int sides = readBe16(msa, 4) + 1;
    const int startTrack = readBe16(msa, 6);
    const int endTrack = readBe16(msa, 8);
    if (spt <= 0 || spt > 32 || sides < 1 || sides > 2 || startTrack < 0
        || endTrack < startTrack || endTrack > 85) {
        setError(error, QObject::tr("MSA header is not a plausible ST floppy"));
        return false;
    }
    const int trackSize = spt * kSectorSize;
    const int trackCount = endTrack + 1;
    raw->fill('\0', trackCount * sides * trackSize);

    int cursor = 10;
    for (int track = startTrack; track <= endTrack; ++track) {
        for (int side = 0; side < sides; ++side) {
            if (cursor + 2 > msa.size()) {
                setError(error, QObject::tr("truncated MSA track header"));
                return false;
            }
            const int length = readBe16(msa, cursor);
            cursor += 2;
            if (length <= 0 || cursor + length > msa.size()) {
                setError(error, QObject::tr("truncated MSA track data"));
                return false;
            }
            const QByteArray packed = msa.mid(cursor, length);
            cursor += length;
            QByteArray trackData;
            if (length == trackSize) {
                trackData = packed;
            } else if (!decompressTrack(packed, trackSize, &trackData)) {
                setError(error, QObject::tr("MSA track %1 side %2 does not decompress")
                                    .arg(track)
                                    .arg(side));
                return false;
            }
            const int offset = (track * sides + side) * trackSize;
            raw->replace(offset, trackSize, trackData);
        }
    }
    return true;
}

bool encodeMsa(const QByteArray &raw, QByteArray *msa, QString *error)
{
    if (raw.size() % kSectorSize != 0 || raw.isEmpty()) {
        setError(error, QObject::tr("raw floppy image is not a whole number of sectors"));
        return false;
    }
    int tracks = 0, sides = 0, spt = 0;
    if (!geometryFromSectors(raw.size() / kSectorSize, &tracks, &sides, &spt)) {
        setError(error, QObject::tr("cannot infer MSA geometry from a %1-byte image")
                            .arg(raw.size()));
        return false;
    }
    const int trackSize = spt * kSectorSize;
    msa->clear();
    msa->resize(10);
    put16b(*msa, 0, 0x0E0F);
    put16b(*msa, 2, quint16(spt));
    put16b(*msa, 4, quint16(sides - 1));
    put16b(*msa, 6, 0);
    put16b(*msa, 8, quint16(tracks - 1));

    for (int track = 0; track < tracks; ++track) {
        for (int side = 0; side < sides; ++side) {
            const int offset = (track * sides + side) * trackSize;
            const QByteArray trackData = raw.mid(offset, trackSize);
            const QByteArray packed = compressTrack(trackData);
            const QByteArray body = packed.size() < trackSize ? packed : trackData;
            QByteArray header(2, '\0');
            put16b(header, 0, quint16(body.size()));
            *msa += header;
            *msa += body;
        }
    }
    return true;
}

struct Geo {
    int sectorSize = kSectorSize;
    int sectorsPerCluster = kSectorsPerCluster;
    int reservedSectors = kReservedSectors;
    int fatCount = kFatCount;
    int fatSectors = kFatSectors;
    int rootEntries = kRootEntries;
    int rootSectors = kRootSectors;
    int totalSectors = kTotalSectors;
    int firstDataSector = kFirstDataSector;
    int clusterSize = kClusterSize;
};

void apply720k(Geo *g)
{
    *g = Geo{};
}

bool parseGeo(const QByteArray &image, Geo *g)
{
    if (image.size() < kSectorSize)
        return false;

    Geo parsed;
    parsed.sectorSize = read16(image, 11);
    parsed.sectorsPerCluster = quint8(image.at(13));
    parsed.reservedSectors = read16(image, 14);
    parsed.fatCount = quint8(image.at(16));
    parsed.rootEntries = read16(image, 17);
    parsed.totalSectors = read16(image, 19);
    if (parsed.totalSectors == 0)
        parsed.totalSectors = int(read32(image, 32));
    parsed.fatSectors = read16(image, 22);

    if (parsed.sectorSize != kSectorSize || parsed.sectorsPerCluster <= 0
        || parsed.reservedSectors <= 0 || parsed.fatCount <= 0 || parsed.fatSectors <= 0
        || parsed.rootEntries <= 0 || parsed.totalSectors <= 0) {
        if (image.size() == 720 * 1024) {
            apply720k(g);
            return true;
        }
        return false;
    }

    // 64-bit throughout: a crafted BPB pushes these products past INT_MAX, and
    // a wrapped — often negative — value sails through the bounds check that
    // exists to contain it. Validating the whole layout here is what lets every
    // later offset be trusted.
    const qint64 rootSectors =
        (qint64(parsed.rootEntries) * 32 + parsed.sectorSize - 1) / parsed.sectorSize;
    const qint64 firstDataSector = qint64(parsed.reservedSectors)
        + qint64(parsed.fatCount) * parsed.fatSectors + rootSectors;
    parsed.clusterSize = parsed.sectorSize * parsed.sectorsPerCluster;
    if (firstDataSector * parsed.sectorSize > image.size()
        || firstDataSector + 2 > parsed.totalSectors
        || qint64(parsed.totalSectors) * parsed.sectorSize > image.size()) {
        if (image.size() == 720 * 1024) {
            apply720k(g);
            return true;
        }
        return false;
    }
    parsed.rootSectors = int(rootSectors);
    parsed.firstDataSector = int(firstDataSector);
    *g = parsed;
    return true;
}

qint64 geoClusterOffset(const Geo &g, int cluster)
{
    return (qint64(g.firstDataSector) + (qint64(cluster) - 2) * g.sectorsPerCluster)
           * g.sectorSize;
}

/// The root directory's bytes, or false with `error` set when the geometry does
/// not describe a root directory inside this image. Shared by both readers so
/// neither can omit the bounds check the other has.
bool rootDirBytes(const QByteArray &raw, const Geo &g, QByteArray *out, QString *error)
{
    const qint64 start =
        (qint64(g.reservedSectors) + qint64(g.fatCount) * g.fatSectors) * g.sectorSize;
    const qint64 bytes = qint64(g.rootSectors) * g.sectorSize;
    if (start < 0 || bytes <= 0 || start + bytes > raw.size()) {
        setError(error, QObject::tr("floppy root directory is truncated"));
        return false;
    }
    *out = raw.mid(int(start), int(bytes));
    return true;
}

/// The display name is joined into paths that become host file names on
/// copy-out, and the on-disk bytes are untrusted: drop control characters and
/// both separators, so a crafted entry cannot smuggle a traversal or an
/// absolute path. parseDirBytes already skips exact "." and "..", and with
/// separators gone a component can never be anything but a flat name.
QString sanitize83Component(const QString &text)
{
    QString out;
    for (const QChar c : text) {
        const ushort u = c.unicode();
        out += (u >= 0x20 && u != '/' && u != '\\') ? c : QLatin1Char('_');
    }
    return out;
}

/// The bytes of one 8.3 name field, up to the padding at its end, as TOS text.
/// Space is the format's pad byte, but a third-party imaging tool may pad with
/// NUL instead; reading that byte as a name character turned the padding into
/// literal underscores in every path that names the entry (`NULPAD.TXT` listed
/// as `NULPAD__.TXT`) and in the 8.3 name a carry-forward wrote back.
QString latin1Field(const QByteArray &field)
{
    const int end = field.indexOf('\0');
    return QString::fromLatin1(end < 0 ? field : field.left(end));
}

QString display83(const QByteArray &name11)
{
    const QString name = sanitize83Component(latin1Field(name11.left(8)).trimmed());
    const QString ext = sanitize83Component(latin1Field(name11.mid(8, 3)).trimmed());
    if (ext.isEmpty())
        return name;
    return name + QLatin1Char('.') + ext;
}

/// Read a cluster chain's bytes, refusing a damaged chain instead of returning
/// whatever could be collected.
///
/// `start` is the entry's first cluster and `fileSize` its declared size (0 for
/// a directory, which has none). Returns false with `error` set when the chain
/// loops back on a cluster it already walked, links to a cluster outside the
/// image, or — for a file — ends before `fileSize` bytes: a damaged chain used
/// to come back as a shorter buffer, indistinguishable from a short file, so
/// `readFileRaw` reported a truncated copy-out as success and `updateImage`
/// carried the truncation into the replacement image and renamed that over the
/// user's own, destroying the orphaned clusters a repair would need. Damage now
/// fails closed, as a damaged MSA archive already does in loadRaw.
///
/// The walk stays bounded: `cluster < 0xFF0` caps the cluster number, the cycle
/// set visits each of those at most once, and every cluster read must lie whole
/// inside the image, so memory cannot outgrow the image itself.
bool readChain(const QByteArray &image, const Geo &g, int start, quint32 fileSize,
               bool isDir, QByteArray *out, QString *error)
{
    out->clear();
    QSet<int> seen;
    int cluster = start;
    const int fatOff = g.reservedSectors * g.sectorSize;
    while (cluster >= 2 && cluster < 0xFF0) {
        if (seen.contains(cluster)) {
            setError(error, QObject::tr("its cluster chain loops back to cluster %1")
                                .arg(cluster));
            out->clear();
            return false;
        }
        seen.insert(cluster);
        const qint64 off = geoClusterOffset(g, cluster);
        if (off < 0 || off + g.clusterSize > image.size()) {
            setError(error, QObject::tr("its cluster chain links to cluster %1, outside "
                                        "the image")
                                .arg(cluster));
            out->clear();
            return false;
        }
        *out += image.mid(int(off), g.clusterSize);
        cluster = int(fat12Get(image, fatOff, cluster));
    }
    if (isDir)
        return true;
    if (quint32(out->size()) < fileSize) {
        setError(error, QObject::tr("only %1 of the %2 bytes its directory entry declares "
                                    "are reachable")
                            .arg(out->size())
                            .arg(fileSize));
        out->clear();
        return false;
    }
    // A chain's last cluster is normally only partly used; the directory entry's
    // size is what says where the file ends.
    out->resize(int(fileSize));
    return true;
}

struct RawDirEntry {
    QString name;
    QByteArray name11;
    bool isDir = false;
    quint16 cluster = 0;
    quint32 size = 0;
};

QVector<RawDirEntry> parseDirBytes(const QByteArray &dir)
{
    QVector<RawDirEntry> out;
    for (int off = 0; off + 32 <= dir.size(); off += 32) {
        const quint8 first = quint8(dir.at(off));
        if (first == 0)
            break;
        if (first == 0xE5)
            continue;
        const quint8 attr = quint8(dir.at(off + 11));
        if (attr == 0x0F)
            continue;
        if ((attr & 0x08) && !(attr & 0x10))
            continue;
        const QByteArray name11 = dir.mid(off, 11);
        const QString name = display83(name11);
        if (name == QLatin1String(".") || name == QLatin1String("..") || name.isEmpty())
            continue;
        RawDirEntry e;
        e.name = name;
        e.name11 = name11;
        e.isDir = attr & 0x10;
        e.cluster = read16(dir, off + 26);
        e.size = read32(dir, off + 28);
        out.append(e);
    }
    return out;
}

const RawDirEntry *dirEntryNamed(const QVector<RawDirEntry> &entries, const QString &name)
{
    for (const RawDirEntry &e : entries) {
        if (e.name.compare(name, Qt::CaseInsensitive) == 0)
            return &e;
    }
    return nullptr;
}

void listDir(const QByteArray &image, const Geo &g, const QByteArray &dirBytes,
             const QString &prefix, int depth, QSet<int> *visitedDirs, QVector<Entry> *out)
{
    // A listing of one disk cannot legitimately be large; the bound turns a
    // hostile image into a truncated answer rather than unbounded memory. It
    // sits above the legitimate worst case — 713 directory clusters x 32
    // entries plus the root's 112 is ~22,928 (reachable with zero-byte
    // files) — so it can only ever clip a hostile listing.
    if (depth > 32 || out->size() > 25000)
        return;
    for (const RawDirEntry &e : parseDirBytes(dirBytes)) {
        Entry item;
        item.path = prefix.isEmpty() ? e.name : prefix + QLatin1Char('/') + e.name;
        item.name11 = e.name11;
        item.isDirectory = e.isDir;
        item.size = e.isDir ? 0 : e.size;
        item.cluster = e.cluster;
        out->append(item);
        if (e.isDir) {
            // Many entries may name the same directory cluster, and a crafted
            // image can even make directories point at each other in a cycle;
            // readChain's own cycle set dies with each call, so the walk
            // carries one that spans it: expand a cluster at most once. This
            // is also what keeps the listing bounded for degenerate trees.
            if (visitedDirs->contains(e.cluster))
                continue;
            visitedDirs->insert(e.cluster);
            // A listing stays best-effort: a damaged directory chain leaves the
            // folder listed but unexpanded instead of blanking the panel. The
            // readers a caller names by path, and the rewriting updateImage,
            // fail closed on the same damage.
            QByteArray child;
            if (readChain(image, g, e.cluster, 0, true, &child, nullptr))
                listDir(image, g, child, item.path, depth + 1, visitedDirs, out);
        }
    }
}

QChar sanitizeChar(QChar c)
{
    const QChar u = c.toUpper();
    if (u.isLetterOrNumber())
        return u;
    static const QString extra = QStringLiteral("_!#$%&()~-{}@^");
    if (extra.contains(u))
        return u;
    return QLatin1Char('_');
}

QString sanitizeComponent(const QString &text, bool allowEmpty)
{
    QString out;
    out.reserve(text.size());
    for (QChar c : text)
        out += sanitizeChar(c);
    if (out.isEmpty())
        return allowEmpty ? QString() : QString(QLatin1Char('_'));
    return out;
}

QByteArray name11FromHost(const QString &component)
{
    QString base = component;
    QString ext;
    const int dot = component.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        base = component.left(dot);
        ext = component.mid(dot + 1);
    }
    base = sanitizeComponent(base, false).left(8);
    ext = sanitizeComponent(ext, true).left(3);
    QByteArray n(11, ' ');
    for (int i = 0; i < base.size() && i < 8; ++i)
        n[i] = char(base.at(i).toLatin1());
    for (int i = 0; i < ext.size() && i < 3; ++i)
        n[8 + i] = char(ext.at(i).toLatin1());
    return n;
}

QByteArray uniqueName11(QByteArray name11, QSet<QByteArray> *used)
{
    if (!used->contains(name11)) {
        used->insert(name11);
        return name11;
    }
    QByteArray base = name11.left(8);
    while (base.endsWith(' '))
        base.chop(1);
    const QByteArray ext = name11.mid(8);
    for (int n = 1; n < 1000; ++n) {
        const QByteArray suffix = QByteArray::number(n);
        QByteArray stem = base.left(qMax(1, 8 - suffix.size()));
        stem += suffix;
        while (stem.size() < 8)
            stem += ' ';
        const QByteArray cand = stem.left(8) + ext;
        if (!used->contains(cand)) {
            used->insert(cand);
            return cand;
        }
    }
    return {};
}

struct Node {
    QByteArray name11;
    bool isDir = false;
    QByteArray data;
    QVector<Node> children;
    int cluster = 0;
    int clusterCount = 0;
};

Node *ensureChildDir(Node *parent, const QByteArray &name11)
{
    for (Node &child : parent->children) {
        if (child.isDir && child.name11 == name11)
            return &child;
    }
    Node child;
    child.name11 = name11;
    child.isDir = true;
    parent->children.append(child);
    return &parent->children.last();
}

int clustersForBytes(int bytes)
{
    if (bytes <= 0)
        return 0;
    return (bytes + kClusterSize - 1) / kClusterSize;
}

int clustersForDir(int entryCount)
{
    const int bytes = entryCount * 32;
    return qMax(1, clustersForBytes(bytes));
}

bool allocateClusters(Node *node, bool isRoot, int *nextCluster, QString *error)
{
    for (Node &child : node->children) {
        if (child.isDir) {
            if (!allocateClusters(&child, false, nextCluster, error))
                return false;
        } else {
            child.clusterCount = clustersForBytes(child.data.size());
            if (child.clusterCount == 0) {
                child.cluster = 0;
                continue;
            }
            if (*nextCluster - 2 + child.clusterCount > kMaxDataClusters) {
                setError(error, QObject::tr("the selected files do not fit on a 720 KiB floppy"));
                return false;
            }
            child.cluster = *nextCluster;
            *nextCluster += child.clusterCount;
        }
    }
    if (isRoot)
        return true;

    node->clusterCount = clustersForDir(2 + node->children.size());
    if (*nextCluster - 2 + node->clusterCount > kMaxDataClusters) {
        setError(error, QObject::tr("the selected files do not fit on a 720 KiB floppy"));
        return false;
    }
    node->cluster = *nextCluster;
    *nextCluster += node->clusterCount;
    return true;
}

void writeFatChain(QVector<quint8> &fat, int start, int count)
{
    for (int c = 0; c < count; ++c)
        setFat12Entry(fat, start + c, c + 1 == count ? 0xfff : quint16(start + c + 1));
}

void writeFatForNode(QVector<quint8> &fat, const Node &node)
{
    if (node.clusterCount > 0)
        writeFatChain(fat, node.cluster, node.clusterCount);
    for (const Node &child : node.children)
        writeFatForNode(fat, child);
}

/// One directory's entries. `parentCluster` is the start cluster of the
/// directory enclosing this one — what FAT requires a `..` entry to hold, and 0
/// for a child of the root directory, which has no cluster of its own.
/// Hard-coding 0 made every subdirectory claim the root as its parent, so a
/// depth-2 tree exported from the project files (SUB/INNER) sent TOS's "open
/// parent" straight to the root and had host-side FAT checkers flag the image;
/// PiST's own reader never noticed, because it skips `.`/`..` and resolves
/// top-down (finding MAJ-39).
void writeDirEntries(QVector<quint8> &image, int offset, const Node &dir, bool includeDots,
                     quint16 parentCluster)
{
    int at = offset;
    if (includeDots) {
        putDirEntry(image, at, ".          ", 0x10, quint16(dir.cluster), 0);
        at += 32;
        putDirEntry(image, at, "..         ", 0x10, parentCluster, 0);
        at += 32;
    }
    for (const Node &child : dir.children) {
        const quint8 attr = child.isDir ? quint8(0x10) : quint8(0x20);
        const quint16 cluster = quint16(child.cluster);
        const quint32 size = child.isDir ? 0 : quint32(child.data.size());
        putDirEntry11(image, at, child.name11, attr, cluster, size);
        at += 32;
    }
}

/// Write every directory's entries and every file's data below `node`.
///
/// `parentCluster` is the enclosing directory's own start cluster, which the
/// directory written here records as its `..`. The root owns no cluster — the
/// FAT layout fixes its directory in the reserved area and allocateClusters
/// leaves `cluster` at 0 — so root's children are the ones whose `..` is 0.
void writeNodeData(QVector<quint8> &image, const Node &node, quint16 parentCluster)
{
    // The root's own entries go to the fixed root area in writeImage; every
    // other directory here is an allocation with a cluster of its own.
    if (node.isDir && node.clusterCount > 0) {
        const int start = clusterOffset(node.cluster);
        writeDirEntries(image, start, node, true, parentCluster);
    }
    if (!node.isDir && node.clusterCount > 0) {
        const int start = clusterOffset(node.cluster);
        for (int i = 0; i < node.data.size(); ++i)
            image[start + i] = quint8(node.data.at(i));
    }
    for (const Node &child : node.children)
        writeNodeData(image, child, quint16(node.cluster));
}

void writeBootSector(QVector<quint8> &image)
{
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
}

/// A `destPath` in the one form the tree walk uses: `/`-separated, no leading
/// separator, and the `.` root shortcut spelled as nothing.
QString normalizeDest(const QString &destPath)
{
    QString dest = QDir::fromNativeSeparators(destPath).trimmed();
    while (dest.startsWith(QLatin1Char('/')))
        dest.remove(0, 1);
    if (dest == QLatin1String("."))
        dest.clear();
    return dest;
}

/// The 8.3 name one path component is written with: the raw field a carried
/// entry brought along (`carried` maps a normalised dest path to the name11 its
/// directory holds), otherwise the component's host spelling. Deriving a name
/// that already exists on the disk instead of carrying it rewrote unrelated
/// entries whenever a file was copied onto a disk — lower-case names came back
/// upper-cased, Latin-1 accents were mangled (`0xE9` → `0xC9`), and a NUL-padded
/// field became literal underscores (finding MIN-38).
QByteArray carriedName11(const QHash<QString, QByteArray> &carried, const QString &key,
                         const QString &component)
{
    const auto it = carried.constFind(key);
    return it == carried.constEnd() ? name11FromHost(component) : it.value();
}

bool insertItem(Node *root, const Item &item, const QHash<QString, QByteArray> &carried,
                QString *error)
{
    const QString dest = normalizeDest(item.destPath);
    if (dest.isEmpty()) {
        if (item.isDirectory)
            return true;
        setError(error, QObject::tr("a file cannot be written to the floppy root without a name"));
        return false;
    }

    const QStringList parts = dest.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    Node *dir = root;
    QString key;
    for (int i = 0; i < parts.size(); ++i) {
        const bool last = (i + 1 == parts.size());
        const bool wantDir = !last || item.isDirectory;
        key = key.isEmpty() ? parts.at(i) : key + QLatin1Char('/') + parts.at(i);
        const QByteArray wanted = carriedName11(carried, key, parts.at(i));

        Node *existing = nullptr;
        for (Node &child : dir->children) {
            if (child.name11 == wanted) {
                existing = &child;
                break;
            }
        }
        if (existing) {
            if (wantDir) {
                if (!existing->isDir) {
                    setError(error, QObject::tr("%1 collides with a file on the floppy").arg(dest));
                    return false;
                }
                dir = existing;
                continue;
            }
            if (existing->isDir) {
                setError(error, QObject::tr("%1 collides with a folder on the floppy")
                                    .arg(dest));
                return false;
            }
            // A second file with the same 8.3 name falls through to the
            // uniquifier below, so copying onto a disk twice duplicates
            // (ONE.TXT, then ONE1.TXT) instead of failing the write.
        }

        QSet<QByteArray> used;
        for (const Node &child : dir->children)
            used.insert(child.name11);
        const QByteArray name11 = uniqueName11(wanted, &used);
        if (name11.isEmpty()) {
            setError(error, QObject::tr("too many name collisions for %1").arg(parts.at(i)));
            return false;
        }
        if (wantDir) {
            dir = ensureChildDir(dir, name11);
            continue;
        }
        Node file;
        file.name11 = name11;
        file.isDir = false;
        file.data = item.data;
        dir->children.append(file);
    }
    return true;
}

void collectPath(const QString &root, const QString &path, bool recurseHidden,
                 QVector<Item> *items, QString *error, bool *ok)
{
    if (!*ok)
        return;
    const QFileInfo info(path);
    if (!info.exists()) {
        setError(error, QObject::tr("missing %1").arg(path));
        *ok = false;
        return;
    }

    QString rel = QDir::fromNativeSeparators(QDir(root).relativeFilePath(info.absoluteFilePath()));
    if (rel.startsWith(QLatin1String("../")) || rel == QLatin1String(".."))
        rel = info.fileName();
    if (rel == QLatin1String("."))
        rel.clear();

    if (info.isDir()) {
        if (!rel.isEmpty()) {
            Item dir;
            dir.destPath = rel;
            dir.isDirectory = true;
            items->append(dir);
        }
        const QDir dir(info.absoluteFilePath());
        const QFileInfoList entries = dir.entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (entry.isSymLink())
                continue;
            if (!recurseHidden && entry.fileName().startsWith(QLatin1Char('.')))
                continue;
            collectPath(root, entry.absoluteFilePath(), false, items, error, ok);
        }
        return;
    }

    if (!info.isFile())
        return;

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QObject::tr("cannot read %1: %2").arg(path, file.errorString()));
        *ok = false;
        return;
    }
    Item item;
    item.destPath = rel.isEmpty() ? info.fileName() : rel;
    item.data = file.readAll();
    items->append(item);
}

} // namespace

bool writeAutoFolderImage(const QString &imagePath, const QString &programPath,
                          QString *error)
{
    QFile program(programPath);
    if (!program.open(QIODevice::ReadOnly)) {
        setError(error, QObject::tr("cannot read %1: %2").arg(programPath, program.errorString()));
        return false;
    }
    const QByteArray prg = program.readAll();

    QVector<quint8> image(kTotalSectors * kSectorSize, 0);

    // Boot sector, with the BPB byte-for-byte the mkfs.vfat reference's.
    writeBootSector(image);

    // Both FAT copies: the reserved entries (media + end-of-chain markers)
    // and our chain. AUTO's directory is one cluster (2); the program starts
    // at cluster 3 and runs contiguously to the end of its size.
    const int prgClusters = (int(prg.size()) + kClusterSize - 1) / kClusterSize;
    // Cluster 2 is the AUTO directory and the program runs contiguously from
    // cluster 3, so kMaxDataClusters - 1 clusters is the capacity. Refuse what
    // does not fit rather than writing past the image and the FAT — the same
    // guard writeImage's allocateClusters applies to the generic export path.
    if (prgClusters > kMaxDataClusters - 1) {
        setError(error, QObject::tr("the program does not fit on a 720 KiB floppy"));
        return false;
    }
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
        setError(error, QObject::tr("cannot write %1: %2").arg(imagePath, out.errorString()));
        return false;
    }
    if (out.write(reinterpret_cast<const char *>(image.constData()), image.size()) != image.size()) {
        setError(error, QObject::tr("short write to %1").arg(imagePath));
        return false;
    }
    return true;
}

bool loadRaw(const QString &imagePath, QByteArray *raw, QString *error)
{
    QFile file(imagePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QObject::tr("cannot read %1: %2").arg(imagePath, file.errorString()));
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (isMsa(bytes))
        return decodeMsa(bytes, raw, error);

    const QString suffix = QFileInfo(imagePath).suffix().toLower();
    if (suffix == QLatin1String("dim") && bytes.size() > 32) {
        *raw = bytes.mid(32);
        return true;
    }
    *raw = bytes;
    return true;
}

bool saveRaw(const QString &imagePath, const QByteArray &raw, QString *error)
{
    QByteArray payload = raw;
    if (QFileInfo(imagePath).suffix().compare(QLatin1String("msa"), Qt::CaseInsensitive) == 0) {
        if (!encodeMsa(raw, &payload, error))
            return false;
    }
    // The image is the user's own file, so this goes through the shared rule: a
    // write that cannot reach the disk leaves the previous image intact rather
    // than a truncated one that no longer parses as FAT12.
    return files::write(imagePath, payload, error);
}

QVector<Entry> listRaw(const QByteArray &raw, QString *error)
{
    Geo geo;
    if (!parseGeo(raw, &geo)) {
        setError(error, QObject::tr("not a FAT12 floppy image"));
        return {};
    }
    QByteArray dirBytes;
    if (!rootDirBytes(raw, geo, &dirBytes, error))
        return {};
    QVector<Entry> entries;
    QSet<int> visitedDirs;
    listDir(raw, geo, dirBytes, QString(), 0, &visitedDirs, &entries);
    return entries;
}

QVector<Entry> listImage(const QString &imagePath, QString *error)
{
    QByteArray raw;
    if (!loadRaw(imagePath, &raw, error))
        return {};
    return listRaw(raw, error);
}

bool looksLikeCanonical720k(const QByteArray &firstSector, qint64 imageSize)
{
    return imageSize == qint64(kTotalSectors) * kSectorSize
        && firstSector.size() >= 11
        && firstSector.mid(3, 8) == QByteArrayLiteral("mkfs.fat");
}

QString normalizeEntryPath(const QString &path)
{
    QString clean = QDir::fromNativeSeparators(path.trimmed());
    while (clean.startsWith(QLatin1Char('/')))
        clean.remove(0, 1);
    while (clean.endsWith(QLatin1Char('/')))
        clean.chop(1);
    return clean;
}

bool readFileRaw(const QByteArray &raw, const QString &entryPath, QByteArray *data,
                 QString *error)
{
    Geo g;
    if (!parseGeo(raw, &g)) {
        setError(error, QObject::tr("not a FAT12 floppy image"));
        return false;
    }
    const QString clean = normalizeEntryPath(entryPath);
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        setError(error, QObject::tr("no entry named on the floppy"));
        return false;
    }

    QByteArray dirBytes;
    if (!rootDirBytes(raw, g, &dirBytes, error))
        return false;
    for (int i = 0; i < parts.size(); ++i) {
        // `found` points into `entries`: the vector must outlive the entry's
        // use, not be the temporary this used to parse inline.
        const QVector<RawDirEntry> entries = parseDirBytes(dirBytes);
        const RawDirEntry *found = dirEntryNamed(entries, parts.at(i));
        if (!found) {
            setError(error, QObject::tr("%1 is not on the floppy").arg(clean));
            return false;
        }
        if (i + 1 == parts.size()) {
            if (found->isDir) {
                setError(error, QObject::tr("%1 is a folder").arg(clean));
                return false;
            }
            // Only a chain that yields the entry's whole declared size is a
            // read; a refusal here keeps a copy-out from silently truncating.
            QByteArray bytes;
            QString chainError;
            if (!readChain(raw, g, found->cluster, found->size, false, &bytes, &chainError)) {
                setError(error, QObject::tr("%1 on the floppy is damaged: %2")
                                    .arg(clean, chainError));
                return false;
            }
            if (data)
                *data = bytes;
            return true;
        }
        if (!found->isDir) {
            setError(error, QObject::tr("%1 is not a folder").arg(clean));
            return false;
        }
        QString dirError;
        if (!readChain(raw, g, found->cluster, 0, true, &dirBytes, &dirError)) {
            setError(error, QObject::tr("%1 on the floppy is damaged: %2")
                                .arg(parts.at(i), dirError));
            return false;
        }
    }
    return false;
}

bool updateImage(const QString &imagePath, const QVector<Item> &additions,
                 const QStringList &removals, QString *error)
{
    const QString suffix = QFileInfo(imagePath).suffix().toLower();
    if (suffix == QLatin1String("dim") || suffix == QLatin1String("ipf")) {
        setError(error, QObject::tr("PiST can only rewrite .st, .img and .msa images"));
        return false;
    }
    if (additions.isEmpty() && removals.isEmpty())
        return true;

    QByteArray raw;
    if (!loadRaw(imagePath, &raw, error))
        return false;
    QString listError;
    const QVector<Entry> entries = listRaw(raw, &listError);
    if (!listError.isEmpty()) {
        setError(error, listError);
        return false;
    }

    QStringList removed;
    for (const QString &path : removals) {
        const QString clean = normalizeEntryPath(path);
        if (!clean.isEmpty())
            removed.append(clean);
    }

    QVector<Item> items;
    Geo g;
    if (!parseGeo(raw, &g)) {
        setError(error, QObject::tr("not a FAT12 floppy image"));
        return false;
    }
    for (const Entry &entry : entries) {
        bool dropped = false;
        for (const QString &r : removed) {
            if (entry.path.compare(r, Qt::CaseInsensitive) == 0
                || entry.path.startsWith(r + QLatin1Char('/'), Qt::CaseInsensitive)) {
                dropped = true;
                break;
            }
        }
        if (dropped)
            continue;
        Item item;
        item.destPath = entry.path;
        item.name11 = entry.name11;
        item.isDirectory = entry.isDirectory;
        // Read each file by its own directory cluster, not by re-resolving its
        // name. A FAT image can hold two entries with the same 8.3 name; a name
        // lookup returns the first for both, so the second's bytes would be
        // silently replaced by the first's (finding B9).
        //
        // A chain that cannot yield the entry's declared size is damage, not a
        // short file: staging it would write the truncation over the user's own
        // image and lose the clusters a repair would need, so the update is
        // refused here, before the first byte is staged. A folder's chain has no
        // declared size to check against, and the walk tolerates a damaged one
        // by leaving the folder unexpanded — verify it too, or the rewrite would
        // quietly drop a whole subtree (finding CRIT-3).
        QString chainError;
        QByteArray carriedData;
        if (!readChain(raw, g, entry.cluster, entry.isDirectory ? 0 : entry.size,
                       entry.isDirectory, &carriedData, &chainError)) {
            setError(error, QObject::tr("%1 on the floppy is damaged: %2")
                                .arg(entry.path, chainError));
            return false;
        }
        if (!entry.isDirectory)
            item.data = carriedData;
        items.append(item);
    }
    items += additions;

    // Stage beside the original with the same suffix, so saveRaw picks the
    // same container format, and only replace the original once the new
    // image is complete.
    const QFileInfo info(imagePath);
    const QString staged = info.absolutePath() + QStringLiteral("/.pist-%1.tmp.%2")
                                              .arg(info.completeBaseName(), suffix);
    if (!writeImage(staged, items, error)) {
        QFile::remove(staged);
        return false;
    }
    // Replace via a same-directory rename dance, never remove-then-rename:
    // QFile::rename refuses to overwrite an existing target, and removing
    // the original first loses it for good when the rename then fails (an
    // emulator holding the image open on Windows, AV/ACL interference, a
    // full directory). Setting the original aside first keeps every failure
    // recoverable: roll the backup forward again and the user's image is
    // exactly as it was.
    const QString backup = info.absolutePath() + QStringLiteral("/.pist-%1.bak.%2")
                                                .arg(info.completeBaseName(), suffix);
    QFile::remove(backup);
    if (!QFile::rename(imagePath, backup)) {
        setError(error, QObject::tr("could not set %1 aside for replacement "
                                    "(is the image in use?)")
                            .arg(imagePath));
        QFile::remove(staged);
        return false;
    }
    if (!QFile::rename(staged, imagePath)) {
        setError(error, QObject::tr("could not replace %1 with the staged image")
                            .arg(imagePath));
        if (QFile::rename(backup, imagePath)) {
            QFile::remove(staged);
            return false;
        }
        const QString previous = error ? *error : QString();
        setError(error, QObject::tr("%1\nThe original was preserved as %2.")
                            .arg(previous, backup));
        QFile::remove(staged);
        return false;
    }
    QFile::remove(backup);
    return true;
}

bool writeImage(const QString &imagePath, const QVector<Item> &items, QString *error)
{
    Node root;
    root.isDir = true;
    // The raw names the caller carried forward for entries that already exist on
    // a disk (`updateImage`): every component of such a path is written with the
    // bytes its directory holds, so an entry the operation did not touch is
    // re-emitted byte for byte. Only a host name is derived from its spelling.
    QHash<QString, QByteArray> carried;
    for (const Item &item : items) {
        if (item.name11.isEmpty())
            continue;
        const QStringList parts =
            normalizeDest(item.destPath).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (!parts.isEmpty())
            carried.insert(parts.join(QLatin1Char('/')), item.name11);
    }
    for (const Item &item : items) {
        if (!insertItem(&root, item, carried, error))
            return false;
    }
    if (root.children.size() > kRootEntries) {
        setError(error, QObject::tr("too many entries in the floppy root directory"));
        return false;
    }

    int nextCluster = 2;
    if (!allocateClusters(&root, true, &nextCluster, error))
        return false;

    QVector<quint8> image(kTotalSectors * kSectorSize, 0);
    writeBootSector(image);

    for (int f = 0; f < kFatCount; ++f) {
        const int fatStart = (kReservedSectors + f * kFatSectors) * kSectorSize;
        QVector<quint8> fat = image.mid(fatStart, kFatSectors * kSectorSize);
        fat[0] = kMediaByte;
        fat[1] = 0xff;
        fat[2] = 0xff;
        writeFatForNode(fat, root);
        for (int i = 0; i < fat.size(); ++i)
            image[fatStart + i] = fat[i];
    }

    const int rootStart = (kReservedSectors + kFatCount * kFatSectors) * kSectorSize;
    writeDirEntries(image, rootStart, root, false, 0);
    writeNodeData(image, root, 0);

    const QByteArray raw(reinterpret_cast<const char *>(image.constData()), image.size());
    return saveRaw(imagePath, raw, error);
}

bool collectHostItems(const QString &rootDirectory, const QStringList &hostPaths,
                      QVector<Item> *items, QString *error)
{
    if (!items) {
        setError(error, QObject::tr("no output list"));
        return false;
    }
    items->clear();
    const QString root = QFileInfo(rootDirectory).absoluteFilePath();
    bool ok = true;
    for (const QString &path : hostPaths) {
        const QFileInfo info(path);
        const bool includeHidden = info.fileName().startsWith(QLatin1Char('.'));
        collectPath(root, info.absoluteFilePath(), includeHidden, items, error, &ok);
        if (!ok)
            return false;
    }
    if (items->isEmpty()) {
        setError(error, QObject::tr("nothing to export"));
        return false;
    }
    return true;
}

} // namespace floppy

} // namespace pist
