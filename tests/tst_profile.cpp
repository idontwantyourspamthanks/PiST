// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include <QtTest>

#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"
#include "emu/AttributedProfile.h"
#include "emu/ProfileData.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

using namespace pist;

// The `profile save` parser: the header fields PiST consumes and, since the
// profiler's TOS/ROM row, the memory-area lines the header carries. Fixtures
// follow the Hatari 2.6.1 save shape documented in ProfileData.h.
class TstProfile : public QObject
{
    Q_OBJECT

private slots:
    void headerAndEntriesParse();
    void regionsAreCapturedWithTheirSpans();
    void aSaveWithoutRegionsStillParses();
    void aSaveWithNoExecutedInstructionsIsAnError();

    // The attribution model (MAJ-44), read straight from the free function:
    // it used to live inside ProfilerView::setProfile with no way to reach it
    // from a test.
    void attributionAnchorsRoutinesAggregatesAndSorts();
    void attributionDropsLinesOutsideTheSourceFile();
    void attributionKeepsRomTimeByRegion();
    void attributionFallsBackToTheRomFloor();
    void attributionLineCountsMatchTheRoutines();
    void attributionReportsAnEmptySaveOrAnUnresolvedMap();
};

namespace {

QString saveText(const QString &areas)
{
    return QStringLiteral("Hatari CPU profile (Hatari v2.6.1)\n"
                          "Cycles/second:\t8021247\n"
                          "Field names:\tExecuted instructions, Used cycles, "
                          "Instruction cache misses, Data cache hits\n"
                          "Field regexp:\t^\\$?([0-9A-Fa-f]+) .*% \\(([^)]*)\\)$\n")
           + areas
           + QStringLiteral("# disassembly with profile data: <instructions percentage>% (...)\n"
                            "start:\n"
                            "$12596 :   moveq #0,d0     0.10% (1, 4, 0, 0)\n"
                            "$12598 :   addq.w #1,d0    97.00% (100, 400, 0, 0)\n");
}

} // namespace

void TstProfile::headerAndEntriesParse()
{
    ProfileData data;
    QString error;
    QVERIFY2(parseProfileText(saveText(QStringLiteral("ST_RAM:\t\t0x000000-0x100000\n")),
                              &data, &error),
             qPrintable(error));
    QCOMPARE(data.processor, QStringLiteral("CPU"));
    QCOMPARE(data.emulator, QStringLiteral("Hatari v2.6.1"));
    QCOMPARE(data.clockHz, 8021247u);
    QCOMPARE(data.lines.size(), 2);
    QCOMPARE(data.lines.at(1).address, 0x12598u);
    QCOMPARE(data.lines.at(1).count, 100ull);
    QCOMPARE(data.lines.at(1).cycles, 400ull);
    QCOMPARE(data.totalCount, 101ull);
    QCOMPARE(data.totalCycles, 404ull);
}

void TstProfile::regionsAreCapturedWithTheirSpans()
{
    // The TOS/ROM row is fed by these: a dropped or misread area line silently
    // sends ROM time back to "unmapped".
    ProfileData data;
    QString error;
    QVERIFY2(parseProfileText(saveText(QStringLiteral("ST_RAM:\t\t0x000000-0x100000\n"
                                                      "ROM_TOS:\t\t0xfc0000-0x1000000\n"
                                                      "CARTRIDGE:\t0xfa0000-0xfc0000\n"
                                                      "PROGRAM_TEXT:\t0x0012554-0x0012600\n")),
                              &data, &error),
             qPrintable(error));
    QCOMPARE(data.regions.size(), 4);
    QCOMPARE(data.regions.at(1).name, QStringLiteral("ROM_TOS"));
    QCOMPARE(data.regions.at(1).first, 0xfc0000u);
    QCOMPARE(data.regions.at(1).last, 0x1000000u);
    QCOMPARE(data.regions.at(2).name, QStringLiteral("CARTRIDGE"));
    QCOMPARE(data.regions.at(3).first, 0x12554u);
}

void TstProfile::aSaveWithoutRegionsStillParses()
{
    // An older save without area lines: no regions, and the ROM-floor fallback
    // in the attribution (kRomFloor) carries the OS share instead.
    ProfileData data;
    QString error;
    QVERIFY2(parseProfileText(saveText(QString()), &data, &error), qPrintable(error));
    QVERIFY(data.regions.isEmpty());
    QCOMPARE(data.lines.size(), 2);
}

void TstProfile::aSaveWithNoExecutedInstructionsIsAnError()
{
    const QString empty = QStringLiteral("Hatari CPU profile (Hatari v2.6.1)\n"
                                         "Cycles/second:\t8021247\n"
                                         "Field names:\tExecuted instructions, Used cycles\n"
                                         "Field regexp:\t^\\$?([0-9A-Fa-f]+) .*% \\(([^)]*)\\)$\n"
                                         "# disassembly with profile data: <instructions percentage>% (...)\n");
    ProfileData data;
    QString error;
    QVERIFY(!parseProfileText(empty, &data, &error));
    QVERIFY(error.contains(QStringLiteral("no profiled instructions")));
}


namespace {

/// A per-module vasm `-L` listing: one `00:OOOOOOOO <offset>  <line>: <text>`
/// row per emitted line, with the section span the rows imply. The shape the
/// linker tests use, because this is the one ProgramLineMap reads.
QString writeAttributionListing(const QString &path, const QString &sourceFile,
                                const QList<QPair<int, quint32>> &linesAndOffsets)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&f);
    quint32 extent = 1;
    for (const auto &entry : linesAndOffsets)
        extent = qMax(extent, entry.second + 2);
    out << "Sections:\n"
        << QStringLiteral("00: \"CODE\" (0-%1)\n").arg(extent, 1, 16).toUpper()
        << "\nSource: \"" << sourceFile << "\"\n";
    for (const auto &entry : linesAndOffsets) {
        out << QStringLiteral("00:%1 0000            \t%2: \tinstruction\n")
                   .arg(entry.second, 8, 16, QLatin1Char('0'))
                   .arg(entry.first);
    }
    out.flush();
    f.close();
    return path;
}

/// A one-module program with live text bases: what the profiler attributes
/// against once the program has built and stopped. A one-file project needs no
/// linker, so there is no map file and the module owns the whole section.
struct AttributionFixture
{
    QTemporaryDir dir;
    QString source;
    QString listing;
    ProgramLineMap map;

    bool build(const QString &name, const QList<QPair<int, quint32>> &linesAndOffsets)
    {
        source = name;
        listing = writeAttributionListing(dir.filePath(QStringLiteral("prog.lst")), name,
                                          linesAndOffsets);
        if (listing.isEmpty())
            return false;
        if (!map.addModule(name, QStringLiteral("prog.o"), listing, nullptr))
            return false;
        LineMap::SectionBases live;
        live.text = 0x12596;
        map.setLiveBases(live);
        return map.isResolved();
    }
};

/// One profiled address's cost, without going through a save file: the
/// attribution reads ProfileData, and none of this is about the parser.
ProfileLine profiledAt(quint32 address, quint64 count, quint64 cycles)
{
    ProfileLine line;
    line.address = address;
    line.count = count;
    line.cycles = cycles;
    return line;
}

ProfileData profileOf(const QList<ProfileLine> &lines)
{
    ProfileData data;
    data.clockHz = 8021247;
    data.lines = lines;
    for (const ProfileLine &line : lines) {
        data.totalCount += line.count;
        data.totalCycles += line.cycles;
    }
    return data;
}

} // namespace

// What the tree shows is per routine with its lines under it: `start` owns
// everything up to the next label, `loop` from its own address on, both sorted
// by cycles (what a 68000 actually spends), the lines hottest first within.
void TstProfile::attributionAnchorsRoutinesAggregatesAndSorts()
{
    AttributionFixture fixture;
    // start: label on line 2, its code on line 3; loop: label and code on 5.
    QVERIFY(fixture.build(QStringLiteral("prog.s"), {{2, 0x0}, {3, 0x2}, {5, 0x4}, {6, 0x6}}));

    const QVector<SymbolEntry> symbols = {
        {QStringLiteral("start"), fixture.source, 2},
        {QStringLiteral("loop"), fixture.source, 5},
        // Another file's label must not anchor anything here.
        {QStringLiteral("other"), QStringLiteral("other.s"), 1},
    };

    const AttributedProfile attributed =
        attributedProfile(profileOf({profiledAt(0x12596, 1, 4),     // line 2, before `loop`
                                     profiledAt(0x12598, 100, 400), // line 3, the hot line
                                     profiledAt(0x1259a, 10, 40)}), // line 5, `loop`
                          fixture.map, fixture.source, symbols);

    QVERIFY(attributed.hasSamples);
    QVERIFY(attributed.resolved);
    QCOMPARE(attributed.routines.size(), 2);

    const AttributedRoutine &start = attributed.routines.at(0);
    QCOMPARE(start.name, QStringLiteral("start"));
    QCOMPARE(start.defLine, 2);
    QCOMPARE(start.count, 101ull);
    QCOMPARE(start.cycles, 404ull);
    QCOMPARE(start.lines.size(), 2);
    QCOMPARE(start.lines.at(0).line, 3);
    QCOMPARE(start.lines.at(0).count, 100ull);
    QCOMPARE(start.lines.at(0).cycles, 400ull);
    QCOMPARE(start.lines.at(1).line, 2);

    const AttributedRoutine &loop = attributed.routines.at(1);
    QCOMPARE(loop.name, QStringLiteral("loop"));
    QCOMPARE(loop.defLine, 5);
    QCOMPARE(loop.count, 10ull);
    QCOMPARE(loop.cycles, 40ull);
    QCOMPARE(loop.lines.size(), 1);
    QCOMPARE(loop.lines.at(0).line, 5);

    QCOMPARE(attributed.unmapped, 0);
    QCOMPARE(attributed.totalCount, 111ull);
    QCOMPARE(attributed.totalCycles, 444ull);
    QCOMPARE(attributed.clockHz, 8021247u);
}

// A profile is attributed against one source file. An address resolving into a
// file the user is not looking at is not a hot line of theirs, and the counts
// of what was dropped stay visible so a partial attribution does not read as a
// complete one.
void TstProfile::attributionDropsLinesOutsideTheSourceFile()
{
    AttributionFixture fixture;
    QVERIFY(fixture.build(QStringLiteral("prog.s"), {{2, 0x0}, {3, 0x2}}));

    const QVector<SymbolEntry> symbols = {
        {QStringLiteral("start"), fixture.source, 2},
    };
    const ProfileData data = profileOf({profiledAt(0x12596, 5, 20), profiledAt(0x12598, 5, 20)});

    const AttributedProfile other =
        attributedProfile(data, fixture.map, QStringLiteral("lib.s"), symbols);
    QVERIFY2(other.routines.isEmpty(), "no line of lib.s is in this profile");
    QCOMPARE(other.unmapped, 2);
    QCOMPARE(other.unmappedCycles, 40ull);
    // The samples are still the run's; only the attribution is empty.
    QCOMPARE(other.totalCount, 10ull);
    QVERIFY(other.resolved);
}

// Time inside the OS is a real share of a frame and belongs to no source line:
// it keeps a row named by the area the save file listed.
void TstProfile::attributionKeepsRomTimeByRegion()
{
    AttributionFixture fixture;
    QVERIFY(fixture.build(QStringLiteral("prog.s"), {{2, 0x0}, {3, 0x2}}));

    ProfileData data = profileOf({profiledAt(0xfc0100, 5, 20),  // ROM_TOS
                                  profiledAt(0xfa0100, 2, 8)}); // CARTRIDGE
    data.regions = {{QStringLiteral("ROM_TOS"), 0xfc0000, 0xffffff},
                    {QStringLiteral("CARTRIDGE"), 0xfa0000, 0xfbffff}};

    const AttributedProfile attributed =
        attributedProfile(data, fixture.map, fixture.source, {});

    QVERIFY(attributed.routines.isEmpty());
    QCOMPARE(attributed.unmapped, 0);
    QCOMPARE(attributed.rom.size(), 2);
    QCOMPARE(attributed.rom.at(0).name, QStringLiteral("ROM_TOS"));
    QCOMPARE(attributed.rom.at(0).count, 5ull);
    QCOMPARE(attributed.rom.at(0).cycles, 20ull);
    QCOMPARE(attributed.rom.at(1).name, QStringLiteral("CARTRIDGE"));
    QCOMPARE(attributed.rom.at(1).cycles, 8ull);
}

// An older save names no areas, and its ROM traffic must not be discarded as
// unmapped. The floor is the *lower* end of ROM space — the cartridge area —
// so nothing that could be source is mistaken for the OS.
void TstProfile::attributionFallsBackToTheRomFloor()
{
    AttributionFixture fixture;
    QVERIFY(fixture.build(QStringLiteral("prog.s"), {{2, 0x0}, {3, 0x2}}));

    QCOMPARE(kRomFloor, 0xfa0000u);

    const AttributedProfile attributed =
        attributedProfile(profileOf({profiledAt(kRomFloor + 0x100, 7, 28), // ROM, no areas
                                     profiledAt(0x8000, 1, 4)}),          // in no module
                          fixture.map, fixture.source, {});

    QVERIFY(attributed.routines.isEmpty());
    QCOMPARE(attributed.rom.size(), 1);
    QCOMPARE(attributed.rom.at(0).name, QStringLiteral("ROM"));
    QCOMPARE(attributed.rom.at(0).count, 7ull);
    QCOMPARE(attributed.rom.at(0).cycles, 28ull);
    QCOMPARE(attributed.unmapped, 1);
    QCOMPARE(attributed.unmappedCycles, 4ull);
}

// The gutter heat and the remote `profile results` verb take their numbers from
// here, so these are the numbers the tree renders too.
void TstProfile::attributionLineCountsMatchTheRoutines()
{
    AttributionFixture fixture;
    QVERIFY(fixture.build(QStringLiteral("prog.s"), {{2, 0x0}, {3, 0x2}, {5, 0x4}}));

    const QVector<SymbolEntry> symbols = {
        {QStringLiteral("start"), fixture.source, 2},
        {QStringLiteral("loop"), fixture.source, 5},
    };
    const AttributedProfile attributed =
        attributedProfile(profileOf({profiledAt(0x12596, 1, 4), profiledAt(0x12598, 100, 400),
                                     profiledAt(0x1259a, 10, 40)}),
                          fixture.map, fixture.source, symbols);

    const QHash<int, quint64> counts = lineCounts(attributed);
    QCOMPARE(counts.size(), 3);
    QCOMPARE(counts.value(3), 100ull);
    QCOMPARE(counts.value(2), 1ull);
    QCOMPARE(counts.value(5), 10ull);

    // An empty attribution is an empty heat map, not a stale one.
    QVERIFY(lineCounts(AttributedProfile()).isEmpty());
}

// The two cases a renderer words differently: a save with no executed
// instruction, and a program whose map cannot resolve yet. Both have to be
// distinguishable from a profile that genuinely attributed nothing.
void TstProfile::attributionReportsAnEmptySaveOrAnUnresolvedMap()
{
    AttributionFixture fixture;
    QVERIFY(fixture.build(QStringLiteral("prog.s"), {{2, 0x0}}));

    const AttributedProfile nothing = attributedProfile(ProfileData(), fixture.map,
                                                        fixture.source, {});
    QVERIFY(!nothing.hasSamples);
    QVERIFY(nothing.resolved);
    QVERIFY(nothing.routines.isEmpty());
    QVERIFY(nothing.rom.isEmpty());

    // The same save before the program has run: no live bases, so no address
    // resolves to a line at all.
    ProgramLineMap unresolved;
    QVERIFY(unresolved.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                                 fixture.listing, nullptr));
    const AttributedProfile unmapped =
        attributedProfile(profileOf({profiledAt(0x12596, 1, 4)}), unresolved,
                          QStringLiteral("prog.s"), {});
    QVERIFY(unmapped.hasSamples);
    QVERIFY2(!unmapped.resolved, "an unresolved map attributes nothing");
    QVERIFY(unmapped.routines.isEmpty());
    QCOMPARE(unmapped.totalCount, 1ull);
}

QTEST_MAIN(TstProfile)
#include "tst_profile.moc"
