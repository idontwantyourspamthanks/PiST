// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/CodeEditor.h"

#include "editor/AsmHighlighter.h"
#include "editor/EditorTheme.h"
#include "support/FileWrite.h"

#include <QContextMenuEvent>
#include <QDir>
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
#include <QStringConverter>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace pist {

namespace {

/// Whether every character of `text` has a Latin-1 byte, i.e. survives
/// toLatin1() without becoming '?'. Characters above U+00FF need UTF-8.
bool fitsLatin1(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.unicode() > 0xFF)
            return false;
    }
    return true;
}

class LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(CodeEditor *editor)
        : QWidget(editor)
        , m_editor(editor)
    {
        setObjectName(QStringLiteral("lineNumberArea"));
        // Blame tips follow the cursor. The number gutter has nothing to say.
        setMouseTracking(true);
    }

    QSize sizeHint() const override
    {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override { m_editor->lineNumberAreaPaintEvent(event); }

    void mousePressEvent(QMouseEvent *event) override
    {
        // The blame lane is not the breakpoint gutter. A click there must not
        // toggle a breakpoint on the line it happens to sit beside.
        if (event->position().x() < m_editor->blameLaneWidth()) {
            event->accept();
            return;
        }
        const int line = m_editor->lineAtY(event->position().y());
        if (line > 0)
            emit m_editor->gutterClicked(line, event->button());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int line = m_editor->lineAtY(event->position().y());
        if (event->position().x() < m_editor->blameLaneWidth() && line > 0)
            setToolTip(m_editor->blameTip(line));
        else
            setToolTip(QString());
        QWidget::mouseMoveEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (event->pos().x() < m_editor->blameLaneWidth())
            return;
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

/// How many hits a keystroke's search keeps. A common token in a large file has
/// thousands of them, and the search re-runs on every keystroke and every cursor
/// move; the label admits the cap ("more than N hits") rather than report a
/// total the walk never reached, and the replace actions re-scan without it, so
/// nothing but the label and the reach of Next/Previous depends on it.
constexpr int kMaxMatches = 2000;

/// The hit the selection is on when it is one of `hits`, else -1.
int indexOfSelectedHit(const QList<QTextCursor> &hits, const QTextCursor &selection)
{
    if (!selection.hasSelection())
        return -1;
    for (int i = 0; i < hits.size(); ++i) {
        if (hits.at(i).selectionStart() == selection.selectionStart()
            && hits.at(i).selectionEnd() == selection.selectionEnd())
            return i;
    }
    return -1;
}

/// The first hit at or after `position`, wrapping to the first one; -1 when
/// there are none.
int indexAtOrAfter(const QList<QTextCursor> &hits, int position)
{
    for (int i = 0; i < hits.size(); ++i) {
        if (hits.at(i).selectionStart() >= position)
            return i;
    }
    return hits.isEmpty() ? -1 : 0; // wrap
}

} // namespace

CodeEditor::CodeEditor(const EditorTheme &theme, QWidget *parent)
    : QPlainTextEdit(parent)
    , m_theme(theme)
{
    setLineWrapMode(QPlainTextEdit::NoWrap);

    m_highlighter = new AsmHighlighter(document(), m_theme);

    // After the highlighter exists: it also applies the theme's font and colours.
    setTheme(theme);

    m_lineNumberArea = new LineNumberArea(this);

    // Blame asks for the visible range once the scroll has settled, not on
    // every pixel of a drag.
    m_blameSettle = new QTimer(this);
    m_blameSettle->setSingleShot(true);
    m_blameSettle->setInterval(200);
    connect(m_blameSettle, &QTimer::timeout, this, [this] {
        if (m_blameShown)
            emit visibleRangeSettled();
    });
    connect(document(), &QTextDocument::contentsChange, this, [this] {
        if (m_blameShown)
            m_blameSettle->start();
    });

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

void CodeEditor::setTheme(const EditorTheme &theme)
{
    m_theme = theme;
    setFont(m_theme.font);
    setTabStopDistance(8 * QFontMetricsF(m_theme.font).horizontalAdvance(QLatin1Char(' ')));

    m_highlighter->setTheme(m_theme);
    refreshExtraSelections();
    if (m_lineNumberArea)
        updateLineNumberAreaWidth(0);
}

int CodeEditor::blameLaneWidth() const
{
    if (!m_blameShown)
        return 0;
    // Wide enough for "uncommitted" at the editor's own size. Authors longer
    // than that elide; the hover carries the rest.
    return fontMetrics().horizontalAdvance(tr("uncommitted")) + 16;
}

int CodeEditor::lineNumberAreaWidth() const
{
    int digits = 1;
    int max = qMax(1, blockCount());
    while (max >= 10) {
        max /= 10;
        ++digits;
    }
    return blameLaneWidth() + 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void CodeEditor::updateLineNumberAreaWidth(int)
{
    // The blame lane appears without the window itself changing size, so there
    // is no resize event to lean on. The geometry rides along with the margins
    // (see updateViewportMargins).
    updateViewportMargins();
}

void CodeEditor::updateViewportMargins()
{
    const int bottom = findBarVisible() ? m_findBarHeight : 0;
    const int top = gotoBarVisible() ? m_gotoBarHeight : 0;
    setViewportMargins(lineNumberAreaWidth(), top, 0, bottom);

    // The gutter has to be laid out from those same margins, never from the raw
    // contents rect: it carries the viewport's coordinate space. The paint walks
    // blockBoundingGeometry(block).translated(contentOffset()) — viewport
    // coordinates — and lineAtY() maps a click back through the same numbers, so
    // a gutter that starts at the contents rect while the viewport starts `top`
    // pixels lower draws every number that much above the text it annotates, and
    // a click beside line N lands on N + top/rowHeight. The bars change the
    // margins without a resize, so this is the one place both are kept in step.
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top() + top, lineNumberAreaWidth(),
                                        qMax(0, cr.height() - top - bottom)));
}

void CodeEditor::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy)
        m_lineNumberArea->scroll(0, dy);
    else
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());

    if (m_blameShown && dy)
        m_blameSettle->start();

    if (rect.contains(viewport()->rect()))
        updateLineNumberAreaWidth(0);
}

void CodeEditor::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    // Re-applies the margins and the gutter geometry that follows them: both
    // depend on the new contents rect.
    updateViewportMargins();
    layoutFindBar();
}

void CodeEditor::scrollContentsBy(int dx, int dy)
{
    QPlainTextEdit::scrollContentsBy(dx, dy);
    // Only the hits in the viewport carry a decoration, so a scroll has to
    // build the ones it brought in — after the viewport has moved, which is
    // where this runs, unlike updateRequest (emitted before the scroll).
    if (dy && findBarVisible())
        refreshExtraSelections();
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
    const int cursorLine = textCursor().blockNumber() + 1;

    // Cursor line, underneath the debugger's PC highlight when they coincide.
    if (cursorLine > 0 && cursorLine != m_currentExecutionLine) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(m_theme.currentLine);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = textCursor();
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // Current execution line, as reported by the debugger.
    if (m_currentExecutionLine > 0 && m_currentExecutionLine <= blockCount()) {
        QTextEdit::ExtraSelection sel;
        sel.format.setBackground(m_theme.executionLine);
        sel.format.setProperty(QTextFormat::FullWidthSelection, true);
        sel.cursor = QTextCursor(document()->findBlockByNumber(m_currentExecutionLine - 1));
        sel.cursor.clearSelection();
        selections.append(sel);
    }

    // Find hits, except the one the selection is on: that one wears the
    // selection colour, and an extra selection would paint over it. Only the
    // hits in the viewport are turned into selections at all: this list is
    // rebuilt on every keystroke and every cursor move, and QPlainTextEdit walks
    // every selection it holds on every paint, so a hit list of thousands cost
    // far more than the handful on screen are worth. The hits are in document
    // order, so the walk starts in the window and stops past it; scrolling
    // rebuilds the window (scrollContentsBy).
    if (findBarVisible() && !m_matches.isEmpty()) {
        int firstVisible = 0;
        int lastVisible = 0;
        visibleLineRange(firstVisible, lastVisible);
        for (int i = 0; i < m_matches.size(); ++i) {
            const int line = m_matches.at(i).blockNumber() + 1;
            if (line < firstVisible)
                continue;
            if (line > lastVisible)
                break;
            if (i == m_matchIndex)
                continue;
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(m_theme.searchMatch);
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
        sel.format.setUnderlineColor(m_theme.error);
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
    // The markers live in the gutter, so repaint it explicitly, as the
    // breakpoint, heat and blame setters do: the extra selections only cover the
    // viewport, and the markers must not depend on their repaint reaching the
    // gutter as a side effect.
    if (m_lineNumberArea)
        m_lineNumberArea->update();
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

void CodeEditor::visibleLineRange(int &first, int &last) const
{
    first = 0;
    last = 0;
    QTextBlock block = firstVisibleBlock();
    if (!block.isValid())
        return;
    first = block.blockNumber() + 1;
    last = first;
    const int bottom = viewport()->height();
    while (block.isValid()) {
        const int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        if (top > bottom)
            break;
        last = block.blockNumber() + 1;
        block = block.next();
    }
}

void CodeEditor::setBlameShown(bool on)
{
    if (m_blameShown == on)
        return;
    m_blameShown = on;
    if (!on) {
        m_blame.clear();
        m_blameSettle->stop();
    } else {
        m_blameSettle->start();
    }
    updateLineNumberAreaWidth(0);
    m_lineNumberArea->update();
}

void CodeEditor::setBlame(const GitBlameMap &lines)
{
    m_blame = lines;
    if (m_lineNumberArea)
        m_lineNumberArea->update();
}

QString CodeEditor::blameTip(int line) const
{
    const auto it = m_blame.constFind(line);
    if (it == m_blame.constEnd() || it->uncommitted)
        return it == m_blame.constEnd() ? QString() : tr("Uncommitted");
    return it->hash + QLatin1Char('\n') + it->author + QStringLiteral(", ") + it->date
        + QLatin1Char('\n') + it->summary;
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
    painter.fillRect(event->rect(), m_theme.gutter);

    // Blame is a lane of its own. Every marker below is drawn in the number
    // gutter, shifted right by the lane, so a breakpoint dot stays where a
    // click toggles it. With the lane off the shift is zero.
    const int lane = blameLaneWidth();
    const int width = m_lineNumberArea->width();
    const int gutter = width - lane;
    if (lane > 0) {
        painter.setPen(m_theme.muted);
        painter.drawLine(lane - 1, event->rect().top(), lane - 1, event->rect().bottom());
    }

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
            if (lane > 0) {
                const auto blamed = m_blame.constFind(line);
                QString text;
                if (blamed == m_blame.constEnd())
                    text.clear();
                else if (blamed->uncommitted)
                    text = tr("uncommitted");
                else
                    text = blamed->author;
                if (!text.isEmpty()) {
                    painter.setPen(blamed->uncommitted ? m_theme.warning : m_theme.muted);
                    const QString elided = fontMetrics().elidedText(text, Qt::ElideRight, lane - 10);
                    painter.drawText(4, top, lane - 8, height, Qt::AlignLeft | Qt::AlignVCenter, elided);
                }
            }

            const auto heat = m_lineHeat.constFind(line);
            if (heat != m_lineHeat.constEnd()) {
                const double scaled = m_lineHeatScale > 0.0
                                          ? std::log(double(*heat)) / m_lineHeatScale
                                          : 1.0;
                QColor tint(0xd8, 0x8a, 0x30); // warm amber, in either theme
                tint.setAlphaF(0.55 * qBound(0.0, scaled, 1.0));
                painter.fillRect(lane, top, gutter, bottom - top, tint);
            }

            if (line == m_currentExecutionLine)
                painter.fillRect(lane, top, 3, bottom - top, m_theme.gutterPc);

            if (m_breakpointLines.contains(line)) {
                // A filled dot, drawn rather than glyph-based so it does not
                // depend on a font that happens to have the character.
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setBrush(m_theme.breakpoint);
                painter.setPen(Qt::NoPen);
                const int d = qMax(5, height - 6);
                // Geometrically centred in the text box, then dropped a little
                // at small type: a tiny disc sits optically high, which was
                // right at 15pt (top+3) and a few pixels high at 10pt.
                int y = top + (height - d) / 2;
                const int pt = font().pointSize();
                if (pt > 0 && pt < 15)
                    y += (15 - pt + 1) / 2;
                painter.drawEllipse(QRect(lane + 2, y, d, d));
                painter.setRenderHint(QPainter::Antialiasing, false);
            }

            if (m_errorLines.contains(line)) {
                painter.setPen(m_theme.error);
                painter.drawText(lane, top, gutter - 6,
                                 height, Qt::AlignRight,
                                 QStringLiteral("!"));
            }

            painter.setPen(line == m_currentExecutionLine ? m_theme.gutterPc : m_theme.gutterText);
            painter.drawText(lane, top, gutter - 6,
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
    m_matchesCapped = false;
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

QList<QTextCursor> CodeEditor::collectMatches(int limit, bool *capped) const
{
    QList<QTextCursor> hits;
    if (capped)
        *capped = false;
    const QString needle = findNeedle();
    if (needle.isEmpty())
        return hits;

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
        // One hit past the limit is what proves the label's "more than": the
        // limit alone only says the walk stopped.
        if (limit > 0 && hits.size() >= limit) {
            if (capped)
                *capped = true;
            break;
        }
        hits.append(hit);
    }
    return hits;
}

QList<QTextCursor> CodeEditor::replacementHits() const
{
    if (!m_matchesCapped)
        return m_matches;
    return collectMatches(0, nullptr);
}

QString CodeEditor::matchStatusText() const
{
    if (m_matchesCapped)
        return tr("%1 of more than %2").arg(m_matchIndex + 1).arg(m_matches.size());
    return tr("%1 of %2").arg(m_matchIndex + 1).arg(m_matches.size());
}

void CodeEditor::refreshMatches(int anchor, bool selectHit)
{
    const QString needle = findNeedle();
    m_matches = collectMatches(kMaxMatches, &m_matchesCapped);

    m_matchIndex = m_matches.isEmpty() ? -1 : indexAtOrAfter(m_matches, qMax(0, anchor));
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
        m_findStatus->setText(matchStatusText());
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
        m_findStatus->setText(matchStatusText());
}

void CodeEditor::findNext()
{
    if (!findBarVisible()) {
        showFindBar(false);
        return;
    }
    if (m_matches.isEmpty())
        return;
    // Navigate from where the caret actually is, not from the hit the search was
    // opened on. An edit in between re-runs refreshMatches from m_findAnchor, so
    // the index alone would send Next backwards to where the search began, and
    // leave the position label describing a hit the user is not on.
    m_replaceNote.clear();
    const int current = indexOfSelectedHit(m_matches, textCursor());
    const int from = current >= 0 ? m_matches.at(current).selectionEnd()
                                  : textCursor().position();
    const int next = indexAtOrAfter(m_matches, from);
    // indexAtOrAfter wraps to the first hit once past the last one; only when
    // the caret is already on it does that mean "the same hit again".
    selectMatch(next == current ? (next + 1) % m_matches.size() : next);
}

void CodeEditor::findPrevious()
{
    if (!findBarVisible()) {
        showFindBar(false);
        return;
    }
    if (m_matches.isEmpty())
        return;
    m_replaceNote.clear();
    const int current = indexOfSelectedHit(m_matches, textCursor());
    const int from = current >= 0 ? m_matches.at(current).selectionStart()
                                  : textCursor().position();
    for (int i = m_matches.size() - 1; i >= 0; --i) {
        if (m_matches.at(i).selectionStart() < from) {
            selectMatch(i);
            return;
        }
    }
    selectMatch(m_matches.size() - 1); // wrap to the last
}

void CodeEditor::replaceOne()
{
    if (!findBarVisible())
        showFindBar(true);
    if (m_matches.isEmpty())
        return;

    // Replacing is one-off and destructive, so it reads the exact hit list: the
    // live one stops at the scan cap, and which hit the caret is nearest is not
    // something the cap is allowed to change.
    const QList<QTextCursor> hits = replacementHits();
    int index = indexOfSelectedHit(hits, textCursor());
    if (index < 0)
        index = indexAtOrAfter(hits, textCursor().position());
    if (index < 0)
        return;

    QTextCursor hit = hits.at(index);
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

    // Same as replaceOne: the count and the hits replaced must be the document's,
    // not the cap's.
    const QList<QTextCursor> hits = replacementHits();

    // Backwards, so the replacements do not shift the positions still to come,
    // and inside one edit block so the lot undoes as a single step.
    QTextCursor undo(document());
    undo.beginEditBlock();
    const int count = hits.size();
    for (int i = count - 1; i >= 0; --i) {
        QTextCursor hit = hits.at(i);
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
    if (!file.open(QIODevice::ReadOnly))
        return false;

    // The bytes are read raw and decoded here rather than by a QTextStream
    // (UTF-8 in both directions). Atari ST sources are ASCII or Latin-1/ST
    // characters: a high-bit byte in a comment or a string literal is not
    // valid UTF-8, and a lenient decode turns it into U+FFFD without
    // reporting anything, so the next save writes EF BF BD over bytes the
    // user never touched. A strict decode separates the two cases: valid
    // UTF-8 stays UTF-8, anything else is the Latin-1 the ST actually used.
    // A UTF-8 byte-order mark is kept as U+FEFF so it round-trips too.
    QByteArray raw = file.readAll();
    // Read before the normalisation below erases the evidence: the style is
    // what saveFile writes back, so an unedited round-trip keeps the file's
    // own line endings instead of rewriting all of them.
    m_lineEnding = raw.contains(QByteArrayLiteral("\r\n")) ? LineEnding::CrLf
                                                          : LineEnding::Lf;
    raw.replace(QByteArrayLiteral("\r\n"), QByteArrayLiteral("\n"));
    QStringDecoder utf8(QStringConverter::Utf8, QStringConverter::Flag::ConvertInitialBom);
    QString text = utf8.decode(raw);
    if (utf8.hasError()) {
        text = QString::fromLatin1(raw);
        m_sourceEncoding = SourceEncoding::Latin1;
    } else {
        m_sourceEncoding = SourceEncoding::Utf8;
    }

    setPlainText(text);
    m_filePath = path;
    document()->setModified(false);
    return true;
}

bool CodeEditor::saveFile(const QString &path)
{
    // The bytes are written in the encoding the file was loaded in, so
    // loading and saving an unedited Latin-1, UTF-8 or plain ASCII source
    // reproduces it exactly. A document that has since grown a character
    // Latin-1 cannot represent is written as UTF-8 rather than mangled.
    // No QIODevice::Text on the write, either: on Windows it would translate
    // every \n to \r\n, so the bytes on disk would differ from the editor's
    // content — visible to the user as a floppy entry that changes shape on
    // save. Line endings are restored instead of translated: the read side
    // records the file's own style, and this writes that same style back, so a
    // CRLF source that was not edited round-trips byte for byte.
    const QString text = toPlainText();
    QByteArray bytes;
    if (m_sourceEncoding == SourceEncoding::Latin1 && fitsLatin1(text)) {
        bytes = text.toLatin1();
    } else {
        bytes = text.toUtf8();
        m_sourceEncoding = SourceEncoding::Utf8;
    }
    // After the encoding, so both of them get the style: '\r' and '\n' are one
    // byte apiece in UTF-8 and in Latin-1 alike.
    if (m_lineEnding == LineEnding::CrLf)
        bytes.replace('\n', QByteArrayLiteral("\r\n"));

    // An extracted document is saved to a location that does not exist yet, so
    // a save creates the missing parents instead of failing outright.
    const QString dir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(dir)) {
        m_lastError = tr("Could not create %1").arg(dir);
        return false;
    }

    // Opening the destination with Truncate and checking the byte count
    // afterwards reports a failed write honestly and still leaves the file
    // empty, which loses the only copy of the user's work — so the write goes
    // through the rule in support/FileWrite.h instead: a temporary file, an
    // explicit flush and a device-error check, and only then the replace.
    QString error;
    if (!files::write(path, bytes, &error)) {
        m_lastError = error;
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
    return m_filePath.isEmpty() ? tr("untitled") : QFileInfo(m_filePath).fileName();
}

} // namespace pist
