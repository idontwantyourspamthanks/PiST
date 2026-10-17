// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// A memory watchpoint: break when the value at an address changes.
///
/// Hatari has no data watchpoints at all (upstream's `w` command is compiled
/// out), so a watch is expressed as a conditional breakpoint using
/// self-inequality: `($addr).w ! ($addr).w`. The debugger's change tracking
/// evaluates that as "fire when the value differs from its previous value",
/// which is exactly a write-watchpoint — with the caveat that it fires only on a
/// change, so a write of the same value, or a read, does not trigger it.
///
/// A watchpoint is asked for by *name* or by address, and a name is the useful
/// form: GEMDOS picks the program's load address afresh on every run, so an
/// address captured last time is wrong this time — which is exactly why
/// breakpoints are file:line and never raw addresses. `symbol` carries the name
/// the user watches; `address` is what the arm plan resolved it to *this* run
/// (`planWatchpoints()`, docs/glm-features.md §5).
struct Watchpoint
{
    /// The label it was asked for by, when it was asked for by name: empty when
    /// it was asked for by address. The label is what the panel and the log show,
    /// and what the arm plan resolves again on every run.
    QString symbol;

    /// The address to watch: the one it was asked for by, or the one the arm plan
    /// resolved `symbol` to. Never remembered between runs for a named watchpoint,
    /// and never written back into the model — the panel is handed the resolved
    /// list for the run, and the stored watchpoint keeps the name.
    quint32 address = 0;

    /// True when `address` is meaningful. A raw address always is; a name's is
    /// only once the arm plan has resolved it against the live bases, so an
    /// unresolved named watchpoint is listed as pending rather than armed at $0.
    bool resolved = false;

    /// Access width: 'b' (byte), 'w' (word) or 'l' (long). Word is the sensible
    /// default for 68k code, which deals in words and longs.
    char width = 'w';

    /// The Hatari `b` command that arms this watchpoint. Meaningful only when
    /// `resolved`: an unresolved name has no address to name yet.
    QString command() const
    {
        const QString a = QStringLiteral("$%1").arg(address, 0, 16);
        return QStringLiteral("b (%1).%2 ! (%1).%2").arg(a).arg(QLatin1Char(width));
    }

    /// How the panel and the log name it: what was asked for, not where it landed
    /// — the name is the thing that stays true across runs.
    QString label() const
    {
        const QString what = symbol.isEmpty() ? QStringLiteral("$%1").arg(address, 0, 16) : symbol;
        return QStringLiteral("watch %1.%2").arg(what).arg(QLatin1Char(width));
    }
};

} // namespace pist
