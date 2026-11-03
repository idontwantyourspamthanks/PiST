// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QVector>

namespace pist {

/// One shifter palette write from a hatari-pist `paltrace save` file. The
/// line (`hbl`), the position on that line, and the cycles since the vertical
/// blank (`frame`) were taken when the color was stored. They are not a frame
/// cycle count divided by 512: 50 Hz and 60 Hz do not share a line length.
struct PaletteEvent
{
    quint32 sequence = 0;
    quint32 pc = 0;
    quint32 reg = 0;
    quint32 color = 0;
    int vbl = 0;
    int hbl = 0;
    int pos = 0;
    int frame = 0;
};

struct PaletteLog
{
    QVector<PaletteEvent> events;
};

/// Read the text `paltrace save` writes. True only for format `HPISTPAL 1`:
/// the header, then one line per write, in the order they happened. A log
/// with no writes is the header alone.
bool parsePalTrace(const QString &text, PaletteLog *log, QString *error);

} // namespace pist
