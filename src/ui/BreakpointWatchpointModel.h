// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>
#include <utility>

namespace pist {

class CodeEditor;
class DebugSessionController;
class IDebugBackend;

/// The IDE's breakpoint and watchpoint models, with the edits that change them
/// (MAJ-41).
///
/// Breakpoints are stored per file:line, never per address: the program is
/// relocated by GEMDOS on every run (docs/PLAN.md §5 rule 6), so an address is
/// only ever the *resolved* form the panel shows. Watchpoints are address-based
/// and armed as self-inequality breakpoints (Hatari has no data watchpoints).
///
/// Every edit here goes through the session controller's one arming gate, so
/// "an edit re-arms the emulator" is decided in exactly one place rather than
/// at six call sites — the shape CRIT-4 was. What the model reads from the
/// window it serves comes in as the `Host` operations below (MIN-89 — it used
/// to be a `friend` of MainWindow).
class BreakpointWatchpointModel : public QObject
{
public:
    /// What the model reads from the window: the symbols and map its label
    /// lookups resolve against, the arming gate it hands every edit to, the
    /// views it shows the result on, and one console line. Callbacks rather
    /// than a `MainWindow *`, so the model's own list is all it owns and the
    /// window's privates stay the window's.
    struct Host
    {
        /// The arming gate every edit goes through (MIN-13).
        std::function<DebugSessionController *()> session;
        /// The program map, which resolves a line and its neighbours.
        std::function<const ProgramLineMap &()> programMap;
        /// The build's symbols, for the `breakpoint <label>` form.
        std::function<const QVector<SymbolEntry> &()> symbols;
        /// The current text editor; the condition edit acts on its file.
        std::function<CodeEditor *()> editor;
        /// Every open text editor, each of which gets this model's markers.
        std::function<QList<CodeEditor *>()> editors;
        /// The debug transport, for the one edit that arms directly (Clear
        /// all with a live session).
        std::function<IDebugBackend *()> backend;
        /// One line on the console.
        std::function<void(const QString &)> log;
        /// Show every breakpoint in the panel.
        std::function<void(const QList<Breakpoint> &)> showBreakpoints;
        /// Show every watchpoint in the panel.
        std::function<void(const QList<Watchpoint> &)> showWatchpoints;
    };

    explicit BreakpointWatchpointModel(Host host, QObject *parent = nullptr);

    const QList<Breakpoint> &breakpoints() const { return m_breakpoints; }
    const QList<Watchpoint> &watchpoints() const { return m_watchpoints; }

    /// Toggle a breakpoint at (file, line) directly — the base-name keying the
    /// whole IDE uses. Shared by toggleBreakpointAtLine (current editor) and
    /// toggleBreakpointAtLabel (any module).
    bool toggle(const QString &file, int line);

    /// Toggle a breakpoint at a symbol's definition. `detail` carries the reply
    /// text either way: "ok <file>:<line> [= 0x…]" or an "error …" naming why
    /// (unknown name, or a position-less symbol). For the remote
    /// `breakpoint <label>` form.
    bool toggleAtLabel(const QString &name, QString *detail);

    void remove(const QString &file, int line);

    /// The Run menu's "Clear Breakpoints": breakpoints only.
    void clear();
    /// The breakpoints dock's "Clear all": breakpoints AND watchpoints (its
    /// button enables when only watchpoints are present).
    void clearAllTargets();

    /// Prompt for and set a breakpoint's extra condition, a breakpoint being
    /// created if the line had none.
    void editCondition(int line);

    /// Prompt for and add an address watchpoint (the Add-watchpoint action).
    void addWatchpoint();
    /// Parse and add a watchpoint by address text (e.g. "$12345" or
    /// "$12345.l"). Separated from the dialog so the remote-control interface
    /// and tests can use it without a prompt. Returns false and sets error on a
    /// bad address.
    bool addWatchpointAddress(const QString &text, QString *error);
    void removeWatchpoint(int index);

    /// Fold the addresses the ArmPlan resolved back into the stored breakpoint
    /// list, so the panel shows where each one actually landed.
    QList<Breakpoint> mergeResolved(const ArmPlan &plan) const;

    /// Push the models to every open editor's gutter and to the breakpoint
    /// panel.
    void refreshMarkers();

private:
    Host m_host;
    QList<Breakpoint> m_breakpoints;
    QList<Watchpoint> m_watchpoints;
};

} // namespace pist
