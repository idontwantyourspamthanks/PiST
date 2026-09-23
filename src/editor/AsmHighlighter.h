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

/// Syntax highlighter for vasm's Motorola (mot) syntax.
///
/// Motorola syntax has no comment prefix shared with other assemblers: `;` is a
/// comment anywhere (but not inside a string), and `*` only in the first
/// column. AsmLex owns those rules, so what is painted here is what the OS-call
/// scanner and navigation read.
///
/// The mnemonic family is the 68000 reference's set (InstrRef) plus the
/// 68020/FPU forms the reference omits, matched by one compiled alternation
/// rather than a pattern per mnemonic.
class AsmHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    /// `theme` supplies the syntax colours. It is a parameter, not a lookup, so
    /// the highlighter cannot be built without knowing what it paints with:
    /// the UI owns the appearance, the editor is handed the result.
    AsmHighlighter(QTextDocument *document, const EditorTheme &theme);

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
    QTextCharFormat m_labelFormat;
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_directiveFormat;
};

} // namespace pist
