// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QList>
#include <QPlainTextEdit>

class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;

namespace pist {

class AsmHighlighter;

/// A plain-text assembly editor with line numbers, current-line highlighting,
/// an error gutter marker, and a find/replace bar over its bottom edge.
class CodeEditor : public QPlainTextEdit
{
    Q_OBJECT

public:
    explicit CodeEditor(QWidget *parent = nullptr);

    bool loadFile(const QString &path);
    bool saveFile(const QString &path);

    /// Reason the last save failed, for the caller to report.
    QString lastError() const { return m_lastError; }
    QString filePath() const { return m_filePath; }

    /// Re-point the editor at a different path without touching the document
    /// or its modified state: the file was renamed on disk underneath us.
    void setFilePath(const QString &path) { m_filePath = path; }

    /// (Re)apply the application font-size preference and theme-driven syntax
    /// colours. Called by the constructor and whenever the preferences change.
    void applyFontPreferences();
    bool isModifiedSinceLoad() const;

    /// File name only, for the window title.
    QString displayName() const;

    /// Mark a line as the current execution point (1-based).
    void setCurrentExecutionLine(int line);
    void clearCurrentExecutionLine();

    /// The highlighted execution line, or 0 when none. Exposed so the debug loop
    /// can be asserted without reaching into the widget's internals: the value is
    /// what the user sees highlighted.
    int currentExecutionLine() const { return m_currentExecutionLine; }

    /// Mark a line with a build-error gutter marker (1-based).
    void setErrorLines(const QList<int> &lines);

    /// Tint the line-number gutter per line, proportional to
    /// log(count)/log(max) of `counts`, so a profiler's hottest lines stand out
    /// at a glance. The scale is logarithmic because execution counts are: a
    /// linear scale would show one hot instruction and a flat field of black.
    ///
    /// An empty map clears the heat; the gutter otherwise renders exactly as it
    /// did before. This is decoration over the existing markers, never a
    /// replacement for them.
    void setLineHeat(const QHash<int, quint64> &counts);
    /// Whether a heat map is currently set, for tests and for the caller that
    /// wants to know whether the gutter needs clearing.
    bool hasLineHeat() const { return !m_lineHeat.isEmpty(); }

    /// Lines that currently hold a breakpoint (1-based).
    void setBreakpointLines(const QList<int> &lines);
    QList<int> breakpointLines() const { return m_breakpointLines; }

    /// Line the gutter is hovering, for click targeting.
    int lineAtY(int y) const;

    void gotoLine(int line);

    /// Whether the find bar is showing, and how many hits the current search
    /// has. Exposed so a test can assert what the user sees without reaching
    /// into the bar's widgets.
    bool findBarVisible() const;
    bool gotoBarVisible() const;
    int findMatchCount() const { return m_matches.size(); }
    /// The hit the selection is currently on, 0-based, or -1.
    int findMatchIndex() const { return m_matchIndex; }

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

signals:
    /// The document's modified state changed, so the window title and any
    /// save-on-close prompt must be updated.
    void modificationChanged(bool modified);

    /// The gutter was clicked on `line` (1-based). The receiver decides whether
    /// this toggles a breakpoint or opens an editor for its condition.
    void gutterClicked(int line, Qt::MouseButton button);
    void gutterContextMenuRequested(int line, const QPoint &globalPos);

public slots:
    /// Show the find bar over the bottom of the editor — the replace row too
    /// when `withReplace` — seeded from the selection, and search as the text
    /// is typed. No-op-free: calling it again just refocuses the bar.
    void showFindBar(bool withReplace = false);
    /// Hide the bar, drop the highlights and put the caret back in the text.
    void hideFindBar();
    /// One-line "go to line" bar, the same shape as find. Enter jumps, Escape
    /// dismisses. Not a modal.
    void showGotoBar();
    void hideGotoBar();
    /// Move to the next/previous hit, wrapping around the document.
    void findNext();
    void findPrevious();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void onCursorPositionChanged();
    void findTextChanged();
    void findOptionsChanged();
    void replaceOne();
    void replaceAll();

private:
    void refreshExtraSelections();
    void buildFindBar();
    void buildGotoBar();
    void updateViewportMargins();
    void layoutFindBar();
    /// Recompute the hits and, when `selectHit` is set, put the selection on
    /// the first one at or after `anchor` (wrapping to the start).
    void refreshMatches(int anchor, bool selectHit);
    void selectMatch(int index);
    /// The hit the selection is on when it is one of ours, else -1.
    int currentMatchFromSelection() const;
    int matchIndexAtOrAfter(int position) const;
    bool isWholeWord(const QTextCursor &hit) const;
    QString findNeedle() const;

    QString m_filePath;
    AsmHighlighter *m_highlighter = nullptr;
    QWidget *m_lineNumberArea = nullptr;
    int m_currentExecutionLine = 0;
    QList<int> m_errorLines;
    QList<int> m_breakpointLines;
    /// Per-line execution counts from a profiling run, for the gutter heat.
    QHash<int, quint64> m_lineHeat;
    /// log(largest count), the divisor that maps a line's count onto 0..1 in the
    /// gutter. Precomputed because the paint runs per visible line; zero when
    /// there is no heat, or when every line has the same count (nothing to
    /// scale against, so every heated line is drawn at full strength).
    double m_lineHeatScale = 0.0;
    QString m_lastError;

    QFrame *m_findBar = nullptr;
    QWidget *m_replaceRow = nullptr;
    QLineEdit *m_findEdit = nullptr;
    QLineEdit *m_replaceEdit = nullptr;
    QToolButton *m_findCase = nullptr;
    QToolButton *m_findWord = nullptr;
    QToolButton *m_next = nullptr;
    QToolButton *m_previous = nullptr;
    QLabel *m_findStatus = nullptr;
    QList<QTextCursor> m_matches;
    int m_matchIndex = -1;
    /// Where the search began, so as-you-type searching holds its place.
    int m_findAnchor = 0;
    int m_findBarHeight = 0;
    QFrame *m_gotoBar = nullptr;
    QLineEdit *m_gotoEdit = nullptr;
    int m_gotoBarHeight = 0;
    QString m_replaceNote;
};

} // namespace pist
