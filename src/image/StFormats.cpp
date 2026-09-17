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

/// Bytes one screen row takes in ST low resolution: 320/16 groups, each four
/// plane words. The whole screen is this times 200 — a 32 KB screenshot, not
/// 320×200 pixels.
constexpr int screenRowBytes = (kStScreenWidth / 16) * 8;

/// Row width a sprite block is padded to: whole 16-pixel groups, plus the one
/// spare group a pre-shifted copy's moved-out pixels need.
int planeRowWidth(int width, bool shifted)
{
    return paddedWidth(width) + (shifted ? 16 : 0);
}

/// Bytes one pixel row occupies: four plane words per 16-pixel group, five
/// when a mask word is interleaved in front of them.
int planeRowBytes(int width, bool shifted, bool masked)
{
    return (planeRowWidth(width, shifted) / 16) * (masked ? 10 : 8);
}

/// Pre-shift counts the format offers: 2, 4 or 8 copies — steps of 8, 4 and 2
/// pixels, the finest the ST's word-per-plane screen needs.
bool isPreShiftCount(int count)
{
    return count == 2 || count == 4 || count == 8;
}

/// Write `srcW`×`srcH` cube-index pixels into `out` as `outW`-wide rows of
/// 16-pixel groups: four plane words each, preceded by the group's mask word
/// when `masked`. The picture sits `shift` pixels in from the left, and
/// everything else — including the pixels a shift pushes past the frame's
/// right edge — is not drawn.
void encodeSpritePlanes(const QVector<int> &src, int srcW, int srcH,
                        const QVector<int> &active, int transparent, int shift, bool masked,
                        uchar *out, int outW)
{
    const int groups = outW / 16;
    uchar *p = out;
    for (int row = 0; row < srcH; ++row) {
        for (int group = 0; group < groups; ++group) {
            quint16 bp[4] = {0, 0, 0, 0};
            quint16 mask = 0;
            for (int px = 0; px < 16; ++px) {
                const int bit = 15 - px;
                const int col = group * 16 + px - shift;
                const int at = row * srcW + col;
                const int value = (col >= 0 && col < srcW && at < src.size())
                                      ? src.at(at)
                                      : kTransparent;
                const int colour = stColourIndex(value, active);
                // A pixel the blitter must keep the screen under: one that is
                // not painted at all, or the palette colour the export was told
                // to leave out (the background register, unless changed).
                if (value < 0 || (transparent >= 0 && colour == transparent)) {
                    mask |= quint16(1u << bit);
                    continue;
                }
                for (int plane = 0; plane < 4; ++plane) {
                    if (colour & (1 << plane))
                        bp[plane] |= quint16(1u << bit);
                }
            }
            if (masked) {
                putBe16(p, mask);
                p += 2;
            }
            for (quint16 word : bp) {
                putBe16(p, word);
                p += 2;
            }
        }
    }
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

QVector<BitplaneBlock> bitplaneLayout(int width, int height, int frameCount,
                                      const BitplaneDataOptions &options)
{
    const int w = qMax(1, width);
    const int h = qMax(1, height);
    const int frames = qMax(1, frameCount);
    QVector<BitplaneBlock> blocks;
    auto add = [&blocks](BitplaneBlock::Kind kind, int frame, int shift, const QString &name,
                         int bytes) {
        BitplaneBlock block;
        block.kind = kind;
        block.frame = frame;
        block.shift = shift;
        block.name = name;
        block.offset = blocks.isEmpty() ? 0 : blocks.last().offset + blocks.last().bytes;
        block.bytes = bytes;
        blocks.append(block);
    };

    if (options.palette)
        add(BitplaneBlock::Kind::Palette, -1, -1, QStringLiteral("palette"), kMaxActive * 2);

    // Frame by frame, so a frame's data is one contiguous run and every frame
    // occupies the same number of bytes — which is what lets the generated
    // source step between frames with a stride instead of a pointer table.
    const int copies = isPreShiftCount(options.preShifts) ? options.preShifts : 0;
    for (int frame = 0; frame < frames; ++frame) {
        const QByteArray suffix = QByteArrayLiteral("_f") + QByteArray::number(frame);
        if (options.sprite) {
            add(BitplaneBlock::Kind::Sprite, frame, -1,
                QStringLiteral("sprite") + QLatin1String(suffix),
                planeRowBytes(w, false, false) * h);
        }
        if (options.masked) {
            add(BitplaneBlock::Kind::Masked, frame, -1,
                QStringLiteral("sprite_masked") + QLatin1String(suffix),
                planeRowBytes(w, false, true) * h);
        }
        // Every pre-shifted copy has the same stride — one group wider than the
        // frame — so copy k is `base + k * bytes`, and the encoder and this map
        // cannot disagree about where a copy starts.
        if (options.shifted) {
            const int stride = planeRowBytes(w, true, false) * h;
            for (int k = 0; k < copies; ++k) {
                add(BitplaneBlock::Kind::Shifted, frame, k,
                    QStringLiteral("sprite_shift%1").arg(k) + QLatin1String(suffix), stride);
            }
        }
        if (options.shiftedMasked) {
            const int stride = planeRowBytes(w, true, true) * h;
            for (int k = 0; k < copies; ++k) {
                add(BitplaneBlock::Kind::ShiftedMasked, frame, k,
                    QStringLiteral("sprite_masked_shift%1").arg(k) + QLatin1String(suffix),
                    stride);
            }
        }
    }
    return blocks;
}

namespace {

/// The checks both exports share: that something is selected, and that a shift
/// the format offers comes with any pre-shifted block.
bool checkBitplaneOptions(const BitplaneDataOptions &options, QString *error)
{
    const bool shifts = options.shifted || options.shiftedMasked;
    if (!options.palette && !options.sprite && !options.masked && !shifts) {
        if (error)
            *error = QStringLiteral("select at least one block to export");
        return false;
    }
    if (shifts && !isPreShiftCount(options.preShifts)) {
        if (error)
            *error = QStringLiteral("pre-shift count must be 2, 4 or 8 (got %1)")
                         .arg(options.preShifts);
        return false;
    }
    return true;
}

/// The phase to export, or null when the index is out of range.
const ImagePhase *checkedPhase(const ImageDocument &doc, int phase, QString *error)
{
    if (phase < 0 || phase >= doc.phaseCount()) {
        if (error)
            *error = QStringLiteral("no such phase");
        return nullptr;
    }
    return &doc.phases().at(phase);
}

} // namespace

QByteArray exportBitplaneData(const ImageDocument &doc, int phase,
                              const BitplaneDataOptions &options, QString *error)
{
    if (error)
        error->clear();
    const ImagePhase *phasePtr = checkedPhase(doc, phase, error);
    if (!phasePtr || !checkBitplaneOptions(options, error))
        return {};
    const ImagePhase &chosen = *phasePtr;
    const bool shifts = options.shifted || options.shiftedMasked;

    const QVector<BitplaneBlock> blocks =
        bitplaneLayout(chosen.cellW, chosen.cellH, chosen.frames.size(), options);
    QByteArray out(blocks.last().offset + blocks.last().bytes, '\0');
    auto *data = reinterpret_cast<uchar *>(out.data());
    const int w = chosen.cellW;
    const int h = chosen.cellH;
    const int step = shifts ? 16 / options.preShifts : 0;
    for (const BitplaneBlock &block : blocks) {
        uchar *p = data + block.offset;
        const QVector<int> &pixels = chosen.frames.at(qMax(0, block.frame)).composite;
        switch (block.kind) {
        case BitplaneBlock::Kind::Palette: {
            const QVector<quint16> table = stColourTable(doc.paletteKind(), doc.active());
            for (int i = 0; i < table.size(); ++i)
                putBe16(p + i * 2, table.at(i));
            break;
        }
        case BitplaneBlock::Kind::Sprite:
            encodeSpritePlanes(pixels, w, h, doc.active(), options.transparent, 0, false, p,
                               planeRowWidth(w, false));
            break;
        case BitplaneBlock::Kind::Masked:
            encodeSpritePlanes(pixels, w, h, doc.active(), options.transparent, 0, true, p,
                               planeRowWidth(w, false));
            break;
        case BitplaneBlock::Kind::Shifted:
            encodeSpritePlanes(pixels, w, h, doc.active(), options.transparent,
                               block.shift * step, false, p, planeRowWidth(w, true));
            break;
        case BitplaneBlock::Kind::ShiftedMasked:
            encodeSpritePlanes(pixels, w, h, doc.active(), options.transparent,
                               block.shift * step, true, p, planeRowWidth(w, true));
            break;
        }
    }
    return out;
}

namespace {

/// The block the generated scroller draws with: the most complete one selected,
/// since a mask blits correctly over whatever is underneath and pre-shifted
/// copies move in smaller steps than a whole group.
const BitplaneBlock *scrollDemoBlock(const QVector<BitplaneBlock> &blocks, bool *masked,
                                     bool *shifted)
{
    const BitplaneBlock::Kind order[] = {
        BitplaneBlock::Kind::ShiftedMasked,
        BitplaneBlock::Kind::Shifted,
        BitplaneBlock::Kind::Masked,
        BitplaneBlock::Kind::Sprite,
    };
    for (BitplaneBlock::Kind kind : order) {
        for (const BitplaneBlock &candidate : blocks) {
            if (candidate.kind != kind || candidate.frame != 0)
                continue;
            *masked = kind == BitplaneBlock::Kind::Masked
                      || kind == BitplaneBlock::Kind::ShiftedMasked;
            *shifted = kind == BitplaneBlock::Kind::Shifted
                       || kind == BitplaneBlock::Kind::ShiftedMasked;
            return &candidate;
        }
    }
    return nullptr;
}

} // namespace

QByteArray exportScrollDemo(const ImageDocument &doc, int phase,
                            const BitplaneDataOptions &options, const QString &dataFile,
                            QString *error)
{
    // The demo addresses the .dat the export writes, block for block, so the
    // two cannot disagree about the data: it pulls the file in with incbin and
    // keeps every offset the layout produced.
    const ImagePhase *phasePtr = checkedPhase(doc, phase, error);
    if (!phasePtr || !checkBitplaneOptions(options, error))
        return {};
    const ImagePhase &chosen = *phasePtr;
    const QVector<BitplaneBlock> blocks =
        bitplaneLayout(chosen.cellW, chosen.cellH, chosen.frames.size(), options);

    bool masked = false;
    bool shifted = false;
    const BitplaneBlock *block = scrollDemoBlock(blocks, &masked, &shifted);
    if (!block) {
        if (error)
            *error = QStringLiteral("the scroll demo needs a sprite block — tick Sprite, "
                                    "Sprite + mask or a pre-shifted block");
        return {};
    }
    const bool hasPalette = options.palette;

    const int w = qMax(1, chosen.cellW);
    const int h = qMax(1, chosen.cellH);
    const int frames = qMax(1, chosen.frames.size());
    const int groupBytes = masked ? 10 : 8;
    const int copyStride = block->bytes;                  // one pre-shifted copy
    const int rowBytes = qMax(groupBytes, copyStride / h);
    const int spriteGroups = qMax(1, rowBytes / groupBytes);
    // Whole groups are written, so clip to the screen row: even a sprite as wide
    // as the screen then meets the next row cleanly instead of running into it.
    const int drawGroups = qMin(spriteGroups, kStScreenWidth / 16);
    const int shifts = shifted ? options.preShifts : 1;
    const int step = 16 / shifts;
    const int phaseShift = step == 8 ? 3 : (step == 4 ? 2 : 1);
    const int spriteY = (kStScreenHeight - h) / 2;
    // How far apart two frames of *this* block are. The layout writes every
    // selected block per frame, so this is the whole frame's bytes — asking the
    // chosen block's own size would skip past the other blocks and animate
    // through the wrong bytes as soon as more than one block is selected.
    int frameStride = 0;
    for (const BitplaneBlock &candidate : blocks) {
        if (candidate.kind == block->kind && candidate.shift == block->shift
            && candidate.frame == 1) {
            frameStride = candidate.offset - block->offset;
            break;
        }
    }
    const int frameTicks = 4; // VBLs each animation frame is shown for

    QByteArray text;
    auto line = [&text](const QByteArray &s) {
        text += s;
        text += '\n';
    };
    auto number = [](int value) { return QByteArray::number(value); };
    auto hex4 = [](int value) {
        return QByteArrayLiteral("$")
               + QByteArray::number(value, 16).rightJustified(4, '0').toUpper();
    };

    const QByteArray dash = QByteArrayLiteral("; ") + QByteArray(75, '-');
    line(dash);
    line(QByteArrayLiteral("; A PiST bitplane export that shows itself off: \"")
         + chosen.name.toLatin1() + "\" scrolls across an ST low-resolution screen,");
    line(QByteArrayLiteral("; animating through its ") + number(frames)
         + " frame(s), one step per VBL, until you press a key.");
    line(";");
    line("; Assemble it in PiST with F7, or by hand:");
    line(";     vasmm68k_mot -Ftos -o scroll.prg scroll.s");
    line(";");
    line(QByteArrayLiteral("; It pulls its data out of the .dat the export wrote beside it, so"));
    line(QByteArrayLiteral("; keep the two files together (vasm looks for an incbin next to the"));
    line(QByteArrayLiteral("; source it is assembling). Every block in that file is named by an"));
    line(QByteArrayLiteral("; `equ` down there; the ones this program draws with are:"));
    for (const BitplaneBlock &entry : blocks) {
        if (entry.frame > 0)
            continue; // the header summarises frame 0; the equates list them all
        if (entry.kind != block->kind && entry.kind != BitplaneBlock::Kind::Palette)
            continue;
        line(QByteArrayLiteral(";     ") + entry.name.toLatin1().leftJustified(24)
             + hex4(entry.offset) + QByteArrayLiteral("  ") + number(entry.bytes));
    }
    if (frames > 1) {
        line(QByteArrayLiteral("; Each frame is the same shape, ") + number(frameStride)
             + " bytes further on: frame f of a block is `base + f*kFrameStride`.");
    }
    line(";");
    line("; A 16-pixel group is four plane words (and a mask word in front of them");
    line("; where the data is masked). The blit ANDs the mask into the screen and ORs");
    line("; the planes over it, which is why a set mask bit keeps what was there.");
    if (options.transparent >= 0) {
        line(QByteArrayLiteral("; The mask leaves out palette colour ") + number(options.transparent)
             + QByteArrayLiteral(", on top of the pixels that are not painted:"));
        line("; the blit keeps the screen under both.");
    } else {
        line("; The mask leaves out only the pixels that are not painted: every painted");
        line("; colour is opaque.");
    }
    if (shifted) {
        line(QByteArrayLiteral("; The drawing is driven by ") + block->name.toLatin1()
             + QByteArrayLiteral(": ") + number(shifts) + " copies, " + number(step)
             + " pixels apart.");
    } else {
        line("; No pre-shifted copies in this data, so the sprite jumps a whole 16-pixel");
        line("; group at a time — tick a pre-shifted block in the export dialog for a");
        line("; finer step.");
    }
    line(";");
    line("; It wants a colour monitor (RGB/VGA/TV), which is what the ST's 320x200");
    line("; four-bitplane screen needs; on any other screen it says so and waits for a");
    line("; key rather than drawing.");
    line(";");
    line("; Supervisor mode is not optional: on the ST the GLUE bus-errors user-mode");
    line("; access to the system area ($0-$7ff) and to the hardware registers, so the");
    line("; palette and the screen base can only be reached after GEMDOS Super(0).");
    line(dash);
    line("");
    line(QByteArrayLiteral("kScreenRowBytes	equ	") + number(screenRowBytes)
         + "		; one screen row: 320/16 groups of four plane words");
    line(QByteArrayLiteral("kScreenBytes	equ	") + number(screenRowBytes * kStScreenHeight)
         + "		; the whole screen, cleared at startup");
    line(QByteArrayLiteral("kSpriteGroups	equ	") + number(spriteGroups)
         + "		; 16-pixel groups in one data row");
    line(QByteArrayLiteral("kSpriteRowBytes	equ	") + number(rowBytes)
         + "		; bytes in one data row: four planes"
         + (masked ? QByteArrayLiteral(" and a mask word") : QByteArrayLiteral("")));
    line(QByteArrayLiteral("kDrawGroups	equ	") + number(drawGroups)
         + "		; groups the blit writes per row");
    line(QByteArrayLiteral("kSpriteWidth	equ	") + number(w) + "		; painted pixels");
    line(QByteArrayLiteral("kSpriteHeight	equ	") + number(h));
    if (shifted) {
        line(QByteArrayLiteral("kShifts		equ	") + number(shifts)
             + "		; pre-shifted copies in the data");
        line(QByteArrayLiteral("kCopyStride	equ	") + number(copyStride)
             + "		; bytes from one copy to the next");
    }
    line(QByteArrayLiteral("kFrames		equ	") + number(frames)
         + "		; animation frames in this phase");
    if (frames > 1) {
        line(QByteArrayLiteral("kFrameStride	equ	") + number(frameStride)
             + "		; bytes from one frame to the next");
        line(QByteArrayLiteral("kFrameTicks	equ	") + number(frameTicks)
             + "		; VBLs each frame is shown for");
    }
    line(QByteArrayLiteral("kStep		equ	") + number(step)
         + "		; pixels the sprite moves per frame");
    line(QByteArrayLiteral("kMaxX		equ	") + number(kStScreenWidth - drawGroups * 16)
         + "		; last x that still fits in the screen row");
    line(QByteArrayLiteral("kSpriteY	equ	") + number(spriteY) + "		; the row it travels along");
    line("");
    line("start:");
    line("	clr.l	-(sp)			; GEMDOS Super(0): into supervisor mode");
    line("	move.w	#$20,-(sp)");
    line("	trap	#1");
    line("	addq.l	#6,sp");
    line("	move.l	d0,saved_ssp		; the stack to go back to on the way out");
    line("");
    if (hasPalette) {
        line("	lea	$ffff8240,a0		; keep the colours we found...");
        line("	lea	saved_palette,a1");
        line("	movem.l	(a0),d0-d7");
        line("	movem.l	d0-d7,(a1)");
    }
    line("	move.w	#4,-(sp)			; XBIOS Getrez");
    line("	trap	#14");
    line("	addq.l	#2,sp");
    line("	tst.w	d0");
    line("	beq	.low_res");
    line("	move.l	#msg_res,-(sp)		; not 320x200 with 4 planes: say so, and");
    line("	move.w	#9,-(sp)			;   wait so the message can be read");
    line("	trap	#1");
    line("	addq.l	#6,sp");
    line("	move.w	#7,-(sp)			; GEMDOS Cnecin");
    line("	trap	#1");
    line("	addq.l	#2,sp");
    line("	bra	quit");
    line(".low_res:");
    if (hasPalette) {
        line("	lea	palette,a1		; ...and install ours. a0 is reloaded: the");
        line("	lea	$ffff8240,a0		;   traps in between clobber it");
        line("	movem.l	(a1),d0-d7");
        line("	movem.l	d0-d7,(a0)");
    }
    line("	move.w	#3,-(sp)			; XBIOS Logbase: the screen TOS draws to");
    line("	trap	#14");
    line("	addq.l	#2,sp");
    line("	movea.l	d0,a6");
    line("");
    line("	move.l	a6,a1			; start from a clean screen");
    line("	move.w	#(kScreenBytes/4)-1,d0");
    line(".clear:");
    line("	clr.l	(a1)+");
    line("	dbf	d0,.clear");
    line("");
    line(".drain:				; a key already waiting must not end this");
    line("	move.w	#$0b,-(sp)		;   on the first frame. GEMDOS Cconis,");
    line("	trap	#1			;   not Crawio (6): handed no character");
    line("	addq.l	#2,sp			;   argument it still answers non-zero");
    line("	tst.w	d0");
    line("	bne	.drain");
    line("");
    line("	moveq	#0,d4			; d4 = x to draw at");
    line("	moveq	#0,d5			; d5 = x already on the screen");
    if (frames > 1) {
        line("	clr.w	anim_frame		; start on the first frame");
        line(QByteArrayLiteral("	move.w	#kFrameTicks,anim_ticks"));
    }
    line(".frame:");
    line("	move.w	#37,-(sp)			; XBIOS Vsync: one step per frame");
    line("	trap	#14");
    line("	addq.l	#2,sp");
    line("	move.w	d5,d0			; rub out where it was...");
    line("	bsr	erase_at");
    line("	move.w	d4,d0			; ...and draw where it is now");
    line("	bsr	blit_at");
    line("	move.w	d4,d5");
    line(QByteArrayLiteral("	add.w	#kStep,d4"));
    line("	cmpi.w	#kMaxX+1,d4");
    line("	blt	.no_wrap");
    line("	moveq	#0,d4");
    line(".no_wrap:");
    if (frames > 1) {
        line("	subq.w	#1,anim_ticks		; animate: hold each frame for a few");
        line("	bgt	.same_frame		;   VBLs, then move to the next");
        line(QByteArrayLiteral("	move.w	#kFrameTicks,anim_ticks"));
        line("	move.w	anim_frame,d2");
        line("	addq.w	#1,d2");
        line(QByteArrayLiteral("	cmpi.w	#kFrames,d2"));
        line("	blt	.store_frame");
        line("	moveq	#0,d2");
        line(".store_frame:");
        line("	move.w	d2,anim_frame");
        line(".same_frame:");
    }
    line("	move.w	#$0b,-(sp)		; GEMDOS Cconis: a key means stop");
    line("	trap	#1");
    line("	addq.l	#2,sp");
    line("	tst.w	d0");
    line("	beq	.frame");
    line("");
    if (hasPalette) {
        line("	lea	saved_palette,a1		; put the colours back");
        line("	movem.l	(a1),d0-d7");
        line("	lea	$ffff8240,a0");
        line("	movem.l	d0-d7,(a0)");
    }
    line("quit:");
    line("	move.l	saved_ssp,-(sp)		; Super(old_ssp): back to user mode");
    line("	move.w	#$20,-(sp)");
    line("	trap	#1");
    line("	addq.l	#6,sp");
    line("	clr.w	-(sp)			; Pterm0");
    line("	trap	#1");
    line("");
    line("; d0.w = x: draw the current animation frame with its left edge there. One");
    line("; data row per screen row, one group at a time, starting x/16 words into the");
    line("; row. Registers: a0 = data, a1 = screen, a6 = screen base, d1-d3, d6, d7");
    line("; scratch.");
    line("blit_at:");
    line("	moveq	#0,d1			; move.w leaves the top half alone, and");
    line("	move.w	d0,d1			;   the address below is 32 bits wide");
    line("	lsr.w	#4,d1			; x/16 whole groups in");
    line("	lsl.w	#3,d1			; a group is four plane words: eight bytes");
    line("	add.l	#kSpriteY*kScreenRowBytes,d1");
    line("	move.l	a6,a1");
    line("	adda.l	d1,a1			; a1 = the sprite's first screen word");
    // The source is the block's first copy on the first frame, plus the frame
    // and the pre-shift. `moveq` because a `move.w` leaves the top half alone,
    // and the two `mulu`s below fill all 32 bits when they run at all.
    line("	moveq	#0,d2");
    if (frames > 1) {
        line("	move.w	anim_frame,d2		; which animation frame...");
        line("	mulu	#kFrameStride,d2");
    }
    if (shifted) {
        line("	move.w	d0,d3			; ...and which pre-shifted copy: the");
        line("	andi.w	#15,d3			;   pixel offset inside the group, one");
        line(QByteArrayLiteral("	lsr.w	#") + number(phaseShift) + ",d3");
        line("	mulu	#kCopyStride,d3			;   per kStep pixels");
        line("	add.l	d3,d2");
    }
    line(QByteArrayLiteral("	lea	") + block->name.toLatin1() + ",a0");
    line("	adda.l	d2,a0");
    line("	move.w	#kSpriteHeight-1,d7");
    line(".row:");
    line("	move.w	#kDrawGroups-1,d6");
    line(".group:");
    if (masked) {
        line("	move.w	(a0)+,d3			; the mask, kept for all four planes");
    }
    for (int plane = 0; plane < 4; ++plane) {
        line("	move.w	(a0)+,d1");
        if (masked)
            line("	and.w	d3,(a1)");
        line("	or.w	d1,(a1)+");
    }
    line("	dbf	d6,.group");
    if (drawGroups < spriteGroups) {
        // A sprite as wide as the screen is clipped to the row: the source has
        // to step the whole row anyway, or the next row starts mid-row.
        line(QByteArrayLiteral("	adda.l	#(kSpriteRowBytes-kDrawGroups*")
             + number(groupBytes) + "),a0");
    }
    line("	adda.l	#(kScreenRowBytes-kDrawGroups*8),a1");
    line("	dbf	d7,.row");
    line("	rts");
    line("");
    line("; d0.w = x: clear the sprite's rectangle, so the next frame does not");
    line("; leave a trail. The background is colour 0, which is what it clears to.");
    line("erase_at:");
    line("	moveq	#0,d1			; as in blit_at: clear the whole register");
    line("	move.w	d0,d1			;   before the word arithmetic");
    line("	lsr.w	#4,d1");
    line("	lsl.w	#3,d1");
    line("	add.l	#kSpriteY*kScreenRowBytes,d1");
    line("	move.l	a6,a1");
    line("	adda.l	d1,a1");
    line("	move.w	#kSpriteHeight-1,d7");
    line(".row:");
    line("	move.w	#kDrawGroups-1,d6");
    line(".group:");
    line("	clr.l	(a1)+");
    line("	clr.l	(a1)+");
    line("	dbf	d6,.group");
    line("	adda.l	#(kScreenRowBytes-kDrawGroups*8),a1");
    line("	dbf	d7,.row");
    line("	rts");
    line("");
    line("; ---------------------------------------------------------------------------");
    line("; The data: the .dat this export wrote, as it was written.");
    line("; ---------------------------------------------------------------------------");
    line("	even");
    line(QByteArrayLiteral("bitplanes:"));
    line(QByteArrayLiteral("	incbin	\"") + dataFile.toUtf8() + "\"");
    line("	even");
    line("");
    for (const BitplaneBlock &entry : blocks) {
        line(entry.name.toLatin1().leftJustified(24) + QByteArrayLiteral("equ	bitplanes+")
             + hex4(entry.offset));
    }
    line("");
    if (hasPalette) {
        line("saved_palette:				; where the old colours are kept");
        line("	dc.w	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0");
    }
    line("saved_ssp:");
    line("	dc.l	0");
    if (frames > 1) {
        line("anim_frame:				; the animation state, in memory: the");
        line("	dc.w	0			;   blit's own registers are all scratch");
        line("anim_ticks:");
        line("	dc.w	0");
    }
    line("");
    line("msg_res:");
    line("	dc.b	'PiST scroll demo: this wants ST low resolution, 320x200 with 4 planes.',13,10");
    line("	dc.b	'Press a key to go back to TOS.',13,10,0");
    line("	even");
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
