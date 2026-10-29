// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/TosProgram.h"

#include <QObject>

namespace pist {

namespace {

constexpr quint16 kMagic = 0x601A;
constexpr quint16 kLongName = 0x0048;

quint16 read16(const QByteArray &bytes, int offset)
{
    return quint16((uchar(bytes.at(offset)) << 8) | uchar(bytes.at(offset + 1)));
}

quint32 read32(const QByteArray &bytes, int offset)
{
    return (quint32(read16(bytes, offset)) << 16) | read16(bytes, offset + 2);
}

QString symbolName(const QByteArray &raw)
{
    const int end = raw.indexOf('\0');
    QString name = QString::fromLatin1(end < 0 ? raw : raw.left(end));
    while (name.endsWith(QLatin1Char(' ')))
        name.chop(1);
    return name;
}

bool parseSymbols(const QByteArray &table, quint32 textSize, quint32 dataSize,
                  QVector<TosSymbol> *out, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (table.size() % 14 != 0)
        return fail(QObject::tr("symbol table length is not a multiple of 14"));

    for (int at = 0; at < table.size();) {
        QByteArray raw = table.mid(at, 8);
        const quint16 type = read16(table, at + 8);
        const quint32 stored = read32(table, at + 10);
        at += 14;
        if ((type & 0xFF) == kLongName) {
            // GST: the next 14 bytes continue the name. They are not a symbol.
            if (at + 14 > table.size())
                return fail(QObject::tr("symbol table is truncated"));
            raw.append(table.mid(at, 14));
            at += 14;
        }

        TosSymbol symbol;
        symbol.name = symbolName(raw);
        symbol.global = (type & 0x2000) != 0;
        const quint16 section = type & ~kLongName;
        if (section & 0x4000) {
            symbol.kind = TosSymbol::Kind::Equate;
            symbol.value = stored;
        } else if (section & 0x0100) {
            symbol.kind = TosSymbol::Kind::Bss;
            symbol.value = stored + textSize + dataSize;
        } else if (section & 0x0200) {
            symbol.kind = TosSymbol::Kind::Text;
            symbol.value = stored;
        } else if (section & 0x0400) {
            symbol.kind = TosSymbol::Kind::Data;
            symbol.value = stored + textSize;
        } else {
            symbol.kind = TosSymbol::Kind::Other;
            symbol.value = stored;
        }
        out->append(symbol);
    }
    return true;
}

bool parseRelocations(const QByteArray &bytes, int offset, quint32 imageSize,
                      QVector<quint32> *out, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (offset + 4 > bytes.size())
        return fail(QObject::tr("relocation table is truncated"));

    quint64 cursor = read32(bytes, offset);
    offset += 4;
    if (cursor == 0)
        return true;
    const auto place = [&](quint64 at) {
        if ((at & 1u) || at + 4 > imageSize)
            return fail(QObject::tr("relocation is not a longword in the image"));
        out->append(quint32(at));
        return true;
    };
    if (!place(cursor))
        return false;

    while (offset < bytes.size()) {
        const uchar step = uchar(bytes.at(offset++));
        if (step == 0)
            return true;
        cursor += step == 1 ? 254 : step;
        if (step == 1)
            continue;
        if (!place(cursor))
            return false;
    }
    return fail(QObject::tr("relocation table is truncated"));
}

} // namespace

bool parseTosProgram(const QByteArray &bytes, TosProgram *out, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!out)
        return fail(QObject::tr("not a TOS executable"));
    *out = TosProgram();
    if (bytes.size() < 28 || read16(bytes, 0) != kMagic)
        return fail(QObject::tr("not a TOS executable"));

    const quint32 text = read32(bytes, 2);
    const quint32 data = read32(bytes, 6);
    const quint32 bss = read32(bytes, 10);
    const quint32 symbols = read32(bytes, 14);
    const quint32 flags = read32(bytes, 22);
    const bool relocatable = read16(bytes, 26) == 0;
    if ((text & 1u) || (data & 1u))
        return fail(QObject::tr("text or data size is odd"));

    const qint64 body = qint64(text) + data + symbols;
    if (body < 0 || 28 + body > bytes.size())
        return fail(QObject::tr("truncated TOS executable"));

    TosProgram program;
    program.textSize = text;
    program.dataSize = data;
    program.bssSize = bss;
    program.flags = flags;
    program.relocatable = relocatable;
    program.image = bytes.mid(28, int(text + data));
    if (!parseSymbols(bytes.mid(28 + int(text + data), int(symbols)), text, data,
                      &program.symbols, error))
        return false;
    if (relocatable
        && !parseRelocations(bytes, int(28 + body), text + data, &program.relocations, error))
        return false;
    *out = program;
    return true;
}

} // namespace pist
