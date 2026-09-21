// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/DisassemblyView.h"

#include "ui/Appearance.h"

#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

DisassemblyView::DisassemblyView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(
        {QStringLiteral("Address"), QStringLiteral("Bytes"), QStringLiteral("Instruction")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    appearance::markMono(m_table);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        const QTableWidgetItem *address = m_table->item(row, 0);
        if (!address)
            return;
        bool ok = false;
        const quint32 value = address->data(Qt::UserRole).toUInt(&ok);
        if (ok)
            emit addressActivated(value);
    });

    layout->addWidget(m_table);
}

void DisassemblyView::setState(const MachineState &state)
{
    m_pc = state.pc;
    m_lastState = state;
    m_haveState = true;
    const auto &lines = state.disassembly;
    m_table->setRowCount(lines.size());
    const appearance::Colors theme = appearance::colors();

    for (int i = 0; i < lines.size(); ++i) {
        const DisasmLine &dl = lines.at(i);
        const QString address =
            QStringLiteral("%1").arg(dl.address, 8, 16, QLatin1Char('0')).toUpper();

        auto *addrItem = new QTableWidgetItem(address);
        addrItem->setData(Qt::UserRole, dl.address);
        auto *bytesItem = new QTableWidgetItem(dl.bytes);
        addrItem->setForeground(theme.address);
        bytesItem->setForeground(theme.hex);

        // Symbol labels are attached to the line they precede.
        const QString text =
            dl.label.isEmpty() ? dl.instruction
                               : QStringLiteral("%1:\t%2").arg(dl.label, dl.instruction);
        auto *textItem = new QTableWidgetItem(text);
        if (!dl.label.isEmpty())
            textItem->setForeground(theme.label);

        m_table->setItem(i, 0, addrItem);
        m_table->setItem(i, 1, bytesItem);
        m_table->setItem(i, 2, textItem);

        if (dl.isCurrentPc) {
            for (int col = 0; col < 3; ++col) {
                m_table->item(i, col)->setBackground(theme.pcRow);
                QFont f = m_table->item(i, col)->font();
                f.setBold(true);
                m_table->item(i, col)->setFont(f);
            }
        }
    }

    m_table->resizeColumnsToContents();
    goToAddress(state.pc);
}

void DisassemblyView::applyAppearance()
{
    appearance::markMono(m_table);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    if (m_haveState)
        setState(m_lastState);
}

void DisassemblyView::goToAddress(quint32 address)
{
    for (int i = 0; i < m_table->rowCount(); ++i) {
        if (!m_table->item(i, 0))
            continue;
        if (m_table->item(i, 0)->text().toUInt(nullptr, 16) == address) {
            m_table->scrollToItem(m_table->item(i, 0), QAbstractItemView::PositionAtCenter);
            return;
        }
    }
}

} // namespace pist
