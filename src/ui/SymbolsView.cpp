// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SymbolsView.h"

#include "build/ProgramLineMap.h"
#include "emu/HexFormat.h"
#include "ui/Appearance.h"

#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pist {

SymbolsView::SymbolsView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("symbolsFilter"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(tr("Filter by name"));
    m_filter->setToolTip(tr("Show only symbols whose name contains this text."));
    connect(m_filter, &QLineEdit::textChanged, this, &SymbolsView::onFilterChanged);
    layout->addWidget(m_filter);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("symbolsTree"));
    m_tree->setHeaderLabels({tr("Name"), tr("Address"), tr("Location")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAllColumnsShowFocus(true);
    // Symbol names are identifiers: the monospace face makes them scannable and
    // matches the editor the user came from.
    appearance::markMono(m_tree);

    // The name is the one column whose width is unpredictable, so it takes the
    // spare space; address and location are fixed-width by nature.
    QHeaderView *header = m_tree->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(kColName, QHeaderView::Stretch);
    header->setSectionResizeMode(kColAddress, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(kColLocation, QHeaderView::ResizeToContents);

    // `activated` covers both the double-click and Enter, so the two gestures
    // cannot drift apart.
    connect(m_tree, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { onItemActivated(item); });
    layout->addWidget(m_tree, 1);

    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("symbolsCount"));
    m_count->setContentsMargins(2, 0, 2, 0);
    layout->addWidget(m_count);

    updateCount();
}

void SymbolsView::setSymbols(const QVector<SymbolEntry> &symbols, const ProgramLineMap *map)
{
    const appearance::Colors theme = appearance::colors();
    m_tree->setUpdatesEnabled(false);
    m_tree->clear();

    for (const SymbolEntry &symbol : symbols) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(kColName, symbol.name);
        item->setForeground(kColName, theme.label);

        // Both columns need a definition site: a symbol the listing's table
        // knows but no source line defines has neither a location to show nor a
        // line for the map to resolve, so both stay blank together.
        const bool positioned = symbol.line > 0 && !symbol.file.isEmpty();
        quint32 address = 0;
        const bool resolved = map && positioned
                           && map->addressFor(symbol.file, symbol.line, &address);
        if (resolved) {
            item->setText(kColAddress, hex::hexAddr(address));
            item->setForeground(kColAddress, theme.address);
        }

        if (positioned) {
            item->setText(kColLocation, QStringLiteral("%1:%2")
                                            .arg(QFileInfo(symbol.file).fileName())
                                            .arg(symbol.line));
            item->setToolTip(kColLocation, symbol.file);
        }

        // A symbol with no source position — a command-line define, or a name a
        // macro built — has nothing to navigate to, so activation is made inert
        // rather than appearing to do something and doing nothing.
        item->setData(kColName, Qt::UserRole, symbol.file);
        item->setData(kColName, Qt::UserRole + 1, symbol.line);
    }

    m_tree->setUpdatesEnabled(true);
    onFilterChanged(m_filter->text());
}

void SymbolsView::onFilterChanged(const QString &text)
{
    const QString needle = text.trimmed();
    int shown = 0;

    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_tree->topLevelItem(row);
        const bool match = needle.isEmpty()
                        || item->text(kColName).contains(needle, Qt::CaseInsensitive);
        item->setHidden(!match);
        if (match)
            ++shown;
    }

    m_shown = shown;
    updateCount();
}

void SymbolsView::onItemActivated(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const QString file = item->data(kColName, Qt::UserRole).toString();
    const int line = item->data(kColName, Qt::UserRole + 1).toInt();
    if (!file.isEmpty() && line > 0)
        emit symbolActivated(file, line);
}

void SymbolsView::updateCount()
{
    const int total = m_tree->topLevelItemCount();
    // The count is the only signal that a filter is hiding rows; without it a
    // narrowed list looks like a program with few symbols.
    m_count->setText(m_shown == total ? tr("%1 symbols").arg(total)
                                      : tr("%1 of %2 symbols").arg(m_shown).arg(total));
}

void SymbolsView::applyAppearance()
{
    appearance::markMono(m_tree);
    // The map is not kept (it belongs to the build, not to this view), so the
    // rows are re-coloured from what they already carry rather than re-resolved.
    const appearance::Colors theme = appearance::colors();
    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_tree->topLevelItem(row);
        item->setForeground(kColName, theme.label);
        if (!item->text(kColAddress).isEmpty())
            item->setForeground(kColAddress, theme.address);
    }
}

} // namespace pist
