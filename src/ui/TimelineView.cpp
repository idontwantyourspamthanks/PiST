// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/TimelineView.h"

#include "ui/Appearance.h"

#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace pist {

TimelineView::TimelineView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *stopsLabel = new QLabel(tr("Recorded stops"), this);
    layout->addWidget(stopsLabel);
    m_stops = new QListWidget(this);
    m_stops->setObjectName(QStringLiteral("timelineStops"));
    appearance::markMono(m_stops);
    layout->addWidget(m_stops, 1);

    auto *instrLabel = new QLabel(tr("Recent instructions"), this);
    layout->addWidget(instrLabel);
    m_instructions = new QListWidget(this);
    m_instructions->setObjectName(QStringLiteral("timelineInstructions"));
    appearance::markMono(m_instructions);
    layout->addWidget(m_instructions, 2);

    connect(m_stops, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        emit stopActivated(item->data(Qt::UserRole).toInt());
    });
    connect(m_instructions, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        const int row = m_instructions->row(item);
        if (row < 0 || row >= m_pcs.size())
            return;
        emit instructionActivated(m_pcs.at(row));
    });
}

void TimelineView::setStops(const QStringList &labels)
{
    m_stops->clear();
    for (int i = 0; i < labels.size(); ++i) {
        auto *item = new QListWidgetItem(labels.at(i), m_stops);
        item->setData(Qt::UserRole, i);
    }
}

void TimelineView::clear()
{
    m_pcs.clear();
    m_stops->clear();
    m_instructions->clear();
}

void TimelineView::setInstructions(const QStringList &labels, const QList<quint32> &pcs)
{
    m_pcs = pcs;
    m_instructions->clear();
    for (const QString &label : labels)
        m_instructions->addItem(label);
}

} // namespace pist
