// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/ProfileData.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QWidget>

class QAction;
class QLabel;
class QLineEdit;
class QTableWidget;

namespace pist {

class ProgramLineMap;

/// The hot lines of a profiling run, as source lines.
///
/// A profiler's raw output is per address; what an assembly developer acts on is
/// per source line — "this routine is 60% of the frame" — so the addresses are
/// resolved through the same ProgramLineMap the debugger uses and their counts
/// summed per line. Lines outside the current source file are dropped, and the
/// status line says how many samples were, so a partially-mapped profile does
/// not read as a complete one.
///
/// Read-only; `setProfile` is called after a `profile save` has been parsed.
class ProfilerView : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilerView(QWidget *parent = nullptr);

public slots:
    /// Show `profile`, with each address resolved to a line of `sourceFile`.
    /// `map` may be null or unresolved, in which case nothing maps and the view
    /// says so rather than showing an empty table; an empty profile clears it.
    void setProfile(const ProfileData &profile, const ProgramLineMap *map,
                    const QString &sourceFile);

    /// Offer the window's profile actions as buttons beside the filter. The
    /// same QActions drive the Run menu, so enabled state and tooltips stay
    /// in sync without a second copy.
    void setActions(QAction *start, QAction *stop);

    /// Clear the view (no session, or a new run).
    void clear();

    /// Per-source-line execution counts of what is currently shown, for the
    /// editor's gutter heat. These are the very numbers the table renders, so
    /// the heat and the table cannot disagree about what is hot.
    QHash<int, quint64> lineCounts() const;

    /// Re-apply the theme font and row metrics after an appearance change.
    void applyAppearance();

signals:
    /// The user activated a hot line; the window should show it in an editor.
    void lineActivated(int line);

private:
    /// One source line's aggregated cost.
    struct LineCost
    {
        int line = 0;
        /// Sum of the line's instructions' execution counts. Cycles are not
        /// kept: the table and the gutter heat both rank by execution, which is
        /// what a developer chasing a hot loop reads first, and a second
        /// silently-unused aggregate would only invite them to disagree.
        quint64 count = 0;
    };

    void populate();
    void applyFilter();
    int lineAtRow(int row) const;

    QLineEdit *m_filter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_status = nullptr;

    /// The aggregated hot lines, sorted by descending count. Kept so filtering
    /// and a theme change can re-render without re-resolving addresses.
    QList<LineCost> m_lines;
    /// Samples whose address resolved to no line of the current file.
    int m_unmapped = 0;
    /// The run's total instruction count, so a line's share is measured against
    /// the whole run — the same denominator Hatari's own percentages use.
    quint64 m_totalCount = 0;
};

} // namespace pist
