// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// One profiled instruction, from a Hatari `profile save` file.
///
/// The address is the Atari address as written in the file, not an index into
/// Hatari's internal profile array: `profile save` disassembles the profiled
/// addresses back to real addresses (Profile_CpuShowAddresses maps through
/// index2address), so what is in the file is directly comparable with a PC and
/// resolvable through ProgramLineMap.
struct ProfileLine
{
    quint32 address = 0;
    /// Executed instruction count at this address ("Executed instructions").
    quint64 count = 0;
    /// CPU cycles spent at this address ("Used cycles").
    quint64 cycles = 0;
};

/// One memory area the emulator attributed profiled addresses to, from the
/// save file's area lines (`ROM_TOS: 0xfc0000-0xfc0100`).
///
/// Retained so time spent inside the OS — trap handlers, often a real share
/// of a frame — can be shown as its own row instead of being discarded as
/// "unmapped" only because it is not in the user's source.
struct ProfileRegion
{
    QString name;
    quint32 first = 0;
    quint32 last = 0; ///< inclusive, as the file writes it
};

/// Parsed `profile save` output.
///
/// The save file is plain text, written by Profile_Save (src/debug/profile.c)
/// and Profile_CpuSave (src/debug/profilecpu.c) in Hatari 2.6.1:
///
///     Hatari CPU profile (Hatari v2.6.1)
///     Cycles/second:	8021247
///     Field names:	Executed instructions, Used cycles, Instruction cache misses, Data cache hits
///     Field regexp:	^\$?([0-9A-Fa-f]+) .*% \(([^)]*)\)$
///     ST_RAM:		0x000000-0x100000
///     ROM_TOS:		0xfc0000-0xfc0100
///     CARTRIDGE:		0xfa0000-0xfb0000
///     PROGRAM_TEXT:	0x0012554-0x0012600
///     # disassembly with profile data: <instructions percentage>% (...
///     start:
///     00e00cfe 4e75  rts  == $e66218   0.16% (48753, 780396, 0, 0)
///     [...]
///     # <callee>: <caller1> = <calls> <types>[ ...], <callee name>
///     0x00125a0: 0x001259c = 3 s 45/120/400/0/0, 0x00126aa = 1 n 12/30/90/0/0, name
///
/// Only the two fields PiST actually uses — instructions and cycles — are
/// retained. The `Field names:` line is kept so a consumer can see what else
/// the file carried (the CPU save also has cache misses/hits; the DSP save puts
/// a cycle difference there instead), and because it is the evidence that the
/// field order is what Profile_Line assumes: profile_priv.h pins instructions
/// and cycles as the first two fields of *both* the CPU and DSP saves.
struct ProfileData
{
    /// The parenthesised emulator identification, empty when absent.
    QString emulator;
    /// "CPU" or "DSP", from the title line.
    QString processor;
    /// Processor clock, Hz, as the emulator configured it. This is the emulated
    /// speed (MachineClocks.CPU_Freq_Emul), so cycles/clockHz is emulated time.
    quint32 clockHz = 0;
    /// Field descriptions exactly as the file lists them, comma-separated.
    QStringList fieldNames;
    /// Profiled instructions, in the order the file lists them (ascending
    /// address, with gaps skipped by the emulator).
    QList<ProfileLine> lines;

    /// Sum of every line's count, i.e. the run's total instruction count, and
    /// the denominator Hatari itself uses for its percentages.
    quint64 totalCount = 0;
    quint64 totalCycles = 0;

    /// The memory areas named in the header (ST_RAM, ROM_TOS, CARTRIDGE,
    /// PROGRAM_TEXT), in the file's order. May be empty in older saves.
    QList<ProfileRegion> regions;

    bool isEmpty() const { return lines.isEmpty(); }
};

/// Parse a profile file written by Hatari's `profile save <file>`.
/// Returns false and sets `error` when the file cannot be read, is not a
/// Hatari profile at all, or contains no executed instruction — the last case
/// being what a `profile on` issued while emulation was already running
/// produces, so the caller can say so plainly instead of showing an empty
/// table.
bool parseProfile(const QString &path, ProfileData *data, QString *error = nullptr);

/// Parse profile text already in hand (the body of the same file), for callers
/// that captured it rather than reading it from disk.
bool parseProfileText(const QString &text, ProfileData *data, QString *error = nullptr);

} // namespace pist
