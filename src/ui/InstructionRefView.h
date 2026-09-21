// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

namespace pist {

/// A searchable list of the 68000 instruction set and the TOS system calls
/// (GEMDOS, BIOS, XBIOS), with the selected entry's details underneath.
///
/// An ST programmer spends most of their time in the instruction set: which
/// flag a shift sets, whether MOVE to SR needs supervisor mode, what a
/// half-remembered mnemonic does. The editor's syntax highlighter only knows
/// *that* a word is a mnemonic, so the reference lives beside it — one click
/// from the word under the cursor to its description. OS calls join the same
/// list: cursor on a `trap #1` line (or the push feeding it) shows the call
/// being made, not just the trap instruction.
class InstructionRefView : public QWidget
{
    Q_OBJECT

public:
    explicit InstructionRefView(QWidget *parent = nullptr);

    /// The mnemonic the panel is currently showing, empty when the filter
    /// matches nothing. Exposed so the entry the user sees can be asserted
    /// without reaching into the widget's internals.
    QString currentMnemonic() const;

public slots:
    /// Re-apply the theme's monospace font and re-measure the detail area.
    /// MainWindow calls this from its own applyAppearance().
    void applyAppearance();

    /// Show, highlight and scroll to the entry for `word`: an editor word,
    /// which may carry a size suffix (`addq.l`) and any casing. Unknown words
    /// — labels, directives, typos — leave the panel untouched. The filter is
    /// cleared first, since it would otherwise hide the entry being asked for.
    void showInstruction(const QString &word);

    /// Show, highlight and scroll to the OS-call entry for (`trap`, `opcode`),
    /// with the call's actual arguments (`args`, in push order) shown in the
    /// detail when given. Unknown calls leave the panel untouched.
    void showOsCall(int trap, int opcode, const QStringList &args = QStringList());

private:
    void applyFilter(const QString &text);
    void updateDetail(QListWidgetItem *item);
    void updateOsDetail(QListWidgetItem *item);
    QListWidgetItem *itemForMnemonic(const QString &mnemonic) const;
    void selectAndScroll(QListWidgetItem *item);

    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_detail = nullptr;
    // The actual arguments of the call the cursor is sitting in, and the list
    // key they belong to; set by showOsCall, cleared by showInstruction, and
    // shown only while the matching row is selected.
    QStringList m_callArgs;
    QString m_callArgsKey;
};

} // namespace pist
