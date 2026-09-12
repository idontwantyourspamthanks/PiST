// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "debug/Breakpoint.h"

#include <QList>
#include <QWidget>

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

    /// Report whether the session has run far enough to resolve addresses.
    void setResolvable(bool resolvable);

signals:
    void breakpointActivated(const QString &file, int line);
    void removeRequested(const QString &file, int line);
    void clearRequested();

private:
    void refresh();

    QList<Breakpoint> m_breakpoints;
    bool m_resolvable = false;
    QTableWidget *m_table = nullptr;
    QPushButton *m_clearButton = nullptr;
};

} // namespace pist
