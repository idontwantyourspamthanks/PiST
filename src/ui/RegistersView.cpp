// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

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
        m_table->setItem(i, 0, new QTableWidgetItem(QStringLiteral("D%1").arg(i)));
        m_table->setItem(i, 1, new QTableWidgetItem(QString()));
        m_table->setItem(i, 2, new QTableWidgetItem(QStringLiteral("A%1").arg(i)));
        m_table->setItem(i, 3, new QTableWidgetItem(QString()));
    }

    layout->addWidget(m_table, 0, 0, 1, 2);

    m_flags = new QTableWidget(4, 2, this);
    m_flags->verticalHeader()->setVisible(false);
    m_flags->horizontalHeader()->setVisible(false);
    m_flags->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_flags->setSelectionMode(QAbstractItemView::NoSelection);
    m_flags->setShowGrid(false);
    m_flags->setFocusPolicy(Qt::NoFocus);

    const QStringList names = {QStringLiteral("PC"), QStringLiteral("SR"),
                               QStringLiteral("USP"), QStringLiteral("ISP")};
    for (int i = 0; i < names.size(); ++i) {
        m_flags->setItem(i, 0, new QTableWidgetItem(names.at(i)));
        m_flags->setItem(i, 1, new QTableWidgetItem(QString()));
    }
    layout->addWidget(m_flags, 1, 0, 1, 2);

    m_table->resizeColumnsToContents();
    m_flags->resizeColumnsToContents();
}

void RegistersView::setValue(int row, int column, quint32 value)
{
    if (auto *item = m_table->item(row, column))
        item->setText(QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper());
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

    m_flags->resizeColumnsToContents();
}

void RegistersView::clear()
{
    for (int i = 0; i < 8; ++i) {
        m_table->item(i, 1)->setText(QString());
        m_table->item(i, 3)->setText(QString());
    }
    for (int i = 0; i < 4; ++i)
        m_flags->item(i, 1)->setText(QString());
}

} // namespace pist
