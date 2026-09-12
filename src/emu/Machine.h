// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>

namespace pist {

/// Emulated machine.
///
/// Machine and TOS ROM are not independent: Hatari rejects some pairings and
/// resolves them by overriding `--machine`, reporting only an `ERROR` log line
/// (see docs/PLAN.md §5 rule 3). So the IDE has to model the pairing itself
/// rather than offering two free-choice dropdowns.
enum class Machine {
    St,
    MegaSt,
    Ste,
    MegaSte,
    Tt,
    Falcon,
};

/// Value for Hatari's `--machine` (`st`, `megast`, `ste`, `megaste`, `tt`, `falcon`).
QString machineCliName(Machine machine);

/// Human-facing name ("ST", "Mega ST", "STe", ...).
QString machineDisplayName(Machine machine);

/// Parse a `--machine` value. Returns false for anything unrecognised.
bool machineFromCliName(const QString &name, Machine *machine);

/// Every machine, in the order they should be offered.
QList<Machine> allMachines();

/// Whether a machine can run a given TOS version.
///
/// The ST/Mega ST and STe rows were **verified against Hatari 2.6.1** by running
/// each ROM/machine combination and reading its override messages. The later
/// rows follow the TOS version scheme (2.x Mega STe/TT, 3.x TT, 4.x Falcon) and
/// are not individually verified here; they are used only to warn, never to
/// refuse.
bool machineAcceptsTos(Machine machine, int versionCode, bool isEmuTos);

/// Machines that can run a given TOS version. Empty when the version matches
/// nothing known, which should be treated as "unknown" rather than "invalid".
QList<Machine> machinesForTos(int versionCode, bool isEmuTos);

/// A short explanation of what TOS a machine needs, for use in a hint or tooltip.
QString requiredTosHint(Machine machine);

} // namespace pist
