// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include <QList>
#include <QString>

namespace pist {

/// m68k register file as reported by Hatari's `cpureg` command.
struct Registers
{
    quint32 d[8] = {};
    quint32 a[8] = {};
    quint32 usp = 0;
    quint32 isp = 0;
    quint16 sr = 0;

    bool flagX = false;
    bool flagN = false;
    bool flagZ = false;
    bool flagV = false;
    bool flagC = false;
    int interruptMask = 0;

    bool valid = false;
};

/// One line of disassembly.
struct DisasmLine
{
    quint32 address = 0;
    QString bytes;
    QString instruction;

    /// Symbol label emitted on its own line above this instruction, if any.
    QString label;

    bool isCurrentPc = false;
};

/// Everything the UI needs to render a stopped machine.
struct MachineState
{
    Registers regs;
    quint32 pc = 0;
    QList<DisasmLine> disassembly;

    /// Live section bases from `info basepage`, used to resolve source lines.
    quint32 textBase = 0;
    quint32 dataBase = 0;
    quint32 bssBase = 0;

    bool hasBases() const { return textBase != 0; }
};

} // namespace pist
