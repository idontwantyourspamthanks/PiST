// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

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
    QString filePath() const { return m_filePath; }
    bool isModifiedSinceLoad() const;

    /// Mark a line as the current execution point (1-based).
    void setCurrentExecutionLine(int line);
    void clearCurrentExecutionLine();

    /// Mark a line with a build-error gutter marker (1-based).
    void setErrorLines(const QList<int> &lines);

    void gotoLine(int line);

    int lineNumberAreaWidth() const;
    void lineNumberAreaPaintEvent(QPaintEvent *event);

signals:
    void cursorLineChanged(int line);

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
};

} // namespace pist
