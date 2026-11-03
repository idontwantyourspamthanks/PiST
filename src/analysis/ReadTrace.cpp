// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/ReadTrace.h"

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

bool parseReadTrace(const QString &text, ReadTraceLog *log, QString *error)
{
    if (log)
        *log = ReadTraceLog();
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
    if (rows.size() < 5 || rows.at(0) != QLatin1String("HPISTREAD 1"))
        return fail(QStringLiteral("not a readtrace file"));

    const QStringList range = rows.at(1).split(QLatin1Char(' '));
    quint32 lo = 0;
    quint32 hi = 0;
    if (range.size() != 3 || range.at(0) != QLatin1String("range")
        || !hexField(range.at(1), 8, &lo) || !hexField(range.at(2), 8, &hi) || lo > hi)
        return fail(QStringLiteral("the readtrace file has no range"));

    const QStringList cap = rows.at(2).split(QLatin1Char(' '));
    bool capOk = false;
    const quint32 capacity = cap.size() == 2 && cap.at(0) == QLatin1String("capacity")
                                 ? cap.at(1).toUInt(&capOk)
                                 : 0;
    if (!capOk || capacity < 1)
        return fail(QStringLiteral("the readtrace file has no capacity"));

    const QStringList drop = rows.at(3).split(QLatin1Char(' '));
    bool dropOk = false;
    const quint64 dropped = drop.size() == 2 && drop.at(0) == QLatin1String("dropped")
                                ? drop.at(1).toULongLong(&dropOk)
                                : 0;
    if (!dropOk)
        return fail(QStringLiteral("the readtrace file has no drop count"));

    const QStringList cost = rows.last().split(QLatin1Char(' '));
    bool offOk = false;
    bool onOk = false;
    bool readsOk = false;
    if (cost.size() != 7 || cost.at(0) != QLatin1String("cost") || cost.at(1) != QLatin1String("off")
        || cost.at(3) != QLatin1String("on") || cost.at(5) != QLatin1String("reads"))
        return fail(QStringLiteral("the readtrace file has no cost line"));
    cost.at(2).toULongLong(&offOk);
    cost.at(4).toULongLong(&onOk);
    cost.at(6).toULongLong(&readsOk);
    if (!offOk || !onOk || !readsOk)
        return fail(QStringLiteral("the readtrace file has no cost line"));

    QVector<ReadEvent> events;
    for (int i = 4; i + 1 < rows.size(); ++i) {
        const QStringList field = rows.at(i).split(QLatin1Char(' '));
        // event <seq> cpu <width> <addr> <value> pc <pc>
        if (field.size() != 8 || field.at(0) != QLatin1String("event")
            || field.at(2) != QLatin1String("cpu") || field.at(6) != QLatin1String("pc"))
            return fail(QStringLiteral("the readtrace file has an event that is not a CPU read"));
        bool seqOk = false;
        bool widthOk = false;
        const quint32 sequence = field.at(1).toUInt(&seqOk);
        const int width = field.at(3).toInt(&widthOk);
        quint32 address = 0;
        quint32 value = 0;
        quint32 pc = 0;
        if (!seqOk || !widthOk || (width != 1 && width != 2 && width != 4)
            || !hexField(field.at(4), 8, &address) || !hexField(field.at(5), width * 2, &value)
            || !hexField(field.at(7), 8, &pc))
            return fail(QStringLiteral("the readtrace file has a CPU read with a bad field"));
        ReadEvent event;
        event.sequence = sequence;
        event.width = width;
        event.address = address;
        event.value = value;
        event.pc = pc;
        events.append(event);
    }

    if (!log)
        return true;
    log->rangeLo = lo;
    log->rangeHi = hi;
    log->capacity = capacity;
    log->dropped = dropped;
    log->events = events;
    return true;
}

} // namespace pist
