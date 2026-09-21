// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ProfilerView.h"

#include "build/ProgramLineMap.h"
#include "ui/Appearance.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

namespace pist {

namespace {

constexpr int kColLine = 0;
constexpr int kColCount = 1;
constexpr int kColPercent = 2;

} // namespace

ProfilerView::ProfilerView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(4);
    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("profilerFilter"));
    m_filter->setPlaceholderText(tr("Filter by line number"));
    m_filter->setClearButtonEnabled(true);
    filterRow->addWidget(m_filter, 1);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("profilerStatus"));
    filterRow->addWidget(m_status);
    layout->addLayout(filterRow);

    m_table = new QTableWidget(0, 3, this);
    m_table->setObjectName(QStringLiteral("profilerTable"));
    m_table->setHorizontalHeaderLabels({tr("Source line"), tr("Count"), tr("%")});
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    appearance::markMono(m_table);
    layout->addWidget(m_table, 1);

    // Double-click is what every other list in the IDE uses for "go there"
    // (BreakpointPanel, StackView, MemoryView), so a hot line behaves the same.
    connect(m_table, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
                if (!item)
                    return;
                const int line = lineAtRow(item->row());
                if (line > 0)
                    emit lineActivated(line);
            });
    connect(m_filter, &QLineEdit::textChanged, this, &ProfilerView::applyFilter);

    // Until the first profile arrives, the status line is the manual: the
    // choreography is short, but nobody should have to find it in a tooltip.
    clear();
}

void ProfilerView::setActions(QAction *start, QAction *stop)
{
    auto *row = m_filter->parentWidget()->layout();
    for (QAction *action : {start, stop}) {
        auto *button = new QToolButton(this);
        button->setDefaultAction(action);
        row->addWidget(button);
    }
}

void ProfilerView::clear()
{
    m_lines.clear();
    m_unmapped = 0;
    m_totalCount = 0;
    m_table->setRowCount(0);
    m_status->setText(tr("Set a breakpoint where measuring ends, Profile Start, "
                         "continue — the hot lines appear here."));
}

void ProfilerView::setProfile(const ProfileData &profile, const ProgramLineMap *map,
                              const QString &sourceFile)
{
    clear();
    if (profile.lines.isEmpty()) {
        m_status->setText(tr("no profile"));
        return;
    }
    m_totalCount = profile.totalCount;

    // Without a resolved map there is no address-to-line mapping at all, which
    // is the normal state before the program has been built and run. Saying so
    // is the difference between "the profiler found nothing" and "profiling is
    // not wired up for this program yet".
    if (!map || map->isEmpty() || !map->isResolved()) {
        m_status->setText(tr("%1 samples, no source map").arg(m_totalCount));
        return;
    }

    // Aggregate per source line: a line's cost is the sum of its instructions,
    // which is what a developer optimises. Addresses belonging to other files
    // of a multi-module program are counted as unmapped for *this* view.
    QHash<int, LineCost> byLine;
    for (const ProfileLine &entry : profile.lines) {
        LineMap::Address where;
        if (!map->lineFor(entry.address, &where) || where.line <= 0
            || !LineMap::sameSource(where.file, sourceFile)) {
            ++m_unmapped;
            continue;
        }
        LineCost &cost = byLine[where.line];
        cost.line = where.line;
        cost.count += entry.count;
    }

    m_lines.reserve(byLine.size());
    for (auto it = byLine.constBegin(); it != byLine.constEnd(); ++it)
        m_lines.append(it.value());
    std::sort(m_lines.begin(), m_lines.end(), [](const LineCost &a, const LineCost &b) {
        if (a.count != b.count)
            return a.count > b.count; // hottest first
        return a.line < b.line;       // stable within a tie
    });

    populate();
}

QHash<int, quint64> ProfilerView::lineCounts() const
{
    QHash<int, quint64> counts;
    counts.reserve(m_lines.size());
    for (const LineCost &cost : m_lines)
        counts.insert(cost.line, cost.count);
    return counts;
}

void ProfilerView::populate()
{
    m_table->setRowCount(m_lines.size());

    for (int row = 0; row < m_lines.size(); ++row) {
        const LineCost &cost = m_lines.at(row);

        auto *lineItem = new QTableWidgetItem(QString::number(cost.line));
        lineItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        // The row's line number rides on the item, so an activation resolves the
        // line the user clicked rather than a row index into an unknown list —
        // which matters because the filter hides rows.
        lineItem->setData(Qt::UserRole, cost.line);

        auto *countItem = new QTableWidgetItem(QString::number(cost.count));
        countItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        // Percentage of the whole run, so a line's share is comparable with
        // Hatari's own `profile counts` output. The count column is what the
        // gutter heat is scaled from.
        const double percent = m_totalCount
                                   ? 100.0 * double(cost.count) / double(m_totalCount)
                                   : 0.0;
        auto *percentItem = new QTableWidgetItem(QString::number(percent, 'f', 2));
        percentItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_table->setItem(row, kColLine, lineItem);
        m_table->setItem(row, kColCount, countItem);
        m_table->setItem(row, kColPercent, percentItem);
    }
    m_table->resizeColumnToContents(kColLine);
    m_table->resizeColumnToContents(kColCount);

    applyFilter();
}

void ProfilerView::applyFilter()
{
    const QString needle = m_filter->text().trimmed();
    int shown = 0;
    for (int row = 0; row < m_lines.size(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, kColLine);
        const bool match = needle.isEmpty()
                           || (item && item->text().contains(needle));
        m_table->setRowHidden(row, !match);
        if (match)
            ++shown;
    }

    QString status;
    if (m_lines.isEmpty()) {
        status = m_unmapped ? tr("%1 addresses, none in this file").arg(m_unmapped)
                            : tr("no profiled instructions in this file");
    } else {
        status = needle.isEmpty() ? tr("%1 hot lines").arg(m_lines.size())
                                  : tr("%1 of %2 lines").arg(shown).arg(m_lines.size());
        if (m_unmapped)
            status += tr(", %1 addresses unmapped").arg(m_unmapped);
    }
    m_status->setText(status);
}

int ProfilerView::lineAtRow(int row) const
{
    if (row < 0 || row >= m_table->rowCount())
        return 0;
    const QTableWidgetItem *item = m_table->item(row, kColLine);
    return item ? item->data(Qt::UserRole).toInt() : 0;
}

void ProfilerView::applyAppearance()
{
    appearance::markMono(m_table);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    applyFilter();
}

} // namespace pist
