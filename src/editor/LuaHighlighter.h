// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "editor/EditorTheme.h"

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace pist {

/// Syntax highlighter for the Lua a stop hook is written in.
///
/// A stop hook is an ordinary `.lua` file opened in the same editor tab as a
/// source file, so it is the one document that is not 68000 assembly and needs
/// the language it is actually written in. What is painted is Lua 5.4's lexical
/// surface: line and block comments, short and long strings, numbers, the
/// keywords, `self`, and the names the hook API binds (`readmem`, `writemem`,
/// …) so a script's privileged calls stand out from its own code.
///
/// Unlike AsmHighlighter — whose `;` comment ends at the newline — a long
/// comment (`--[[ … ]]`) and a long string (`[[ … ]]`) may run past the end of
/// the line, so this one carries the open construct across blocks in the block
/// state and only the matching closer ends it.
class LuaHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    /// `theme` supplies the syntax colours. It is a parameter, not a lookup, for
    /// the same reason AsmHighlighter's is: the UI owns the appearance (MIN-86
    /// keeps `src/editor/` out of `src/ui/`), so the highlighter cannot be built
    /// without knowing what it paints with.
    LuaHighlighter(QTextDocument *document, const EditorTheme &theme);

    /// Adopt a different theme: rebuild the rules and re-highlight. A theme
    /// equal to the current one is ignored, so a refresh that changes nothing
    /// does not repaint the document.
    void setTheme(const EditorTheme &theme);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    void buildRules();

    EditorTheme m_theme;

    QVector<Rule> m_rules;
    /// The two formats the block walk applies itself, because a construct that
    /// spans lines cannot be found by a single-block pattern match.
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_stringFormat;
};

} // namespace pist
