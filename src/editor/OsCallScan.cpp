// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/OsCallScan.h"

#include "editor/OsCallRef.h"

#include <QRegularExpression>

namespace pist {

namespace {

// How far either scan looks. The real guard against misattribution is the
// sequence rules below — a scan stops at the first line that is not a push,
// blank or comment — so this is only a sanity cap, and it must be generous
// enough for the widest documented call: Flopfmt takes nine arguments, ten
// consecutive push lines before its trap.
constexpr int kScanBound = 16;

/// The line with its comment removed: `;` starts a comment anywhere, `*` only
/// in the first column (Motorola convention, same as AsmHighlighter).
QString codePart(const QString &line)
{
    if (line.startsWith(QLatin1Char('*')))
        return QString();
    const int semi = line.indexOf(QLatin1Char(';'));
    return (semi < 0 ? line : line.left(semi)).trimmed();
}

/// The instruction part of a line: comment stripped, and a `label:` prefix
/// removed when one is present — vasm lets a label share its line with an
/// instruction (`start: move.l #msg,-(a7)`), and the push still counts as
/// part of the call sequence. A line holding only a label returns empty,
/// which is how the scans tell a sequence boundary from a blank.
QString instructionPart(const QString &line)
{
    static const QRegularExpression labelRe(QStringLiteral("^[A-Za-z_.$][\\w.$]*:\\s*"));
    QString code = codePart(line);
    return code.remove(labelRe).trimmed();
}

const QRegularExpression &trapRe()
{
    static const QRegularExpression re(QStringLiteral("^trap\\s+#(\\d+)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

/// The trap number of a `trap #n` line that names an OS layer, 0 otherwise.
int osTrap(const QString &code)
{
    const QRegularExpressionMatch m = trapRe().match(code);
    if (!m.hasMatch())
        return 0;
    const int n = m.captured(1).toInt();
    return (n == 1 || n == 13 || n == 14) ? n : 0;
}

const QRegularExpression &wordPushRe()
{
    // The capture keeps the `#`, so an immediate displays the way source
    // writes it ("Calling with: #3"); resolveTrap strips it for the lookup.
    static const QRegularExpression re(QStringLiteral("^move\\.w\\s+(#[^,]+),-\\((?:sp|a7)\\)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression &clrWordRe()
{
    static const QRegularExpression re(QStringLiteral("^clr\\.w\\s+-\\((?:sp|a7)\\)$"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

/// A word-sized push of an immediate: the fn-number push, and word arguments.
bool isWordPush(const QString &code, QString *operand)
{
    const QRegularExpressionMatch m = wordPushRe().match(code);
    if (m.hasMatch()) {
        if (operand)
            *operand = m.captured(1).trimmed();
        return true;
    }
    if (clrWordRe().match(code).hasMatch()) {
        if (operand)
            *operand = QStringLiteral("0");
        return true;
    }
    return false;
}

/// A long-sized push: pointer arguments and 32-bit values.
bool isLongPush(const QString &code, QString *operand)
{
    static const QRegularExpression peaRe(QStringLiteral("^pea\\s+(.+)$"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression moveRe(QStringLiteral("^move\\.l\\s+([^,]+),-\\((?:sp|a7)\\)$"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression clrRe(QStringLiteral("^clr\\.l\\s+-\\((?:sp|a7)\\)$"),
                                          QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch pea = peaRe.match(code);
    if (pea.hasMatch()) {
        if (operand)
            *operand = pea.captured(1).trimmed();
        return true;
    }
    const QRegularExpressionMatch move = moveRe.match(code);
    if (move.hasMatch()) {
        if (operand)
            *operand = move.captured(1).trimmed();
        return true;
    }
    if (clrRe.match(code).hasMatch()) {
        if (operand)
            *operand = QStringLiteral("0");
        return true;
    }
    return false;
}

/// The function number named by an immediate operand: any radix source uses
/// ($hex, %binary, 0x, decimal), or -1 when the operand is not numeric (a
/// register push, or a symbolic name to resolve through the table).
int parseNumber(const QString &operand, bool *ok)
{
    *ok = true;
    QString text = operand.trimmed();
    if (text.startsWith(QLatin1Char('$')))
        return text.mid(1).toInt(ok, 16);
    if (text.startsWith(QLatin1Char('%')))
        return text.mid(1).toInt(ok, 2);
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        return text.toInt(ok, 0);
    bool numeric = false;
    const int value = text.toInt(&numeric, 10);
    if (numeric)
        return value;
    *ok = false;
    return -1;
}

/// Resolve the call made by the trap at `trapIndex`: scan backwards for the
/// nearest word push (the function number), then keep collecting the argument
/// pushes above it. On a register push or an undocumented number, `call`
/// stays nullptr while `trapContext` is still set — the panel then shows the
/// generic trap entry rather than nothing.
OsCallMatch resolveTrap(const QStringList &lines, int trapIndex, int trap)
{
    OsCallMatch match;
    match.trapContext = true;

    int fnIndex = -1;
    QString fnOperand;
    for (int i = trapIndex - 1, scanned = 0; i >= 0 && scanned < kScanBound; --i) {
        const QString raw = lines.at(i);
        if (codePart(raw).isEmpty())
            continue; // blanks and comments ride inside a sequence, uncounted
        ++scanned;
        const QString code = instructionPart(raw);
        if (code.isEmpty())
            break; // a line holding only a label: the sequence is over
        if (osTrap(code))
            break; // a nested trap: the sequence is over
        if (isWordPush(code, &fnOperand)) {
            fnIndex = i;
            break;
        }
        break; // any other instruction ends the push sequence
    }
    if (fnIndex < 0)
        return match;

    // The immediate marker is display, not value: "#9" resolves as 9,
    // "#Cconws" as Cconws.
    QString fnValue = fnOperand;
    if (fnValue.startsWith(QLatin1Char('#')))
        fnValue.remove(0, 1);

    bool numeric = false;
    const int number = parseNumber(fnValue, &numeric);
    if (numeric)
        match.call = osCallRef(trap, number);
    else
        match.call = osCallRefByName(trap, fnValue);

    // Arguments: the pushes directly above the function word, nearest first,
    // reversed into push (source) order at the end. A reserved zero word
    // (Mshrink, Frename) is collected like any other argument.
    QStringList reversed;
    for (int i = fnIndex - 1, scanned = 0; i >= 0 && scanned < kScanBound; --i) {
        const QString raw = lines.at(i);
        if (codePart(raw).isEmpty())
            continue; // blanks and comments do not count against the bound
        ++scanned;
        const QString code = instructionPart(raw);
        if (code.isEmpty())
            break; // a line holding only a label
        QString operand;
        if (isLongPush(code, &operand) || isWordPush(code, &operand))
            reversed.append(operand);
        else
            break;
    }
    for (int i = reversed.size() - 1; i >= 0; --i)
        match.args.append(reversed.at(i));
    return match;
}

} // namespace

OsCallMatch osCallAt(const QStringList &lines, int lineIndex)
{
    OsCallMatch none;
    if (lineIndex < 0 || lineIndex >= lines.size())
        return none;

    const QString code = instructionPart(lines.at(lineIndex));
    if (code.isEmpty())
        return none;

    if (const int trap = osTrap(code))
        return resolveTrap(lines, lineIndex, trap);

    // A push line: the call is named by the trap below it.
    QString ignored;
    if (!isWordPush(code, &ignored) && !isLongPush(code, &ignored))
        return none;
    for (int i = lineIndex + 1, scanned = 0; i < lines.size() && scanned < kScanBound; ++i) {
        const QString raw = lines.at(i);
        if (codePart(raw).isEmpty())
            continue; // blanks and comments do not count against the bound
        ++scanned;
        const QString below = instructionPart(raw);
        if (below.isEmpty())
            break; // a line holding only a label
        if (const int trap = osTrap(below))
            return resolveTrap(lines, i, trap);
        // The argument pushes sit between this line and the trap; anything
        // else means the push is not an OS-call push.
        QString pushOperand;
        if (!isWordPush(below, &pushOperand) && !isLongPush(below, &pushOperand))
            break;
    }
    return none;
}

} // namespace pist
