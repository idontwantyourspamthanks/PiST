// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/DisassemblyView.h"

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

    QFont mono = m_table->font();
    mono.setFamily(QStringLiteral("monospace"));
    m_table->setFont(mono);

    layout->addWidget(m_table);
}

void DisassemblyView::setState(const MachineState &state)
{
    m_pc = state.pc;
    const auto &lines = state.disassembly;
    m_table->setRowCount(lines.size());

    for (int i = 0; i < lines.size(); ++i) {
        const DisasmLine &dl = lines.at(i);
        const QString address =
            QStringLiteral("%1").arg(dl.address, 8, 16, QLatin1Char('0')).toUpper();

        m_table->setItem(i, 0, new QTableWidgetItem(address));
        m_table->setItem(i, 1, new QTableWidgetItem(dl.bytes));

        // Symbol labels are attached to the line they precede.
        const QString text =
            dl.label.isEmpty() ? dl.instruction
                               : QStringLiteral("%1:\t%2").arg(dl.label, dl.instruction);
        m_table->setItem(i, 2, new QTableWidgetItem(text));

        if (dl.isCurrentPc) {
            for (int c = 0; c < 3; ++c) {
                m_table->item(i, c)->setBackground(QColor(0xff, 0xf3, 0xc4));
                QFont f = m_table->item(i, c)->font();
                f.setBold(true);
                m_table->item(i, c)->setFont(f);
            }
        }
    }

    m_table->resizeColumnsToContents();
    goToAddress(state.pc);
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

void DisassemblyView::clear()
{
    m_table->setRowCount(0);
}

} // namespace pist
