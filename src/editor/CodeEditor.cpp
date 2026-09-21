// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/CodeEditor.h"

#include "editor/AsmHighlighter.h"
#include "ui/Appearance.h"

#include <QContextMenuEvent>
#include <QFile>
#include <QFrame>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QTextBlock>
#include <QTimer>
#include <QTextStream>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

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

bool isWordCharacter(const QChar &character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_');
}

} // namespace

CodeEditor::CodeEditor(QWidget *parent)
    : QPlainTextEdit(parent)
{
    setLineWrapMode(QPlainTextEdit::NoWrap);

    m_highlighter = new AsmHighlighter(document());

    // After the highlighter exists: it also applies the theme's syntax colours.
    applyFontPreferences();

    m_lineNumberArea = new LineNumberArea(this);

    connect(this, &QPlainTextEdit::blockCountChanged, this, &CodeEditor::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeEditor::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::onCursorPositionChanged);

    connect(document(), &QTextDocument::modificationChanged, this,
            &CodeEditor::modificationChanged);

    buildFindBar();
    buildGotoBar();

    updateLineNumberAreaWidth(0);
}

void CodeEditor::buildFindBar()
{
    m_findBar = new QFrame(this);
    m_findBar->setObjectName(QStringLiteral("editorFindBar"));
    m_findBar->setFrameShape(QFrame::StyledPanel);
    m_findBar->hide();

    auto *rows = new QVBoxLayout(m_findBar);
    rows->setContentsMargins(6, 4, 6, 4);
    rows->setSpacing(4);

    auto *findRow = new QHBoxLayout;
    findRow->setSpacing(4);
    m_findEdit = new QLineEdit(m_findBar);
    m_findEdit->setObjectName(QStringLiteral("editorFindText"));
    m_findEdit->setPlaceholderText(tr("Find"));
    m_findEdit->setClearButtonEnabled(true);
    findRow->addWidget(m_findEdit, 1);

    auto addToggle = [this, findRow](QToolButton *&button, const QString &objectName,
                                     const QString &text, const QString &tip) {
        button = new QToolButton(m_findBar);
        button->setObjectName(objectName);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setAutoRaise(true);
        findRow->addWidget(button);
    };
    addToggle(m_findCase, QStringLiteral("editorFindCase"), tr("Aa"),
              tr("Match case"));
    addToggle(m_findWord, QStringLiteral("editorFindWord"), tr("Word"),
              tr("Match whole words only"));

    m_previous = new QToolButton(m_findBar);
    m_previous->setObjectName(QStringLiteral("editorFindPrevious"));
    m_previous->setText(QStringLiteral("\u25b2"));
    m_previous->setToolTip(tr("Previous hit (Shift+Enter)"));
    m_previous->setAutoRaise(true);
    findRow->addWidget(m_previous);

    m_next = new QToolButton(m_findBar);
    m_next->setObjectName(QStringLiteral("editorFindNext"));
    m_next->setText(QStringLiteral("\u25bc"));
    m_next->setToolTip(tr("Next hit (Enter, F3)"));
    m_next->setAutoRaise(true);
    findRow->addWidget(m_next);

    m_findStatus = new QLabel(m_findBar);
    m_findStatus->setObjectName(QStringLiteral("editorFindStatus"));
    m_findStatus->setMinimumWidth(fontMetrics().horizontalAdvance(QStringLiteral("000 of 000")));
    findRow->addWidget(m_findStatus);

    auto *close = new QToolButton(m_findBar);
    close->setObjectName(QStringLiteral("editorFindClose"));
    close->setText(QStringLiteral("\u2715"));
    close->setToolTip(tr("Close (Esc)"));
    close->setAutoRaise(true);
    findRow->addWidget(close);

    rows->addLayout(findRow);

    m_replaceRow = new QWidget(m_findBar);
    auto *replaceRow = new QHBoxLayout(m_replaceRow);
    replaceRow->setContentsMargins(0, 0, 0, 0);
    replaceRow->setSpacing(4);
    m_replaceEdit = new QLineEdit(m_replaceRow);
    m_replaceEdit->setObjectName(QStringLiteral("editorReplaceText"));
    m_replaceEdit->setPlaceholderText(tr("Replace with"));
    replaceRow->addWidget(m_replaceEdit, 1);
    auto *replaceButton = new QPushButton(tr("Replace"), m_replaceRow);
    replaceButton->setObjectName(QStringLiteral("editorReplaceOne"));
    replaceRow->addWidget(replaceButton);
    auto *replaceAllButton = new QPushButton(tr("Replace All"), m_replaceRow);
    replaceAllButton->setObjectName(QStringLiteral("editorReplaceAll"));
    replaceRow->addWidget(replaceAllButton);
    m_replaceRow->hide();
    rows->addWidget(m_replaceRow);

    connect(m_findEdit, &QLineEdit::textChanged, this, &CodeEditor::findTextChanged);
    connect(m_findEdit, &QLineEdit::returnPressed, this, &CodeEditor::findNext);
    connect(m_findCase, &QToolButton::toggled, this, &CodeEditor::findOptionsChanged);
    connect(m_findWord, &QToolButton::toggled, this, &CodeEditor::findOptionsChanged);
    connect(m_next, &QToolButton::clicked, this, &CodeEditor::findNext);
    connect(m_previous, &QToolButton::clicked, this, &CodeEditor::findPrevious);
    connect(close, &QToolButton::clicked, this, &CodeEditor::hideFindBar);
    connect(replaceButton, &QPushButton::clicked, this, &CodeEditor::replaceOne);
    connect(replaceAllButton, &QPushButton::clicked, this, &CodeEditor::replaceAll);
    connect(m_replaceEdit, &QLineEdit::returnPressed, this, &CodeEditor::replaceOne);

    // Escape from inside the bar, and Shift+Enter for the other direction: the
    // line edits would otherwise swallow both.
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_findBar);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &CodeEditor::hideFindBar);
    m_findEdit->installEventFilter(this);
    m_replaceEdit->installEventFilter(this);

    // An edit can add or remove hits, so the highlights have to follow. The
    // selection is left where the user left it.
    connect(document(), &QTextDocument::contentsChanged, this, [this] {
        if (findBarVisible())
            refreshMatches(m_findAnchor, false);
    });
}

void CodeEditor::buildGotoBar()
{
    m_gotoBar = new QFrame(this);
    m_gotoBar->setObjectName(QStringLiteral("editorGotoBar"));
    m_gotoBar->setFrameShape(QFrame::StyledPanel);
    m_gotoBar->hide();

    auto *row = new QHBoxLayout(m_gotoBar);
    row->setContentsMargins(6, 4, 6, 4);
    row->addWidget(new QLabel(tr("Go to line:"), m_gotoBar));
    m_gotoEdit = new QLineEdit(m_gotoBar);
    m_gotoEdit->setObjectName(QStringLiteral("editorGotoLine"));
    m_gotoEdit->setPlaceholderText(tr("Line number"));
    m_gotoEdit->setClearButtonEnabled(true);
    row->addWidget(m_gotoEdit, 1);

    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_gotoBar);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &CodeEditor::hideGotoBar);
    connect(m_gotoEdit, &QLineEdit::returnPressed, this, [this] {
        bool ok = false;
        const int line = m_gotoEdit->text().trimmed().toInt(&ok);
        if (!ok || line <= 0)
            return;
        // After the key has finished. Moving focus inside returnPressed hands
        // the same Return to the editor, which then inserts a line.
        QTimer::singleShot(0, this, [this, line] {
            gotoLine(line);
            if (textCursor().blockNumber() == line - 1)
                hideGotoBar();
        });
    });
    m_gotoEdit->installEventFilter(this);
}

void CodeEditor::applyFontPreferences()
{
    const QFont font = appearance::editorFont();
    setFont(font);
    setTabStopDistance(8 * QFontMetricsF(font).horizontalAdvance(QLatin1Char(' ')));

    m_highlighter->setDarkMode(appearance::darkModeActive());
    refreshExtraSelections();
    if (m_lineNumberArea)
        m_lineNumberArea->update();
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    updateViewportMargins();
}

void CodeEditor::updateViewportMargins()
{
    const int bottom = findBarVisible() ? m_findBarHeight : 0;
    const int top = gotoBarVisible() ? m_gotoBarHeight : 0;
    setViewportMargins(lineNumberAreaWidth(), top, 0, bottom);
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
    layoutFindBar();
}

void CodeEditor::layoutFindBar()
{
    const QRect cr = contentsRect();
    const int left = cr.left() + lineNumberAreaWidth();
    const int width = qMax(0, cr.width() - lineNumberAreaWidth());
    if (m_findBar && findBarVisible()) {
        m_findBar->setGeometry(left, cr.bottom() - m_findBarHeight + 1, width, m_findBarHeight);
    }
    if (m_gotoBar && gotoBarVisible())
        m_gotoBar->setGeometry(left, cr.top(), width, m_gotoBarHeight);
}

void CodeEditor::onCursorPositionChanged()
{
    refreshExtraSelections();
}

void CodeEditor::refreshExtraSelections()
{
    QList<QTextEdit::ExtraSelection> selections;
    const appearance::Colors c = appearance::colors();
    const int cursorLine = textCursor().blockNumber() + 1;

    // Cursor line, underneath the debugger's PC highlight when they coincide.
    if (cursorLine > 0 && cursorLine != m_currentExecutionLine) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(c.currentLine);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // Current execution line, as reported by the debugger.
    if (m_currentExecutionLine > 0 && m_currentExecutionLine <= blockCount()) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(c.executionLine);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(document()->findBlockByNumber(m_currentExecutionLine - 1));
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // Find hits, except the one the selection is on: that one wears the
    // selection colour, and an extra selection would paint over it.
    if (findBarVisible()) {
        for (int i = 0; i < m_matches.size(); ++i) {
            if (i == m_matchIndex)
                continue;
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(c.searchMatch);
            sel.format.setProperty(QTextFormat::FullWidthSelection, false);
            sel.cursor = m_matches.at(i);
            selections.append(sel);
        }
    }

    // Build errors.
    for (int line : m_errorLines) {
        if (line <= 0 || line > blockCount())
            continue;
        QTextEdit::ExtraSelection sel;
        sel.format.setUnderlineStyle(QTextCharFormat::WaveUnderline);
        sel.format.setUnderlineColor(c.error);
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

void CodeEditor::setLineHeat(const QHash<int, quint64> &counts)
{
    m_lineHeat.clear();
    quint64 maximum = 0;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
        // A zero count is not heat: it would tint a cold line at the bottom of
        // the scale for no reason. Profile saves omit unexecuted addresses
        // anyway, so this only guards a caller passing a full line-number map.
        if (it.value() == 0)
            continue;
        m_lineHeat.insert(it.key(), it.value());
        if (it.value() > maximum)
            maximum = it.value();
    }
    // log(count)/log(max): execution counts span orders of magnitude, and only a
    // logarithmic scale lets a merely busy line be visible beside a runaway one.
    // A single distinct count has no scale to speak of, so it paints at full
    // strength rather than all-but-invisibly.
    m_lineHeatScale = maximum > 1 ? std::log(double(maximum)) : 0.0;
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
    const appearance::Colors c = appearance::colors();
    painter.fillRect(event->rect(), c.gutter);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());

    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const int line = blockNumber + 1;
            const int height = fontMetrics().height();

            // Profiler heat, painted first so every existing marker — the PC
            // bar, the breakpoint dot, the error bang, the number itself —
            // stays on top of it and remains as legible as before.
            const auto heat = m_lineHeat.constFind(line);
            if (heat != m_lineHeat.constEnd()) {
                const double scaled = m_lineHeatScale > 0.0
                                          ? std::log(double(*heat)) / m_lineHeatScale
                                          : 1.0;
                QColor tint(0xd8, 0x8a, 0x30); // warm amber, in either theme
                tint.setAlphaF(0.55 * qBound(0.0, scaled, 1.0));
                painter.fillRect(0, top, m_lineNumberArea->width(), bottom - top, tint);
            }

            if (line == m_currentExecutionLine)
                painter.fillRect(0, top, 3, bottom - top, c.gutterPc);

            if (m_breakpointLines.contains(line)) {
                // A filled dot, drawn rather than glyph-based so it does not
                // depend on a font that happens to have the character.
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setBrush(c.breakpoint);
                painter.setPen(Qt::NoPen);
                const int d = qMax(5, height - 6);
                // Geometrically centred in the text box, then dropped a little
                // at small type: a tiny disc sits optically high, which was
                // right at 15pt (top+3) and a few pixels high at 10pt.
                int y = top + (height - d) / 2;
                const int pt = font().pointSize();
                if (pt > 0 && pt < 15)
                    y += (15 - pt + 1) / 2;
                painter.drawEllipse(QRect(2, y, d, d));
                painter.setRenderHint(QPainter::Antialiasing, false);
            }

            if (m_errorLines.contains(line)) {
                painter.setPen(c.error);
                painter.drawText(0, top, m_lineNumberArea->width() - 6,
                                 height, Qt::AlignRight,
                                 QStringLiteral("!"));
            }

            painter.setPen(line == m_currentExecutionLine ? c.gutterPc : c.gutterText);
            painter.drawText(0, top, m_lineNumberArea->width() - 6,
                             height, Qt::AlignRight, QString::number(line));
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

bool CodeEditor::findBarVisible() const
{
    return m_findBar && m_findBar->isVisible();
}

bool CodeEditor::gotoBarVisible() const
{
    return m_gotoBar && m_gotoBar->isVisible();
}

void CodeEditor::showGotoBar()
{
    if (!m_gotoBar)
        return;
    m_gotoBarHeight = m_gotoBar->sizeHint().height();
    m_gotoBar->show();
    updateViewportMargins();
    layoutFindBar();
    m_gotoEdit->setText(QString::number(textCursor().blockNumber() + 1));
    m_gotoEdit->selectAll();
    m_gotoEdit->setFocus();
}

void CodeEditor::hideGotoBar()
{
    if (!m_gotoBar || !gotoBarVisible())
        return;
    m_gotoBar->hide();
    m_gotoBarHeight = 0;
    updateViewportMargins();
    setFocus();
}

QString CodeEditor::findNeedle() const
{
    return m_findEdit ? m_findEdit->text() : QString();
}

void CodeEditor::showFindBar(bool withReplace)
{
    if (!m_findBar)
        return;
    const bool wasVisible = findBarVisible();

    // Seeded from a single-line selection, the way every editor does it, but
    // only the first time: reopening must not clobber what is being searched.
    if (!wasVisible) {
        const QTextCursor cursor = textCursor();
        const QString selected = cursor.selectedText();
        if (!selected.isEmpty() && !selected.contains(QChar::ParagraphSeparator))
            m_findEdit->setText(selected);
        m_findAnchor = cursor.selectionStart();
    }

    m_replaceRow->setVisible(withReplace);
    m_findBarHeight = m_findBar->sizeHint().height();
    m_findBar->show();
    layoutFindBar();
    updateViewportMargins();
    if (withReplace)
        m_replaceEdit->setFocus();
    else
        m_findEdit->setFocus();
    m_findEdit->selectAll();
    refreshMatches(m_findAnchor, true);
}

void CodeEditor::hideFindBar()
{
    if (!findBarVisible())
        return;
    m_findBar->hide();
    m_matches.clear();
    m_matchIndex = -1;
    m_replaceNote.clear();
    m_findBarHeight = 0;
    updateViewportMargins();
    refreshExtraSelections();
    setFocus();
}

void CodeEditor::findTextChanged()
{
    m_replaceNote.clear();
    // As-you-type searches from where the bar was opened, not from the hit the
    // caret was last moved to, so refining the needle does not wander.
    refreshMatches(findBarVisible() ? m_findAnchor : textCursor().position(), true);
}

void CodeEditor::findOptionsChanged()
{
    m_replaceNote.clear();
    refreshMatches(m_findAnchor, true);
}

bool CodeEditor::isWholeWord(const QTextCursor &hit) const
{
    const int start = hit.selectionStart();
    const int end = hit.selectionEnd();
    if (start > 0 && isWordCharacter(document()->characterAt(start - 1)))
        return false;
    return !isWordCharacter(document()->characterAt(end));
}

void CodeEditor::refreshMatches(int anchor, bool selectHit)
{
    m_matches.clear();
    const QString needle = findNeedle();
    if (!needle.isEmpty()) {
        QTextDocument::FindFlags flags;
        if (m_findCase && m_findCase->isChecked())
            flags |= QTextDocument::FindCaseSensitively;
        const bool wholeWord = m_findWord && m_findWord->isChecked();
        QTextCursor at(document());
        while (true) {
            const QTextCursor hit = document()->find(needle, at, flags);
            if (hit.isNull())
                break;
            at = hit;
            if (wholeWord && !isWholeWord(hit))
                continue;
            m_matches.append(hit);
        }
    }

    m_matchIndex = m_matches.isEmpty() ? -1 : matchIndexAtOrAfter(qMax(0, anchor));
    if (selectHit && m_matchIndex >= 0)
        selectMatch(m_matchIndex);
    else
        refreshExtraSelections();

    if (!m_findStatus)
        return;
    if (!m_replaceNote.isEmpty())
        m_findStatus->setText(m_replaceNote);
    else if (needle.isEmpty())
        m_findStatus->clear();
    else if (m_matches.isEmpty())
        m_findStatus->setText(tr("no matches"));
    else
        m_findStatus->setText(tr("%1 of %2").arg(m_matchIndex + 1).arg(m_matches.size()));
}

int CodeEditor::matchIndexAtOrAfter(int position) const
{
    for (int i = 0; i < m_matches.size(); ++i) {
        if (m_matches.at(i).selectionStart() >= position)
            return i;
    }
    return m_matches.isEmpty() ? -1 : 0; // wrap
}

void CodeEditor::selectMatch(int index)
{
    if (index < 0 || index >= m_matches.size())
        return;
    m_matchIndex = index;
    setTextCursor(m_matches.at(index));
    ensureCursorVisible();
    refreshExtraSelections();
    if (m_findStatus && m_replaceNote.isEmpty())
        m_findStatus->setText(tr("%1 of %2").arg(index + 1).arg(m_matches.size()));
}

void CodeEditor::findNext()
{
    if (!findBarVisible()) {
        showFindBar(false);
        return;
    }
    if (m_matches.isEmpty())
        return;
    const int from = m_matchIndex >= 0 ? m_matches.at(m_matchIndex).selectionEnd()
                                       : textCursor().position();
    const int next = matchIndexAtOrAfter(from);
    // matchIndexAtOrAfter wraps to the first hit once past the last one; only
    // when the caret is already on it does that mean "the same hit again".
    selectMatch(next == m_matchIndex ? (next + 1) % m_matches.size() : next);
}

void CodeEditor::findPrevious()
{
    if (!findBarVisible()) {
        showFindBar(false);
        return;
    }
    if (m_matches.isEmpty())
        return;
    const int from = m_matchIndex >= 0 ? m_matches.at(m_matchIndex).selectionStart()
                                       : textCursor().position();
    for (int i = m_matches.size() - 1; i >= 0; --i) {
        if (m_matches.at(i).selectionStart() < from) {
            selectMatch(i);
            return;
        }
    }
    selectMatch(m_matches.size() - 1); // wrap to the last
}

int CodeEditor::currentMatchFromSelection() const
{
    const QTextCursor selection = textCursor();
    if (!selection.hasSelection())
        return -1;
    for (int i = 0; i < m_matches.size(); ++i) {
        if (m_matches.at(i).selectionStart() == selection.selectionStart()
            && m_matches.at(i).selectionEnd() == selection.selectionEnd())
            return i;
    }
    return -1;
}

void CodeEditor::replaceOne()
{
    if (!findBarVisible())
        showFindBar(true);
    if (m_matches.isEmpty())
        return;
    int index = currentMatchFromSelection();
    if (index < 0) {
        findNext();
        index = m_matchIndex;
        if (index < 0)
            return;
    }

    QTextCursor hit = m_matches.at(index);
    hit.insertText(m_replaceEdit->text());
    m_replaceNote = tr("1 replaced");
    // The document change re-ran the search; come back to the next hit after
    // the text that was just written.
    refreshMatches(hit.position(), true);
    m_findEdit->setFocus();
}

void CodeEditor::replaceAll()
{
    if (!findBarVisible())
        showFindBar(true);
    if (m_matches.isEmpty())
        return;

    // Backwards, so the replacements do not shift the positions still to come,
    // and inside one edit block so the lot undoes as a single step.
    QTextCursor undo(document());
    undo.beginEditBlock();
    const int count = m_matches.size();
    for (int i = count - 1; i >= 0; --i) {
        QTextCursor hit = m_matches.at(i);
        hit.insertText(m_replaceEdit->text());
    }
    undo.endEditBlock();

    m_replaceNote = tr("%1 replaced").arg(count);
    refreshMatches(m_findAnchor, false);
    m_findStatus->setText(m_replaceNote);
    m_findEdit->setFocus();
}

void CodeEditor::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && findBarVisible()) {
        hideFindBar();
        return;
    }
    if (event->key() == Qt::Key_Escape && gotoBarVisible()) {
        hideGotoBar();
        return;
    }
    // A new line keeps the indent of the line being left. Tab stops stay at
    // eight columns; this does not try to understand the opcode.
    const bool bareReturn = (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && (event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) == 0;
    if (bareReturn) {
        const QTextCursor cursor = textCursor();
        const QString line = cursor.block().text();
        int spaces = 0;
        while (spaces < line.size()
               && (line.at(spaces) == QLatin1Char(' ') || line.at(spaces) == QLatin1Char('\t')))
            ++spaces;
        QTextCursor insert = cursor;
        insert.insertText(QStringLiteral("\n") + line.left(spaces));
        setTextCursor(insert);
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

bool CodeEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress
        && (watched == m_findEdit || watched == m_replaceEdit || watched == m_gotoEdit)) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            if (watched == m_gotoEdit)
                hideGotoBar();
            else
                hideFindBar();
            return true;
        }
        if (watched != m_gotoEdit
            && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && (key->modifiers() & Qt::ShiftModifier)) {
            findPrevious();
            return true;
        }
    }
    return QPlainTextEdit::eventFilter(watched, event);
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
    // No QIODevice::Text on the write: on Windows it would translate every \n
    // to \r\n, so the bytes on disk would differ from the editor's content —
    // visible to the user as a floppy entry that changes shape on save. The
    // read side keeps the flag, so a CRLF file still displays sensibly and is
    // normalised to LF on the next save.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
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
