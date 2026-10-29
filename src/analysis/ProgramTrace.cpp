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

const AnalysisOverride *markAt(const AnalysisNotes &notes, quint32 address);

bool listedAt(quint32 address, const QHash<quint32, QStringList> &labels, const AnalysisNotes &notes)
{
    return labels.contains(address) || notes.comments.contains(address);
}

QString withMark(const QString &note, quint32 address, const AnalysisNotes &notes)
{
    const AnalysisOverride *mark = markAt(notes, address);
    if (!mark)
        return note;
    const QString kind = mark->kind == AnalysisOverride::Kind::Code ? QStringLiteral("code")
                                                                    : QStringLiteral("data");
    return note.isEmpty() ? kind : kind + QStringLiteral(", ") + note;
}

void appendSpan(QVector<ListingRow> *rows, const QByteArray &image, quint32 from, quint32 to,
                const QHash<quint32, DecodedInsn> &instructions,
                const QHash<quint32, QStringList> &labels, const QSet<quint32> &relocations,
                const AnalysisNotes &notes)
{
    quint32 address = from;
    while (address < to) {
        if (instructions.contains(address)) {
            const DecodedInsn insn = instructions.value(address);
            if (insn.length <= 0 || address + quint32(insn.length) > to)
                break;
            ListingRow row;
            row.address = address;
            row.length = quint32(insn.length);
            row.label = labels.value(address).join(QStringLiteral(", "));
            row.bytes = formatBytes(image, address, insn.length);
            row.text = insn.text;
            row.note = withMark(noteFor(address, insn.length, insn.indirect, relocations), address, notes);
            row.comment = notes.comments.value(address);
            rows->append(row);
            address += quint32(insn.length);
            continue;
        }
        quint32 end = address + 1;
        const AnalysisOverride *mark = markAt(notes, address);
        while (end < to && end - address < 8 && !instructions.contains(end) && !listedAt(end, labels, notes)
               && markAt(notes, end) == mark)
            ++end;
        ListingRow row;
        row.address = address;
        row.length = end - address;
        row.label = labels.value(address).join(QStringLiteral(", "));
        row.bytes = formatBytes(image, address, int(end - address));
        row.text = formatDc(image, address, int(end - address));
        row.note = withMark(noteFor(address, int(end - address), false, relocations), address, notes);
        row.comment = notes.comments.value(address);
        rows->append(row);
        address = end;
    }
}

const AnalysisOverride *markAt(const AnalysisNotes &notes, quint32 address)
{
    for (const AnalysisOverride &range : notes.overrides) {
        if (range.length > 0 && address >= range.address && (address - range.address) < range.length)
            return &range;
    }
    return nullptr;
}

int rowContaining(const QVector<ListingRow> &rows, quint32 address)
{
    int spanning = -1;
    for (int i = 0; i < rows.size(); ++i) {
        const ListingRow &row = rows.at(i);
        if (row.length == 0 && row.address == address)
            return i;
        if (row.length > 0 && address >= row.address && (address - row.address) < row.length)
            spanning = i;
    }
    return spanning;
}

void attachXrefs(QVector<ListingRow> *rows, const QHash<quint32, QVector<quint32>> &inbound,
                 const QHash<quint32, QStringList> &labels)
{
    for (auto it = inbound.cbegin(); it != inbound.cend(); ++it) {
        const int index = rowContaining(*rows, it.key());
        if (index < 0)
            continue;
        QVector<quint32> sources = it.value();
        std::sort(sources.begin(), sources.end());
        sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
        QStringList names;
        for (quint32 source : sources) {
            const QStringList at = labels.value(source);
            names << (at.isEmpty() ? QStringLiteral("$%1").arg(source, 0, 16) : at.first());
        }
        rows->operator[](index).xrefs = names.join(QStringLiteral(", "));
    }
}

} // namespace

ProgramListing listProgram(const QByteArray &bytes, const AnalysisNotes &notes)
{
    ProgramListing listing;
    TosProgram program;
    if (!parseTosProgram(bytes, &program, &listing.error))
        return listing;

    const quint32 imageSize = program.textSize + program.dataSize;
    listing.imageSize = imageSize;
    listing.endAddress = imageSize + program.bssSize;

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
    for (auto it = notes.names.cbegin(); it != notes.names.cend(); ++it) {
        names.labels.insert(it.key(), it.value());
        QStringList &at = labels[it.key()];
        if (!at.contains(it.value()))
            at.prepend(it.value());
    }
    for (auto it = equateCount.cbegin(); it != equateCount.cend(); ++it) {
        if (it.value() == 1 && it.key() != 0)
            names.equates.insert(it.key(), equateName.value(it.key()));
    }
    for (quint32 reloc : program.relocations)
        names.relocations.insert(reloc);

    const auto dataAt = [&](quint32 address) {
        const AnalysisOverride *mark = markAt(notes, address);
        return mark && mark->kind == AnalysisOverride::Kind::Data;
    };
    const auto codeAt = [&](quint32 address) {
        const AnalysisOverride *mark = markAt(notes, address);
        return mark && mark->kind == AnalysisOverride::Kind::Code;
    };
    const auto canStart = [&](quint32 address) {
        if ((address & 1u) || address >= imageSize || dataAt(address))
            return false;
        return address < program.textSize || codeAt(address);
    };
    const auto limitFor = [&](quint32 address) {
        quint32 limit = address < program.textSize ? program.textSize : imageSize;
        if (address >= program.textSize) {
            const AnalysisOverride *mark = markAt(notes, address);
            limit = mark ? std::min(mark->address + mark->length, imageSize) : address;
        }
        for (const AnalysisOverride &range : notes.overrides) {
            if (range.kind == AnalysisOverride::Kind::Data && range.address > address
                && range.address < limit)
                limit = range.address;
        }
        return limit;
    };

    QHash<quint32, DecodedInsn> instructions;
    QHash<quint32, QVector<quint32>> inbound;
    QSet<quint32> seen;
    QVector<quint32> queue;
    if (canStart(0))
        queue.append(0);
    for (const AnalysisOverride &range : notes.overrides) {
        if (range.kind == AnalysisOverride::Kind::Code && canStart(range.address))
            queue.append(range.address);
    }
    while (!queue.isEmpty()) {
        const quint32 address = queue.takeFirst();
        if (seen.contains(address) || !canStart(address))
            continue;
        seen.insert(address);
        const quint32 limit = limitFor(address);
        DecodedInsn insn = decode68k(program.image, address, limit, names);
        if (insn.length <= 0 || address + quint32(insn.length) > limit)
            continue;
        if (endsProgram(program.image, address))
            insn.flow = DecodedInsn::Flow::Stop;
        instructions.insert(address, insn);
        QSet<quint32> targets;
        for (quint32 ref : insn.refs)
            targets.insert(ref);
        if (insn.hasTarget && !insn.indirect)
            targets.insert(insn.target);
        for (quint32 target : targets)
            inbound[target].append(address);
        if (insn.hasTarget && canStart(insn.target) && !seen.contains(insn.target))
            queue.append(insn.target);
        if (insn.flow == DecodedInsn::Flow::Fall || insn.flow == DecodedInsn::Flow::Call) {
            const quint32 next = address + quint32(insn.length);
            if (canStart(next) && !seen.contains(next))
                queue.append(next);
        }
    }

    appendSpan(&listing.rows, program.image, 0, program.textSize, instructions, labels,
               names.relocations, notes);
    appendSpan(&listing.rows, program.image, program.textSize, imageSize, instructions, labels,
               names.relocations, notes);
    if (program.bssSize > 0) {
        const quint32 base = imageSize;
        ListingRow block;
        block.address = base;
        block.length = program.bssSize;
        block.label = labels.value(base).join(QStringLiteral(", "));
        block.text = QStringLiteral("ds.b %1").arg(program.bssSize);
        block.comment = notes.comments.value(base);
        listing.rows.append(block);
        QVector<quint32> extras;
        for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
            if (it.key() > base && it.key() < base + program.bssSize)
                extras.append(it.key());
        }
        for (auto it = notes.comments.cbegin(); it != notes.comments.cend(); ++it) {
            if (it.key() > base && it.key() < base + program.bssSize && !labels.contains(it.key()))
                extras.append(it.key());
        }
        std::sort(extras.begin(), extras.end());
        extras.erase(std::unique(extras.begin(), extras.end()), extras.end());
        for (quint32 address : extras) {
            ListingRow row;
            row.address = address;
            row.label = labels.value(address).join(QStringLiteral(", "));
            row.text = QStringLiteral("bss");
            row.comment = notes.comments.value(address);
            listing.rows.append(row);
        }
    }
    attachXrefs(&listing.rows, inbound, labels);

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
