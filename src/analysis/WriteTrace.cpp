// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/WriteTrace.h"

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

} // namespace

bool parseWriteTrace(const QString &text, WriteTraceLog *log, QString *error)
{
    if (log)
        *log = WriteTraceLog();
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
    if (rows.size() < 5 || rows.at(0) != QLatin1String("HPISTWRITE 1"))
        return fail(QStringLiteral("not a writetrace file"));

    const QStringList range = rows.at(1).split(QLatin1Char(' '));
    quint32 lo = 0;
    quint32 hi = 0;
    if (range.size() != 3 || range.at(0) != QLatin1String("range")
        || !hexField(range.at(1), 8, &lo) || !hexField(range.at(2), 8, &hi) || lo > hi)
        return fail(QStringLiteral("the writetrace file has no range"));

    const QStringList cap = rows.at(2).split(QLatin1Char(' '));
    bool capOk = false;
    const quint32 capacity = cap.size() == 2 && cap.at(0) == QLatin1String("capacity")
                                 ? cap.at(1).toUInt(&capOk)
                                 : 0;
    if (!capOk || capacity < 1)
        return fail(QStringLiteral("the writetrace file has no capacity"));

    const QStringList drop = rows.at(3).split(QLatin1Char(' '));
    bool dropOk = false;
    const quint64 dropped = drop.size() == 2 && drop.at(0) == QLatin1String("dropped")
                                ? drop.at(1).toULongLong(&dropOk)
                                : 0;
    if (!dropOk)
        return fail(QStringLiteral("the writetrace file has no drop count"));

    const QStringList cost = rows.last().split(QLatin1Char(' '));
    bool offOk = false;
    bool onOk = false;
    bool writesOk = false;
    if (cost.size() != 7 || cost.at(0) != QLatin1String("cost") || cost.at(1) != QLatin1String("off")
        || cost.at(3) != QLatin1String("on") || cost.at(5) != QLatin1String("writes"))
        return fail(QStringLiteral("the writetrace file has no cost line"));
    cost.at(2).toULongLong(&offOk);
    cost.at(4).toULongLong(&onOk);
    cost.at(6).toULongLong(&writesOk);
    if (!offOk || !onOk || !writesOk)
        return fail(QStringLiteral("the writetrace file has no cost line"));

    WriteTraceLog parsed;
    parsed.rangeLo = lo;
    parsed.rangeHi = hi;
    parsed.capacity = capacity;
    parsed.dropped = dropped;
    const int eventCount = rows.size() - 5;
    if (quint64(eventCount) > capacity)
        return fail(QStringLiteral("the writetrace file has more events than its capacity"));
    parsed.events.reserve(eventCount);
    for (int i = 0; i < eventCount; ++i) {
        const QStringList field = rows.at(4 + i).split(QLatin1Char(' '));
        if (field.size() < 7 || field.at(0) != QLatin1String("event"))
            return fail(QStringLiteral("the writetrace file has a line that is not an event"));
        bool seqOk = false;
        const quint32 sequence = field.at(1).toUInt(&seqOk);
        if (!seqOk || sequence != quint32(i + 1))
            return fail(QStringLiteral("the writetrace file's events are not in order"));
        WriteEvent event;
        event.sequence = sequence;
        const QString origin = field.at(2);
        if (origin == QLatin1String("cpu"))
            event.origin = WriteOrigin::Cpu;
        else if (origin == QLatin1String("blitter"))
            event.origin = WriteOrigin::Blitter;
        else if (origin == QLatin1String("unknown"))
            event.origin = WriteOrigin::Unknown;
        else
            return fail(QStringLiteral("the writetrace file has an origin that is not cpu, blitter or unknown"));
        bool widthOk = false;
        event.width = field.at(3).toInt(&widthOk);
        if (!widthOk || (event.width != 1 && event.width != 2 && event.width != 4))
            return fail(QStringLiteral("the writetrace file has a width that is not 1, 2 or 4"));
        const int digits = event.width * 2;
        if (!hexField(field.at(4), 8, &event.address) || !hexField(field.at(5), digits, &event.oldValue)
            || !hexField(field.at(6), digits, &event.newValue))
            return fail(QStringLiteral("the writetrace file has an event whose address or value is not hex"));
        const bool cpu = event.origin == WriteOrigin::Cpu;
        if (cpu) {
            if (field.size() != 9 || field.at(7) != QLatin1String("pc")
                || !hexField(field.at(8), 8, &event.pc))
                return fail(QStringLiteral("the writetrace file has a cpu event with no pc"));
        } else if (field.size() != 7) {
            return fail(QStringLiteral("the writetrace file has a pc on a write that has no writer"));
        }
        parsed.events.append(event);
    }
    if (log)
        *log = parsed;
    if (error)
        error->clear();
    return true;
}

} // namespace pist
