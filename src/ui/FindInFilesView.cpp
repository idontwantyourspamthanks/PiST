// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FindInFilesView.h"

#include "ui/Appearance.h"

#include <QDir>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pist {
namespace {

/// How long the typing has to stop before the walk starts. Long enough that a
/// word typed at speed is one search, short enough that it feels like it follows.
constexpr int kDebounceMs = 250;

/// Item data roles for a results row. The rows carry their own facts rather than
/// an index into a side list — there is nothing to keep in step.
constexpr int kPathRole = Qt::UserRole;
/// The match's span within the preview string, as (column, length).
constexpr int kSpanRole = Qt::UserRole + 1;
/// How many hits this file row stands for, on the file row itself.
constexpr int kCountRole = Qt::UserRole + 2;

/// Paints a hit's line with the matched span marked in the same colour the
/// editor uses for its own find hits, so a match looks like a match in both
/// places. File rows carry no span and fall through to the ordinary painting.
class MatchSpanDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const QVariant span = index.data(kSpanRole);
        if (!span.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // Draw the row's text as usual, then lay the span over it: the delegate
        // is about a highlight, not about re-implementing item painting.
        QStyledItemDelegate::paint(painter, option, index);

        const QPoint where = span.toPoint();
        const QString text = index.data(Qt::DisplayRole).toString();
        if (where.x() < 0 || where.y() <= 0 || where.x() + where.y() > text.size())
            return;

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QFontMetrics metrics(opt.font);
        const QRect rect = opt.rect.adjusted(metrics.horizontalAdvance(text.left(where.x())) + 2, 0, 0, 0);
        const int width = metrics.horizontalAdvance(text.mid(where.x(), where.y()));
        QRect spanRect(rect.x(), rect.y(), width, rect.height());
        spanRect = spanRect.intersected(opt.rect);
        if (spanRect.isEmpty())
            return;

        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(appearance::colors().searchMatch);
        painter->drawRect(spanRect);
        painter->restore();
    }
};

} // namespace

FindInFilesView::FindInFilesView(QWidget *parent)
    : QWidget(parent)
    , m_model(new FindInFiles(this))
    , m_debounce(new QTimer(this))
{
    setObjectName(QStringLiteral("findInFilesView"));

    m_debounce->setSingleShot(true);
    m_debounce->setInterval(kDebounceMs);
    connect(m_debounce, &QTimer::timeout, this, &FindInFilesView::runQuery);

    auto *queryRow = new QHBoxLayout;
    queryRow->setSpacing(4);
    m_query = new QLineEdit(this);
    m_query->setObjectName(QStringLiteral("findInFilesQuery"));
    m_query->setPlaceholderText(tr("Find in the project's files"));
    m_query->setClearButtonEnabled(true);
    queryRow->addWidget(m_query, 1);

    const auto addToggle = [this, queryRow](QToolButton *&button, const QString &objectName,
                                            const QString &text, const QString &tip) {
        button = new QToolButton(this);
        button->setObjectName(objectName);
        button->setText(text);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setAutoRaise(true);
        queryRow->addWidget(button);
        connect(button, &QToolButton::toggled, this, [this] {
            m_debounce->stop();
            runQuery();
        });
    };
    // The same two options, spelled the same way, as the editor's find bar: the
    // counts are meant to be comparable.
    addToggle(m_case, QStringLiteral("findInFilesCase"), tr("Aa"), tr("Match case"));
    addToggle(m_word, QStringLiteral("findInFilesWord"), tr("Word"), tr("Match whole words only"));

    m_scopeLabel = new QLabel(this);
    m_scopeLabel->setObjectName(QStringLiteral("findInFilesScope"));
    m_scopeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_scopeLabel->setWordWrap(true);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("findInFilesStatus"));
    m_status->setText(tr("Type to search the project's files."));

    m_results = new QTreeWidget(this);
    m_results->setObjectName(QStringLiteral("findInFilesResults"));
    m_results->setHeaderLabels({tr("File / Line"), tr("Match")});
    m_results->setRootIsDecorated(true);
    m_results->setUniformRowHeights(true);
    m_results->setItemDelegateForColumn(1, new MatchSpanDelegate(m_results));
    m_results->header()->setStretchLastSection(true);
    m_results->header()->setMinimumSectionSize(48);
    m_results->setTextElideMode(Qt::ElideMiddle);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    layout->addLayout(queryRow);
    layout->addWidget(m_scopeLabel);
    layout->addWidget(m_status);
    layout->addWidget(m_results, 1);

    connect(m_query, &QLineEdit::textChanged, this, [this] {
        m_debounce->start();
    });
    connect(m_query, &QLineEdit::returnPressed, this, [this] {
        m_debounce->stop();
        runQuery();
    });
    connect(m_results, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        activate(item);
    });
    connect(m_model, &FindInFiles::hitsFound, this, &FindInFilesView::addHits);
    connect(m_model, &FindInFiles::finished, this, &FindInFilesView::showSummary);

    // F4 and Shift+F4 tour *these* results whenever the dock has the focus, and
    // are the diagnostics tour's everywhere else. The keys are claimed where the
    // list is rather than bound as a second shortcut: F4 is already the window's
    // (Next Diagnostic), and Qt answers two shortcuts on one key with "ambiguous
    // shortcut overload" — neither fires, which a dock bound that way would have
    // discovered the first time someone pressed F4 in it.
    m_results->installEventFilter(this);
    m_query->installEventFilter(this);
}

void FindInFilesView::setScope(const QString &directory)
{
    if (m_scope == directory)
        return;
    m_scope = directory;
    m_scopeLabel->setText(directory.isEmpty() ? tr("No project directory yet.")
                                              : tr("In %1").arg(QDir::toNativeSeparators(directory)));
    if (!m_query->text().isEmpty()) {
        m_debounce->stop();
        runQuery();
    }
}

void FindInFilesView::focusQuery()
{
    m_query->setFocus();
    m_query->selectAll();
}

void FindInFilesView::runQuery()
{
    m_results->clear();
    m_status->setText(m_query->text().isEmpty() ? tr("Type to search the project's files.")
                                                : tr("Searching…"));
    // A new query cancels the walk that was running: `start` does that, so no
    // result of the previous query can arrive after its rows are gone.
    m_model->start(m_scope, m_query->text(),
                   FindOptions{m_case->isChecked(), m_word->isChecked()});
}

void FindInFilesView::addHits(const QString &path, const QList<FindInFilesHit> &hits)
{
    auto *fileItem = new QTreeWidgetItem(m_results);
    // Relative to the scope, since that is the tree the user is looking at; the
    // absolute path is what navigation needs, and it is kept on the row.
    const QString shown = m_scope.isEmpty() ? path : QDir(m_scope).relativeFilePath(path);
    fileItem->setText(0, QDir::toNativeSeparators(shown));
    fileItem->setText(1, tr("%1 hit(s)").arg(hits.size()));
    fileItem->setData(0, kCountRole, hits.size());
    fileItem->setFirstColumnSpanned(false);
    fileItem->setExpanded(true);

    for (const FindInFilesHit &hit : hits) {
        auto *item = new QTreeWidgetItem(fileItem);
        item->setText(0, QString::number(hit.line));
        item->setText(1, hit.preview);
        item->setToolTip(1, hit.preview);
        item->setData(0, kPathRole, path);
        item->setData(1, kSpanRole, QPoint(hit.column, hit.length));
        item->setData(1, Qt::UserRole + 3, hit.line);
    }
}

void FindInFilesView::showSummary(const FindInFilesSummary &summary)
{
    if (m_query->text().isEmpty()) {
        m_status->setText(tr("Type to search the project's files."));
        return;
    }

    // The counts are stated, not implied: what was searched, what was skipped,
    // and — when the cap stopped the walk — that the total is a floor.
    QString text = tr("%1 hit(s) in %2 file(s) searched").arg(summary.hits).arg(summary.files);
    if (summary.capped)
        text += tr(" — more follow");
    if (summary.skipped > 0)
        text += tr(" · %1 file(s) skipped").arg(summary.skipped);
    m_status->setText(text);
}

QList<QTreeWidgetItem *> FindInFilesView::hitItems() const
{
    QList<QTreeWidgetItem *> items;
    for (int i = 0; i < m_results->topLevelItemCount(); ++i) {
        QTreeWidgetItem *file = m_results->topLevelItem(i);
        for (int j = 0; j < file->childCount(); ++j)
            items.append(file->child(j));
    }
    return items;
}

void FindInFilesView::activate(QTreeWidgetItem *item)
{
    if (!item)
        return;
    const QVariant path = item->data(0, kPathRole);
    if (!path.isValid())
        return; // a file row: expanding it is all a double-click means
    const QPoint span = item->data(1, kSpanRole).toPoint();
    m_results->setCurrentItem(item);
    if (navigate)
        navigate(path.toString(), item->data(1, Qt::UserRole + 3).toInt(), span.x(), span.y());
}

QSize FindInFilesView::sizeHint() const
{
    // The left group starts from this width (applyFactoryDockSizes pins it).
    // Height is whatever the column has; the result list scrolls.
    return QSize(280, 420);
}

bool FindInFilesView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_F4) {
            // Claim the key before the window's own F4 (Next Diagnostic) can see
            // it: while there is focus here, F4 is this list's tour.
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            tour(key->modifiers() & Qt::ShiftModifier ? -1 : +1);
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindInFilesView::tour(int direction)
{
    const QList<QTreeWidgetItem *> items = hitItems();
    if (items.isEmpty())
        return;

    // From where the user is, wrapping — the same shape as the diagnostics tour
    // (MainWindow::stepDiagnostic), for the same reason: stepping is about the
    // list on screen, not about starting over.
    const int at = items.indexOf(m_results->currentItem());
    const int count = items.size();
    const int next = at < 0 ? (direction > 0 ? 0 : count - 1)
                            : ((at + direction) % count + count) % count;
    activate(items.at(next));
}

} // namespace pist
