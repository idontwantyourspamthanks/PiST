// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/IncludeNav.h"

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

/// A defining occurrence of a symbol at the start of the code field: `foo:`,
/// `foo equ 5`, `foo set 5` or `foo = 5`. Leading whitespace is allowed, which
/// is how the demos indent local labels. `set` is included with `equ` because
/// AsmHighlighter lists both among the assignment directives and vasm treats
/// either as defining the symbol.
const QRegularExpression &labelRe()
{
    static const QRegularExpression re(
        QStringLiteral("^[ \\t]*([A-Za-z_.][A-Za-z0-9_.$]*)[ \\t]*(?::|=|\\bequ\\b|\\bset\\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

/// The assembler's identifier alphabet, kept identical to AsmHighlighter's
/// label pattern so a token found here is a token that is highlighted there.
/// `$` is in it because vasm allows it in symbol names.
bool isWordChar(QChar c)
{
    const bool asciiLetter = (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
                          || (c >= QLatin1Char('a') && c <= QLatin1Char('z'));
    const bool asciiDigit = (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
    return asciiLetter || asciiDigit || c == QLatin1Char('_') || c == QLatin1Char('.')
        || c == QLatin1Char('$');
}

/// The code field of a line: everything before a `;` comment. Motorola syntax
/// starts a comment anywhere, so nothing after the `;` can define anything.
QString codePart(const QString &line)
{
    const int semicolon = line.indexOf(QLatin1Char(';'));
    return (semicolon >= 0) ? line.left(semicolon) : line;
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

    const QRegularExpression &re = labelRe();
    const QStringList lines = documentText.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        // `*` in the first column is a whole-line comment (AsmHighlighter), so a
        // definition mentioned in one is not a definition.
        if (line.startsWith(QLatin1Char('*')))
            continue;

        const auto match = re.match(codePart(line));
        if (!match.hasMatch())
            continue;
        if (QString::compare(match.captured(1), word, Qt::CaseInsensitive) == 0)
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

    if (isWordChar(lineText.at(at))) {
        int start = at;
        while (start > 0 && isWordChar(lineText.at(start - 1)))
            --start;
        int end = at;
        while (end < last && isWordChar(lineText.at(end + 1)))
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
    if (end < 0 || !isWordChar(lineText.at(end))) {
        int start = at;
        while (start <= last && !isWordChar(lineText.at(start)))
            ++start;
        if (start > last)
            return {};
        end = start;
        while (end < last && isWordChar(lineText.at(end + 1)))
            ++end;
        return lineText.mid(start, end - start + 1);
    }

    int start = end;
    while (start > 0 && isWordChar(lineText.at(start - 1)))
        --start;
    return lineText.mid(start, end - start + 1);
}

} // namespace pist
