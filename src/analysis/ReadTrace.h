// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QVector>

namespace pist {

/// One CPU data read from a hatari-pist `readtrace save` file. `value` is
/// significant in the low `width` bytes. Instruction fetches are not events.
struct ReadEvent
{
    quint32 sequence = 0;
    int width = 0;
    quint32 address = 0;
    quint32 value = 0;
    quint32 pc = 0;
};

/// The log `readtrace save` writes. Events are the oldest ones the ring kept.
/// `dropped` counts later reads that were not stored.
struct ReadTraceLog
{
    quint32 rangeLo = 0;
    quint32 rangeHi = 0;
    quint32 capacity = 0;
    quint64 dropped = 0;
    QVector<ReadEvent> events;
};

/// Read the text `readtrace save` writes. True only for format `HPISTREAD 1`.
bool parseReadTrace(const QString &text, ReadTraceLog *log, QString *error);

} // namespace pist
