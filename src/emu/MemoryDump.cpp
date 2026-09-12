// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/MemoryDump.h"

#include <QRegularExpression>

namespace pist {

namespace {

/// `00012596: 48 7a 00 0c ...  Hz..?<..NA\OK`
/// The address is eight hex digits; the hex field is one or more 2/4/8-digit
/// groups; everything after the double space is the character column.
const QRegularExpression &rowRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^([0-9A-Fa-f]{6,8}):\s+((?:[0-9A-Fa-f]{2,8}\s+)*[0-9A-Fa-f]{2,8}))"),
        QRegularExpression::MultilineOption);
    return re;
}

/// Matches a single hex group in the data field.
const QRegularExpression &groupRe()
{
    static const QRegularExpression re(QStringLiteral(R"([0-9A-Fa-f]{2,8})"));
    return re;
}

} // namespace

QList<MemoryRow> parseMemoryDump(const QString &response)
{
    QList<MemoryRow> rows;

    auto it = rowRe().globalMatch(response);
    while (it.hasNext()) {
        const auto match = it.next();

        MemoryRow row;
        row.address = match.captured(1).toUInt(nullptr, 16);

        // Groups may be 2, 4 or 8 hex digits wide depending on whether the dump
        // was requested as bytes, words or longs. Normalise everything to bytes
        // so the view and any future export agree, and so a mixed dump still
        // renders sensibly.
        auto groups = groupRe().globalMatch(match.captured(2));
        while (groups.hasNext()) {
            const QString group = groups.next().captured();
            if (group.size() <= 2) {
                row.bytes.append(static_cast<quint8>(group.toUInt(nullptr, 16)));
            } else {
                // Multi-byte groups are big-endian machine words.
                for (int i = 0; i + 1 < group.size(); i += 2) {
                    const QString byte = group.mid(i, 2);
                    row.bytes.append(static_cast<quint8>(byte.toUInt(nullptr, 16)));
                }
            }
        }

        if (!row.bytes.isEmpty())
            rows.append(row);
    }

    return rows;
}

QString renderMemoryChars(const QVector<quint8> &bytes)
{
    QString text;
    text.reserve(bytes.size());
    for (quint8 byte : bytes) {
        // Printable ASCII only; the ST's own character set differs, so anything
        // else becomes a placeholder rather than a misleading glyph.
        text.append((byte >= 0x20 && byte < 0x7f) ? QChar(byte) : QLatin1Char('.'));
    }
    return text;
}

} // namespace pist
