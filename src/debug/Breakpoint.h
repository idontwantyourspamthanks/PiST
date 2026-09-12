// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/LineMap.h"
#include "build/ProgramLineMap.h"

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

    /// Whether this breakpoint can be armed at all, given the current listing.
    bool isResolvable() const { return line > 0 && !file.isEmpty() && enabled; }

    QString label() const { return QStringLiteral("%1:%2").arg(file).arg(line); }
};

/// The commands needed to arm a set of breakpoints, plus what could not be armed.
struct ArmPlan
{
    /// One Hatari `b` command per armable breakpoint, in input order.
    QStringList commands;

    /// Breakpoints that were armed, with `address` and `resolved` filled in.
    QList<Breakpoint> armed;

    /// Human-readable labels of breakpoints that had no address, because their
    /// line emitted no code or data.
    QStringList unresolved;
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

} // namespace pist
