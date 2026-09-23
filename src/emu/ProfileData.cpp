// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/ProfileData.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

namespace pist {

namespace {

using Re = const QRegularExpression &;

// `Hatari CPU profile (Hatari v2.6.1)` — Profile_Save writes
// "<emulator> <processor> profile [(<info>)]"; the parenthesised part is
// PROG_NAME, present when the build names itself.
Re titleRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^(\S+)\s+(\S+)\s+profile(?:\s+\(([^)]*)\))?\s*$)RX"));
    return re;
}

// `Cycles/second:\t8021247`
Re clockRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^Cycles/second:\s*(\d+)\s*$)RX"));
    return re;
}

// `Field names:\tExecuted instructions, Used cycles, ...`
Re fieldsRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^Field names:\s*(.*?)\s*$)RX"));
    return re;
}

// `Field regexp:\t^\$?([0-9A-Fa-f]+) .*% \(([^)]*)\)$`
Re fieldRegexpRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^Field regexp:\s*(.*?)\s*$)RX"));
    return re;
}

// `ST_RAM:\t\t0x000000-0x100000` — one of the memory-area lines Profile_CpuSave
// writes between the header and the disassembly. The name and span are kept so
// time inside ROM can be attributed to the OS rather than discarded: PiST
// already knows the program's extent from the line map, but a ROM address is
// nobody's source line, and "unmapped" should not silently swallow it.
Re areaRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^([^:]+):\s*0x([0-9A-Fa-f]+)-0x([0-9A-Fa-f]+)\s*$)RX"));
    return re;
}

// A profile disassembly line, in either disassembler's shape. Which one appears
// depends on Hatari's `bDisasmUAE` user setting, not on the build — the same
// split emu/HatariTextParse.cpp handles for the `d` command:
//
//   WinUAE core:  `00e00cfe 4e75  rts  == $e66218   0.16% (48753, 780396, 0, 0)`
//   external:     `$e5af38 :   rts           0.00% (12, 0, 12, 0)`
//
// CPU addresses may be bare or `$`-prefixed and in either case; DSP addresses
// carry a `p:` prefix (profiledsp.c's saved regexp is `^p:([0-9a-f]+) .*% \((.*)\)$`).
// The tail is the parenthesised, comma-separated field list, whose order is
// pinned by the file's own `Field names:` line — instructions then cycles first,
// for both the CPU and the DSP save (profile_priv.h says so explicitly, and
// counters_t declares calls/count/cycles in that order).
Re entryRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"RX(^(?:\$|p:)?([0-9A-Fa-f]+)\s+\S.*% \(([^)]*)\)\s*$)RX"));
    return re;
}

/// Whether the line is a symbol label printed above a function's instructions
/// (`start:`), which the parser skips. A caller/callee record is distinguished
/// by its `=`; a label can even contain spaces (the post-processor's own
/// `^([._a-zA-Z(][^$?@;]*):$` allows them).
bool isSymbolLabel(const QString &line)
{
    return line.endsWith(QLatin1Char(':')) && !line.contains(QLatin1Char('='));
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

bool parseProfileText(const QString &text, ProfileData *data, QString *error)
{
    if (!data)
        return fail(error, QObject::tr("no destination for the parsed profile"));

    ProfileData parsed;

    // Split by hand and trim a trailing '\r', so a file that travelled through
    // a Windows editor still parses.
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
    }

    int at = 0;
    const auto next = [&lines, &at]() -> QString {
        return at < lines.size() ? lines.at(at++) : QString();
    };

    // --- header: exactly the four lines Profile_Save writes, in order --------
    const QString title = next();
    const auto titleMatch = titleRe().match(title);
    if (!titleMatch.hasMatch())
        return fail(error, QObject::tr("not a Hatari profile: the first line is "
                                       "not '<emulator> <processor> profile': '%1'")
                               .arg(title.trimmed()));
    parsed.processor = titleMatch.captured(2);
    parsed.emulator = titleMatch.captured(3);

    const QString clockLine = next();
    const auto clockMatch = clockRe().match(clockLine);
    if (!clockMatch.hasMatch())
        return fail(error, QObject::tr("invalid Cycles/second line: '%1'")
                               .arg(clockLine.trimmed()));
    // Checked: an unparseable or overflowing value becomes a silent 0 without
    // the flag, and a profile claiming a 0 Hz clock is worse than a refusal —
    // every cycle-to-time conversion downstream divides by it (MIN-5).
    bool clockOk = false;
    parsed.clockHz = clockMatch.captured(1).toUInt(&clockOk);
    if (!clockOk)
        return fail(error, QObject::tr("Cycles/second value is not a 32-bit count: '%1'")
                               .arg(clockMatch.captured(1)));

    const QString fieldsLine = next();
    const auto fieldsMatch = fieldsRe().match(fieldsLine);
    if (!fieldsMatch.hasMatch())
        return fail(error, QObject::tr("invalid Field names line: '%1'")
                               .arg(fieldsLine.trimmed()));
    const QStringList fields = fieldsMatch.captured(1).split(QLatin1Char(','));
    for (const QString &field : fields) {
        const QString trimmed = field.trimmed();
        if (!trimmed.isEmpty())
            parsed.fieldNames.append(trimmed);
    }
    if (parsed.fieldNames.size() < 2)
        return fail(error, QObject::tr("Field names lists %1 field(s): the "
                                       "instructions and cycles fields Profile_Save "
                                       "writes first are missing")
                               .arg(parsed.fieldNames.size()));

    // The regexp line is a format marker only. PiST matches the two documented
    // disassembly shapes itself rather than compiling a regexp out of the file:
    // a regexp that compiled but matched nothing would silently yield an empty
    // profile instead of an error.
    const QString regexpLine = next();
    if (!fieldRegexpRe().match(regexpLine).hasMatch())
        return fail(error, QObject::tr("invalid Field regexp line: '%1'")
                               .arg(regexpLine.trimmed()));

    // --- memory areas, up to the disassembly comment -------------------------
    while (at < lines.size()) {
        const QString line = lines.at(at).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            ++at;
            continue;
        }
        const QRegularExpressionMatch area = areaRe().match(line);
        if (area.hasMatch()) {
            ProfileRegion region;
            region.name = area.captured(1).trimmed();
            // Checked, like every other number in this file: an address that
            // overflows 32 bits parses to a silent 0, and a zeroed span turns a
            // region's samples into unmapped ones without a word of complaint
            // (MIN-5).
            bool firstOk = false;
            bool lastOk = false;
            region.first = area.captured(2).toUInt(&firstOk, 16);
            region.last = area.captured(3).toUInt(&lastOk, 16);
            if (!firstOk || !lastOk)
                return fail(error, QObject::tr("memory-area span at line %1 overflows a "
                                               "32-bit address: '%2'")
                                       .arg(at + 1)
                                       .arg(line));
            parsed.regions.append(region);
            ++at;
            continue;
        }
        break;
    }

    // --- disassembly: the profiled instructions ------------------------------
    quint32 previous = 0;
    bool havePrevious = false;
    while (at < lines.size()) {
        const QString line = lines.at(at).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
            ++at;
            continue;
        }
        // "[...]" is the emulator's marker for an address gap; the gap itself
        // holds no profiled instruction.
        if (line == QLatin1String("[...]")) {
            ++at;
            continue;
        }
        if (isSymbolLabel(line)) {
            ++at;
            continue;
        }

        const auto match = entryRe().match(line);
        if (!match.hasMatch())
            break; // the caller/callee section, or the end of the profiled data

        const quint32 address = match.captured(1).toUInt(nullptr, 16);
        if (havePrevious && address < previous)
            return fail(error,
                        QObject::tr("profile addresses are out of order at line %1 "
                                    "('$%2' after '$%3')")
                            .arg(at + 1)
                            .arg(address, 0, 16)
                            .arg(previous, 0, 16));
        previous = address;
        havePrevious = true;

        const QStringList values = match.captured(2).split(QLatin1Char(','));
        if (values.size() < 2)
            return fail(error,
                        QObject::tr("profile line %1 carries no instructions/cycles "
                                    "pair: '%2'")
                            .arg(at + 1)
                            .arg(line));
        bool okCount = false;
        bool okCycles = false;
        ProfileLine entry;
        entry.address = address;
        entry.count = values.at(0).trimmed().toULongLong(&okCount);
        entry.cycles = values.at(1).trimmed().toULongLong(&okCycles);
        if (!okCount || !okCycles)
            return fail(error,
                        QObject::tr("profile line %1 has a non-numeric count: '%2'")
                            .arg(at + 1)
                            .arg(line));

        parsed.totalCount += entry.count;
        parsed.totalCycles += entry.cycles;
        parsed.lines.append(entry);
        ++at;
    }

    if (parsed.lines.isEmpty()) {
        // Exactly what a `profile on` issued while the emulation was already
        // running produces: collection only covers the run between one stop and
        // the next, and the buffers are zeroed when it begins (Profile_CpuStart,
        // called from DebugCpu_SetDebugging on continue). Naming that is more
        // use than an empty table.
        return fail(error, QObject::tr("no profiled instructions in the file "
                                       "(was profiling enabled while the program "
                                       "was stopped, before it ran?)"));
    }

    *data = parsed;
    return true;
}

bool parseProfile(const QString &path, ProfileData *data, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(error, QObject::tr("cannot read profile '%1': %2")
                               .arg(path, file.errorString()));

    QTextStream stream(&file);
    return parseProfileText(stream.readAll(), data, error);
}

} // namespace pist
