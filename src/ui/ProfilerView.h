// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/AttributedProfile.h"

#include <QString>
#include <QWidget>

class QAction;
class QCheckBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace pist {

/// The hot spots of a profiling run, as routines and source lines.
///
/// A renderer, and nothing else: the attribution — resolving each address
/// through the symbol table and the line map, anchoring it to a routine,
/// separating ROM time — is `attributedProfile()`, which the profiler
/// controller calls before handing the result here (MAJ-44). This widget used
/// to do that analysis inside `setProfile` and hold the only copy of it, so the
/// editor's gutter heat and the remote `profile results` verb both had to read
/// the numbers back out of a dock.
///
/// What is shown is two currencies: execution counts (what the gutter heat
/// scales from) and cycles (what a 68000 actually spends — a divs is not a
/// moveq). Time spent inside ROM trap handlers keeps its TOS/ROM row.
///
/// Read-only; `setProfile` is called with an already-attributed profile.
class ProfilerView : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilerView(QWidget *parent = nullptr);

public slots:
    /// Show `profile`. An empty profile clears the view; one with no resolved
    /// map says so rather than showing an empty table.
    void setProfile(const AttributedProfile &profile);

    /// Clear the view (no session, or a new run).
    void clear();

    /// Offer the window's profile actions as buttons beside the filter. The
    /// same QActions drive the Run menu, so enabled state and tooltips stay
    /// in sync without a second copy.
    void setActions(QAction *start, QAction *stop, QAction *toCursor = nullptr);

    /// Show a transient message in the status line — the feedback for a
    /// profile action that could not run (the console gets it too, but the
    /// console dock may not be open).
    void showMessage(const QString &message);

    /// Re-apply the theme font and row metrics after an appearance change.
    void applyAppearance();

signals:
    /// The user activated a hot line or a routine; the window should show it
    /// in an editor.
    void lineActivated(int line);

private:
    void populate();
    void applyFilter();
    void setRow(QTreeWidgetItem *item, const QString &name, quint64 count, quint64 cycles) const;

    QHBoxLayout *m_buttonRow = nullptr;
    QLineEdit *m_filter = nullptr;
    QCheckBox *m_showAll = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;

    /// What the tree and the status line render. The widget keeps it only so
    /// that filtering and re-applying the theme can redraw; nothing outside
    /// reads it — the gutter heat and the remote JSON read the controller's
    /// copy of the same value.
    AttributedProfile m_profile;

    /// Rows below this share of the run's cycles are noise; hidden unless the
    /// "Show all" box is checked.
    static constexpr double kMinShare = 0.001;
};

} // namespace pist
