// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"

#include <QList>
#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace pist {

/// Lists the session's breakpoints and their resolution state.
///
/// The list is worth having because a breakpoint can be *stored* yet not armed:
/// a line that emits no code has no address, and an address cannot be resolved
/// at all until the program has started. Without a panel those cases are silent,
/// and the user is left wondering why a breakpoint never fires.
class BreakpointPanel : public QWidget
{
    Q_OBJECT

public:
    explicit BreakpointPanel(QWidget *parent = nullptr);

    void setBreakpoints(const QList<Breakpoint> &breakpoints);

    /// Watchpoints are address-based rather than source-line, so they are
    /// listed separately, below the breakpoints, but armed and cleared with them.
    void setWatchpoints(const QList<Watchpoint> &watchpoints);

    /// Report whether the session has run far enough to resolve addresses.
    void setResolvable(bool resolvable);

    /// Re-apply the theme colours to the current list.
    void applyAppearance();

    /// The line shown when the list is empty. The window passes the live
    /// toggle shortcut, so Common's F9 does not leave an F8 hint behind.
    void setEmptyHint(const QString &text);

signals:
    void breakpointActivated(const QString &file, int line);
    void removeRequested(const QString &file, int line);
    void clearRequested();

    /// A watchpoint row was double-clicked (to navigate memory to it), or its
    /// removal was requested, by index into the watchpoint list.
    void watchpointActivated(quint32 address);
    void watchpointRemoveRequested(int index);

private:
    void refresh();

    QList<Breakpoint> m_breakpoints;
    QList<Watchpoint> m_watchpoints;
    bool m_resolvable = false;
    QLabel *m_empty = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_clearButton = nullptr;
};

} // namespace pist
