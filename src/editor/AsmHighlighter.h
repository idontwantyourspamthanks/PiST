// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVector>

namespace pist {

/// Syntax highlighter for vasm's Motorola (mot) syntax.
///
/// Motorola syntax has no comment prefix shared with other assemblers: `;` is a
/// comment anywhere, and `*` only in the first column.
class AsmHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT

public:
    explicit AsmHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    struct Rule
    {
        QRegularExpression pattern;
        QTextCharFormat format;
    };

    void buildRules();

    QVector<Rule> m_rules;
    QTextCharFormat m_labelFormat;
    QTextCharFormat m_commentFormat;
    QTextCharFormat m_directiveFormat;
    QTextCharFormat m_preprocessorFormat;

    QRegularExpression m_labelPattern;
    QRegularExpression m_wholeLineComment;
};

} // namespace pist
