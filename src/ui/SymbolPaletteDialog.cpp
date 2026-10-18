// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SymbolPaletteDialog.h"

#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pist {
namespace {

/// The palette is a jump list, not a report: it shows the best matches for what is
/// typed, and 200 of them is more than anyone scrolls.
constexpr int kMaxRows = 200;

} // namespace

SymbolPaletteDialog::SymbolPaletteDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("symbolPalette"));
    setWindowTitle(tr("Go to Symbol"));
    // A jump list is a gesture, not a step that blocks the window: the window opens
    // the file the palette names.
    setModal(false);
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);

    m_filter = new QLineEdit(this);
    m_filter->setObjectName(QStringLiteral("symbolPaletteFilter"));
    m_filter->setPlaceholderText(tr("Symbol name"));
    m_filter->setClearButtonEnabled(true);

    m_list = new QTreeWidget(this);
    m_list->setObjectName(QStringLiteral("symbolPaletteList"));
    m_list->setHeaderLabels({tr("Symbol"), tr("Kind"), tr("Defined at")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->header()->setStretchLastSection(true);

    m_empty = new QLabel(this);
    m_empty->setObjectName(QStringLiteral("symbolPaletteEmpty"));
    m_empty->setWordWrap(true);
    m_empty->hide();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);
    layout->addWidget(m_filter);
    layout->addWidget(m_empty);
    layout->addWidget(m_list, 1);
    resize(520, 360);

    connect(m_filter, &QLineEdit::textChanged, this, &SymbolPaletteDialog::refresh);
    connect(m_filter, &QLineEdit::returnPressed, this, &SymbolPaletteDialog::activateCurrent);
    connect(m_list, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *, int) { activateCurrent(); });
    // Up/Down belong to the list even while the filter has the caret: this is a
    // jump list, and the arrow keys are how you pick from one.
    m_filter->installEventFilter(this);
}

void SymbolPaletteDialog::setSymbols(const QList<CompletionItem> &candidates)
{
    QList<CompletionItem> symbols;
    for (const CompletionItem &candidate : candidates) {
        if (candidate.kind != CompletionKind::Label && candidate.kind != CompletionKind::Equate)
            continue;
        if (candidate.file.isEmpty() || candidate.line <= 0)
            continue; // nothing to jump to
        symbols.append(candidate);
    }
    m_model.setItems(symbols);
    refresh();
}

void SymbolPaletteDialog::focusFilter()
{
    m_filter->selectAll();
    m_filter->setFocus();
}

void SymbolPaletteDialog::refresh()
{
    const QList<CompletionItem> matches = m_model.matches(m_filter->text(), kMaxRows);
    m_list->clear();
    for (const CompletionItem &match : matches) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setText(0, match.text);
        item->setText(1, match.kind == CompletionKind::Equate ? tr("equate") : tr("label"));
        item->setText(2, QStringLiteral("%1:%2").arg(match.file).arg(match.line));
        item->setData(0, Qt::UserRole, match.file);
        item->setData(0, Qt::UserRole + 1, match.line);
    }
    if (m_list->topLevelItemCount() > 0)
        m_list->setCurrentItem(m_list->topLevelItem(0));

    // An empty palette says *why* it is empty: a project with no symbols yet and a
    // filter that matches nothing are different answers, and neither is a guess.
    const bool noSymbols = m_model.items().isEmpty();
    m_empty->setText(noSymbols
                         ? tr("No symbols yet — build the project to list its labels and equates.")
                         : tr("No symbol matches “%1”.").arg(m_filter->text()));
    m_empty->setVisible(matches.isEmpty());
    m_list->setVisible(!matches.isEmpty());
}

void SymbolPaletteDialog::activateCurrent()
{
    QTreeWidgetItem *item = m_list->currentItem();
    if (!item && m_list->topLevelItemCount() > 0)
        item = m_list->topLevelItem(0);
    if (!item)
        return;

    const QString file = item->data(0, Qt::UserRole).toString();
    const int line = item->data(0, Qt::UserRole + 1).toInt();
    if (file.isEmpty() || line <= 0)
        return;

    hide();
    emit symbolActivated(file, line);
}

bool SymbolPaletteDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_filter && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            const int count = m_list->topLevelItemCount();
            if (count > 0) {
                int row = m_list->currentItem() ? m_list->currentIndex().row() : -1;
                row += key->key() == Qt::Key_Down ? 1 : -1;
                row = ((row % count) + count) % count;
                m_list->setCurrentItem(m_list->topLevelItem(row));
            }
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

} // namespace pist
