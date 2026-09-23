// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/AsmHighlighter.h"

#include "editor/AsmLex.h"
#include "editor/InstrRef.h"

#include <QStringList>

namespace pist {

namespace {

/// Mnemonics vasm's mot module accepts that the 68000 reference does not list:
/// the 68020+ system instructions and the FPU. The reference is a 68000
/// reference on purpose (InstrRef says why), so the highlighter unions the two
/// instead of keeping a second hand-written copy of the 68000 set — that copy
/// had already drifted by nineteen entries, and the `blo.s` in demo/hello.s
/// went uncoloured.
const char *const kExtraMnemonics[] = {
    // 68020+.
    "bkpt", "rtd", "callm", "rtm", "extb",
    // FPU.
    "fmove", "fadd", "fsub", "fmul", "fdiv", "fsqrt", "fabs", "fneg", "fint",
    "fintrz", "fcmp", "ftst", "fsin", "fcos", "ftan", "fatan", "fgetexp",
    "fgetman", "fscale", "fmod", "frem", "fsgldiv", "fsglmul", "fmovecr",
    "fsave", "frestore", "faddd", "fsubd", "fmuld", "fdivd",
    nullptr
};

/// The mnemonic family as one alternation of whole words: `\b(?:move|…)\b`.
/// Built once from the reference — so adding a mnemonic to InstrRef is enough —
/// and compiled once per theme and scanned once per line, where a pattern per
/// mnemonic cost one compile each per theme and one scan each per line. `\b` at
/// both ends keeps a size suffix (`move.w`, `blo.s`) inside the match and stops
/// a shorter alternative (`add`) matching inside a longer mnemonic (`adda`).
const QString &mnemonicPattern()
{
    static const QString pattern = [] {
        QStringList words;
        const QList<InstructionInfo> &table = instructionTable();
        words.reserve(table.size() + 40); // the extras below
        for (const InstructionInfo &info : table)
            words.append(info.mnemonic.toLower());
        for (int i = 0; kExtraMnemonics[i]; ++i)
            words.append(QString::fromLatin1(kExtraMnemonics[i]));
        return QStringLiteral("\\b(?:%1)\\b").arg(words.join(QLatin1Char('|')));
    }();
    return pattern;
}

} // namespace

AsmHighlighter::AsmHighlighter(QTextDocument *document, const EditorTheme &theme)
    : QSyntaxHighlighter(document)
    , m_theme(theme)
{
    buildRules();
}

void AsmHighlighter::setTheme(const EditorTheme &theme)
{
    if (m_theme == theme)
        return;
    m_theme = theme;
    m_rules.clear();
    buildRules();
    rehighlight();
}

void AsmHighlighter::buildRules()
{
    const QColor keywordColor = m_theme.keyword;
    const QColor registerColor = m_theme.registerName;
    const QColor numberColor = m_theme.number;
    const QColor stringColor = m_theme.string;
    const QColor directiveColor = m_theme.directive;
    const QColor labelColor = m_theme.label;
    const QColor commentColor = m_theme.comment;

    QTextCharFormat keyword;
    keyword.setForeground(keywordColor);
    keyword.setFontWeight(QFont::Bold);

    QTextCharFormat registerFmt;
    registerFmt.setForeground(registerColor);

    QTextCharFormat number;
    number.setForeground(numberColor);

    QTextCharFormat string;
    string.setForeground(stringColor);

    m_directiveFormat.setForeground(directiveColor);

    QTextCharFormat label;
    label.setForeground(labelColor);
    m_labelFormat = label;

    QTextCharFormat comment;
    comment.setForeground(commentColor);
    comment.setFontItalic(true);
    m_commentFormat = comment;

    // Mnemonics are matched as whole words, case-insensitively, anywhere in the
    // line's code field — one rule for the whole family. The string rule is
    // applied after it, so a quoted string's contents are repainted over any
    // mnemonic, register or number colour a rule finds inside the quotes.
    {
        Rule r;
        r.pattern = QRegularExpression(mnemonicPattern(),
                                       QRegularExpression::CaseInsensitiveOption);
        r.format = keyword;
        m_rules.append(r);
    }

    // Registers: d0-d7, a0-a7, sp, usp, isp, sr, ccr, pc, plus the mnemonic-ish
    // names for the stack pointer.
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral("\\b([daDA][0-7]|sp|usp|isp|sr|ccr|pc)\\b"),
                                       QRegularExpression::CaseInsensitiveOption);
        r.format = registerFmt;
        m_rules.append(r);
    }

    // Numbers: $hex, %binary, 0x hex, @ octal, decimal.
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral("(\\$[0-9A-Fa-f]+|%[01]+|0x[0-9A-Fa-f]+|@[0-7]+|\\b\\d+\\b)"));
        r.format = number;
        m_rules.append(r);
    }

    // Strings, in both of Motorola syntax's quote styles: `'…'` is a string
    // exactly as `"…"` is (vasm's mot module treats them alike), and every
    // string in the shipped demos is single-quoted.
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral(
            "\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\"" // "…"
            "|"
            "'[^'\\\\]*(\\\\.[^'\\\\]*)*'")); // '…'
        r.format = string;
        m_rules.append(r);
    }

    // Directives and assembler controls (dc.b, ds.l, equ, section, end, opt...).
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral(
            "^\\s*(dc\\.[bwls]|ds\\.[bwls]|dcb\\.[bwls]|equ|set|even|odd|cnop|align|"
            "section|text|data|bss|end|include|incbin|incdir|opt|macro|endm|repeat|"
            "endr|if|else|endif|ifd|ifnd|org|rorg|rsreset|rs|global|xdef|xref|clrfo|"
            "clrso|far|near|import|export)\\b"),
            QRegularExpression::CaseInsensitiveOption);
        r.format = m_directiveFormat;
        m_rules.append(r);
    }
}

void AsmHighlighter::highlightBlock(const QString &text)
{
    // AsmLex decides where the code field ends: a `;` starts a comment unless
    // it is inside a quoted string, and `*` in the first column makes the whole
    // line a comment. A whole-line comment has no code at all, so nothing else
    // is painted and the comment colour covers the line below.
    const int comment = asmlex::commentStart(text);
    const int codeLength = (comment < 0) ? text.length() : comment;

    for (const Rule &rule : m_rules) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            if (match.capturedStart() >= codeLength)
                continue;
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    // A label may be indented: vasm reads the first field as a label wherever
    // it starts, which is how the demos indent their local labels.
    int labelStart = 0;
    int labelLength = 0;
    if (asmlex::isLabelDefinition(text, nullptr, &labelStart, &labelLength)
        && labelStart < codeLength)
        setFormat(labelStart, labelLength, m_labelFormat);

    if (comment >= 0)
        setFormat(comment, text.length() - comment, m_commentFormat);
}

} // namespace pist
