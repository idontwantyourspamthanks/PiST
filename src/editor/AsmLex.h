// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QChar>
#include <QString>

namespace pist {
namespace asmlex {

/// The lexical rules of vasm's Motorola (mot) syntax, in one place.
///
/// The syntax highlighter, the OS-call scanner and include/label navigation all
/// ask the same questions of a source line, and they used to answer them with
/// three copies of the same rules — which had drifted apart: the highlighter
/// missed indented labels, navigation missed a `*` comment, and all three read
/// a `;` inside a quoted string as a comment. They agree here instead, on what
/// vasm actually accepts:
///
///  - `;` starts a comment anywhere, except inside a quoted string;
///  - `*` in the first column makes the whole line a comment;
///  - an identifier is ASCII: letters, digits, `_`, `.` and `$`;
///  - a label may be indented, because vasm reads the first field as a label
///    wherever it starts, and the demos indent their local labels.

/// Whether `c` can appear anywhere in an identifier (a label, symbol or
/// operand). ASCII only: vasm's symbol table is ASCII, so the Unicode letters
/// QChar::isLetter() accepts are not part of a name.
inline bool isWordChar(QChar c)
{
    return (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('a') && c <= QLatin1Char('z'))
        || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('_')
        || c == QLatin1Char('.') || c == QLatin1Char('$');
}

/// Whether `c` can start an identifier. A digit or `$` cannot: `$` prefixes a
/// hexadecimal literal, and a leading digit is a number.
inline bool isWordStart(QChar c)
{
    return (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || c == QLatin1Char('_')
        || c == QLatin1Char('.');
}

/// Where the line's comment starts, or -1 when the whole line is code: 0 for a
/// line whose first character is `*` (the whole line is a comment), otherwise
/// the offset of the first `;` that is not inside a quoted string.
///
/// Both `"…"` and `'…'` open a string — Motorola syntax gives them the same
/// meaning — and `\` escapes the next character inside one. A quote that never
/// closes swallows the rest of the line, so a `;` after it belongs to the
/// string rather than starting a comment.
inline int commentStart(const QString &line)
{
    if (line.startsWith(QLatin1Char('*')))
        return 0;

    QChar quote;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (!quote.isNull()) {
            if (c == QLatin1Char('\\'))
                ++i; // the escaped character cannot close the string
            else if (c == quote)
                quote = QChar();
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            quote = c;
        } else if (c == QLatin1Char(';')) {
            return i;
        }
    }
    return -1;
}

/// The line's code field: everything before the comment, trimmed. Empty for a
/// whole-line `*` comment, for a blank line, and for a line that is only a
/// comment.
inline QString codePart(const QString &line)
{
    const int comment = commentStart(line);
    return (comment < 0 ? line : line.left(comment)).trimmed();
}

/// Whether the line defines a label: a name followed by `:` at the start of the
/// code field — indented or not, and with or without an instruction after the
/// colon.
///
/// `symbol` receives the name when asked for, and `start` and `length` its span
/// in `line`: the name is `line.mid(start, length)` and its colon follows the
/// name (after any spaces). A definition mentioned in a comment is not one.
inline bool isLabelDefinition(const QString &line, QString *symbol = nullptr, int *start = nullptr,
                              int *length = nullptr)
{
    const int comment = commentStart(line);
    const int end = (comment < 0) ? line.size() : comment;

    int i = 0;
    while (i < end && (line.at(i) == QLatin1Char(' ') || line.at(i) == QLatin1Char('\t')))
        ++i;
    if (i >= end || !isWordStart(line.at(i)))
        return false;

    const int nameStart = i;
    while (i < end && isWordChar(line.at(i)))
        ++i;
    const int nameEnd = i;
    while (i < end && (line.at(i) == QLatin1Char(' ') || line.at(i) == QLatin1Char('\t')))
        ++i;
    if (i >= end || line.at(i) != QLatin1Char(':'))
        return false;

    if (symbol)
        *symbol = line.mid(nameStart, nameEnd - nameStart);
    if (start)
        *start = nameStart;
    if (length)
        *length = nameEnd - nameStart;
    return true;
}

/// The instruction a line carries: its code field with a label definition in
/// front of it removed. Empty when the line holds only a label — which is how
/// the OS-call scans read the end of a call sequence — or only a comment.
inline QString instructionPart(const QString &line)
{
    const QString code = codePart(line);
    int length = 0;
    if (!isLabelDefinition(code, nullptr, nullptr, &length))
        return code;
    // Past the name: the colon (with any spaces before it), then whatever
    // instruction the line shares with the label.
    QString rest = code.mid(length).trimmed();
    if (rest.startsWith(QLatin1Char(':')))
        rest.remove(0, 1);
    return rest.trimmed();
}

} // namespace asmlex
} // namespace pist
