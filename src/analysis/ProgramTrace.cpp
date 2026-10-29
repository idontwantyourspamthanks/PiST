// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/ProgramTrace.h"

#include "analysis/M68kDisasm.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace pist {

namespace {

quint16 read16(const QByteArray &image, quint32 address)
{
    return quint16((uchar(image.at(int(address))) << 8) | uchar(image.at(int(address + 1))));
}

QString hexByte(uchar value)
{
    return QStringLiteral("%1").arg(value, 2, 16, QLatin1Char('0'));
}

QString formatBytes(const QByteArray &image, quint32 address, int length)
{
    QStringList parts;
    for (int i = 0; i < length; ++i)
        parts << hexByte(uchar(image.at(int(address) + i)));
    return parts.join(QLatin1Char(' '));
}

QString formatDc(const QByteArray &image, quint32 address, int length)
{
    QStringList parts;
    for (int i = 0; i < length; ++i)
        parts << QStringLiteral("$") + hexByte(uchar(image.at(int(address) + i)));
    return QStringLiteral("dc.b ") + parts.join(QLatin1Char(','));
}

bool endsProgram(const QByteArray &image, quint32 address)
{
    if (address + 2 > quint32(image.size()) || read16(image, address) != 0x4E41)
        return false;
    // GEMDOS Pterm0 is `clr.w -(sp)` / `trap #1`. Pterm is the same trap with
    // 0 or $4C pushed as a word. Other trap #1 calls return.
    if (address >= 2 && read16(image, address - 2) == 0x4267)
        return true;
    if (address >= 4 && read16(image, address - 4) == 0x3F3C) {
        const quint16 immediate = read16(image, address - 2);
        return immediate == 0 || immediate == 0x004C;
    }
    return false;
}

QString noteFor(quint32 address, int length, bool indirect, const QSet<quint32> &relocations)
{
    QStringList notes;
    if (indirect)
        notes << QStringLiteral("unresolved");
    for (quint32 reloc : relocations) {
        if (reloc >= address && reloc < address + quint32(length)) {
            notes << QStringLiteral("reloc");
            break;
        }
    }
    return notes.join(QStringLiteral(", "));
}

void appendSpan(QVector<ListingRow> *rows, const QByteArray &image, quint32 from, quint32 to,
                const QHash<quint32, DecodedInsn> &instructions,
                const QHash<quint32, QStringList> &labels, const QSet<quint32> &relocations)
{
    quint32 address = from;
    while (address < to) {
        if (instructions.contains(address)) {
            const DecodedInsn insn = instructions.value(address);
            if (insn.length <= 0 || address + quint32(insn.length) > to)
                break;
            ListingRow row;
            row.address = address;
            row.label = labels.value(address).join(QStringLiteral(", "));
            row.bytes = formatBytes(image, address, insn.length);
            row.text = insn.text;
            row.note = noteFor(address, insn.length, insn.indirect, relocations);
            rows->append(row);
            address += quint32(insn.length);
            continue;
        }
        quint32 end = address + 1;
        while (end < to && end - address < 8 && !instructions.contains(end) && !labels.contains(end))
            ++end;
        ListingRow row;
        row.address = address;
        row.label = labels.value(address).join(QStringLiteral(", "));
        row.bytes = formatBytes(image, address, int(end - address));
        row.text = formatDc(image, address, int(end - address));
        row.note = noteFor(address, int(end - address), false, relocations);
        rows->append(row);
        address = end;
    }
}

} // namespace

ProgramListing listProgram(const QByteArray &bytes)
{
    ProgramListing listing;
    TosProgram program;
    if (!parseTosProgram(bytes, &program, &listing.error))
        return listing;

    QHash<quint32, QStringList> labels;
    QHash<quint32, int> equateCount;
    QHash<quint32, QString> equateName;
    DisasmNames names;
    for (const TosSymbol &symbol : program.symbols) {
        if (symbol.name.isEmpty())
            continue;
        if (symbol.kind == TosSymbol::Kind::Equate) {
            equateCount[symbol.value] += 1;
            equateName.insert(symbol.value, symbol.name);
            continue;
        }
        labels[symbol.value].append(symbol.name);
        if (!names.labels.contains(symbol.value))
            names.labels.insert(symbol.value, symbol.name);
    }
    for (auto it = equateCount.cbegin(); it != equateCount.cend(); ++it) {
        if (it.value() == 1 && it.key() != 0)
            names.equates.insert(it.key(), equateName.value(it.key()));
    }
    for (quint32 reloc : program.relocations)
        names.relocations.insert(reloc);

    QHash<quint32, DecodedInsn> instructions;
    if (program.textSize >= 2) {
        QSet<quint32> seen;
        QVector<quint32> queue;
        queue.append(0);
        while (!queue.isEmpty()) {
            const quint32 address = queue.takeFirst();
            if (seen.contains(address) || address >= program.textSize || (address & 1u))
                continue;
            seen.insert(address);
            DecodedInsn insn = decode68k(program.image, address, program.textSize, names);
            if (insn.length <= 0 || address + quint32(insn.length) > program.textSize)
                continue;
            if (endsProgram(program.image, address))
                insn.flow = DecodedInsn::Flow::Stop;
            instructions.insert(address, insn);
            const auto enqueue = [&](quint32 target) {
                if ((target & 1u) == 0 && target < program.textSize && !seen.contains(target))
                    queue.append(target);
            };
            if (insn.hasTarget)
                enqueue(insn.target);
            if (insn.flow == DecodedInsn::Flow::Fall || insn.flow == DecodedInsn::Flow::Call)
                enqueue(address + quint32(insn.length));
        }
    }

    appendSpan(&listing.rows, program.image, 0, program.textSize, instructions, labels,
               names.relocations);
    appendSpan(&listing.rows, program.image, program.textSize, program.textSize + program.dataSize,
               {}, labels, names.relocations);
    if (program.bssSize > 0) {
        const quint32 base = program.textSize + program.dataSize;
        ListingRow block;
        block.address = base;
        block.label = labels.value(base).join(QStringLiteral(", "));
        block.text = QStringLiteral("ds.b %1").arg(program.bssSize);
        listing.rows.append(block);
        QVector<quint32> extras;
        for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
            if (it.key() > base && it.key() < base + program.bssSize)
                extras.append(it.key());
        }
        std::sort(extras.begin(), extras.end());
        for (quint32 address : extras) {
            ListingRow row;
            row.address = address;
            row.label = labels.value(address).join(QStringLiteral(", "));
            row.text = QStringLiteral("bss");
            listing.rows.append(row);
        }
    }

    listing.summary = QObject::tr("%1 bytes of code, %2 bytes of data, %3 bytes of bss, "
                                  "%4 symbols, %5 relocations")
                          .arg(program.textSize)
                          .arg(program.dataSize)
                          .arg(program.bssSize)
                          .arg(program.symbols.size())
                          .arg(program.relocations.size());
    listing.error.clear();
    return listing;
}

} // namespace pist
