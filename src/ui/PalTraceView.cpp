// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/PalTraceView.h"

#include "emu/HexFormat.h"
#include "ui/Appearance.h"

#include <QHeaderView>
#include <QLabel>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace pist {

namespace {

QString hexDigits(quint32 value, int digits)
{
    return QString::number(value, 16).rightJustified(digits, QLatin1Char('0')).toUpper();
}

} // namespace

PalTraceView::PalTraceView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_trace = new QToolButton(this);
    m_trace->setObjectName(QStringLiteral("paletteTraceButton"));
    m_trace->setText(tr("Trace"));
    m_trace->setCheckable(true);
    m_trace->setAutoRaise(true);
    m_trace->setToolTip(tr("Record palette writes. The list fills at each stop "
                           "and does not stop the machine."));
    layout->addWidget(m_trace, 0, Qt::AlignLeft);
    connect(m_trace, &QToolButton::toggled, this, &PalTraceView::traceToggled);

    m_summary = new QLabel(tr("Trace palette writes."), this);
    m_summary->setObjectName(QStringLiteral("paletteSummary"));
    m_summary->setWordWrap(true);
    m_summary->setToolTip(tr("The line and the position on it were taken when the color "
                             "was stored. 50 Hz and 60 Hz do not share a 512-cycle line."));
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, 9, this);
    m_table->setObjectName(QStringLiteral("paletteListing"));
    m_table->setHorizontalHeaderLabels({tr("#"), tr("PC"), tr("Register"), tr("Color"), tr("VBL"),
                                        tr("Line"), tr("Position"), tr("Frame"), tr("Source")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setToolTip(tr("Double-click a row to open its source line. "
                           "This does not move the machine."));
    appearance::markMono(m_table);
    layout->addWidget(m_table);

    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (row < 0 || row >= m_events.size())
            return;
        emit eventActivated(m_events.at(row).pc);
    });
}

void PalTraceView::setTrace(const PaletteLog &log, const QStringList &where)
{
    m_events = log.events;
    m_table->setRowCount(m_events.size());
    for (int i = 0; i < m_events.size(); ++i) {
        const PaletteEvent &event = m_events.at(i);
        const QString source = i < where.size() ? where.at(i) : QString();
        const QStringList cells = {QString::number(event.sequence),
                                   hex::hex32(event.pc),
                                   hexDigits(event.reg, 6),
                                   hexDigits(event.color, 4),
                                   QString::number(event.vbl),
                                   QString::number(event.hbl),
                                   QString::number(event.pos),
                                   QString::number(event.frame),
                                   source};
        for (int col = 0; col < cells.size(); ++col)
            m_table->setItem(i, col, new QTableWidgetItem(cells.at(col)));
    }
    m_summary->setText(tr("%1 palette writes").arg(m_events.size()));
}

void PalTraceView::clear()
{
    m_events.clear();
    m_table->setRowCount(0);
    m_summary->setText(tr("Trace palette writes."));
}

void PalTraceView::setTraceArmed(bool armed)
{
    if (!m_trace)
        return;
    const QSignalBlocker blocker(m_trace);
    m_trace->setChecked(armed);
}

} // namespace pist
