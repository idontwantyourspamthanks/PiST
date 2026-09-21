// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/ProfileData.h"

#include "build/SymbolTable.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVector>
#include <QWidget>

class QAction;
class QCheckBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

namespace pist {

class ProgramLineMap;

/// The hot spots of a profiling run, as routines and source lines.
///
/// A profiler's raw output is per address; what an assembly developer acts on
/// is per routine and per line — "clearScreen is 60% of the frame" — so the
/// addresses are resolved through the same ProgramLineMap the debugger uses,
/// attributed to the nearest code label, and summed. Lines outside the
/// current source file are dropped, and the status line says how many samples
/// were, so a partially-mapped profile does not read as a complete one.
///
/// Two currencies are shown: execution counts (what the gutter heat scales
/// from) and cycles (what a 68000 actually spends — a divs is not a moveq).
/// Time spent inside ROM trap handlers is kept as a TOS/ROM row rather than
/// discarded as unmapped.
///
/// Read-only; `setProfile` is called after a `profile save` has been parsed.
class ProfilerView : public QWidget
{
    Q_OBJECT

public:
    explicit ProfilerView(QWidget *parent = nullptr);

public slots:
    /// Show `profile`, with each address resolved to a line of `sourceFile`
    /// and attributed to a routine from `symbols`. `map` may be null or
    /// unresolved, in which case nothing maps and the view says so rather
    /// than showing an empty table; an empty profile clears it.
    void setProfile(const ProfileData &profile, const ProgramLineMap *map,
                    const QString &sourceFile, const QVector<SymbolEntry> &symbols);

    /// Clear the view (no session, or a new run).
    void clear();

    /// Offer the window's profile actions as buttons beside the filter. The
    /// same QActions drive the Run menu, so enabled state and tooltips stay
    /// in sync without a second copy.
    void setActions(QAction *start, QAction *stop);

    /// Per-source-line execution counts of what is currently shown, for the
    /// editor's gutter heat. These are the very numbers the tree renders, so
    /// the heat and the tree cannot disagree about what is hot.
    QHash<int, quint64> lineCounts() const;

    /// Re-apply the theme font and row metrics after an appearance change.
    void applyAppearance();

signals:
    /// The user activated a hot line or a routine; the window should show it
    /// in an editor.
    void lineActivated(int line);

private:
    /// One source line's aggregated cost, with its routine.
    struct LineCost
    {
        int line = 0;
        QString routine;
        quint64 count = 0;
        quint64 cycles = 0;
    };

    /// One routine's aggregate, plus its lines, hottest first within.
    struct RoutineCost
    {
        QString name;
        int defLine = 0; ///< the label's source line, for activation
        quint64 count = 0;
        quint64 cycles = 0;
        QList<LineCost> lines;
    };

    void populate();
    void applyFilter();
    void setRow(QTreeWidgetItem *item, const QString &name, quint64 count, quint64 cycles) const;

    QLineEdit *m_filter = nullptr;
    QCheckBox *m_showAll = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;

    /// The routines of the current file, sorted by descending cycles.
    QList<RoutineCost> m_routines;
    /// ROM regions with profiled time (ROM_TOS, CARTRIDGE), sorted likewise.
    QList<RoutineCost> m_rom;
    /// Samples whose address resolved to neither a line of the current file
    /// nor a ROM region.
    int m_unmapped = 0;
    quint64 m_unmappedCycles = 0;

    quint64 m_totalCount = 0;
    quint64 m_totalCycles = 0;
    quint32 m_clockHz = 0;

    /// Rows below this share of the run's cycles are noise; hidden unless the
    /// "Show all" box is checked.
    static constexpr double kMinShare = 0.001;
};

} // namespace pist
