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

/// A searchable list of the 68000 instruction set, with the selected
/// instruction's effect on the condition codes spelled out underneath.
///
/// An ST programmer spends most of their time in the instruction set: which
/// flag a shift sets, whether MOVE to SR needs supervisor mode, what a
/// half-remembered mnemonic does. The editor's syntax highlighter only knows
/// *that* a word is a mnemonic, so the reference lives beside it — one click
/// from the word under the cursor to its description.
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

private:
    void applyFilter(const QString &text);
    void updateDetail(QListWidgetItem *item);
    QListWidgetItem *itemForMnemonic(const QString &mnemonic) const;

    QLineEdit *m_filter = nullptr;
    QListWidget *m_list = nullptr;
    QLabel *m_detail = nullptr;
};

} // namespace pist
