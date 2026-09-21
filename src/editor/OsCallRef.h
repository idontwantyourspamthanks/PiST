// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>

namespace pist {

/// One operating-system call of the TOS reference: a GEMDOS, BIOS or XBIOS
/// function reached through a 68000 trap.
///
/// `trap` selects the layer: 1 = GEMDOS, 13 = BIOS, 14 = XBIOS. `prototype`
/// is the canonical C signature; `returns` a short phrase describing what d0
/// holds, or empty when the call returns nothing meaningful. `availability`
/// is empty when the call exists in every TOS version, otherwise the floor
/// ("TOS 1.04+", "TOS 2.06, ST-Book"). `stackBytes` is the number of bytes
/// the caller pops after the trap (function word plus arguments) — what the
/// trailing `addq.l`/`lea` in the canonical binding corrects by.
struct OsCallInfo
{
    int trap;
    int opcode;
    QString name;
    QString prototype;
    QString summary;
    QString returns;
    QString availability;
    int stackBytes;
};

/// The whole reference, in the order it should be listed: GEMDOS grouped by
/// name prefix, then XBIOS and BIOS grouped by subsystem — grouped the way a
/// manual groups it, so the panel reads like one.
const QList<OsCallInfo> &osCallTable();

/// The layer's display name ("GEMDOS", "BIOS", "XBIOS"), empty for a trap
/// number that is not an OS-call layer.
QString osCallLayerName(int trap);

/// The layer's lower-case key ("gemdos", "bios", "xbios"), used where a
/// machine-readable tag is needed (list-item keys, tests).
QString osCallLayerKey(int trap);

/// The entry for (`trap`, `opcode`), or nullptr when the number is not a
/// documented call of that layer.
const OsCallInfo *osCallRef(int trap, int opcode);

/// The entry of `trap` named `name` (case-insensitive), or nullptr. Source
/// that pushes a symbolic function number (`move.w #Cconws,-(sp)`) resolves
/// through here.
const OsCallInfo *osCallRefByName(int trap, const QString &name);

} // namespace pist
