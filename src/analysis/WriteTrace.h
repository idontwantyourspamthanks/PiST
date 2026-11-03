// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QVector>

namespace pist {

/// Who stored the bytes. A CPU event is the only one with a PC. A blitter
/// event and an unknown event (a debugger poke, or any store that was not
/// tagged) have none, and the missing PC is not filled from the stopped one.
enum class WriteOrigin { Unknown, Cpu, Blitter };

/// One write from a hatari-pist `writetrace save` file. `oldValue` and
/// `newValue` are significant in the low `width` bytes.
struct WriteEvent
{
    quint32 sequence = 0;
    WriteOrigin origin = WriteOrigin::Unknown;
    int width = 0;
    quint32 address = 0;
    quint32 oldValue = 0;
    quint32 newValue = 0;
    quint32 pc = 0;
};

/// The log `writetrace save` writes. Events are the oldest ones the ring kept,
/// in the order they happened. `dropped` counts later writes that were not
/// stored. The host-time `cost` line is checked and not kept: it is not a
/// fact about the guest.
struct WriteTraceLog
{
    quint32 rangeLo = 0;
    quint32 rangeHi = 0;
    quint32 capacity = 0;
    quint64 dropped = 0;
    QVector<WriteEvent> events;
};

/// Read the text `writetrace save` writes. True only for format `HPISTWRITE 1`:
/// the header, the range, the capacity, the drop count, the events, and the
/// cost line. An empty ring is those lines and no events.
bool parseWriteTrace(const QString &text, WriteTraceLog *log, QString *error);

} // namespace pist
