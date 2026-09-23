// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/RegistersView.h"

#include "emu/HexFormat.h"
#include "ui/Appearance.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QSignalBlocker>
#include <QTableWidget>

namespace pist {

RegistersView::RegistersView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableWidget(8, 4, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Reg"), tr("Value"), tr("Reg"), tr("Value")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setAlternatingRowColors(true);
    appearance::markMono(m_table);

    for (int i = 0; i < 8; ++i) {
        auto *dName = new QTableWidgetItem(QStringLiteral("D%1").arg(i));
        auto *dVal = new QTableWidgetItem(QString());
        auto *aName = new QTableWidgetItem(QStringLiteral("A%1").arg(i));
        auto *aVal = new QTableWidgetItem(QString());
        // The value cells are editable; the register-name cells are not.
        dName->setFlags(dName->flags() & ~Qt::ItemIsEditable);
        aName->setFlags(aName->flags() & ~Qt::ItemIsEditable);
        dVal->setData(Qt::UserRole, QStringLiteral("d%1").arg(i));
        aVal->setData(Qt::UserRole, QStringLiteral("a%1").arg(i));
        m_table->setItem(i, 0, dName);
        m_table->setItem(i, 1, dVal);
        m_table->setItem(i, 2, aName);
        m_table->setItem(i, 3, aVal);
    }

    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table, 0, 0, 1, 2);

    m_flags = new QTableWidget(4, 2, this);
    m_flags->verticalHeader()->setVisible(false);
    m_flags->horizontalHeader()->setVisible(false);
    m_flags->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_flags->setSelectionMode(QAbstractItemView::NoSelection);
    m_flags->setShowGrid(false);
    m_flags->setFocusPolicy(Qt::NoFocus);
    m_flags->setAlternatingRowColors(true);
    appearance::markMono(m_flags);

    // USP and ISP are not settable by name in Hatari (its own todo notes SP/SSP
    // register names are unimplemented), so only PC and SR get editable cells.
    const QStringList names = {QStringLiteral("PC"), QStringLiteral("SR"),
                               QStringLiteral("USP"), QStringLiteral("ISP")};
    const QStringList regNames = {QStringLiteral("pc"), QStringLiteral("sr"),
                                  QString(), QString()};
    for (int i = 0; i < names.size(); ++i) {
        auto *name = new QTableWidgetItem(names.at(i));
        auto *val = new QTableWidgetItem(QString());
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        if (regNames.at(i).isEmpty())
            val->setFlags(val->flags() & ~Qt::ItemIsEditable);
        else
            val->setData(Qt::UserRole, regNames.at(i));
        m_flags->setItem(i, 0, name);
        m_flags->setItem(i, 1, val);
    }
    layout->addWidget(m_flags, 1, 0, 1, 2);

    m_flagRow = new QWidget(this);
    m_flagRow->setObjectName(QStringLiteral("flagChips"));
    auto *chips = new QHBoxLayout(m_flagRow);
    chips->setContentsMargins(4, 2, 4, 2);
    chips->setSpacing(4);
    const QStringList flagNames = {QStringLiteral("X"), QStringLiteral("N"),
                                   QStringLiteral("Z"), QStringLiteral("V"),
                                   QStringLiteral("C")};
    for (int i = 0; i < flagNames.size(); ++i) {
        auto *chip = new QLabel(flagNames.at(i), m_flagRow);
        chip->setObjectName(QStringLiteral("flagChip") + flagNames.at(i));
        chip->setAlignment(Qt::AlignCenter);
        chip->setMinimumWidth(22);
        m_flagChip[i] = chip;
        chips->addWidget(chip);
    }
    chips->addStretch();
    layout->addWidget(m_flagRow, 2, 0, 1, 2);

    m_placeholder = new QLabel(tr("Registers appear when the machine stops"), this);
    m_placeholder->setObjectName(QStringLiteral("registersPlaceholder"));
    m_placeholder->setWordWrap(true);
    m_placeholder->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_placeholder, 3, 0, 1, 2);

    // A blank table looks like a dump that failed. Nothing is known until a stop.
    m_table->hide();
    m_flags->hide();
    m_flagRow->hide();

    m_table->resizeColumnsToContents();
    m_flags->resizeColumnsToContents();

    connect(m_table, &QTableWidget::itemChanged, this, &RegistersView::onCellEdited);
    connect(m_flags, &QTableWidget::itemChanged, this, &RegistersView::onCellEdited);
}

void RegistersView::setValue(int row, int column, quint32 value, const QColor &changedInk)
{
    auto *item = m_table->item(row, column);
    if (!item)
        return;
    const QString text = hex::hex32(value);
    const bool changed = !item->text().isEmpty() && item->text() != text;
    const QSignalBlocker blocker(m_table);
    item->setText(text);
    item->setForeground(changed ? changedInk : palette().color(QPalette::Text));
    m_lastValues[item] = text;
}

void RegistersView::setState(const MachineState &state)
{
    if (!state.regs.valid)
        return;

    m_lastState = state;
    m_haveState = true;

    // One palette for the whole update: every cell and flag below paints from
    // it, and asking for it per cell rebuilt it (QSettings reads included) for
    // an answer that cannot change inside one call.
    const appearance::Colors c = appearance::colors();

    for (int i = 0; i < 8; ++i) {
        setValue(i, 1, state.regs.d[i], c.changed);
        setValue(i, 3, state.regs.a[i], c.changed);
    }

    const QSignalBlocker flagsBlocker(m_flags);

    auto setFlag = [&](int row, const QString &text) {
        auto *item = m_flags->item(row, 1);
        if (!item)
            return;
        const bool changed = !item->text().isEmpty() && item->text() != text;
        item->setText(text);
        item->setForeground(changed ? c.changed : palette().color(QPalette::Text));
        m_lastValues[item] = text;
    };

    setFlag(0, hex::hex32(state.pc));
    setFlag(1, QStringLiteral("%1").arg(state.regs.sr, 4, 16, QLatin1Char('0')).toUpper());
    const bool flags[5] = {state.regs.flagX, state.regs.flagN, state.regs.flagZ,
                           state.regs.flagV, state.regs.flagC};
    for (int i = 0; i < 5; ++i) {
        QLabel *chip = m_flagChip[i];
        if (!chip)
            continue;
        chip->setStyleSheet(QStringLiteral(
            "QLabel { color: %1; background: %2; border-radius: 3px; padding: 0 4px; }")
                                .arg(flags[i] ? QStringLiteral("#07140a") : c.muted.name(),
                                     flags[i] ? QStringLiteral("#2fa04c") : QStringLiteral("transparent")));
    }
    m_placeholder->hide();
    m_table->show();
    m_flags->show();
    m_flagRow->show();
    setFlag(2, hex::hex32(state.regs.usp));
    setFlag(3, hex::hex32(state.regs.isp));

    m_flags->resizeColumnsToContents();
}

void RegistersView::applyAppearance()
{
    appearance::markMono(m_table);
    appearance::markMono(m_flags);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    m_flags->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    if (m_haveState)
        setState(m_lastState);
}

void RegistersView::setEditingEnabled(bool enabled)
{
    const auto triggers = enabled
        ? (QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed)
        : QAbstractItemView::NoEditTriggers;
    m_table->setEditTriggers(triggers);
    m_flags->setEditTriggers(triggers);
}

void RegistersView::onCellEdited(QTableWidgetItem *item)
{
    if (!item)
        return;
    const QString reg = item->data(Qt::UserRole).toString();
    if (reg.isEmpty())
        return; // a read-only cell; nothing to do

    // Parse the committed value as hex, tolerating $ / 0x / bare forms.
    QString text = item->text().trimmed();
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text = text.mid(2);
    if (text.startsWith(QLatin1Char('$')))
        text = text.mid(1);
    bool ok = false;
    const quint32 value = text.toUInt(&ok, 16);
    if (!ok) {
        // Reject bad input by restoring the item to its last valid value.
        m_table->blockSignals(true);
        m_flags->blockSignals(true);
        item->setText(m_lastValues.value(item));
        m_table->blockSignals(false);
        m_flags->blockSignals(false);
        return;
    }
    m_lastValues[item] = item->text();
    emit registerEdited(reg, value);
}

} // namespace pist
