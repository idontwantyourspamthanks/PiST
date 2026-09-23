// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QChar>
#include <QString>
#include <QVector>

namespace pist {
namespace hex {

/// Hex formatting, in the three shapes the tree actually renders.
///
/// The spelling of a hex string was re-derived at every call site — fifteen-odd
/// of them across the debug panes, the remote-control JSON and the dump
/// renderer — each one its own `QStringLiteral("%1").arg(value, 8, 16,
/// QLatin1Char('0'))` with its own width, padding and letter case (finding
/// MIN-54). One miss in that copy is a pane showing 7 digits or a parser on the
/// other side of the protocol rejecting a reply.
///
/// Header-only and Qt-Core-only, so every module — emu, control, ui — can take
/// it without a link edge.

/// Which alphabet a hex string uses.
///
/// Both are deliberate, and the difference is not cosmetic: the debug panes and
/// the toolchain's own listings are upper case, while the text a *program*
/// reads — the remote-control JSON, and the dump lines MemoryDump parses back —
/// is lower case. Choosing one here is what stops that being re-decided per
/// site.
enum class Case { Upper, Lower };

/// Eight hexadecimal digits, zero padded: `00012596`. The address and word
/// form the register, stack, memory and disassembly panes show.
inline QString hex32(quint32 value, Case letterCase = Case::Upper)
{
    const QString digits = QString::number(value, 16).rightJustified(8, QLatin1Char('0'));
    return letterCase == Case::Upper ? digits.toUpper() : digits;
}

/// `$00012596`: an address in the form the symbol pane and the Atari toolchain
/// spell one.
inline QString hexAddr(quint32 value)
{
    return QLatin1Char('$') + hex32(value);
}

/// Two hexadecimal digits, zero padded: `0c`. One byte of a dump, a memory pane
/// cell or a byte column.
inline QString hexByte(quint8 value, Case letterCase = Case::Upper)
{
    const QString digits = QString::number(value, 16).rightJustified(2, QLatin1Char('0'));
    return letterCase == Case::Upper ? digits.toUpper() : digits;
}

/// A dump line's address and hex field: `00012596: 48 7a 00 0c`. The layout
/// Hatari's own `memdump` prints, which MemoryDump parses back — so this is the
/// one place that spelling may change. The character column that follows it is
/// the caller's (see renderMemoryChars).
inline QString hexDumpRow(quint32 address, const QVector<quint8> &bytes)
{
    QString text = hex32(address, Case::Lower) + QLatin1Char(':');
    for (quint8 byte : bytes)
        text += QLatin1Char(' ') + hexByte(byte, Case::Lower);
    return text;
}

} // namespace hex
} // namespace pist
