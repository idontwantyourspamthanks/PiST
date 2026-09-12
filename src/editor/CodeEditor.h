// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QPlainTextEdit>

namespace pist {

class AsmHighlighter;

/// A plain-text assembly editor with line numbers, current-line highlighting
/// and an error gutter marker.
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

    /// Lines that currently hold a breakpoint (1-based).
    void setBreakpointLines(const QList<int> &lines);
    QList<int> breakpointLines() const { return m_breakpointLines; }

    /// Line the gutter is hovering, for click targeting.
    int lineAtY(int y) const;

    void gotoLine(int line);

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

protected:
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void onCursorPositionChanged();

private:
    void refreshExtraSelections();

    QString m_filePath;
    AsmHighlighter *m_highlighter = nullptr;
    QWidget *m_lineNumberArea = nullptr;
    int m_currentExecutionLine = 0;
    QList<int> m_errorLines;
    QList<int> m_breakpointLines;
    QString m_lastError;
};

} // namespace pist
