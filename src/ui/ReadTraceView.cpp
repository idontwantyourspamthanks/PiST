// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ReadTraceView.h"

#include "emu/HexFormat.h"
#include "ui/Appearance.h"

#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

QString hexWidth(quint32 value, int width)
{
    const int digits = width * 2;
    return QString::number(value, 16).rightJustified(digits, QLatin1Char('0')).toUpper();
}

} // namespace

ReadTraceView::ReadTraceView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_summary = new QLabel(tr("Trace reads in a memory pane."), this);
    m_summary->setObjectName(QStringLiteral("readTraceSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, 5, this);
    m_table->setObjectName(QStringLiteral("readTraceListing"));
    m_table->setHorizontalHeaderLabels(
        {tr("#"), tr("Address"), tr("Value"), tr("PC"), tr("Source")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setToolTip(tr("Double-click a row to open its source line. "
                           "Right-click it for Previous Read, which returns "
                           "the machine to that instruction."));
    appearance::markMono(m_table);
    layout->addWidget(m_table);

    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        quint32 pc = 0;
        if (!cpuPcAt(row, &pc))
            return;
        emit eventActivated(pc, m_events.at(row).address);
    });
    m_table->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table->viewport(), &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_table->rowAt(pos.y());
        quint32 pc = 0;
        QMenu menu(this);
        auto *action = menu.addAction(tr("Previous Read"));
        action->setObjectName(QStringLiteral("previousReadAction"));
        action->setEnabled(cpuPcAt(row, &pc));
        if (menu.exec(m_table->viewport()->mapToGlobal(pos)) != action || !action->isEnabled())
            return;
        emit previousReadRequested(pc);
    });
}

bool ReadTraceView::cpuPcAt(int row, quint32 *pc) const
{
    if (!pc || row < 0 || row >= m_events.size())
        return false;
    *pc = m_events.at(row).pc;
    return true;
}

void ReadTraceView::setTrace(const ReadTraceLog &log, const QStringList &where)
{
    m_events = log.events;
    m_table->setRowCount(m_events.size());
    for (int i = 0; i < m_events.size(); ++i) {
        const ReadEvent &event = m_events.at(i);
        const QString source = i < where.size() ? where.at(i) : QString();
        const QStringList cells = {QString::number(event.sequence), hex::hex32(event.address),
                                   hexWidth(event.value, event.width), hex::hex32(event.pc), source};
        for (int col = 0; col < cells.size(); ++col)
            m_table->setItem(i, col, new QTableWidgetItem(cells.at(col)));
    }
    QString summary = tr("%1 reads from $%2 to $%3")
                          .arg(m_events.size())
                          .arg(hex::hex32(log.rangeLo), hex::hex32(log.rangeHi));
    if (log.dropped)
        summary = tr("%1 reads kept, %2 dropped, from $%3 to $%4. The list is the oldest.")
                      .arg(m_events.size())
                      .arg(log.dropped)
                      .arg(hex::hex32(log.rangeLo), hex::hex32(log.rangeHi));
    m_summary->setText(summary);
}

void ReadTraceView::clear()
{
    m_events.clear();
    m_table->setRowCount(0);
    m_summary->setText(tr("Trace reads in a memory pane."));
}

} // namespace pist
