// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/InstructionRefView.h"

#include "editor/InstrRef.h"
#include "ui/Appearance.h"

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace pist {

InstructionRefView::InstructionRefView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("instructionRefFilter"));
    m_filter->setPlaceholderText(tr("Filter instructions"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("instructionRefList"));
    m_list->setAlternatingRowColors(true);
    m_list->setUniformItemSizes(true);
    appearance::markMono(m_list);
    for (const InstructionInfo &info : instructionTable()) {
        const QString label = QStringLiteral("%1  %2").arg(info.mnemonic, info.summary);
        auto *item = new QListWidgetItem(label, m_list);
        // A dock is often too narrow for the whole summary, so the clipped row
        // gets the full text as a tooltip.
        item->setToolTip(label);
        // The mnemonic doubles as the item's key, so a jump can find its row
        // without depending on where the table happens to list it.
        item->setData(Qt::UserRole, info.mnemonic);
    }
    layout->addWidget(m_list, 1);

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("instructionRefDetail"));
    m_detail->setWordWrap(true);
    m_detail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Reserve two lines: a label that grows as the selection changes would
    // make the whole dock jump about while the user is browsing.
    m_detail->setMinimumHeight(2 * fontMetrics().height());
    m_detail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    layout->addWidget(m_detail);

    connect(m_filter, &QLineEdit::textChanged, this, &InstructionRefView::applyFilter);
    connect(m_list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current) { updateDetail(current); });

    applyFilter(QString());
}

QString InstructionRefView::currentMnemonic() const
{
    const QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QListWidgetItem *InstructionRefView::itemForMnemonic(const QString &mnemonic) const
{
    const QString key = mnemonic.toLower();
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        if (item->data(Qt::UserRole).toString().toLower() == key)
            return item;
    }
    return nullptr;
}

void InstructionRefView::applyFilter(const QString &text)
{
    const QString needle = text.trimmed().toLower();

    // A hidden item cannot be current, so the selection is decided in the same
    // pass: the first surviving row, or nothing when the filter matches none.
    QListWidgetItem *firstVisible = nullptr;
    for (int row = 0; row < m_list->count(); ++row) {
        QListWidgetItem *item = m_list->item(row);
        const bool visible = needle.isEmpty() || item->text().contains(needle, Qt::CaseInsensitive);
        item->setHidden(!visible);
        if (visible && !firstVisible)
            firstVisible = item;
    }

    if (firstVisible)
        m_list->setCurrentItem(firstVisible);
    else
        m_list->setCurrentItem(nullptr);
}

void InstructionRefView::updateDetail(QListWidgetItem *item)
{
    if (!item) {
        m_detail->clear();
        return;
    }

    // The list holds a composed label, so the entry is re-read from the
    // reference itself rather than parsed back out of the row text.
    const InstructionInfo *info = instructionRef(item->data(Qt::UserRole).toString());
    if (!info) {
        m_detail->clear();
        return;
    }

    QString flags = info->flags;
    if (flags.isEmpty())
        flags = tr("Flags: not known."); // unknown, not "none": say so
    else
        flags = tr("Flags: %1").arg(flags);
    m_detail->setText(flags);
}

void InstructionRefView::applyAppearance()
{
    // The font-size preference has changed, so the two-line reservation made
    // from the old metrics has to be redone.
    m_detail->setMinimumHeight(2 * fontMetrics().height());
}

void InstructionRefView::showInstruction(const QString &word)
{
    const InstructionInfo *info = instructionRef(word);
    if (!info)
        return; // a label, a directive or a typo: leave the panel as it is

    QListWidgetItem *item = itemForMnemonic(info->mnemonic);
    if (!item)
        return;

    // A hidden row cannot be selected, so a filter that hides this entry is
    // dropped. A filter that already shows it is left alone, because it is the
    // one the user typed.
    if (item->isHidden())
        m_filter->clear(); // triggers applyFilter, which shows every row again

    m_list->setCurrentItem(item);
    m_list->scrollToItem(item, QAbstractItemView::PositionAtCenter);
}

} // namespace pist
