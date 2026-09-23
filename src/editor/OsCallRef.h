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
/// trailing `addq.l`/`lea` in the canonical binding corrects by. It is derived
/// from `prototype` and the layout members below by `osCallStackBytes()`
/// (OsCallBinding.h), which is also what the binding generates its cleanup
/// from; the suite pins the listed value to that derivation.
///
/// The last members describe the calls whose canonical binding is more than
/// their arguments pushed in reverse declaration order. They are data rather
/// than conditionals in the generator, so the irregular shapes stay with the
/// entry they belong to and cannot be keyed on a trap number or a parameter
/// name that a future rename would silently break.
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

    /// Words the canonical binding pushes as reserved zeroes between the
    /// arguments and the function number, which the C prototype does not
    /// describe: Mshrink and Frename each push one, and their C bindings add it
    /// silently. Zero for every other call.
    int reservedWords = 0;

    /// The declaration index of an argument the call fixes instead of the
    /// caller choosing it, or -1 when every argument is the caller's. Dbmsg's
    /// `rsrvd` is index 0 and is pushed as `reservedArgValue` rather than as a
    /// placeholder, so renaming the parameter cannot change the glue.
    int reservedArg = -1;

    /// What the argument at `reservedArg` pushes. Dbmsg's reserved word must
    /// be 5 for the call to work.
    int reservedArgValue = 0;

    /// Pexec's varargs prototype does not describe its fixed argument block —
    /// env, cmdline and name, three longs — which the binding pushes first.
    bool fixedArgBlock = false;
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
