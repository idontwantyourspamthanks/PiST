// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QLineEdit>
#include <QPointer>
#include <QStringList>

class QAbstractItemView;
class QCompleter;
class QEvent;
class QKeyEvent;
class QStringListModel;

namespace pist {

/// The debugger console's command entry: a QLineEdit that recalls the session's
/// commands and completes the word at the cursor.
///
/// The history is deliberately in-memory only. A command is an instruction to
/// one run of the debugger, and recalling an earlier session's command would
/// look like the emulator had accepted something it never received.
///
/// Everything else still behaves like a plain QLineEdit, so returnPressed()
/// keeps meaning "the user submitted this line". The command reaches the
/// history here first, which makes recall order the order commands ran in.
class ConsoleInput : public QLineEdit
{
    Q_OBJECT

public:
    explicit ConsoleInput(QWidget *parent = nullptr);

    /// The words Tab completes from: debugger commands, register names and
    /// symbols. Replacing the list at any time is expected — symbols only exist
    /// once a build has produced them, and the console is usable before that.
    void setCompletions(const QStringList &words);

    /// The commands accepted this session, oldest first.
    QStringList history() const { return m_history; }

protected:
    /// Tab is claimed here rather than in keyPressEvent(): QWidget acts on Tab
    /// before that and would walk the focus chain into the next panel, which is
    /// not what Tab does in a console.
    bool event(QEvent *event) override;

    /// The candidate list is a popup, and a popup can take the keyboard for
    /// itself, so its keys have to be answered here as well as in
    /// keyPressEvent().
    bool eventFilter(QObject *watched, QEvent *event) override;

    /// The arrows and Enter are seen here so the widget and the history stay in
    /// step: Enter records the command *before* the base class emits
    /// returnPressed() for the console to act on.
    void keyPressEvent(QKeyEvent *event) override;

private:
    /// The word the cursor is on, or the word just typed before it. Not the
    /// whole line: a command line is a command plus its arguments, and it is
    /// `dis` in `m $12596 dis` that is worth completing.
    QString wordAtCursor(int *start = nullptr, int *length = nullptr) const;

    /// Replace that word, leaving the rest of the line alone and the caret after
    /// the inserted text.
    void replaceWordAtCursor(const QString &replacement);

    /// Up (offset +1) and Down (offset -1) through the session's commands. Down
    /// past the newest command brings back whatever was being typed. Returns
    /// false when the key means nothing here, so the caret can still move.
    bool recall(int offset);

    /// Record the current line as a command, unless it is blank or repeats the
    /// command before it.
    void noteAcceptedCommand();

    /// Tab: insert the only candidate, extend to the candidates' common prefix,
    /// or — once the candidates agree on nothing more than was typed — list
    /// them, with later Tabs walking through the list.
    void completeOrCycle();
    /// Move to the next listed candidate, wrapping so Tab keeps cycling.
    void stepCycle();

    /// The candidates for `word`, taken from the completer itself so that what
    /// the popup lists and what Tab walks can never disagree.
    QStringList matchesFor(const QString &word);
    /// Show the candidates, marking `highlight` as the one currently in the
    /// line; -1 marks none, since none of them is in the line yet.
    void showCandidates(int highlight);
    /// A candidate was picked out of the list.
    void acceptCandidate(const QString &candidate);

    void resetCycle();
    void hidePopup();
    bool popupVisible() const;

    QStringListModel *m_model = nullptr;
    QCompleter *m_completer = nullptr;
    /// The completer's popup, which the completer owns and deletes with itself.
    QPointer<QAbstractItemView> m_popup;

    QStringList m_history;
    /// -1 while the user is editing a line of their own, otherwise the index
    /// into the history being shown. Down walks forward through it again.
    int m_historyIndex = -1;
    /// What the user was typing when recall started. Taken away by the first
    /// recall and given back by walking Down past the newest command.
    QString m_pending;

    /// The candidates the last Tab listed, the word that produced them, where
    /// that word starts, and which candidate is in the line (-1 = the word as
    /// typed, none of them yet).
    QStringList m_cycleMatches;
    QString m_cycleStem;
    int m_cycleStart = 0;
    int m_cycleIndex = -1;
};

} // namespace pist
