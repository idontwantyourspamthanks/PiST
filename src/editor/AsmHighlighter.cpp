// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/AsmHighlighter.h"

#include <QLatin1StringView>

namespace pist {

namespace {

/// Every m68k mnemonic vasm's mot module accepts, plus the common aliases.
/// Kept as a plain list so the highlighter stays data-driven and cheap to extend.
const char *const kMnemonics[] = {
    // Data movement
    "move", "movea", "movem", "movep", "moveq", "lea", "pea", "link", "unlk", "exg",
    // Integer arithmetic
    "add", "adda", "addi", "addq", "addx", "sub", "suba", "subi", "subq", "subx",
    "cmp", "cmpa", "cmpi", "cmpm", "mulu", "muls", "divu", "divs", "neg", "negx",
    "ext", "extb", "clr", "tst", "abcd", "sbcd", "nbcd",
    // Logic and shifts
    "and", "andi", "or", "ori", "eor", "eori", "not", "lsl", "lsr", "asl", "asr",
    "rol", "ror", "roxl", "roxr", "btst", "bset", "bclr", "bchg", "swap",
    // Program control
    "bra", "bsr", "bcc", "bcs", "beq", "bge", "bgt", "bhi", "ble", "bls", "blt",
    "bmi", "bne", "bpl", "bvc", "bvs", "jmp", "jsr", "rts", "rtr", "rte", "nop",
    "dbra", "dbf", "dbeq", "dbne", "trap", "trapv", "chk", "illegal", "reset",
    "stop", "scc", "seq", "sne", "st", "sf", "sge", "sgt", "shi", "sle", "sls",
    "slt", "smi", "spl", "svc", "svs",
    // Bit / BCD / system
    "tas", "bkpt", "rtd", "callm", "rtm",
    // FPU
    "fmove", "fadd", "fsub", "fmul", "fdiv", "fsqrt", "fabs", "fneg", "fint",
    "fintrz", "fcmp", "ftst", "fsin", "fcos", "ftan", "fatan", "fgetexp",
    "fgetman", "fscale", "fmod", "frem", "fsgldiv", "fsglmul", "fmovecr",
    "fsave", "frestore", "faddd", "fsubd", "fmuld", "fdivd",
    nullptr
};

} // namespace

AsmHighlighter::AsmHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    buildRules();
}

void AsmHighlighter::buildRules()
{
    QTextCharFormat keyword;
    keyword.setForeground(QColor(0x1f, 0x6f, 0xbf));
    keyword.setFontWeight(QFont::Bold);

    QTextCharFormat registerFmt;
    registerFmt.setForeground(QColor(0x8a, 0x3f, 0xbf));

    QTextCharFormat number;
    number.setForeground(QColor(0x0f, 0x7f, 0x5f));

    QTextCharFormat string;
    string.setForeground(QColor(0xa0, 0x50, 0x00));

    m_directiveFormat.setForeground(QColor(0x9a, 0x35, 0x5f));

    QTextCharFormat label;
    label.setForeground(QColor(0x00, 0x6a, 0x6a));
    m_labelFormat = label;

    QTextCharFormat comment;
    comment.setForeground(QColor(0x80, 0x80, 0x80));
    comment.setFontItalic(true);
    m_commentFormat = comment;

    // Mnemonics are matched as whole words, case-insensitively, and only when
    // they appear in an operand/opcode position (start of the code field).
    for (int i = 0; kMnemonics[i]; ++i) {
        Rule r;
        r.pattern = QRegularExpression(
            QStringLiteral("\\b%1\\b").arg(QString::fromLatin1(kMnemonics[i])),
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

    // Strings.
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral("\"[^\"\\\\]*(\\\\.[^\"\\\\]*)*\""));
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

    // Labels: an identifier followed by a colon, or starting in column 0.
    m_labelPattern = QRegularExpression(QStringLiteral("^([A-Za-z_.][A-Za-z0-9_.$]*):"));

    // Motorola syntax: `*` in the first column is a whole-line comment.
    m_wholeLineComment = QRegularExpression(QStringLiteral("^\\*.*"));
}

void AsmHighlighter::highlightBlock(const QString &text)
{
    // Whole-line `*` comment takes precedence over everything else.
    if (m_wholeLineComment.match(text).hasMatch()) {
        setFormat(0, text.length(), m_commentFormat);
        return;
    }

    // `;` starts a comment anywhere on the line.
    const int semicolon = text.indexOf(QLatin1Char(';'));
    const int codeLength = (semicolon >= 0) ? semicolon : text.length();

    for (const Rule &rule : m_rules) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const auto match = it.next();
            if (match.capturedStart() >= codeLength)
                continue;
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    auto labelMatch = m_labelPattern.match(text);
    if (labelMatch.hasMatch() && labelMatch.capturedStart() < codeLength)
        setFormat(labelMatch.capturedStart(1), labelMatch.capturedLength(1), m_labelFormat);

    if (semicolon >= 0)
        setFormat(semicolon, text.length() - semicolon, m_commentFormat);
}

} // namespace pist
