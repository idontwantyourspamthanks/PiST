// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/CodeEditor.h"

#include "editor/AsmHighlighter.h"

#include <QContextMenuEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QTextBlock>
#include <QTextStream>

namespace pist {

namespace {

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor)
        , m_editor(editor)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->lineNumberAreaPaintEvent(event); }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int line = m_editor->lineAtY(event->position().y());
        if (line > 0)
            emit m_editor->gutterClicked(line, event->button());
        QWidget::mousePressEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const int line = m_editor->lineAtY(event->pos().y());
        if (line > 0)
            emit m_editor->gutterContextMenuRequested(line, event->globalPos());
    }

private:
    CodeEditor *m_editor;
};

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(font.pointSize() + 1);
    setFont(font);
    setTabStopDistance(8 * QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));
    setLineWrapMode(QPlainTextEdit::NoWrap);

    m_highlighter = new AsmHighlighter(document());

    m_lineNumberArea = new LineNumberArea(this);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::onCursorPositionChanged);

    connect(document(), &QTextDocument::modificationChanged, this,
            &CodeEditor::modificationChanged);

    updateLineNumberAreaWidth(0);
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    return 12 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(
        QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void CodeEditor::onCursorPositionChanged()
{
    refreshExtraSelections();
}

void CodeEditor::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> selections;

    // Current execution line, as reported by the debugger.
    if (m_currentExecutionLine > 0 && m_currentExecutionLine <= blockCount()) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(QColor(0xff, 0xf3, 0xc4));
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(document()->findBlockByNumber(m_currentExecutionLine - 1));
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // Build errors.
    for (int line : m_errorLines) {
        if (line <= 0 || line > blockCount())
            continue;
        QTextEdit::ExtraSelection sel;
        sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        sel.format.setUnderlineColor(QColor(0xd0, 0x30, 0x30));
        sel.format.setProperty(QTextFormat::FullWidthSelection, false);
        QTextCursor cursor(document()->findBlockByNumber(line - 1));
        cursor.select(QTextCursor::LineUnderCursor);
        sel.cursor = cursor;
        selections.append(sel);
    }

    setExtraSelections(selections);
}

void CodeEditor::setCurrentExecutionLine(int line)
{
    m_currentExecutionLine = line;
    refreshExtraSelections();
}

void CodeEditor::clearCurrentExecutionLine()
{
    m_currentExecutionLine = 0;
    refreshExtraSelections();
}

void CodeEditor::setErrorLines(const QList<int> &lines)
{
    m_errorLines = lines;
    refreshExtraSelections();
}

void CodeEditor::setBreakpointLines(const QList<int> &lines)
{
    m_breakpointLines = lines;
    if (m_lineNumberArea)
        m_lineNumberArea->update();
}

// Maps a y offset inside the gutter to a block number, so a click lands on the
// line the user actually aimed at rather than the nearest text position.
int CodeEditor::lineAtY(int y) const
{
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int blockNumber = block.blockNumber();

    while (block.isValid()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (y >= top && y < top + height)
            return blockNumber + 1;
        top += height;
        block = block.next();
        ++blockNumber;
    }
    return 0;
}

void CodeEditor::gotoLine(int line)
{
    if (line <= 0 || line > blockCount())
        return;
    QTextCursor cursor(document()->findBlockByNumber(line - 1));
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

void CodeEditor::lineNumberAreaPaintEvent(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), QColor(0xf0, 0xf0, 0xf0));

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int line = blockNumber + 1;

            if (m_breakpointLines.contains(line)) {
                // A filled dot, drawn rather than glyph-based so it does not
                // depend on a font that happens to have the character.
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setBrush(QColor(0xc0, 0x30, 0x30));
                painter.setPen(Qt::NoPen);
                const int d = fontMetrics().height() - 6;
                painter.drawEllipse(QRect(2, top + 3, d, d));
                painter.setRenderHint(QPainter::Antialiasing, false);
            }

            if (m_errorLines.contains(line)) {
                painter.setPen(QColor(0xd0, 0x30, 0x30));
                painter.drawText(0, top, m_lineNumberArea->width() - 6,
                                 fontMetrics().height(), Qt::AlignRight,
                                 QStringLiteral("!"));
            }

            painter.setPen(line == m_currentExecutionLine ? QColor(0x00, 0x60, 0x60)
                                                          : QColor(0x80, 0x80, 0x80));
            painter.drawText(0, top, m_lineNumberArea->width() - 6,
                             fontMetrics().height(), Qt::AlignRight, QString::number(line));
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

bool CodeEditor::loadFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    setPlainText(stream.readAll());
    m_filePath = path;
    document()->setModified(false);
    return true;
}

bool CodeEditor::saveFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    stream << toPlainText();
    stream.flush();
    // Without this the document is marked clean even when the write was
    // truncated, which loses the only copy of the user's work.
    if (stream.status() != QTextStream::Ok || file.error() != QFileDevice::NoError) {
        m_lastError = file.errorString();
        return false;
    }

    m_filePath = path;
    document()->setModified(false);
    return true;
}

bool CodeEditor::isModifiedSinceLoad() const
{
    return document()->isModified();
}

QString CodeEditor::displayName() const
{
    return m_filePath.isEmpty() ? QStringLiteral("untitled") : QFileInfo(m_filePath).fileName();
}

} // namespace pist
