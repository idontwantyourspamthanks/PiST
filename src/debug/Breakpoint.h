// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/LineMap.h"
#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
#include "debug/Watchpoint.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// A breakpoint anchored to a source line rather than an address.
///
/// Addresses are not stable: the program is relocated by GEMDOS each time it
/// runs, so a breakpoint stored as an address would point at the wrong code
/// after any change to the program or its environment. Anchoring to file:line
/// and resolving at arm time keeps breakpoints correct across rebuilds
/// (docs/PLAN.md §5 rule 6).
struct Breakpoint
{
    QString file;
    int line = 0;

    /// Extra Hatari condition, ANDed with the program-counter test. This is the
    /// only watchpoint mechanism available: upstream Hatari has no memory
    /// watchpoints at all (`grep -rni watchpoint src/debug/` finds none), so
    /// "break when d0 = $1234" or "break when (buf) = $ff" is expressed here.
    QString condition;

    bool enabled = true;

    /// Filled in when the breakpoint is resolved against a running session.
    quint32 address = 0;
    bool resolved = false;

    /// A Lua script to run when this breakpoint stops the machine, *instead of*
    /// the project's stop hook. Empty is the usual case and means the project's.
    /// Read at the stop, not part of the armed command, so changing it needs no
    /// re-arm. The default is written out because the struct is built with brace
    /// lists all over the tree (`Breakpoint{file, line, cond, enabled, addr,
    /// resolved}`); a bare declaration would make every one of them a
    /// `-Wmissing-field-initializers` warning.
    QString scriptPath{};

    /// Whether this breakpoint can be armed at all, given the current listing.
    bool isResolvable() const { return line > 0 && !file.isEmpty() && enabled; }

    QString label() const { return QStringLiteral("%1:%2").arg(file).arg(line); }
};

/// The armed breakpoint whose resolved address is `pc`, or nullptr.
///
/// This is as far as the IDE can honestly go in naming a stop's cause: the
/// transport reports a prompt, not which breakpoint fired. Two breakpoints
/// resolved to the same address are indistinguishable, and the first is named;
/// a watchpoint is not in this set at all, because it fires on a *data change*
/// and its address says nothing about the program counter.
const Breakpoint *breakpointAt(const QList<Breakpoint> &breakpoints, quint32 pc);

/// The absolute path a breakpoint's script is read from, given what the gutter
/// dialog was told and the source the breakpoint is in.
///
/// Empty stays empty — the project's hook runs instead — and an absolute path is
/// kept as it is. A relative one is resolved against the directory of that
/// source, which is the rule `ProjectSettings` already uses for the project's own
/// `hooks.scriptPath`, and where *New script…* puts one. The alternative is
/// resolving against whatever working directory PiST happens to have been launched
/// with, which is not something the person typing a path can see.
QString resolveScriptPath(const QString &typed, const QString &sourceFile);

/// The commands needed to arm a set of breakpoints, plus what could not be armed.
struct ArmPlan
{
    /// One Hatari `b` command per armable breakpoint, in input order.
    QStringList commands;

    /// Breakpoints that were armed, with `address` and `resolved` filled in.
    QList<Breakpoint> armed;

    /// Human-readable labels of breakpoints that could not be armed: their line
    /// emitted no code, or their own condition is one Hatari's breakpoint
    /// grammar rejects.
    QStringList unresolved;

    /// Why each entry of `unresolved` was left out, parallel to it. The two
    /// causes are different problems — a line with no address, a condition the
    /// debugger will refuse — and the panel and console can only say which if
    /// the plan carries it (MIN-10).
    QStringList unresolvedReasons;
};

/// Resolve source-line breakpoints to Hatari `b` commands.
///
/// Pure function, so the resolution rules are testable without an emulator.
/// The map must have live bases from a *running* session: the program's load
/// address is only known after it has been executed, which is why arming happens
/// after the entry stop (docs/PLAN.md §5 rule 6).
/// The line map is the *program* map rather than a single-module one: a linked
/// program has several modules, each with its own base, so resolving a line needs
/// the whole set (docs/PLAN.md §4.3).
/// Bases are not passed separately: the program map holds one per module, once
/// the program is running, so it is the single source of truth for where a line
/// lives.
ArmPlan planBreakpoints(const QList<Breakpoint> &breakpoints,
                        const ProgramLineMap &lineMap);

/// The commands needed to arm a set of watchpoints, plus what could not be armed.
///
/// The same shape as ArmPlan, and for the same reason: what a watchpoint watches
/// has to be resolved at arm time, and a watchpoint that cannot be resolved must
/// say so rather than be armed at a wrong address (0 would read as "watch the
/// vector table" and fire at once).
struct WatchpointPlan
{
    /// One Hatari `b` command per armable watchpoint, in input order.
    QStringList commands;

    /// Watchpoints that were armed, with `address` and `resolved` filled in.
    QList<Watchpoint> armed;

    /// Labels of watchpoints that could not be armed.
    QStringList unresolved;

    /// Why each entry of `unresolved` was left out, parallel to it: no such
    /// symbol, an equate, a label that emits nothing, or a name the bases cannot
    /// place yet.
    QStringList unresolvedReasons;
};

/// Resolve watchpoints to Hatari `b` commands.
///
/// Pure function, so the resolution rules are testable without an emulator.
/// A watchpoint asked for by address is already resolved and passes through; one
/// asked for by *name* goes through `symbols` and the map's live bases, which is
/// the same rule `labelAddress()` gives a hook script's `sym()` and the remote
/// `breakpoint <label>` verb — one rule, several callers, so a label means the
/// same thing wherever it is named.
///
/// Resolved on every arm, never remembered: GEMDOS relocates the program each
/// run, so a name's address is only good for the run it was resolved in.
WatchpointPlan planWatchpoints(const QList<Watchpoint> &watchpoints,
                               const QVector<SymbolEntry> &symbols,
                               const ProgramLineMap &lineMap);

/// Move the breakpoints keyed to `oldFile` over to `newFile`, leaving every
/// other entry untouched and the order unchanged. The store is keyed by base
/// name (BreakpointWatchpointModel), so a rename that keeps the name (a move
/// to another directory) needs no migration and a rename that changes it
/// would otherwise strand the file's breakpoints under a name that no longer
/// exists (docs/untitled-plan.md §7.2).
///
/// Pure function, so the migration rule is testable without the model. The two
/// names are the keys the store uses — base names such as `util.s`, never full
/// paths — because that is what the model is handed, and so what a rename must
/// be able to rewrite.
QList<Breakpoint> renameFile(const QList<Breakpoint> &breakpoints,
                             const QString &oldFile, const QString &newFile);

} // namespace pist
