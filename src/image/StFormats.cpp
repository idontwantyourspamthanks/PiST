// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "image/StFormats.h"

#include "image/Palette.h"

#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QPair>

#include <algorithm>
#include <cstring>

namespace pist {

namespace {

quint16 be16(const uchar *p)
{
    return quint16((p[0] << 8) | p[1]);
}

quint32 be32(const uchar *p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

void putBe16(uchar *p, quint16 v)
{
    p[0] = uchar(v >> 8);
    p[1] = uchar(v);
}

void putBe32(uchar *p, quint32 v)
{
    p[0] = uchar(v >> 24);
    p[1] = uchar(v >> 16);
    p[2] = uchar(v >> 8);
    p[3] = uchar(v);
}

QVector<int> decodeStBitplanes(const uchar *data, int width, int height)
{
    QVector<int> pixels(width * height, 0);
    const int groups = width / 16;
    const uchar *p = data;
    for (int row = 0; row < height; ++row) {
        for (int group = 0; group < groups; ++group) {
            const quint16 bp[4] = {be16(p), be16(p + 2), be16(p + 4), be16(p + 6)};
            p += 8;
            for (int px = 0; px < 16; ++px) {
                const int bit = 15 - px;
                const int ci = ((bp[0] >> bit) & 1) | (((bp[1] >> bit) & 1) << 1)
                               | (((bp[2] >> bit) & 1) << 2) | (((bp[3] >> bit) & 1) << 3);
                pixels[row * width + group * 16 + px] = ci;
            }
        }
    }
    return pixels;
}

void encodeStBitplanes(const QVector<int> &pixels, int srcW, int srcH,
                       const QVector<int> &active, uchar *out, int outW, int outH)
{
    const int groups = outW / 16;
    uchar *p = out;
    for (int row = 0; row < outH; ++row) {
        for (int group = 0; group < groups; ++group) {
            quint16 bp[4] = {0, 0, 0, 0};
            for (int px = 0; px < 16; ++px) {
                const int col = group * 16 + px;
                int ci = 0;
                if (row < srcH && col < srcW) {
                    const int idx = row * srcW + col;
                    if (idx >= 0 && idx < pixels.size())
                        ci = stColourIndex(pixels.at(idx), active);
                }
                const int bit = 15 - px;
                for (int plane = 0; plane < 4; ++plane) {
                    if (ci & (1 << plane))
                        bp[plane] |= quint16(1u << bit);
                }
            }
            for (quint16 word : bp) {
                putBe16(p, word);
                p += 2;
            }
        }
    }
}

ImportedSheet sheetFromIndexed(int width, int height, const QVector<int> &stIndices,
                               const QVector<Rgb> &paletteRgb, PaletteKind kind)
{
    ImportedSheet sheet;
    sheet.width = width;
    sheet.height = height;
    sheet.kind = kind;
    sheet.active.resize(kMaxActive);
    for (int i = 0; i < kMaxActive; ++i) {
        const Rgb rgb = i < paletteRgb.size() ? paletteRgb.at(i) : Rgb{};
        sheet.active[i] = nearestCubeIndex(kind, rgb);
    }
    sheet.pixels.resize(width * height);
    for (int i = 0; i < sheet.pixels.size(); ++i) {
        const int ci = (i < stIndices.size()) ? stIndices.at(i) : 0;
        sheet.pixels[i] = sheet.active.at(qBound(0, ci, kMaxActive - 1));
    }
    return sheet;
}

QVector<uchar> unpackBits(const uchar *src, int srcLen, int unpackedSize)
{
    QVector<uchar> out(unpackedSize, 0);
    int si = 0, di = 0;
    while (si < srcLen && di < unpackedSize) {
        const qint8 n = qint8(src[si++]);
        if (n >= 0) {
            const int count = n + 1;
            for (int i = 0; i < count && di < unpackedSize && si < srcLen; ++i)
                out[di++] = src[si++];
        } else if (n != -128) {
            const int count = 1 - n;
            if (si >= srcLen)
                break;
            const uchar byte = src[si++];
            for (int i = 0; i < count && di < unpackedSize; ++i)
                out[di++] = byte;
        }
    }
    return out;
}

bool stosSizeOk(int width, int height, QString *error)
{
    const int wu = width / 16;
    if (width % 16 != 0 || wu < 1 || wu > 4) {
        if (error)
            *error = QStringLiteral("STOS width must be 16–64 and a multiple of 16 (got %1)")
                         .arg(width);
        return false;
    }
    if (height < 2 || height > 64) {
        if (error)
            *error = QStringLiteral("STOS height must be 2–64 (got %1)").arg(height);
        return false;
    }
    return true;
}

int paddedWidth(int width)
{
    return (width + 15) & ~15;
}

} // namespace

StImageFormat stFormatFromPath(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("pim"))
        return StImageFormat::Pim;
    if (suffix == QLatin1String("pi1"))
        return StImageFormat::Pi1;
    if (suffix == QLatin1String("neo"))
        return StImageFormat::Neo;
    if (suffix == QLatin1String("iff") || suffix == QLatin1String("ilbm"))
        return StImageFormat::Iff;
    if (suffix == QLatin1String("png"))
        return StImageFormat::Png;
    if (suffix == QLatin1String("mbk"))
        return StImageFormat::Mbk;
    if (suffix == QLatin1String("s") || suffix == QLatin1String("asm"))
        return StImageFormat::Assembler;
    if (suffix == QLatin1String("bin"))
        return StImageFormat::BitplaneBin;
    return StImageFormat::Unknown;
}

bool isPimPath(const QString &path)
{
    return stFormatFromPath(path) == StImageFormat::Pim;
}

bool isImportableImagePath(const QString &path)
{
    switch (stFormatFromPath(path)) {
    case StImageFormat::Pi1:
    case StImageFormat::Neo:
    case StImageFormat::Iff:
    case StImageFormat::Png:
        return true;
    default:
        return false;
    }
}

bool importPi1(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error)
{
    if (bytes.size() < 32034) {
        if (error)
            *error = QStringLiteral("file too small to be a valid PI1");
        return false;
    }
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    QVector<Rgb> palette;
    for (int i = 0; i < kMaxActive; ++i)
        palette.append(rgbFromSteWord(be16(data + 2 + i * 2)));
    const QVector<int> indices = decodeStBitplanes(data + 34, kStScreenWidth, kStScreenHeight);
    *out = sheetFromIndexed(kStScreenWidth, kStScreenHeight, indices, palette, kind);
    return true;
}

bool importNeo(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error)
{
    if (bytes.size() < 32128) {
        if (error)
            *error = QStringLiteral("file too small to be a valid NEO");
        return false;
    }
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    QVector<Rgb> palette;
    for (int i = 0; i < kMaxActive; ++i)
        palette.append(rgbFromSteWord(be16(data + 4 + i * 2)));
    const QVector<int> indices = decodeStBitplanes(data + 128, kStScreenWidth, kStScreenHeight);
    *out = sheetFromIndexed(kStScreenWidth, kStScreenHeight, indices, palette, kind);
    return true;
}

bool importIff(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error)
{
    const auto *data = reinterpret_cast<const uchar *>(bytes.constData());
    const int len = bytes.size();
    auto four = [&](int o) {
        return QByteArray(reinterpret_cast<const char *>(data + o), 4);
    };
    if (len < 12 || four(0) != "FORM" || four(8) != "ILBM") {
        if (error)
            *error = QStringLiteral("not a valid ILBM IFF file");
        return false;
    }

    int width = 0, height = 0, nPlanes = 4, compression = 0;
    QVector<Rgb> cmap;
    int bodyOffset = -1, bodySize = 0;
    int pos = 12;
    while (pos <= len - 8) {
        const QByteArray chunkId = four(pos);
        const quint32 chunkSize = be32(data + pos + 4);
        const int dataStart = pos + 8;
        if (quint64(dataStart) + chunkSize > quint64(len))
            break;
        if (chunkId == "BMHD" && chunkSize >= 20) {
            width = be16(data + dataStart);
            height = be16(data + dataStart + 2);
            nPlanes = data[dataStart + 8];
            compression = data[dataStart + 10];
        } else if (chunkId == "CMAP") {
            const int entries = int(chunkSize / 3);
            for (int i = 0; i < entries; ++i) {
                cmap.append(Rgb{data[dataStart + i * 3], data[dataStart + i * 3 + 1],
                                data[dataStart + i * 3 + 2]});
            }
        } else if (chunkId == "BODY") {
            bodyOffset = dataStart;
            bodySize = int(chunkSize);
        }
        pos = dataStart + int(chunkSize) + int(chunkSize & 1);
    }

    if (bodyOffset < 0 || width <= 0 || height <= 0) {
        if (error)
            *error = QStringLiteral("IFF file is missing a bitmap body");
        return false;
    }
    if (width > kStScreenWidth || height > kStScreenHeight) {
        if (error)
            *error = QStringLiteral("IFF image is larger than ST low-res (%1×%2)")
                         .arg(kStScreenWidth)
                         .arg(kStScreenHeight);
        return false;
    }

    const int rawRowbytes = (width + 7) / 8;
    const int rowbytes = rawRowbytes + (rawRowbytes % 2);
    const int unpackedLen = height * nPlanes * rowbytes;
    QVector<uchar> body;
    if (compression == 1)
        body = unpackBits(data + bodyOffset, bodySize, unpackedLen);
    else {
        body = QVector<uchar>(unpackedLen, 0);
        const int copy = qMin(bodySize, unpackedLen);
        memcpy(body.data(), data + bodyOffset, size_t(copy));
    }

    QVector<int> stIndices(width * height, 0);
    int bpos = 0;
    for (int row = 0; row < height; ++row) {
        for (int plane = 0; plane < nPlanes; ++plane) {
            for (int byte = 0; byte < rowbytes; ++byte) {
                const uchar b = (bpos < body.size()) ? body.at(bpos++) : 0;
                for (int bit = 0; bit < 8; ++bit) {
                    const int col = byte * 8 + bit;
                    if (col < width && (b & (1 << (7 - bit))))
                        stIndices[row * width + col] |= (1 << plane);
                }
            }
        }
    }

    if (cmap.isEmpty()) {
        for (int i = 0; i < kMaxActive; ++i)
            cmap.append(Rgb{});
    }
    *out = sheetFromIndexed(width, height, stIndices, cmap, kind);
    return true;
}

bool importPng(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error)
{
    QImage image;
    if (!image.loadFromData(bytes, "PNG")) {
        if (error)
            *error = QStringLiteral("not a valid PNG image");
        return false;
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    if (image.width() > kStScreenWidth || image.height() > kStScreenHeight) {
        image = image.scaled(kStScreenWidth, kStScreenHeight, Qt::KeepAspectRatio,
                             Qt::FastTransformation);
    }
    if (image.width() < 1 || image.height() < 1) {
        if (error)
            *error = QStringLiteral("PNG image is empty");
        return false;
    }

    ImportedSheet sheet;
    sheet.width = image.width();
    sheet.height = image.height();
    sheet.kind = kind;
    sheet.pixels.resize(sheet.width * sheet.height);
    QHash<int, int> counts;
    for (int y = 0; y < image.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb px = line[x];
            const int idx = y * sheet.width + x;
            if (qAlpha(px) < 16) {
                sheet.pixels[idx] = kTransparent;
                continue;
            }
            const int cube = nearestCubeIndex(kind, Rgb{quint8(qRed(px)), quint8(qGreen(px)),
                                                        quint8(qBlue(px))});
            sheet.pixels[idx] = cube;
            counts[cube] += 1;
        }
    }

    QList<QPair<int, int>> ranked;
    ranked.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it)
        ranked.append(qMakePair(it.value(), it.key()));
    std::sort(ranked.begin(), ranked.end(), [](const QPair<int, int> &a, const QPair<int, int> &b) {
        return a.first > b.first;
    });
    for (int i = 0; i < ranked.size() && sheet.active.size() < kMaxActive; ++i)
        sheet.active.append(ranked.at(i).second);
    if (sheet.active.isEmpty())
        sheet.active = defaultActiveIndices(kind);
    *out = sheet;
    return true;
}

ImageDocument spriteSafeDocument(const ImageDocument &doc, QString *error)
{
    const QVector<int> composite = doc.pixels();
    // Painted colour words, in the palette's own order.
    QVector<int> used;
    for (int word : doc.active()) {
        if (word >= 0 && !used.contains(word) && composite.contains(word))
            used.append(word);
    }
    // Colour 0 carrying no sprite pixels is already sprite-safe.
    if (used.isEmpty() || used.first() != doc.active().first())
        return doc;

    if (used.size() > 15) {
        if (error)
            *error = QStringLiteral("the sprite uses all 16 colours; sprite-safe export "
                                    "needs a free slot for the background");
        return {};
    }

    // The reserved slot prefers the sheet's own background colour, then
    // black, then the first free word at all.
    int reserved = -1;
    const int background = doc.background();
    if (background != doc.active().first() && !used.contains(background))
        reserved = background;
    if (reserved < 0 && !used.contains(0))
        reserved = 0;
    for (int word = 0; reserved < 0 && word <= 0x777; ++word) {
        if (!used.contains(word))
            reserved = word;
    }

    QVector<int> active;
    active.append(reserved);
    for (int word : used)
        active.append(word);

    ImageDocument out = ImageDocument::create(doc.width(), doc.height(), doc.paletteKind());
    out.setActive(active);
    out.setBackground(reserved);
    QVector<int> indices;
    indices.reserve(composite.size());
    for (int i = 0; i < composite.size(); ++i)
        indices.append(i);
    out.restoreIndices(indices, composite);
    out.setModified(false);
    return out;
}

bool importStImage(const QByteArray &bytes, StImageFormat format, PaletteKind kind,
                   ImportedSheet *out, QString *error)
{
    switch (format) {
    case StImageFormat::Pi1:
        return importPi1(bytes, kind, out, error);
    case StImageFormat::Neo:
        return importNeo(bytes, kind, out, error);
    case StImageFormat::Iff:
        return importIff(bytes, kind, out, error);
    case StImageFormat::Png:
        return importPng(bytes, kind, out, error);
    default:
        if (error)
            *error = QStringLiteral("unsupported import format");
        return false;
    }
}

bool applyImport(ImageDocument *doc, const ImportedSheet &sheet, bool append, QString *error)
{
    if (!doc)
        return false;
    if (append) {
        if (sheet.width != doc->width() || sheet.height != doc->height()) {
            if (error)
                *error = QStringLiteral("imported image is %1×%2, but the document is %3×%4")
                             .arg(sheet.width)
                             .arg(sheet.height)
                             .arg(doc->width())
                             .arg(doc->height());
            return false;
        }
        const int index = doc->addFrame();
        doc->setCurrentFrame(index);
        QVector<int> indices;
        indices.reserve(sheet.pixels.size());
        for (int i = 0; i < sheet.pixels.size(); ++i)
            indices.append(i);
        doc->restoreIndices(indices, sheet.pixels);
        return true;
    }

    ImageDocument next = ImageDocument::create(sheet.width, sheet.height, sheet.kind);
    next.setActive(sheet.active);
    QVector<int> indices;
    indices.reserve(sheet.pixels.size());
    for (int i = 0; i < sheet.pixels.size(); ++i)
        indices.append(i);
    next.restoreIndices(indices, sheet.pixels);
    next.setModified(true);
    doc->replaceWith(next);
    return true;
}

QByteArray exportPi1(const ImageDocument &doc, int frame, QString *error)
{
    Q_UNUSED(error);
    QByteArray out(32034, '\0');
    auto *data = reinterpret_cast<uchar *>(out.data());
    putBe16(data, 0);
    const QVector<quint16> table = stColourTable(doc.paletteKind(), doc.active());
    for (int i = 0; i < kMaxActive; ++i)
        putBe16(data + 2 + i * 2, table.at(i));
    encodeStBitplanes(doc.frame(frame), doc.width(), doc.height(), doc.active(), data + 34,
                      kStScreenWidth, kStScreenHeight);
    return out;
}

QByteArray exportNeo(const ImageDocument &doc, int frame, const QString &name, QString *error)
{
    Q_UNUSED(error);
    QByteArray out(32128, '\0');
    auto *data = reinterpret_cast<uchar *>(out.data());
    putBe16(data, 0);
    putBe16(data + 2, 0);
    const QVector<quint16> table = stColourTable(doc.paletteKind(), doc.active());
    for (int i = 0; i < kMaxActive; ++i)
        putBe16(data + 4 + i * 2, table.at(i));
    const QByteArray name8 = name.left(8).toLatin1();
    memcpy(data + 36, name8.constData(), size_t(name8.size()));
    putBe16(data + 50, 0);
    putBe16(data + 52, 0);
    putBe16(data + 54, quint16(qMin(doc.width(), kStScreenWidth)));
    putBe16(data + 56, quint16(qMin(doc.height(), kStScreenHeight)));
    encodeStBitplanes(doc.frame(frame), doc.width(), doc.height(), doc.active(), data + 128,
                      kStScreenWidth, kStScreenHeight);
    return out;
}

QByteArray exportIff(const ImageDocument &doc, int frame, QString *error)
{
    Q_UNUSED(error);
    const int screenW = kStScreenWidth;
    const int screenH = kStScreenHeight;
    const int rowbytes = 40;
    const int nPlanes = 4;
    QByteArray cmap(16 * 3, '\0');
    for (int i = 0; i < kMaxActive && i < doc.active().size(); ++i) {
        const Rgb rgb = cubeRgb(doc.paletteKind(), doc.active().at(i));
        cmap[i * 3] = char(rgb.r);
        cmap[i * 3 + 1] = char(rgb.g);
        cmap[i * 3 + 2] = char(rgb.b);
    }

    QByteArray body(screenH * nPlanes * rowbytes, '\0');
    const QVector<int> &pixels = doc.frame(frame);
    int bpos = 0;
    for (int row = 0; row < screenH; ++row) {
        for (int plane = 0; plane < nPlanes; ++plane) {
            for (int byte = 0; byte < rowbytes; ++byte) {
                uchar b = 0;
                for (int bit = 0; bit < 8; ++bit) {
                    const int col = byte * 8 + bit;
                    int ci = 0;
                    if (row < doc.height() && col < doc.width())
                        ci = stColourIndex(pixels.at(row * doc.width() + col), doc.active());
                    if (ci & (1 << plane))
                        b |= uchar(1 << (7 - bit));
                }
                body[bpos++] = char(b);
            }
        }
    }

    const int formBody = 4 + 28 + (8 + 48) + (8 + body.size());
    QByteArray out(8 + formBody, '\0');
    auto *data = reinterpret_cast<uchar *>(out.data());
    int pos = 0;
    auto chunk = [&](const char *id, int size) {
        memcpy(data + pos, id, 4);
        pos += 4;
        putBe32(data + pos, quint32(size));
        pos += 4;
    };
    chunk("FORM", formBody);
    memcpy(data + pos, "ILBM", 4);
    pos += 4;
    chunk("BMHD", 20);
    putBe16(data + pos, quint16(screenW));
    pos += 2;
    putBe16(data + pos, quint16(screenH));
    pos += 2;
    putBe16(data + pos, 0);
    pos += 2;
    putBe16(data + pos, 0);
    pos += 2;
    data[pos++] = uchar(nPlanes);
    data[pos++] = 0;
    data[pos++] = 0;
    data[pos++] = 0;
    putBe16(data + pos, 0);
    pos += 2;
    data[pos++] = 1;
    data[pos++] = 1;
    putBe16(data + pos, quint16(screenW));
    pos += 2;
    putBe16(data + pos, quint16(screenH));
    pos += 2;
    chunk("CMAP", 48);
    memcpy(data + pos, cmap.constData(), 48);
    pos += 48;
    chunk("BODY", body.size());
    memcpy(data + pos, body.constData(), size_t(body.size()));
    return out;
}

QByteArray exportPng(const ImageDocument &doc, int frame, QString *error)
{
    QImage image(doc.width(), doc.height(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const QVector<int> &pixels = doc.frame(frame);
    for (int y = 0; y < doc.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < doc.width(); ++x) {
            const int cube = pixels.at(y * doc.width() + x);
            if (cube < 0) {
                line[x] = 0;
                continue;
            }
            const Rgb rgb = cubeRgb(doc.paletteKind(), cube);
            line[x] = qRgba(rgb.r, rgb.g, rgb.b, 255);
        }
    }
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG")) {
        if (error)
            *error = QStringLiteral("could not encode PNG");
        return {};
    }
    return out;
}

QByteArray exportStosMbk(const ImageDocument &doc, int maskColour, int bankNumber, QString *error)
{
    if (!stosSizeOk(doc.width(), doc.height(), error))
        return {};
    if (bankNumber < 1 || bankNumber > 16) {
        if (error)
            *error = QStringLiteral("STOS bank number must be 1–16 (got %1)").arg(bankNumber);
        return {};
    }

    const int w = doc.width();
    const int h = doc.height();
    const int wu = w / 16;
    const int count = doc.frameCount();
    const int spriteLen = wu * h * 10;
    const int bodyLen = 0x16 + count * 8 + 36 + count * spriteLen;
    const int alloc = ((bodyLen + 255) / 256) * 256;
    QByteArray out(0x12 + bodyLen, '\0');
    auto *data = reinterpret_cast<uchar *>(out.data());
    int pos = 0;
    auto u16 = [&](quint16 v) {
        putBe16(data + pos, v);
        pos += 2;
    };
    auto u32 = [&](quint32 v) {
        putBe32(data + pos, v);
        pos += 4;
    };
    auto bytes = [&](const char *s, int n) {
        memcpy(data + pos, s, size_t(n));
        pos += n;
    };

    bytes("Lionpoubnk", 10);
    u16(0);
    u16(quint16(bankNumber));
    u16(0x8100);
    u16(quint16(alloc));

    const char sig[] = {'\x19', '\x86', '\x19', '\x87'};
    bytes(sig, 4);
    u32(0x12);
    u32(quint32(bodyLen - 4));
    u32(quint32(bodyLen - 4));
    u16(quint16(count));
    u32(0);

    int dataOff = count * 8 + 36;
    for (int i = 0; i < count; ++i) {
        u32(quint32(dataOff));
        data[pos++] = uchar(wu);
        data[pos++] = uchar(h);
        u16(0);
        dataOff += spriteLen;
    }

    bytes("PALT", 4);
    const QVector<quint16> table = stColourTable(doc.paletteKind(), doc.active());
    for (quint16 word : table)
        u16(word);

    for (int fi = 0; fi < count; ++fi) {
        const QVector<int> &frame = doc.frame(fi);
        for (int row = 0; row < h; ++row) {
            for (int unit = 0; unit < wu; ++unit) {
                quint16 mw = 0;
                for (int px = 0; px < 16; ++px) {
                    const int x = unit * 16 + px;
                    const int raw = frame.at(row * w + x);
                    if (raw < 0 || stColourIndex(raw, doc.active()) == maskColour)
                        mw |= quint16(1u << (15 - px));
                }
                u16(mw);
            }
        }
        for (int row = 0; row < h; ++row) {
            for (int unit = 0; unit < wu; ++unit) {
                quint16 bp[4] = {0, 0, 0, 0};
                for (int px = 0; px < 16; ++px) {
                    const int x = unit * 16 + px;
                    const int ci = stColourIndex(frame.at(row * w + x), doc.active());
                    const int bit = 15 - px;
                    for (int plane = 0; plane < 4; ++plane) {
                        if (ci & (1 << plane))
                            bp[plane] |= quint16(1u << bit);
                    }
                }
                for (quint16 word : bp)
                    u16(word);
            }
        }
    }
    return out;
}

QByteArray exportBitplanes(const ImageDocument &doc, int frame, QString *error)
{
    Q_UNUSED(error);
    const int outW = paddedWidth(doc.width());
    const int bytes = (outW / 16) * doc.height() * 8;
    QByteArray out(bytes, '\0');
    encodeStBitplanes(doc.frame(frame), doc.width(), doc.height(), doc.active(),
                      reinterpret_cast<uchar *>(out.data()), outW, doc.height());
    return out;
}

QByteArray exportAssembler(const ImageDocument &doc, int frame, QString *error)
{
    Q_UNUSED(error);
    const QByteArray planes = exportBitplanes(doc, frame, nullptr);
    const QVector<quint16> table = stColourTable(doc.paletteKind(), doc.active());
    QByteArray text;
    text += QByteArray("; generated by PiST — ")
          + QByteArray::number(doc.width()) + "x" + QByteArray::number(doc.height())
          + ", 4 bitplanes\n";
    text += "palette:\n";
    text += "\tdc.w\t";
    for (int i = 0; i < table.size(); ++i) {
        if (i)
            text += ",";
        text += QByteArray("$")
              + QByteArray::number(table.at(i), 16).rightJustified(3, '0').toUpper();
    }
    text += "\n";
    text += "sprite:\n";
    const auto *data = reinterpret_cast<const uchar *>(planes.constData());
    const int words = planes.size() / 2;
    for (int i = 0; i < words; ++i) {
        if (i % 8 == 0)
            text += "\tdc.w\t";
        else
            text += ",";
        const quint16 w = be16(data + i * 2);
        text += QByteArray("$") + QByteArray::number(w, 16).rightJustified(4, '0').toUpper();
        if (i % 8 == 7 || i == words - 1)
            text += "\n";
    }
    return text;
}

ImageDocument composeSheet(const ImageDocument &doc, int sheetIndex, QString *error)
{
    if (sheetIndex < 0 || sheetIndex >= doc.sheets().size()) {
        if (error)
            *error = QStringLiteral("no such sprite sheet");
        return {};
    }
    const ImageSheet &sheet = doc.sheets().at(sheetIndex);
    ImageDocument out = ImageDocument::create(sheet.width, sheet.height, doc.paletteKind());
    out.setActive(doc.active());
    out.setBackground(doc.background());

    QVector<int> canvas(out.width() * out.height(), kTransparent);
    const int w = out.width();
    const int h = out.height();
    for (const ImagePhase &phase : doc.phases()) {
        if (phase.sheet != sheetIndex)
            continue;
        for (int k = 0; k < phase.frames.size(); ++k) {
            const QVector<int> &composite = phase.frames.at(k).composite;
            const int originX = phase.x + k * phase.cellW;
            for (int row = 0; row < phase.cellH; ++row) {
                const int y = phase.y + row;
                if (y < 0 || y >= h)
                    continue;
                for (int col = 0; col < phase.cellW; ++col) {
                    const int x = originX + col;
                    if (x < 0 || x >= w)
                        continue;
                    const int value = composite.at(row * phase.cellW + col);
                    if (value >= 0)
                        canvas[y * w + x] = value;
                }
            }
        }
    }
    out.replaceActiveLayer(canvas);
    out.setModified(false);
    return out;
}

QVector<QVector<int>> sliceSheetCells(const ImportedSheet &sheet, int x, int y,
                                      int cellW, int cellH, int count)
{
    QVector<QVector<int>> cells;
    if (cellW <= 0 || cellH <= 0 || count <= 0)
        return cells;
    for (int k = 0; k < count; ++k) {
        QVector<int> cell(cellW * cellH, kTransparent);
        const int originX = x + k * cellW;
        for (int row = 0; row < cellH; ++row) {
            const int sy = y + row;
            if (sy < 0 || sy >= sheet.height)
                continue;
            for (int col = 0; col < cellW; ++col) {
                const int sx = originX + col;
                if (sx < 0 || sx >= sheet.width)
                    continue;
                cell[row * cellW + col] = sheet.pixels.at(sy * sheet.width + sx);
            }
        }
        cells.append(cell);
    }
    return cells;
}

} // namespace pist
