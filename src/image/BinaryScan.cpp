// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "image/BinaryScan.h"

#include "image/StFormats.h"

namespace pist {
namespace {

int be16(const QByteArray &bytes, int at)
{
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + at);
    return (int(p[0]) << 8) | int(p[1]);
}

quint32 be32(const QByteArray &bytes, int at)
{
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + at);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

bool fourAt(const QByteArray &bytes, int at, const char *text)
{
    return at >= 0 && at + 4 <= bytes.size() && bytes.mid(at, 4) == QByteArray(text, 4);
}

} // namespace

bool parseAvrHeader(const QByteArray &header, AvrInfo *out, QString *error)
{
    const auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("no info"));
    if (header.size() < 128 || header.left(4) != QByteArrayLiteral("2BIT"))
        return fail(QStringLiteral("not an AVR file (need 2BIT)"));

    const int channels = be16(header, 12);
    if (channels != 0)
        return fail(QStringLiteral("stereo AVR"));

    const int bits = be16(header, 14);
    if (bits == 16)
        return fail(QStringLiteral("16-bit AVR"));
    if (bits != 8)
        return fail(QStringLiteral("unsupported AVR depth %1").arg(bits));

    const int sign = be16(header, 16);
    if (sign != 0 && sign != 0xFFFF)
        return fail(QStringLiteral("unsupported AVR sign"));

    const int rate = int(be32(header, 22) & 0x00FFFFFFu);
    if (rate < 4000 || rate > 50000)
        return fail(QStringLiteral("AVR rate must be in 4000..50000 (got %1)").arg(rate));

    const quint32 length = be32(header, 26);
    if (length < 1 || length > 65536)
        return fail(QStringLiteral("AVR length must be in 1..65536 (got %1)").arg(length));

    out->rate = rate;
    out->signedSamples = sign == 0xFFFF;
    out->length = length;
    return true;
}

QByteArray wavFromAvr(const QByteArray &fileBytes, QString *error)
{
    AvrInfo info;
    if (!parseAvrHeader(fileBytes.left(128), &info, error))
        return {};
    if (quint32(fileBytes.size()) < 128u + info.length) {
        if (error)
            *error = QStringLiteral("short AVR");
        return {};
    }
    return wavFromPcm8(fileBytes.mid(128, int(info.length)), info.rate, info.signedSamples, error);
}

QVector<MagicHit> scanMagic(const QByteArray &bytes)
{
    QVector<MagicHit> hits;
    const auto add = [&](int offset, const char *kind) {
        if (hits.size() >= 64)
            return;
        MagicHit hit;
        hit.offset = offset;
        hit.kind = QString::fromLatin1(kind);
        hits.append(hit);
    };
    for (int i = 0; i < bytes.size() && hits.size() < 64; ++i) {
        if (fourAt(bytes, i, "2BIT")) {
            AvrInfo info;
            if (parseAvrHeader(bytes.mid(i, 128), &info, nullptr)) {
                add(i, "avr");
                continue;
            }
        }
        if (fourAt(bytes, i, "FORM") && i + 12 <= bytes.size() && fourAt(bytes, i + 8, "ILBM")) {
            add(i, "iff");
            continue;
        }
        if (fourAt(bytes, i, "ICE!") || fourAt(bytes, i, "Ice!")) {
            add(i, "ice");
            continue;
        }
        if (fourAt(bytes, i, "ATM5"))
            add(i, "atomik");
    }
    return hits;
}

QString packerMagic(const QByteArray &bytes)
{
    if (fourAt(bytes, 0, "ICE!") || fourAt(bytes, 0, "Ice!"))
        return QStringLiteral("ice");
    if (fourAt(bytes, 0, "ATM5"))
        return QStringLiteral("atomik");
    return {};
}

SndhTags parseSndhTags(const QByteArray &bytes)
{
    SndhTags tags;
    const int window = qMin(bytes.size(), 512);
    const int at = bytes.left(window).indexOf(QByteArrayLiteral("SNDH"));
    if (at < 0)
        return tags;
    tags.found = true;
    int pos = at + 4;
    int count = 0;
    while (pos + 4 <= bytes.size() && count < 32) {
        const QByteArray tag = bytes.mid(pos, 4);
        if (tag == QByteArrayLiteral("HDNS"))
            return tags;
        ++count;
        if (tag == QByteArrayLiteral("TITL") || tag == QByteArrayLiteral("COMM")
            || tag == QByteArrayLiteral("RIPP") || tag == QByteArrayLiteral("CONV")
            || tag == QByteArrayLiteral("YEAR")) {
            const int start = pos + 4;
            const int limit = qMin(128, bytes.size() - start);
            int end = -1;
            for (int i = 0; i < limit; ++i) {
                if (bytes.at(start + i) == '\0') {
                    end = i;
                    break;
                }
            }
            if (end < 0) {
                tags.truncated = true;
                return tags;
            }
            const QString text = QString::fromLatin1(bytes.constData() + start, end);
            if (tag == QByteArrayLiteral("TITL"))
                tags.title = text;
            else if (tag == QByteArrayLiteral("COMM"))
                tags.composer = text;
            else if (tag == QByteArrayLiteral("RIPP"))
                tags.ripper = text;
            else if (tag == QByteArrayLiteral("CONV"))
                tags.converter = text;
            else
                tags.year = text;
            pos = start + end + 1;
            continue;
        }
        tags.truncated = true;
        return tags;
    }
    tags.truncated = true;
    return tags;
}

} // namespace pist
