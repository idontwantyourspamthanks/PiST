// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
#include "emu/ProfileData.h"

#include <QCoreApplication>
#include <QHash>
#include <QList>
#include <QString>
#include <QVector>
#include <algorithm>

namespace pist {

/// The floor of the ST's ROM space.
///
/// A save file normally names its memory areas (`ROM_TOS`, `CARTRIDGE`, …) and
/// those decide where ROM time went. Older saves have no area lines at all, and
/// then the address alone has to say it: the cartridge area starts at $fa0000
/// and TOS's ROM at $fc0000, so the lower of the two is the floor — anything at
/// or above it is the OS's time, not a source line's, whatever the machine.
inline constexpr quint32 kRomFloor = 0xfa0000;

/// One source line's aggregated cost.
struct AttributedLine
{
    int line = 0;
    quint64 count = 0;
    quint64 cycles = 0;
};

/// One routine's aggregate, plus its lines, hottest first within.
struct AttributedRoutine
{
    QString name;
    int defLine = 0; ///< the label's source line, for activation
    quint64 count = 0;
    quint64 cycles = 0;
    QList<AttributedLine> lines;
};

/// A profile save with every address resolved to a routine and a line, or to
/// the ROM area it ran in — plain data, with no widget behind it (MAJ-44).
///
/// This used to be computed inside the Profiler dock as `setProfile` ran, and
/// the result lived only in the widget's members: the tree population read it,
/// the editor's gutter heat asked the dock for `lineCounts()`, and the remote
/// `profile results` verb reached into the dock for the same numbers. Two of
/// those three consumers were programmatic APIs depending on a dock having been
/// constructed and populated, and the analysis itself had no test that could
/// reach it.
struct AttributedProfile
{
    /// The routines of the profiled source file, sorted by descending cycles.
    QList<AttributedRoutine> routines;
    /// ROM regions with profiled time (ROM_TOS, CARTRIDGE), sorted likewise.
    QList<AttributedRoutine> rom;
    /// Samples whose address resolved to neither a line of the source file nor
    /// a ROM region.
    int unmapped = 0;
    quint64 unmappedCycles = 0;

    quint64 totalCount = 0;
    quint64 totalCycles = 0;
    quint32 clockHz = 0;

    /// The profile had executed instructions at all. False is what a `profile
    /// on` issued mid-run produces, and the view says "no profile" for it
    /// rather than showing an empty table.
    bool hasSamples = false;
    /// The map was usable — resolved, non-empty — so address-to-line worked.
    /// False means nothing *could* be attributed, which is the normal state
    /// before the program has been built and run, and reads quite differently
    /// to the user from "the profiler found nothing".
    bool resolved = false;
};

/// Attribute `profile` to `sourceFile` through `map`, anchoring each address to
/// the nearest preceding code label of `symbols`.
///
/// A profiler's raw output is per address; what an assembly developer acts on
/// is per routine and per line — "clearScreen is 60% of the frame" — so the
/// addresses are resolved through the same ProgramLineMap the debugger uses,
/// attributed to the nearest code label, and summed. Lines outside `sourceFile`
/// are dropped (the counts of what was dropped stay in `unmapped`, so a
/// partially mapped profile does not read as a complete one).
///
/// Two currencies are aggregated: execution counts (what the gutter heat scales
/// from) and cycles (what a 68000 actually spends — a divs is not a moveq).
/// Time spent inside ROM trap handlers is kept as a TOS/ROM row rather than
/// discarded as unmapped only because it is not in the user's source.
///
/// Defined in the header rather than in a .cpp of its own: it is pure over its
/// arguments, and its two halves live in different modules — ProfileData in
/// emu/, ProgramLineMap and SymbolTable in build/ — so an implementation file
/// would make pist_emu link pist_build for a function no emu/ code calls. The
/// callers (the profiler controller, and the profiler tests) already link both.
inline AttributedProfile attributedProfile(const ProfileData &profile,
                                           const ProgramLineMap &map,
                                           const QString &sourceFile,
                                           const QVector<SymbolEntry> &symbols)
{
    AttributedProfile attributed;
    attributed.totalCount = profile.totalCount;
    attributed.totalCycles = profile.totalCycles;
    attributed.clockHz = profile.clockHz;
    attributed.hasSamples = !profile.isEmpty();
    attributed.resolved = !map.isEmpty() && map.isResolved();
    if (!attributed.hasSamples || !attributed.resolved)
        return attributed;

    /// A code label with its resolved address: the anchor a hot address's
    /// routine is read from.
    struct Anchor
    {
        quint32 address = 0;
        QString name;
        int defLine = 0;
    };

    // Routine anchors: the current file's code labels with their resolved
    // addresses. A label's own line often emits no code, so the anchor is the
    // first line at or after it that did.
    QList<Anchor> anchors;
    for (const SymbolEntry &sym : symbols) {
        if (sym.file.isEmpty() || !LineMap::sameSource(sym.file, sourceFile))
            continue;
        const int codeLine = map.nextCodeLine(sym.file, sym.line);
        if (codeLine <= 0)
            continue;
        quint32 address = 0;
        if (!map.codeAddressFor(sym.file, codeLine, &address))
            continue;
        anchors.append({address, sym.name, sym.line});
    }
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor &a, const Anchor &b) { return a.address < b.address; });

    const auto routineFor = [&anchors](quint32 address) -> const Anchor * {
        const auto past = std::upper_bound(anchors.begin(), anchors.end(), address,
                                           [](quint32 value, const Anchor &anchor) {
                                               return value < anchor.address;
                                           });
        return past == anchors.begin() ? nullptr : &*(past - 1);
    };

    // The ROM areas an address can belong to: time in trap handlers is the
    // OS's, not any source line's. Name-based, with the well-known ROM floor
    // (kRomFloor) as the fallback for a save with no area lines.
    const auto romRegionFor = [&profile](quint32 address) -> QString {
        for (const ProfileRegion &region : profile.regions) {
            if (address >= region.first && address <= region.last
                && (region.name == QLatin1String("ROM_TOS")
                    || region.name == QLatin1String("CARTRIDGE")))
                return region.name;
        }
        if (profile.regions.isEmpty() && address >= kRomFloor)
            return QStringLiteral("ROM");
        return QString();
    };

    QHash<int, int> lineIndex;     // line -> index in its routine's lines
    QHash<QString, int> routineOf; // routine name -> index in routines
    QHash<QString, int> romOf;     // region name -> index in rom
    for (const ProfileLine &entry : profile.lines) {
        LineMap::Address where;
        if (map.lineFor(entry.address, &where) && where.line > 0
            && LineMap::sameSource(where.file, sourceFile)) {
            const Anchor *anchor = routineFor(entry.address);
            // The bucket for a line before the file's first label. Translated
            // in the view's own context, which is where this string has always
            // lived ("pist::ProfilerView" is what moc passes for tr() here) —
            // it is the only user-visible string this function produces.
            const QString routine = anchor
                ? anchor->name
                : QCoreApplication::translate("pist::ProfilerView", "(no routine)");
            int ri = routineOf.value(routine, -1);
            if (ri < 0) {
                ri = attributed.routines.size();
                routineOf.insert(routine, ri);
                AttributedRoutine cost;
                cost.name = routine;
                cost.defLine = anchor ? anchor->defLine : 0;
                attributed.routines.append(cost);
            }
            AttributedRoutine &routineCost = attributed.routines[ri];
            routineCost.count += entry.count;
            routineCost.cycles += entry.cycles;

            const int li = lineIndex.value(where.line, -1);
            if (li >= 0) {
                routineCost.lines[li].count += entry.count;
                routineCost.lines[li].cycles += entry.cycles;
            } else {
                lineIndex.insert(where.line, routineCost.lines.size());
                routineCost.lines.append({where.line, entry.count, entry.cycles});
            }
            continue;
        }

        const QString region = romRegionFor(entry.address);
        if (!region.isEmpty()) {
            int ri = romOf.value(region, -1);
            if (ri < 0) {
                ri = attributed.rom.size();
                romOf.insert(region, ri);
                AttributedRoutine cost;
                cost.name = region;
                attributed.rom.append(cost);
            }
            attributed.rom[ri].count += entry.count;
            attributed.rom[ri].cycles += entry.cycles;
            continue;
        }

        ++attributed.unmapped;
        attributed.unmappedCycles += entry.cycles;
    }

    for (AttributedRoutine &routine : attributed.routines) {
        std::sort(routine.lines.begin(), routine.lines.end(),
                  [](const AttributedLine &a, const AttributedLine &b) {
                      return a.cycles != b.cycles ? a.cycles > b.cycles : a.line < b.line;
                  });
    }
    const auto byCyclesDesc = [](const AttributedRoutine &a, const AttributedRoutine &b) {
        return a.cycles != b.cycles ? a.cycles > b.cycles : a.name < b.name;
    };
    std::sort(attributed.routines.begin(), attributed.routines.end(), byCyclesDesc);
    std::sort(attributed.rom.begin(), attributed.rom.end(), byCyclesDesc);
    return attributed;
}

/// Per-source-line execution counts of an attributed profile, for the editor's
/// gutter heat and the remote `profile results` verb. These are the very
/// numbers the tree renders, so the heat, the tree and the JSON cannot disagree
/// about what is hot — and neither of the two programmatic readers needs the
/// dock that shows them.
inline QHash<int, quint64> lineCounts(const AttributedProfile &profile)
{
    QHash<int, quint64> counts;
    for (const AttributedRoutine &routine : profile.routines) {
        for (const AttributedLine &line : routine.lines)
            counts.insert(line.line, line.count);
    }
    return counts;
}

} // namespace pist
