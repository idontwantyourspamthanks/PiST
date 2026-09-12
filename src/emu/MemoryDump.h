// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QList>
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

/// Render the character column for a row, using printable ASCII and '.' for
/// everything else. Kept separate so the view and tests agree on it.
QString renderMemoryChars(const QVector<quint8> &bytes);

} // namespace pist
