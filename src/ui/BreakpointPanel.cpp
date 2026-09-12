// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/BreakpointPanel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

constexpr int kColLocation = 0;
constexpr int kColState = 1;
constexpr int kColCondition = 2;

} // namespace

BreakpointPanel::BreakpointPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Location"), tr("State"), tr("Condition")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);

    connect(m_table, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
                if (!item)
                    return;
                const int row = item->row();
                if (row < 0)
                    return;
                if (row < m_breakpoints.size()) {
                    const Breakpoint &bp = m_breakpoints.at(row);
                    emit breakpointActivated(bp.file, bp.line);
                } else if (row - m_breakpoints.size() < m_watchpoints.size()) {
                    emit watchpointActivated(m_watchpoints.at(row - m_breakpoints.size()).address);
                }
            });

    layout->addWidget(m_table);

    auto *buttons = new QHBoxLayout;
    auto *remove = new QPushButton(tr("Remove"), this);
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row < 0)
            return;
        if (row < m_breakpoints.size()) {
            const Breakpoint &bp = m_breakpoints.at(row);
            emit removeRequested(bp.file, bp.line);
        } else if (row - m_breakpoints.size() < m_watchpoints.size()) {
            emit watchpointRemoveRequested(row - m_breakpoints.size());
        }
    });
    buttons->addWidget(remove);

    m_clearButton = new QPushButton(tr("Clear all"), this);
    connect(m_clearButton, &QPushButton::clicked, this, &BreakpointPanel::clearRequested);
    buttons->addWidget(m_clearButton);
    buttons->addStretch(1);
    layout->addLayout(buttons);
}

void BreakpointPanel::setBreakpoints(const QList<Breakpoint> &breakpoints)
{
    m_breakpoints = breakpoints;
    refresh();
}

void BreakpointPanel::setWatchpoints(const QList<Watchpoint> &watchpoints)
{
    m_watchpoints = watchpoints;
    refresh();
}

void BreakpointPanel::setResolvable(bool resolvable)
{
    if (m_resolvable == resolvable)
        return;
    m_resolvable = resolvable;
    refresh();
}

void BreakpointPanel::refresh()
{
    m_table->setRowCount(m_breakpoints.size());

    for (int row = 0; row < m_breakpoints.size(); ++row) {
        const Breakpoint &bp = m_breakpoints.at(row);

        auto *location = new QTableWidgetItem(bp.label());
        m_table->setItem(row, kColLocation, location);

        // The state column exists to make the silent cases visible: a breakpoint
        // on a line that emits nothing will never fire, and before the program
        // starts none of them can be resolved.
        QString state;
        QColor colour;
        if (!bp.enabled) {
            state = tr("disabled");
            colour = QColor(0x80, 0x80, 0x80);
        } else if (bp.resolved) {
            state = QStringLiteral("$%1").arg(bp.address, 0, 16).toUpper();
            colour = QColor(0x20, 0x70, 0x30);
        } else if (m_resolvable) {
            state = tr("no code on this line");
            colour = QColor(0xc0, 0x60, 0x00);
        } else {
            state = tr("pending (run to resolve)");
            colour = QColor(0x80, 0x80, 0x80);
        }
        auto *stateItem = new QTableWidgetItem(state);
        stateItem->setForeground(colour);
        m_table->setItem(row, kColState, stateItem);

        m_table->setItem(row, kColCondition, new QTableWidgetItem(bp.condition));
    }

    // Watchpoints are listed below the breakpoints. They are always resolvable
    // (an address is an address), so the state column just shows the width.
    const int firstWatch = m_breakpoints.size();
    m_table->setRowCount(firstWatch + m_watchpoints.size());
    for (int i = 0; i < m_watchpoints.size(); ++i) {
        const Watchpoint &wp = m_watchpoints.at(i);
        const int row = firstWatch + i;
        m_table->setItem(row, kColLocation, new QTableWidgetItem(wp.label()));
        auto *state = new QTableWidgetItem(tr("armed"));
        state->setForeground(QColor(0x20, 0x70, 0x30));
        m_table->setItem(row, kColState, state);
        m_table->setItem(row, kColCondition, new QTableWidgetItem(wp.command().mid(2)));
    }

    m_table->resizeColumnsToContents();
    m_clearButton->setEnabled(!m_breakpoints.isEmpty() || !m_watchpoints.isEmpty());
}

} // namespace pist
