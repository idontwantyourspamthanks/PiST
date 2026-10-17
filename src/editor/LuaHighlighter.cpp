// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/LuaHighlighter.h"

#include <QString>

namespace pist {

namespace {

/// No long comment or long string is open: the block state's resting value
/// (`QTextBlock`'s own "unset" is -1, and QSyntaxHighlighter reads that as it).
constexpr int kNoState = -1;
/// Set in the block state for an open long *comment*, as opposed to a long
/// string, so the carried construct paints in the right colour.
constexpr int kCommentFlag = 0x1000;
/// The number of '=' signs of the open long bracket, in the state's low bits, so
/// `--[==[` is only closed by `]==]` and not by a `]]` inside it.
constexpr int kLevelMask = 0x0fff;

/// The number of '=' signs between the brackets of the long bracket opening at
/// `pos`, or -1 when `pos` does not start one. `[[` gives 0, `[=[` gives 1, and
/// a plain `[` not followed by `[` or `=[` gives -1 (Lua's longest-match rule
/// makes the '='s part of the delimiter, so `[[` does not open at `[=`).
int longBracketLevelAt(const QString &text, int pos)
{
    if (pos < 0 || pos >= text.length() || text.at(pos) != QLatin1Char('['))
        return -1;
    int level = 0;
    int i = pos + 1;
    while (i < text.length() && text.at(i) == QLatin1Char('=')) {
        ++level;
        ++i;
    }
    return (i < text.length() && text.at(i) == QLatin1Char('[')) ? level : -1;
}

/// The closing bracket that ends a long bracket of `level`: `]]` for 0, `]=]`
/// for 1, and so on.
QString longBracketCloser(int level)
{
    QString closer;
    closer.reserve(level + 2);
    closer += QLatin1Char(']');
    closer += QString(level, QLatin1Char('='));
    closer += QLatin1Char(']');
    return closer;
}

} // namespace

LuaHighlighter::LuaHighlighter(QTextDocument *document, const EditorTheme &theme)
    : QSyntaxHighlighter(document)
    , m_theme(theme)
{
    buildRules();
}

void LuaHighlighter::setTheme(const EditorTheme &theme)
{
    if (m_theme == theme)
        return;
    m_theme = theme;
    m_rules.clear();
    buildRules();
    rehighlight();
}

void LuaHighlighter::buildRules()
{
    QTextCharFormat keyword;
    keyword.setForeground(m_theme.keyword);
    keyword.setFontWeight(QFont::Bold);

    // `self` is not one of Lua's reserved words — it is the implicit name of a
    // method's receiver — but it cannot be reassigned, so it is painted as one.
    const QTextCharFormat self = keyword;

    // The names the hook API binds into a script's environment. There is no
    // dedicated function colour in the theme; these borrow the assembler
    // directives' "provided by the environment" colour rather than inventing
    // one here. A theme without that colour would simply paint them as code.
    QTextCharFormat hook;
    hook.setForeground(m_theme.directive);

    QTextCharFormat number;
    number.setForeground(m_theme.number);

    m_stringFormat.setForeground(m_theme.string);

    m_commentFormat.setForeground(m_theme.comment);
    m_commentFormat.setFontItalic(true);

    // Keywords: whole words, case-sensitive (Lua spells them in lower case
    // only, so unlike the mnemonic rule there is no case-insensitive option).
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral(
            "\\b(?:and|break|do|else|elseif|end|false|for|function|goto|if|in|local|"
            "nil|not|or|repeat|return|then|true|until|while)\\b"));
        r.format = keyword;
        m_rules.append(r);
    }

    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral("\\bself\\b"));
        r.format = self;
        m_rules.append(r);
    }

    // The hook API. Whole words, so an identifier that merely contains a name
    // (`logged`, `printing`) is not a call.
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral(
            "\\b(?:readmem|writemem|readreg|writereg|log|print|continue|resume)\\b"));
        r.format = hook;
        m_rules.append(r);
    }

    // Numbers: hex (with the optional hex-float exponent), decimal integers and
    // floats with an optional exponent, and a leading-dot fraction. The `\b`
    // sits inside the alternatives that need it: `a1` stays one identifier, and
    // `x = .5` still paints the fraction, where a leading `\b` before the dot
    // would find no boundary at all.
    {
        Rule r;
        r.pattern = QRegularExpression(QStringLiteral(
            "(?:\\b0[xX][0-9A-Fa-f]+(?:\\.[0-9A-Fa-f]*)?(?:[pP][+-]?\\d+)?"
            "|\\b\\d+(?:\\.\\d*)?(?:[eE][+-]?\\d+)?"
            "|\\.\\d+(?:[eE][+-]?\\d+)?)"));
        r.format = number;
        m_rules.append(r);
    }
}

void LuaHighlighter::highlightBlock(const QString &text)
{
    // The single-block rules paint first; the walk below repaints over them, so
    // a keyword or number inside a comment or a string never shows through. The
    // reverse order would need every rule to know where the comments and
    // strings are, which is exactly what the walk is for.
    for (const Rule &rule : m_rules) {
        auto it = rule.pattern.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch match = it.next();
            setFormat(match.capturedStart(), match.capturedLength(), rule.format);
        }
    }

    // A long comment or long string opened on an earlier line is still open at
    // the top of this one; `level` is the '=' count its closer must match.
    const int previous = previousBlockState();
    bool inComment = previous != kNoState && (previous & kCommentFlag);
    bool inString = previous != kNoState && !(previous & kCommentFlag);
    int level = (previous == kNoState) ? 0 : (previous & kLevelMask);

    setCurrentBlockState(kNoState);

    const int length = text.length();
    int i = 0;
    // Where the open construct began on this line, so its opening bracket is
    // painted with its body. For a resumed construct that is the line start.
    int modeStart = 0;

    while (i < length) {
        if (inComment || inString) {
            const QString closer = longBracketCloser(level);
            const QTextCharFormat &format = inComment ? m_commentFormat : m_stringFormat;
            const int at = text.indexOf(closer, i);
            if (at < 0) {
                setFormat(modeStart, length - modeStart, format);
                break; // the construct is still open at the end of the block
            }
            const int end = at + closer.length();
            setFormat(modeStart, end - modeStart, format);
            i = end;
            inComment = inString = false;
            continue;
        }

        const QChar ch = text.at(i);

        // `--`: a long bracket right after it opens a block comment, anything
        // else runs to the end of the line.
        if (ch == QLatin1Char('-') && i + 1 < length && text.at(i + 1) == QLatin1Char('-')) {
            const int bracket = longBracketLevelAt(text, i + 2);
            if (bracket >= 0) {
                modeStart = i;
                level = bracket;
                inComment = true;
                i += 2; // at the opening `[`; the branch above paints from modeStart
                continue;
            }
            setFormat(i, length - i, m_commentFormat);
            break; // a line comment owns the rest of the block
        }

        // `[[` (or `[=[` …) opens a long string.
        if (ch == QLatin1Char('[')) {
            const int bracket = longBracketLevelAt(text, i);
            if (bracket >= 0) {
                modeStart = i;
                level = bracket;
                inString = true;
                continue;
            }
            ++i;
            continue;
        }

        // A short string: the same quote ends it and a backslash escapes the
        // next character. An unterminated one is a Lua syntax error, so it stops
        // at the end of the line rather than leaking into the next block.
        if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
            const QChar quote = ch;
            int j = i + 1;
            while (j < length) {
                const QChar c = text.at(j);
                if (c == QLatin1Char('\\')) {
                    j += 2;
                    continue;
                }
                ++j;
                if (c == quote)
                    break;
            }
            setFormat(i, qMin(j, length) - i, m_stringFormat);
            i = j;
            continue;
        }

        ++i;
    }

    // Carry an unterminated long comment or long string into the next block.
    if (inComment || inString)
        setCurrentBlockState((inComment ? kCommentFlag : 0) | level);
}

} // namespace pist
