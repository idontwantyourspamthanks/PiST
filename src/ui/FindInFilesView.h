// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "ui/FindInFiles.h"

#include <QWidget>

#include <functional>

class QLabel;
class QLineEdit;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace pist {

/// The Find in Files dock: a query, the scope it searches, and what it found,
/// grouped by file (docs/glm-features.md §3).
///
/// A *view*: the walk is `FindInFiles`, and what a hit opens is the window's
/// business, handed in as `navigate` — the same seam shape the profiler and the
/// stop-hooks dialog use, so this class knows nothing about MainWindow.
class FindInFilesView : public QWidget
{
    Q_OBJECT

public:
    explicit FindInFilesView(QWidget *parent = nullptr);

    /// Where a hit should be shown, for a double-click, Return, and the tour.
    /// `column` and `length` are the match's span within its line, which is what
    /// lets the editor mark the match rather than just its line.
    std::function<void(const QString &path, int line, int column, int length)> navigate;

    /// The directory to search, shown above the query. Setting it re-runs the
    /// current query: the results of the old scope are not answers about this one.
    void setScope(const QString &directory);
    QString scope() const { return m_scope; }

    /// Put the caret in the query field, for the Search menu's entry.
    void focusQuery();

    /// A sidebar's width, not a bottom strip's height. The list scrolls for
    /// whatever height the project-files group is given.
    QSize sizeHint() const override;

protected:
    /// F4 / Shift+F4 tour the results while the dock has focus, claimed here so
    /// they do not collide with the window's diagnostics tour.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// Start the walk for the query as it stands now, clearing what the previous
    /// one found.
    void runQuery();
    void addHits(const QString &path, const QList<FindInFilesHit> &hits);
    void showSummary(const FindInFilesSummary &summary);
    /// Every hit row, in the order the results show them, for the tour.
    QList<QTreeWidgetItem *> hitItems() const;
    void activate(QTreeWidgetItem *item);
    void tour(int direction);

    FindInFiles *m_model = nullptr;
    QTimer *m_debounce = nullptr;
    QString m_scope;
    QLabel *m_scopeLabel = nullptr;
    QLineEdit *m_query = nullptr;
    QToolButton *m_case = nullptr;
    QToolButton *m_word = nullptr;
    QLabel *m_status = nullptr;
    QTreeWidget *m_results = nullptr;
};

} // namespace pist
