// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/IncludeNav.h"

#include "editor/AsmLex.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace pist {

namespace {

/// `include "lib.s"` / `include 'lib.s'`, with the directive at the start of the
/// code field. Anchoring it there is what keeps a comment that merely mentions
/// an include (`; include "lib.s"`, or `* include ...`) from naming a file: the
/// comment marker is in the way, so the pattern cannot match.
///
/// The two quote styles are separate alternatives rather than a backreference,
/// so the body may contain the other quote (`include "d'artagnan.s"`) but never
/// its own: an unterminated quote names nothing instead of swallowing the rest
/// of the line, and a trailing comment's quotes stay out of the match.
const QRegularExpression &includeRe()
{
    static const QRegularExpression re(
        QStringLiteral("^\\s*include\\s+(?:\"([^\"\\n]*)\"|'([^'\\n]*)')"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

/// A defining occurrence of a symbol that is not the `name:` form isLabelDefinition
/// reads: `foo equ 5`, `foo set 5` and `foo = 5`. `set` is listed with `equ`
/// because vasm treats either as defining the symbol. The identifier alphabet
/// is the same ASCII one asmlex::isWordChar() accepts.
const QRegularExpression &assignmentRe()
{
    static const QRegularExpression re(
        QStringLiteral("^([A-Za-z_.][A-Za-z0-9_.$]*)[ \\t]*(?:=|\\bequ\\b|\\bset\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

} // namespace

QString includeTargetAt(const QString &lineText)
{
    const auto match = includeRe().match(lineText);
    if (!match.hasMatch())
        return {};
    // Exactly one of the two quote alternatives took part; an empty name (the
    // `include ""` form) is no target either.
    const QString target = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
    return target;
}

QString resolveInclude(const QString &name, const QString &currentFileDir,
                       const QStringList &includePaths)
{
    if (name.isEmpty())
        return {};

    // The including file's own directory is searched first, then the -I paths in
    // the order the project lists them. That order was checked against vasm:
    // with the same `lib.s` in both the source's directory and an -I directory,
    // the assembly takes the one beside the source.
    QStringList dirs;
    dirs << currentFileDir;
    dirs += includePaths;

    for (const QString &dir : dirs) {
        // An empty entry means the process's own directory, which is also where
        // the build runs the assembler from.
        const QString candidate = dir.isEmpty() ? name : QDir(dir).filePath(name);
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile())
            return info.absoluteFilePath();
    }
    return {};
}

int labelLine(const QString &documentText, const QString &word)
{
    if (word.isEmpty())
        return 0;

    const QStringList lines = documentText.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        // asmlex::codePart drops a `*` whole-line comment and everything after a
        // `;`, so a definition mentioned in a comment is not a definition.
        const QString code = asmlex::codePart(lines.at(i));
        if (code.isEmpty())
            continue;

        QString symbol;
        if (!asmlex::isLabelDefinition(code, &symbol)) {
            const auto match = assignmentRe().match(code);
            if (!match.hasMatch())
                continue;
            symbol = match.captured(1);
        }
        if (QString::compare(symbol, word, Qt::CaseInsensitive) == 0)
            return i + 1;
    }
    return 0;
}

QString wordAtCursor(const QString &lineText, int column)
{
    if (lineText.isEmpty() || column < 0)
        return {};

    // One past the end of the line means the last character, so a click just
    // past a word still finds that word.
    const int last = lineText.length() - 1;
    const int at = qMin(column, last);

    if (asmlex::isWordChar(lineText.at(at))) {
        int start = at;
        while (start > 0 && asmlex::isWordChar(lineText.at(start - 1)))
            --start;
        int end = at;
        while (end < last && asmlex::isWordChar(lineText.at(end + 1)))
            ++end;
        return lineText.mid(start, end - start + 1);
    }

    // Punctuation and whitespace carry no token of their own. When the character
    // immediately to the left is a word character the caret is at the end of that
    // word, so its token wins: a click on the `+` of `kMaxX+1` and on the `,` of
    // `1,d4` lands on the token being left. Otherwise the token ahead serves,
    // which is what makes a click on the `#` of `#kMaxX`, on the leading tab of an
    // indented line, or at column 0 work.
    int end = at - 1;
    if (end < 0 || !asmlex::isWordChar(lineText.at(end))) {
        int start = at;
        while (start <= last && !asmlex::isWordChar(lineText.at(start)))
            ++start;
        if (start > last)
            return {};
        end = start;
        while (end < last && asmlex::isWordChar(lineText.at(end + 1)))
            ++end;
        return lineText.mid(start, end - start + 1);
    }

    int start = end;
    while (start > 0 && asmlex::isWordChar(lineText.at(start - 1)))
        --start;
    return lineText.mid(start, end - start + 1);
}

} // namespace pist
