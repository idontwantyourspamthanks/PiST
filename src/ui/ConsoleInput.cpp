// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ConsoleInput.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QCompleter>
#include <QEvent>
#include <QKeyEvent>
#include <QStringListModel>

namespace pist {

namespace {

/// The longest prefix every candidate shares, compared without case and
/// capitalised the way the first of them is. The symbol table is the authority
/// on how a name is spelled, and completing to a spelling nothing uses would
/// produce a command that does not resolve.
QString commonPrefix(const QStringList &words)
{
    QString prefix = words.first();
    for (int i = 1; i < words.size() && !prefix.isEmpty(); ++i) {
        const QString &other = words.at(i);
        const int limit = qMin(prefix.size(), other.size());
        int shared = 0;
        while (shared < limit && prefix.at(shared).toLower() == other.at(shared).toLower())
            ++shared;
        prefix.truncate(shared);
    }
    return prefix;
}

} // namespace

ConsoleInput::ConsoleInput(QWidget *parent)
    : QLineEdit(parent)
{
    // The completer is built before the model it filters, and QObject destroys
    // children in the order they were added: the completer, which holds a bare
    // pointer to that model, is therefore torn down before the model goes.
    m_completer = new QCompleter(this);
    m_model = new QStringListModel(this);
    m_completer->setModel(m_model);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setCompletionMode(QCompleter::PopupCompletion);
    // setWidget(), not QLineEdit::setCompleter(): the latter wires
    // QCompleter::activated() to setText(), which would replace the whole
    // command line with a candidate instead of the one word being completed.
    m_completer->setWidget(this);
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated),
            this, &ConsoleInput::acceptCandidate);

    // Created here so its identity is settled. It is a top-level window the
    // completer owns and deletes with itself, and asking for it while listing is
    // already under way would leave the wrong view under this pointer.
    m_popup = m_completer->popup();
    // Installed after the completer's own filter, which makes this one run
    // first: the keys the console gives a different meaning to are claimed here
    // instead of being read as list navigation.
    m_popup->installEventFilter(this);

    // Editing the line by hand narrows the word away from the candidates that
    // were listed, so the listing is stale from then on. Only textEdited() ends
    // a listing: programmatic changes do not emit it, and replacing the word as
    // the cycle walks candidates must not end the walk.
    connect(this, &QLineEdit::textEdited, this, [this] {
        resetCycle();
        hidePopup();
    });
}

void ConsoleInput::setCompletions(const QStringList &words)
{
    resetCycle();
    hidePopup();
    m_model->setStringList(words);
}

bool ConsoleInput::event(QEvent *event)
{
    // QWidget acts on Tab before keyPressEvent() and would walk the focus chain
    // into the next panel; in a console Tab is completion, and a console is not
    // a form to be tabbed through.
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier) {
            completeOrCycle();
            return true;
        }
    }
    return QLineEdit::event(event);
}

bool ConsoleInput::eventFilter(QObject *watched, QEvent *event)
{
    // A popup takes the keyboard for itself, in which case these keys arrive
    // here rather than at keyPressEvent().
    if (watched == m_popup && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->modifiers() == Qt::NoModifier) {
            switch (key->key()) {
            case Qt::Key_Tab:
                completeOrCycle();
                return true;
            case Qt::Key_Up:
                if (recall(1))
                    return true;
                break;
            case Qt::Key_Down:
                if (recall(-1))
                    return true;
                break;
            case Qt::Key_Escape:
                // Dismisses the list, not the line: the user is choosing among
                // candidates, not abandoning the command being typed.
                resetCycle();
                hidePopup();
                return true;
            default:
                break;
            }
        }
    }
    return QLineEdit::eventFilter(watched, event);
}

void ConsoleInput::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Up:
        if (event->modifiers() == Qt::NoModifier && recall(1)) {
            event->accept();
            return;
        }
        break;
    case Qt::Key_Down:
        if (event->modifiers() == Qt::NoModifier && recall(-1)) {
            event->accept();
            return;
        }
        break;
    case Qt::Key_Escape:
        if (popupVisible()) {
            resetCycle();
            hidePopup();
            event->accept();
            return;
        }
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // Recorded before the base class emits returnPressed(), so the console
        // acts on exactly the line the history just took.
        resetCycle();
        hidePopup();
        noteAcceptedCommand();
        break;
    default:
        break;
    }
    QLineEdit::keyPressEvent(event);
}

bool ConsoleInput::recall(int offset)
{
    // Recalling replaces the line, so whatever the last Tab listed was listed
    // for a word that is no longer there.
    resetCycle();
    hidePopup();

    if (m_history.isEmpty())
        return false;

    if (offset > 0) {
        if (m_historyIndex < 0) {
            // Half-typed text is a command the user meant to finish; walking
            // back down to it must not have thrown it away silently.
            m_pending = text();
            m_historyIndex = m_history.size() - 1;
        } else if (m_historyIndex > 0) {
            --m_historyIndex;
        } else {
            // Already at the oldest command. Consumed all the same: falling
            // through would send the caret to the start of the line, which is
            // not what Up means once the history has been reached for.
            return true;
        }
    } else if (m_historyIndex >= 0) {
        if (++m_historyIndex >= m_history.size()) {
            // Past the newest command lies the line the user was writing when
            // they first reached for the history.
            m_historyIndex = -1;
            setText(m_pending);
            setCursorPosition(text().size());
            return true;
        }
    } else {
        return false;
    }

    setText(m_history.at(m_historyIndex));
    setCursorPosition(text().size());
    return true;
}

void ConsoleInput::noteAcceptedCommand()
{
    m_historyIndex = -1;
    m_pending.clear();

    const QString command = text().trimmed();
    if (command.isEmpty())
        return;
    // Repeating a command is how a debugger is stepped, so the repeat is worth
    // running again but not worth a second entry in the history.
    if (!m_history.isEmpty() && m_history.last() == command)
        return;
    m_history.append(command);
}

void ConsoleInput::completeOrCycle()
{
    int start = 0;
    const QString word = wordAtCursor(&start);
    if (word.isEmpty()) {
        resetCycle();
        return;
    }

    // A Tab following a listing walks that listing: the word still sits where
    // the candidates were offered for, and still begins the way they all do.
    if (!m_cycleMatches.isEmpty() && start == m_cycleStart
        && word.startsWith(m_cycleStem, Qt::CaseInsensitive)) {
        stepCycle();
        return;
    }

    resetCycle();
    const QStringList matches = matchesFor(word);
    if (matches.isEmpty()) {
        hidePopup();
        return;
    }
    if (matches.size() == 1) {
        replaceWordAtCursor(matches.first());
        hidePopup();
        return;
    }

    const QString shared = commonPrefix(matches);
    if (shared.size() > word.size()) {
        // The candidates agree on more than was typed, so extend the line and
        // stop there: what comes next can only be answered by choosing among
        // them, which is a later Tab's business.
        replaceWordAtCursor(shared);
        hidePopup();
        return;
    }

    // Ambiguous, and nothing left to add. Listing the candidates is the only
    // answer; further Tabs walk them in turn.
    m_cycleMatches = matches;
    m_cycleStem = shared;
    m_cycleStart = start;
    m_cycleIndex = -1;
    showCandidates(-1);
}

void ConsoleInput::stepCycle()
{
    m_cycleIndex = (m_cycleIndex + 1) % m_cycleMatches.size();
    replaceWordAtCursor(m_cycleMatches.at(m_cycleIndex));
    showCandidates(m_cycleIndex);
}

QStringList ConsoleInput::matchesFor(const QString &word)
{
    // Filtering through the completer, rather than beside it, is what keeps the
    // candidates Tab walks and the candidates the popup lists one list in one
    // order.
    m_completer->setCompletionPrefix(word);
    const QAbstractItemModel *offered = m_completer->completionModel();
    QStringList matches;
    matches.reserve(offered->rowCount());
    for (int row = 0; row < offered->rowCount(); ++row)
        matches.append(offered->index(row, 0).data(Qt::EditRole).toString());
    return matches;
}

void ConsoleInput::showCandidates(int highlight)
{
    m_completer->setCompletionPrefix(m_cycleStem);
    m_completer->complete();
    if (highlight < 0 || !m_popup)
        return;

    // Marking the row only says which candidate the line holds; QCompleter's
    // activated() is wired to acceptCandidate(), so this cannot rewrite the line
    // behind the cycle's back.
    const QAbstractItemModel *offered = m_completer->completionModel();
    if (highlight < offered->rowCount())
        m_popup->setCurrentIndex(offered->index(highlight, 0));
}

void ConsoleInput::acceptCandidate(const QString &candidate)
{
    // Reached by clicking a row in the popup. An empty candidate means the
    // completer had no row to offer, which is not something to write into the
    // line.
    if (candidate.isEmpty())
        return;
    replaceWordAtCursor(candidate);
    resetCycle();
    hidePopup();
}

void ConsoleInput::resetCycle()
{
    m_cycleMatches.clear();
    m_cycleStem.clear();
    m_cycleStart = 0;
    m_cycleIndex = -1;
}

void ConsoleInput::hidePopup()
{
    if (popupVisible())
        m_popup->hide();
}

bool ConsoleInput::popupVisible() const
{
    return m_popup && m_popup->isVisible();
}

QString ConsoleInput::wordAtCursor(int *start, int *length) const
{
    const QString line = text();
    const int cursor = qBound(0, cursorPosition(), line.size());
    // The caret at a word's end belongs to that word: that is where completion
    // is asked for, immediately after typing it.
    int from = cursor;
    while (from > 0 && !line.at(from - 1).isSpace())
        --from;
    int to = cursor;
    while (to < line.size() && !line.at(to).isSpace())
        ++to;

    if (start)
        *start = from;
    if (length)
        *length = to - from;
    return line.mid(from, to - from);
}

void ConsoleInput::replaceWordAtCursor(const QString &replacement)
{
    int start = 0;
    int length = 0;
    wordAtCursor(&start, &length);
    const QString line = text();
    setText(line.left(start) + replacement + line.mid(start + length));
    setCursorPosition(start + replacement.size());
}

} // namespace pist
