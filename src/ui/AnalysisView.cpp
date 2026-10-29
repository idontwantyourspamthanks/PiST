// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/AnalysisView.h"

#include "analysis/ProgramTrace.h"
#include "ui/Appearance.h"

#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

AnalysisView::AnalysisView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("analysisView"));

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("analysisSummary"));
    m_summary->setWordWrap(true);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_listing = new QTableWidget(this);
    m_listing->setObjectName(QStringLiteral("analysisListing"));
    m_listing->setColumnCount(5);
    m_listing->setHorizontalHeaderLabels({tr("Address"), tr("Label"), tr("Bytes"), tr("Instruction"),
                                          tr("Note")});
    m_listing->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listing->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_listing->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listing->verticalHeader()->hide();
    m_listing->setShowGrid(false);
    m_listing->setAlternatingRowColors(true);
    m_listing->horizontalHeader()->setStretchLastSection(false);
    m_listing->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    appearance::markMono(m_listing);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_summary);
    layout->addWidget(m_listing, 1);
}

void AnalysisView::setIdentity(const QString &path, const QString &name)
{
    m_path = path;
    if (!name.isEmpty())
        m_name = name;
}

QString AnalysisView::filePath() const
{
    return m_path;
}

QString AnalysisView::displayName() const
{
    return m_name.isEmpty() ? tr("Analysis") : m_name;
}

void AnalysisView::setBytes(const QByteArray &bytes)
{
    const ProgramListing listing = listProgram(bytes);
    m_summary->setText(listing.error.isEmpty() ? listing.summary : listing.error);
    m_listing->setRowCount(listing.rows.size());
    for (int row = 0; row < listing.rows.size(); ++row) {
        const ListingRow &source = listing.rows.at(row);
        const QStringList cells = {
            QStringLiteral("$%1").arg(source.address, 0, 16),
            source.label,
            source.bytes,
            source.text,
            source.note,
        };
        for (int column = 0; column < cells.size(); ++column) {
            auto *item = new QTableWidgetItem(cells.at(column));
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_listing->setItem(row, column, item);
        }
    }
    m_listing->resizeColumnsToContents();
    m_listing->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
}

} // namespace pist
