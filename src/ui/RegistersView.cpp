// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/RegistersView.h"

#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>

namespace pist {

RegistersView::RegistersView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QGridLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableWidget(8, 4, this);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Reg"), QStringLiteral("Value"), QStringLiteral("Reg"), QStringLiteral("Value")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);

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

    m_table->resizeColumnsToContents();
    m_flags->resizeColumnsToContents();

    connect(m_table, &QTableWidget::itemChanged, this, &RegistersView::onCellEdited);
    connect(m_flags, &QTableWidget::itemChanged, this, &RegistersView::onCellEdited);
}

void RegistersView::setValue(int row, int column, quint32 value)
{
    if (auto *item = m_table->item(row, column)) {
        const QString text = QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
        item->setText(text);
        m_lastValues[item] = text;
    }
}

void RegistersView::setState(const MachineState &state)
{
    if (!state.regs.valid)
        return;

    for (int i = 0; i < 8; ++i) {
        setValue(i, 1, state.regs.d[i]);
        setValue(i, 3, state.regs.a[i]);
    }

    m_flags->item(0, 1)->setText(
        QStringLiteral("%1").arg(state.pc, 8, 16, QLatin1Char('0')).toUpper());
    m_flags->item(1, 1)->setText(
        QStringLiteral("%1  X%2 N%3 Z%4 V%5 C%6")
            .arg(state.regs.sr, 4, 16, QLatin1Char('0')).toUpper()
            .arg(state.regs.flagX ? 1 : 0)
            .arg(state.regs.flagN ? 1 : 0)
            .arg(state.regs.flagZ ? 1 : 0)
            .arg(state.regs.flagV ? 1 : 0)
            .arg(state.regs.flagC ? 1 : 0));
    m_flags->item(2, 1)->setText(
        QStringLiteral("%1").arg(state.regs.usp, 8, 16, QLatin1Char('0')).toUpper());
    m_flags->item(3, 1)->setText(
        QStringLiteral("%1").arg(state.regs.isp, 8, 16, QLatin1Char('0')).toUpper());

    for (int i = 0; i < 4; ++i) {
        if (auto *item = m_flags->item(i, 1))
            m_lastValues[item] = item->text();
    }

    m_flags->resizeColumnsToContents();
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
