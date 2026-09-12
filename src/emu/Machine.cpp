// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/Machine.h"

namespace pist {

namespace {

/// TOS 1.x is split between the ST family and the STe family, and Hatari refuses
/// to mix them:
///
///   TOS 1.04 + --machine ste -> "TOS versions <= 1.4 work only in" -> switches to ST
///   TOS 1.06 + --machine st  -> "1.06 and 1.62 are for Atari STE only" -> switches to STE
///
/// 1.06 and 1.62 are both STe ROMs; 1.62 is the later release and fixes a number
/// of bugs, so it is the preferred STe choice when both are available.
bool isStTos(int version) { return version >= 0x0100 && version < 0x0106; }
bool isSteTos(int version) { return version == 0x0106 || version == 0x0162; }

} // namespace

QString machineCliName(Machine machine)
{
    switch (machine) {
    case Machine::St:      return QStringLiteral("st");
    case Machine::MegaSt:  return QStringLiteral("megast");
    case Machine::Ste:     return QStringLiteral("ste");
    case Machine::MegaSte: return QStringLiteral("megaste");
    case Machine::Tt:      return QStringLiteral("tt");
    case Machine::Falcon:  return QStringLiteral("falcon");
    }
    return QStringLiteral("st");
}

QString machineDisplayName(Machine machine)
{
    switch (machine) {
    case Machine::St:      return QStringLiteral("ST");
    case Machine::MegaSt:  return QStringLiteral("Mega ST");
    case Machine::Ste:     return QStringLiteral("STe");
    case Machine::MegaSte: return QStringLiteral("Mega STe");
    case Machine::Tt:      return QStringLiteral("TT");
    case Machine::Falcon:  return QStringLiteral("Falcon");
    }
    return QStringLiteral("ST");
}

bool machineFromCliName(const QString &name, Machine *machine)
{
    const QString lower = name.toLower();
    struct Entry { const char *cli; Machine machine; };
    static const Entry entries[] = {
        {"st", Machine::St},         {"megast", Machine::MegaSt},
        {"ste", Machine::Ste},       {"megaste", Machine::MegaSte},
        {"tt", Machine::Tt},         {"falcon", Machine::Falcon},
    };
    for (const Entry &entry : entries) {
        if (lower == QLatin1String(entry.cli)) {
            if (machine)
                *machine = entry.machine;
            return true;
        }
    }
    return false;
}

QList<Machine> allMachines()
{
    return {Machine::St, Machine::MegaSt, Machine::Ste,
            Machine::MegaSte, Machine::Tt, Machine::Falcon};
}

bool machineAcceptsTos(Machine machine, int versionCode, bool isEmuTos)
{
    // EmuTOS adapts itself to whatever machine it is run on.
    if (isEmuTos)
        return true;

    switch (machine) {
    case Machine::St:
    case Machine::MegaSt:
        return isStTos(versionCode);

    case Machine::Ste:
    case Machine::MegaSte:
        // Mega STe shipped with TOS 2.05/2.06 as well as the STe ROMs.
        return isSteTos(versionCode)
            || (machine == Machine::MegaSte && versionCode >= 0x0200 && versionCode < 0x0300);

    case Machine::Tt:
        return versionCode >= 0x0200 && versionCode < 0x0400;

    case Machine::Falcon:
        return versionCode >= 0x0400 && versionCode < 0x0500;
    }
    return false;
}

QList<Machine> machinesForTos(int versionCode, bool isEmuTos)
{
    QList<Machine> result;
    for (Machine machine : allMachines()) {
        if (machineAcceptsTos(machine, versionCode, isEmuTos))
            result.append(machine);
    }
    return result;
}

QString requiredTosHint(Machine machine)
{
    switch (machine) {
    case Machine::St:
    case Machine::MegaSt:
        return QStringLiteral("TOS 1.00-1.04 (an ST-family ROM)");
    case Machine::Ste:
        return QStringLiteral("TOS 1.06 or 1.62 (both STe ROMs; 1.62 is the later bugfix release)");
    case Machine::MegaSte:
        return QStringLiteral("TOS 1.06, 1.62, or 2.05/2.06");
    case Machine::Tt:
        return QStringLiteral("TOS 2.06 or 3.0x");
    case Machine::Falcon:
        return QStringLiteral("TOS 4.0x");
    }
    return {};
}

} // namespace pist
