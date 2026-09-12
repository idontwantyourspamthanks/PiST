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
struct Watchpoint
{
    quint32 address = 0;

    /// Access width: 'b' (byte), 'w' (word) or 'l' (long). Word is the sensible
    /// default for 68k code, which deals in words and longs.
    char width = 'w';

    /// The Hatari `b` command that arms this watchpoint.
    QString command() const
    {
        const QString a = QStringLiteral("$%1").arg(address, 0, 16);
        return QStringLiteral("b (%1).%2 ! (%1).%2").arg(a).arg(QLatin1Char(width));
    }

    QString label() const
    {
        return QStringLiteral("watch $%1.%2")
            .arg(address, 0, 16)
            .arg(QLatin1Char(width));
    }
};

} // namespace pist
