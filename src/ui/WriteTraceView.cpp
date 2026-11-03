// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/WriteTraceView.h"

#include "emu/HexFormat.h"
#include "ui/Appearance.h"

#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

QString originText(WriteOrigin origin)
{
    switch (origin) {
    case WriteOrigin::Cpu:
        return WriteTraceView::tr("CPU");
    case WriteOrigin::Blitter:
        return WriteTraceView::tr("Blitter");
    case WriteOrigin::Unknown:
        return WriteTraceView::tr("Unknown");
    }
    return WriteTraceView::tr("Unknown");
}

QString hexWidth(quint32 value, int width)
{
    const int digits = width * 2;
    return QString::number(value, 16).rightJustified(digits, QLatin1Char('0')).toUpper();
}

} // namespace

WriteTraceView::WriteTraceView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_summary = new QLabel(tr("Trace writes in a memory pane."), this);
    m_summary->setObjectName(QStringLiteral("writeTraceSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, 7, this);
    m_table->setObjectName(QStringLiteral("writeTraceListing"));
    m_table->setHorizontalHeaderLabels(
        {tr("#"), tr("Who"), tr("Address"), tr("Old"), tr("New"), tr("PC"), tr("Source")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setToolTip(tr("Double-click a CPU row to open its source line. "
                           "Right-click a CPU row for Previous Write, which returns "
                           "the machine to that instruction."));
    appearance::markMono(m_table);
    layout->addWidget(m_table);

    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || row >= m_events.size())
            return;
        const WriteEvent &event = m_events.at(row);
        emit eventActivated(event.pc, event.origin == WriteOrigin::Cpu, event.address);
    });
    m_table->viewport()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table->viewport(), &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_table->rowAt(pos.y());
        quint32 pc = 0;
        QMenu menu(this);
        auto *action = menu.addAction(tr("Previous Write"));
        action->setObjectName(QStringLiteral("previousWriteAction"));
        action->setEnabled(cpuPcAt(row, &pc));
        if (menu.exec(m_table->viewport()->mapToGlobal(pos)) != action || !action->isEnabled())
            return;
        emit previousWriteRequested(pc);
    });
}


bool WriteTraceView::cpuPcAt(int row, quint32 *pc) const
{
    if (!pc || row < 0 || row >= m_events.size())
        return false;
    const WriteEvent &event = m_events.at(row);
    if (event.origin != WriteOrigin::Cpu)
        return false;
    *pc = event.pc;
    return true;
}

void WriteTraceView::setTrace(const WriteTraceLog &log, const QStringList &where)
{
    m_events = log.events;
    m_table->setRowCount(m_events.size());
    for (int i = 0; i < m_events.size(); ++i) {
        const WriteEvent &event = m_events.at(i);
        const QString pc = event.origin == WriteOrigin::Cpu ? hex::hex32(event.pc) : QString();
        const QString source = i < where.size() ? where.at(i) : QString();
        const QStringList cells = {QString::number(event.sequence),
                                   originText(event.origin),
                                   hex::hex32(event.address),
                                   hexWidth(event.oldValue, event.width),
                                   hexWidth(event.newValue, event.width),
                                   pc,
                                   source};
        for (int col = 0; col < cells.size(); ++col)
            m_table->setItem(i, col, new QTableWidgetItem(cells.at(col)));
    }
    QString summary = tr("%1 writes from $%2 to $%3")
                          .arg(m_events.size())
                          .arg(hex::hex32(log.rangeLo), hex::hex32(log.rangeHi));
    if (log.dropped)
        summary = tr("%1 writes kept, %2 dropped, from $%3 to $%4. The list is the oldest.")
                      .arg(m_events.size())
                      .arg(log.dropped)
                      .arg(hex::hex32(log.rangeLo), hex::hex32(log.rangeHi));
    m_summary->setText(summary);
}

void WriteTraceView::clear()
{
    m_events.clear();
    m_table->setRowCount(0);
    m_summary->setText(tr("Trace writes in a memory pane."));
}

} // namespace pist
