// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/SymbolTable.h"

#include "build/LineMap.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

#include <algorithm>

namespace pist {

namespace {

// `Source: "sym.s"` — the only thing that ties a body line number to a file.
const QRegularExpression &sourceRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^\s*Source:\s+"([^"]+)")RX"));
    return re;
}

// The `HH:HHHHHHHH <bytes>` prefix of a body line that emitted bytes. Only that
// prefix is dropped: such a line still carries its source line number and text,
// and a label defined on the same line as an instruction (`start: moveq`) lives
// there.
const QRegularExpression &bytesRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^\s*[0-9A-Fa-f]{2}:[0-9A-Fa-f]{8}\s+[0-9A-Fa-f]*\s+)RX"));
    return re;
}

// Any offset prefix, with or without a byte column. These are the lines that
// carry no source line number: a data item's continuation lines (the first one
// is lined, the rest are not). A section offset reads like a line number, so
// they must be rejected before the line-number field is examined.
const QRegularExpression &offsetLineRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^\s*[0-9A-Fa-f]{2}:[0-9A-Fa-f]{8}(\s|$))RX"));
    return re;
}

// The source line-number field: `    12: text`. A letter between the number and
// the colon marks an expansion line — `1M` for a macro, `1R` for a `rept` —
// whose number counts lines inside the expansion, not lines of the file, so it
// names a position this listing cannot state.
const QRegularExpression &numberedRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^\s*(\d+)([A-Za-z])?:[ ]?(.*)$)RX"));
    return re;
}

// `name:` at the start of the source text.
const QRegularExpression &labelRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^([A-Za-z_.$@][A-Za-z0-9_.$@]*):)RX"));
    return re;
}

// `name equ value`, `name set value` and `name = value`. vasm's own symbol table
// lists these as definitions, and their line is where the assignment is made.
const QRegularExpression &equRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^([A-Za-z_.$@][A-Za-z0-9_.$@]*)\s*(?:=\s*\S|(?:equ|set)\s))RX"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// A name at the start of the source text with no colon after it. Whether this is
// a definition cannot be told from the line alone — `even`, `dc.b` and a macro
// name take the same shape — so these are held back and kept only when the
// listing's own symbol table names them.
const QRegularExpression &bareNameRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^([A-Za-z_.$@][A-Za-z0-9_.$@]*)(\s|$))RX"));
    return re;
}

// The macro delimiters, in either spelling vasm accepts: `MACRO name` and
// `name MACRO` open, `ENDM` / `ENDMACRO` close. Both are directives, so they are
// recognised by their first two whitespace-separated tokens rather than by
// column — a listing indents them like any other directive, and the `name MACRO`
// form puts the macro's own name first, which must not be mistaken for a symbol.
bool isMacroDelimiter(const QString &statement, bool *opens)
{
    const QStringList tokens = statement.split(QRegularExpression(QStringLiteral("\\s+")),
                                               Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return false;

    const auto is = [](const QString &token, QLatin1String word) {
        return token.compare(word, Qt::CaseInsensitive) == 0;
    };

    if (is(tokens.at(0), QLatin1String("macro"))) {
        *opens = true;
        return true;
    }
    if (is(tokens.at(0), QLatin1String("endm")) || is(tokens.at(0), QLatin1String("endmacro"))) {
        *opens = false;
        return true;
    }
    if (tokens.size() >= 2 && is(tokens.at(1), QLatin1String("macro"))) {
        *opens = true;
        return true;
    }
    return false;
}

// `count                           00:00000012` in the appended by-name table.
// The `E:`/`S:` prefixes of an equate and a `set` are one hex digit, where a
// section index is two.
const QRegularExpression &tableNameRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^(\S+)\s+[0-9A-Fa-f]{1,2}:[0-9A-Fa-f]{8}(\s|$))RX"));
    return re;
}

// `exp_lab                         external EXP` — an `xdef` this module never
// defines, which the table still names.
const QRegularExpression &tableExternalRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^(\S+)\s+external\b)RX"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

// `00000012 count` in the by-value table, which is all a listing carries when it
// has no by-name one.
const QRegularExpression &tableValueRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^[0-9A-Fa-f]{8}\s+(\S+)\s*$)RX"));
    return re;
}

/// The symbol a line of one of the appended tables names, or an empty string.
QString tableSymbolName(const QString &line, int section)
{
    if (section == 1) {
        auto byName = tableNameRe().match(line);
        if (byName.hasMatch())
            return byName.captured(1);
        auto external = tableExternalRe().match(line);
        if (external.hasMatch())
            return external.captured(1);
        return QString();
    }
    auto byValue = tableValueRe().match(line);
    return byValue.hasMatch() ? byValue.captured(1) : QString();
}

} // namespace

QVector<SymbolEntry> symbolsFromListing(const QString &listingPath, const QString &sourceFile)
{
    QFile file(listingPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    const bool filterByFile = !sourceFile.isEmpty();
    const auto keep = [&](const QString &definitionFile) {
        return !filterByFile || LineMap::sameSource(definitionFile, sourceFile);
    };

    QString currentFile;
    QVector<SymbolEntry> definitions;
    QVector<SymbolEntry> bareCandidates;
    QSet<QString> tableNames;
    QVector<QString> tableOrder;
    bool inMacroBody = false;

    enum Section { Body, ByName, ByValue };
    int section = Body;

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString raw = stream.readLine();

        if (section != Body) {
            if (raw.trimmed() == QLatin1String("Symbols by value:")) {
                section = ByValue;
                continue;
            }
            const QString name = tableSymbolName(raw, section);
            if (!name.isEmpty() && !tableNames.contains(name)) {
                tableNames.insert(name);
                tableOrder.append(name);
            }
            continue;
        }

        auto source = sourceRe().match(raw);
        if (source.hasMatch()) {
            currentFile = source.captured(1);
            continue;
        }

        // A listing may carry only one of the two tables, so each is recognised
        // on its own. `Symbols by value:` names are position-less like the
        // by-name ones, so a body definition still takes precedence.
        if (raw.trimmed() == QLatin1String("Symbols by name:")) {
            section = ByName;
            continue;
        }
        if (raw.trimmed() == QLatin1String("Symbols by value:")) {
            section = ByValue;
            continue;
        }

        QString text = raw;
        auto bytes = bytesRe().match(text);
        if (bytes.hasMatch()) {
            text = text.mid(bytes.capturedLength());
        } else if (offsetLineRe().match(text).hasMatch()) {
            continue;
        }

        auto numbered = numberedRe().match(text);
        if (!numbered.hasMatch())
            continue;
        if (!numbered.captured(2).isEmpty())
            continue; // an expansion line: its number is not a file line
        const int line = numbered.captured(1).toInt();
        if (line <= 0)
            continue;

        // The listing keeps the source text verbatim, so a definition is what
        // starts in the first column. An indented line is an instruction, a
        // directive or an operand.
        const QString statement = numbered.captured(3);
        if (statement.isEmpty())
            continue;

        // Macro delimiters are themselves indented — `\tMACRO name`, `\tENDM` —
        // so they are recognised before the column check below, which would
        // otherwise discard them. They are directives, never symbols, so reading
        // them here cannot shadow a definition.
        bool opensMacro = false;
        if (isMacroDelimiter(statement, &opensMacro)) {
            inMacroBody = opensMacro;
            continue;
        }

        // A line inside a macro definition is the macro's own text, not a
        // definition in the assembled program: vasm emits the body only when the
        // macro is expanded, and then under a `1M` number that is skipped above.
        // A label there is template text — its position is the caller's, and a
        // name built from a parameter has no spelling in this file at all — so
        // macro internals contribute nothing to the symbol list.
        if (inMacroBody)
            continue;

        if (statement.at(0).isSpace())
            continue;

        auto label = labelRe().match(statement);
        if (label.hasMatch()) {
            if (keep(currentFile))
                definitions.append(SymbolEntry{label.captured(1), currentFile, line});
            continue;
        }

        auto equ = equRe().match(statement);
        if (equ.hasMatch()) {
            if (keep(currentFile))
                definitions.append(SymbolEntry{equ.captured(1), currentFile, line});
            continue;
        }

        auto bare = bareNameRe().match(statement);
        if (bare.hasMatch() && keep(currentFile))
            bareCandidates.append(SymbolEntry{bare.captured(1), currentFile, line});
    }

    QVector<SymbolEntry> out;
    QSet<QString> seen;
    const auto add = [&](const SymbolEntry &entry) {
        if (seen.contains(entry.name))
            return;
        seen.insert(entry.name);
        out.append(entry);
    };

    // The body first: its definitions are the ones that carry a source position,
    // and the appended table repeats them without one. vasm names a symbol once
    // and reuses `.loop` style locals within a scope, so the first definition of
    // a name is the only one this flat list can honestly report.
    for (const SymbolEntry &entry : definitions)
        add(entry);

    for (const SymbolEntry &entry : bareCandidates) {
        if (tableNames.contains(entry.name))
            add(entry);
    }

    // Names only the table knows — a command-line `-D`, a macro-generated label —
    // have no position in any source file. They are reported for an unfiltered
    // call alone: a caller asking about one file would otherwise see every other
    // module's position-less names repeated under it.
    if (!filterByFile) {
        for (const QString &name : tableOrder)
            add(SymbolEntry{name, QString(), 0});
    }

    // Name order, because that is how a symbol is looked up — and it is what
    // vasm's own table presents. The tie-break keeps the order stable for the
    // same name defined in more than one file.
    std::sort(out.begin(), out.end(), [](const SymbolEntry &a, const SymbolEntry &b) {
        const int byName = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        if (byName != 0)
            return byName < 0;
        if (a.line != b.line)
            return a.line < b.line;
        return a.file < b.file;
    });

    return out;
}

} // namespace pist
