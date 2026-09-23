// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace pist {

/// One row of a Hatari `memdump`.
struct MemoryRow
{
    quint32 address = 0;
    QVector<quint8> bytes;
};

/// Parse the output of Hatari's `memdump` command.
///
/// The format is one row per 16 bytes:
///
///     00012596: 48 7a 00 0c 3f 3c 00 09 4e 41 5c 8f 60 fe 4f 4b  Hz..?<..NA\OK
///
/// Byte width depends on the command: `m` gives bytes, `m w` words
/// (`487a 000c ...`), `m l` longs. Only the addresses and hex are parsed; the
/// character column is discarded because Hatari renders it with a raw byte
/// mapping that does not survive decoding as UTF-8, and we render our own.
QList<MemoryRow> parseMemoryDump(const QString &response);

/// Render rows back as the `memdump` text Hatari prints — the inverse of
/// parseMemoryDump, one `00012596: 48 7a 00 0c  Hz..?<..NA\OK` line per row.
///
/// For the transport that never sees that text: the hrdb fork answers a dump
/// with a uuencoded payload, so its rows are decoded rather than parsed and
/// this is how the log still gets a readable dump.
QString renderMemoryDump(const QList<MemoryRow> &rows);

/// Render the character column for a row, using printable ASCII and '.' for
/// everything else. Kept separate so the view and tests agree on it.
QString renderMemoryChars(const QVector<quint8> &bytes);

/// Read the 32-bit big-endian value at `offset`, or 0 when fewer than four
/// bytes remain. The 68000 is big-endian, so this is the byte order a pointer
/// in memory actually has.
quint32 readLongBE(const QByteArray &bytes, int offset);

/// The same read for callers holding a dump's bytes as parsed (a row, or a
/// run of them) rather than as the text they were parsed from.
quint32 readLongBE(const QVector<quint8> &bytes, int offset);

/// Whether a 32-bit value could be an address on an ST: non-zero, even (the
/// 68000 faults on odd word/long accesses), and inside the 24-bit address bus.
/// The emulator's own state uses 32-bit registers, so pointers routinely carry
/// junk in the top byte; those cannot be dereferenced and are rejected.
bool looksLikeAddress(quint32 value);

} // namespace pist

// A dump's rows cross the backend boundary on signals (MAJ-45), so they are
// Qt metatypes: a queued connection or a QSignalSpy would otherwise be unable
// to carry them. Declared here, beside the type, and registered by the
// backends that emit them.
Q_DECLARE_METATYPE(pist::MemoryRow)
Q_DECLARE_METATYPE(QList<pist::MemoryRow>)
