// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/AssemblyExport.h"

#include "analysis/ProgramTrace.h"
#include "analysis/TosProgram.h"
#include "support/FileWrite.h"

#include <QFile>
#include <QMap>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>

namespace pist {

namespace {

QStringList labelNames(const QString &label)
{
    if (label.isEmpty())
        return {};
    return label.split(QStringLiteral(", "), Qt::SkipEmptyParts);
}

void appendLabels(QString *out, const QStringList &names)
{
    for (const QString &name : names)
        *out += name + QStringLiteral(":\n");
}

void appendInstruction(QString *out, const ListingRow &row)
{
    appendLabels(out, labelNames(row.label));
    *out += QLatin1Char('\t');
    *out += row.text;
    if (!row.comment.isEmpty()) {
        *out += QStringLiteral(" ; ");
        *out += row.comment;
    }
    *out += QLatin1Char('\n');
}

/// A label can sit on a row that already starts there, inside a `dc.b` run
/// (the next listing splits the run), or anywhere in bss. It cannot sit in the
/// middle of an instruction: that row is not split, so the name would be used
/// and never defined.
bool canPlaceLabel(quint32 address, const ProgramListing &listing)
{
    if (address >= listing.endAddress)
        return false;
    if (address >= listing.imageSize)
        return true;
    for (const ListingRow &row : listing.rows) {
        if (row.address == address)
            return true;
        if (row.length > 0 && address > row.address && (address - row.address) < row.length)
            return row.text.startsWith(QStringLiteral("dc.b"));
    }
    return false;
}

struct BssMark {
    QStringList names;
    QString comment;
};

void appendBss(QString *out, const ProgramListing &listing)
{
    if (listing.bssSize == 0)
        return;
    QMap<quint32, BssMark> marks;
    for (const ListingRow &row : listing.rows) {
        if (row.address < listing.imageSize || row.address >= listing.endAddress)
            continue;
        const QStringList names = labelNames(row.label);
        if (names.isEmpty() && row.comment.isEmpty())
            continue;
        BssMark &mark = marks[row.address];
        for (const QString &name : names) {
            if (!mark.names.contains(name))
                mark.names.append(name);
        }
        if (mark.comment.isEmpty())
            mark.comment = row.comment;
    }

    *out += QStringLiteral("\tbss\n");
    quint32 cursor = listing.imageSize;
    const quint32 end = listing.imageSize + listing.bssSize;
    for (auto it = marks.cbegin(); it != marks.cend(); ++it) {
        if (it.key() < cursor || it.key() > end)
            continue;
        if (it.key() > cursor)
            *out += QStringLiteral("\tds.b %1\n").arg(it.key() - cursor);
        appendLabels(out, it->names);
        if (!it->comment.isEmpty())
            *out += QStringLiteral("\t; ") + it->comment + QLatin1Char('\n');
        cursor = it.key();
    }
    if (end > cursor)
        *out += QStringLiteral("\tds.b %1\n").arg(end - cursor);
}

} // namespace

QString exportAssembly(const QByteArray &bytes, const AnalysisNotes &notes, QString *error)
{
    if (error)
        error->clear();
    TosProgram program;
    QString parseError;
    if (!parseTosProgram(bytes, &program, &parseError)) {
        if (error)
            *error = parseError;
        return {};
    }
    ProgramListing listing = listProgram(bytes, notes);
    if (!listing.error.isEmpty()) {
        if (error)
            *error = listing.error;
        return {};
    }

    QSet<QString> taken;
    QSet<quint32> labeled;
    const auto claim = [&taken](const QString &name) {
        if (!name.isEmpty())
            taken.insert(name.toLower());
    };
    for (const TosSymbol &symbol : program.symbols) {
        claim(symbol.name);
        if (symbol.kind != TosSymbol::Kind::Equate && !symbol.name.isEmpty())
            labeled.insert(symbol.value);
    }
    for (auto it = notes.names.cbegin(); it != notes.names.cend(); ++it) {
        claim(it.value());
        labeled.insert(it.key());
    }

    // `$4(pc)` is not a stable operand: vasm's `-Ftos` and `-Fbin` disagree
    // about it. A label at the target is the form both encode as the
    // displacement the decoder read.
    AnalysisNotes named = notes;
    bool added = false;
    for (quint32 address : listing.referenced) {
        if (labeled.contains(address) || !canPlaceLabel(address, listing))
            continue;
        QString name = QStringLiteral("loc_%1").arg(address, 0, 16);
        while (taken.contains(name.toLower()))
            name += QLatin1Char('_');
        claim(name);
        named.names.insert(address, name);
        labeled.insert(address);
        added = true;
    }
    if (added) {
        listing = listProgram(bytes, named);
        if (!listing.error.isEmpty()) {
            if (error)
                *error = listing.error;
            return {};
        }
    }

    QString out = QStringLiteral("; PiST analysis export\n");
    for (const TosSymbol &symbol : program.symbols) {
        if (symbol.kind != TosSymbol::Kind::Equate || symbol.name.isEmpty())
            continue;
        out += symbol.name + QStringLiteral(" equ $") + QString::number(symbol.value, 16)
               + QLatin1Char('\n');
    }

    bool inData = false;
    bool inText = false;
    for (const ListingRow &row : listing.rows) {
        if (row.address >= listing.imageSize)
            break;
        if (row.address >= listing.textSize) {
            if (!inData) {
                out += QStringLiteral("\tdata\n");
                inData = true;
            }
        } else if (!inText) {
            out += QStringLiteral("\ttext\n");
            inText = true;
        }
        appendInstruction(&out, row);
    }
    appendBss(&out, listing);
    return out;
}

bool assembledImageMatches(const QString &assembler, const QString &source,
                           const QByteArray &original, QString *error)
{
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };
    if (error)
        error->clear();
    if (assembler.isEmpty())
        return fail(QObject::tr("The assembler was not found."));

    TosProgram want;
    QString parseError;
    if (!parseTosProgram(original, &want, &parseError))
        return fail(parseError);

    QTemporaryDir dir;
    if (!dir.isValid())
        return fail(QObject::tr("Could not create a directory for the assembler."));
    const QString srcPath = dir.filePath(QStringLiteral("export.s"));
    const QString prgPath = dir.filePath(QStringLiteral("export.prg"));
    if (!files::write(srcPath, source.toUtf8(), &parseError))
        return fail(parseError);

    // The project's own build is a different file, with the project's flags.
    // This check is only the exported source, and it must not replace that
    // output. vasm is a subprocess: it is not linked, and nothing of it is
    // written beside the original program.
    QProcess process;
    process.setWorkingDirectory(dir.path());
    process.start(assembler, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"),
                              QStringLiteral("-o"), prgPath, srcPath});
    if (!process.waitForStarted(10000))
        return fail(QObject::tr("Could not start the assembler."));
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        return fail(QObject::tr("The assembler did not finish."));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        return fail(detail.isEmpty() ? QObject::tr("The assembler failed.") : detail);
    }

    QFile produced(prgPath);
    if (!produced.open(QIODevice::ReadOnly))
        return fail(QObject::tr("The assembler did not write a program."));
    TosProgram got;
    if (!parseTosProgram(produced.readAll(), &got, &parseError))
        return fail(parseError);
    if (got.textSize == want.textSize && got.dataSize == want.dataSize && got.bssSize == want.bssSize
        && got.image == want.image)
        return true;
    if (got.textSize != want.textSize || got.dataSize != want.dataSize || got.bssSize != want.bssSize)
        return fail(QObject::tr("Assembled text %1, data %2 and bss %3 do not match the original "
                                "text %4, data %5 and bss %6.")
                        .arg(got.textSize)
                        .arg(got.dataSize)
                        .arg(got.bssSize)
                        .arg(want.textSize)
                        .arg(want.dataSize)
                        .arg(want.bssSize));
    int at = 0;
    const int shared = std::min(got.image.size(), want.image.size());
    while (at < shared && got.image.at(at) == want.image.at(at))
        ++at;
    return fail(QObject::tr("The assembled bytes differ at image address $%1.").arg(at, 0, 16));
}

} // namespace pist
