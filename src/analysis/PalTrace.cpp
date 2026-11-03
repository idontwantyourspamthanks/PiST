// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/PalTrace.h"

namespace pist {

namespace {

bool hexField(const QString &text, int digits, quint32 *value)
{
    if (text.size() != digits)
        return false;
    for (const QChar ch : text) {
        if (!ch.isDigit() && (ch < QLatin1Char('a') || ch > QLatin1Char('f')))
            return false;
    }
    bool ok = false;
    *value = text.toUInt(&ok, 16);
    return ok;
}

bool decField(const QString &text, int *value)
{
    if (text.isEmpty())
        return false;
    int i = 0;
    if (text.at(0) == QLatin1Char('-')) {
        if (text.size() == 1)
            return false;
        i = 1;
    }
    for (; i < text.size(); ++i) {
        if (!text.at(i).isDigit())
            return false;
    }
    bool ok = false;
    *value = text.toInt(&ok);
    return ok;
}

} // namespace

bool parsePalTrace(const QString &text, PaletteLog *log, QString *error)
{
    if (log)
        *log = PaletteLog();
    const auto fail = [error](const QString &reason) {
        if (error)
            *error = reason;
        return false;
    };

    QStringList rows;
    for (QString line : text.split(QLatin1Char('\n'))) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        rows.append(line);
    }
    while (!rows.isEmpty() && rows.last().isEmpty())
        rows.removeLast();
    if (rows.isEmpty() || rows.at(0) != QLatin1String("HPISTPAL 1"))
        return fail(QStringLiteral("not a paltrace file"));

    PaletteLog parsed;
    parsed.events.reserve(rows.size() - 1);
    for (int i = 1; i < rows.size(); ++i) {
        const QStringList field = rows.at(i).split(QLatin1Char(' '));
        if (field.size() != 16 || field.at(0) != QLatin1String("event")
            || field.at(2) != QLatin1String("pc") || field.at(4) != QLatin1String("reg")
            || field.at(6) != QLatin1String("value") || field.at(8) != QLatin1String("vbl")
            || field.at(10) != QLatin1String("hbl") || field.at(12) != QLatin1String("pos")
            || field.at(14) != QLatin1String("frame"))
            return fail(QStringLiteral("the paltrace file has a line that is not a palette write"));
        bool seqOk = false;
        const quint32 sequence = field.at(1).toUInt(&seqOk);
        if (!seqOk || sequence != quint32(i))
            return fail(QStringLiteral("the paltrace file's writes are not in order"));
        PaletteEvent event;
        event.sequence = sequence;
        if (!hexField(field.at(3), 8, &event.pc) || !hexField(field.at(5), 6, &event.reg)
            || !hexField(field.at(7), 4, &event.color) || !decField(field.at(9), &event.vbl)
            || !decField(field.at(11), &event.hbl) || !decField(field.at(13), &event.pos)
            || !decField(field.at(15), &event.frame))
            return fail(QStringLiteral("the paltrace file has a write whose fields are not numbers"));
        parsed.events.append(event);
    }
    if (log)
        *log = parsed;
    if (error)
        error->clear();
    return true;
}

} // namespace pist
