// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QColor>
#include <QFont>

namespace pist {

/// What the editor paints with: the monospace font, the effective darkness,
/// and exactly the colours `CodeEditor` and `AsmHighlighter` read.
///
/// The editor is themed from outside rather than looking the appearance up
/// itself: the UI composes one of these from the application appearance
/// (`appearance::editorTheme()`) and hands it to `CodeEditor`, which passes the
/// colours on to its highlighter. That is what keeps `src/editor/` free of any
/// `src/ui/` include — the module edge runs one way, ui → editor (MIN-86) — with
/// the same values reaching the same widgets as before, just as a parameter.
///
/// The theme is required at construction, not defaulted: a `CodeEditor` built
/// with no theme would silently paint a colour set nobody chose.
struct EditorTheme
{
    QFont font;

    /// The theme's effective darkness ("system" already resolved). Carried
    /// because the editor's colours are a function of it.
    bool dark = false;

    /// Syntax colours — the rules `AsmHighlighter` compiles.
    QColor keyword;
    QColor registerName;
    QColor number;
    QColor string;
    QColor directive;
    QColor label;
    QColor comment;

    /// Editor chrome — the extra selections and the line-number gutter.
    QColor currentLine;
    QColor executionLine;
    QColor searchMatch;
    QColor error;
    QColor gutter;
    QColor gutterText;
    QColor gutterPc;
    QColor breakpoint;
    QColor muted;
    QColor warning;
};

/// Value equality, so re-applying an unchanged theme is a no-op instead of a
/// re-highlight of the whole document.
inline bool operator==(const EditorTheme &a, const EditorTheme &b)
{
    return a.font == b.font && a.dark == b.dark && a.keyword == b.keyword
        && a.registerName == b.registerName && a.number == b.number && a.string == b.string
        && a.directive == b.directive && a.label == b.label && a.comment == b.comment
        && a.currentLine == b.currentLine && a.executionLine == b.executionLine
        && a.searchMatch == b.searchMatch && a.error == b.error && a.gutter == b.gutter
        && a.gutterText == b.gutterText && a.gutterPc == b.gutterPc
        && a.breakpoint == b.breakpoint && a.muted == b.muted && a.warning == b.warning;
}

inline bool operator!=(const EditorTheme &a, const EditorTheme &b)
{
    return !(a == b);
}

} // namespace pist
