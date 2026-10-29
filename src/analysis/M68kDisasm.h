// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QHash>
#include <QSet>
#include <QString>

namespace pist {

/// Names the disassembler may substitute for a number. Empty maps leave every
/// operand numeric. Labels are image-relative text, data, and bss addresses.
/// Equates are used for an immediate only, and never for zero.
struct DisasmNames
{
    QHash<quint32, QString> labels;
    QHash<quint32, QString> equates;
    QSet<quint32> relocations;
};

/// One 68000 instruction. `length` 0 means the bytes at `address` are not a
/// complete instruction inside the limit (the caller keeps them as data).
/// A `dc.w` result is a word the decoder will not treat as code.
struct DecodedInsn
{
    enum class Flow { Fall, Stop, Branch, Call };

    int length = 0;
    QString text;
    Flow flow = Flow::Fall;
    bool hasTarget = false;
    quint32 target = 0;
    /// A jump or call whose destination is a register, not an address.
    bool indirect = false;
};

/// Decode the 68000 instruction at `address`. `limit` is the exclusive end of
/// the bytes that belong to code, so an instruction cannot run into the data
/// segment. 68020 full extensions and 32-bit branches are `dc.w`.
DecodedInsn decode68k(const QByteArray &image, quint32 address, quint32 limit,
                      const DisasmNames &names = DisasmNames());

} // namespace pist
