// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Multi-module source mapping. Separate compilation breaks the assumption that
// one listing's offsets can be added straight to the live section base: under a
// linker every module restarts its offsets from zero, so each needs its own
// base. That base comes from vlink's map, and these tests pin both the parsing
// and the resulting address arithmetic.

#include "build/LinkMap.h"
#include "build/ProgramLineMap.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace pist;

namespace {

/// A verbatim `vlink -b ataritos -M` map for the two-module fixture below.
/// Real output, not a hand-written approximation.
QString writeLinkMap(QTemporaryDir &dir)
{
    const QString path = dir.filePath(QStringLiteral("prog.map"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&f);
    out << "\n"
           "Files:\n"
           "  main.o:  CODE 0(a) hex\n"
           "  lib.o:  CODE a(8) hex\n"
           "\n"
           "\n"
           "Section mapping (numbers in hex):\n"
           "------------------------------\n"
           "  00000000 .text  (size 12, allocated 10)\n"
           "           00000000 - 0000000a main.o(CODE)\n"
           "           0000000a - 00000012 lib.o(CODE)\n"
           "\n"
           "\n"
           "Symbols of .text:\n"
           "  0x00000000 start: global reloc, size 0\n"
           "  0x00000008 loop: local reloc, size 0\n"
           "  0x0000000a helper: global reloc, size 0\n"
           "  0x0000000e msg: local reloc, size 0\n";
    out.flush();
    f.close();
    return path;
}

/// A per-module vasm `-L` listing. Offsets restart at zero in each module, which
/// is the whole problem.
///
/// The section is named the way `-Fvobj` names it, which is the format a linked
/// module is assembled in — and the spelling the linker uses in its diagnostics
/// and its map (`main.o (CODE+0x4)`), so it is also the one a diagnostic's
/// section has to match.
QString writeModuleListing(QTemporaryDir &dir, const QString &name,
                           const QList<QPair<int, quint32>> &linesAndOffsets,
                           const QString &sourceFile)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&f);

    // Each module's listing reports its *own* section extent, so modules do not
    // overlap. Computing it from the entries keeps the fixture honest — a shared
    // span would let one module claim another's addresses.
    quint32 extent = 1;
    for (const auto &entry : linesAndOffsets)
        extent = qMax(extent, entry.second + 2);

    out << "Sections:\n"
        << QStringLiteral("00: \"CODE\" (0-%1)\n").arg(extent, 1, 16).toUpper()
        << "\n"
           "Source: \"" << sourceFile << "\"\n";
    for (const auto &entry : linesAndOffsets) {
        out << QStringLiteral("00:%1 0000            \t%2: \tinstruction\n")
                   .arg(entry.second, 8, 16, QLatin1Char('0'))
                   .arg(entry.first);
    }
    out.flush();
    f.close();
    return path;
}

/// One module's placement in a map fixture.
struct MapPlacement
{
    QString objectFile;
    quint32 start = 0;
    quint32 end = 0;
    QString section = QStringLiteral("CODE");
};

/// A `Section mapping` block naming its modules exactly as given.
///
/// vlink never writes paths here — measured with 0.18a, `-M` prints `util.o`
/// even for an absolute object path, and its diagnostics name modules the same
/// way. A map that names paths is the shape `LinkMap` promises to honour, and
/// the only one in which a pair of same-named modules is distinguishable at all.
QString writeLinkMapNaming(QTemporaryDir &dir, const QString &name,
                           const QList<MapPlacement> &placements, quint32 textSize)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&f);

    const auto hex = [](quint32 value, int width) {
        return QString::number(value, 16).rightJustified(width, QLatin1Char('0'));
    };
    const auto placementLine = [&hex](const MapPlacement &p) {
        return hex(p.start, 8) + QStringLiteral(" - ") + hex(p.end, 8)
             + QLatin1Char(' ') + p.objectFile + QLatin1Char('(') + p.section
             + QStringLiteral(")\n");
    };

    out << "\nFiles:\n";
    for (const MapPlacement &p : placements)
        out << QStringLiteral("  %1:  %2 %3(%4) hex\n")
                   .arg(QFileInfo(p.objectFile).fileName(), p.section)
                   .arg(hex(p.start, 1), hex(p.end - p.start, 1));

    out << "\n\nSection mapping (numbers in hex):\n"
           "------------------------------\n"
        << QStringLiteral("  %1 .text  (size %2)\n").arg(hex(0, 8), hex(textSize, 1));
    for (const MapPlacement &p : placements)
        out << QStringLiteral("           ") + placementLine(p);

    out.flush();
    f.close();
    return path;
}

/// Two modules assembled from *same-named* sources in different directories —
/// the collision MAJ-40 is about. The sources and objects are real files so the
/// paths are what canonicalisation actually resolves, and each listing is
/// honest about its own extent, so the two modules' spans do not overlap.
struct SameNamedPair
{
    QTemporaryDir dir;
    QString source1;
    QString source2;
    QString object1;
    QString object2;
    QString listing1;
    QString listing2;

    bool build();
};

bool SameNamedPair::build()
{
    if (!dir.isValid())
        return false;
    if (!QDir(dir.path()).mkpath(QStringLiteral("dir1"))
        || !QDir(dir.path()).mkpath(QStringLiteral("dir2")))
        return false;

    source1 = dir.filePath(QStringLiteral("dir1/util.s"));
    source2 = dir.filePath(QStringLiteral("dir2/util.s"));
    object1 = dir.filePath(QStringLiteral("dir1/util.o"));
    object2 = dir.filePath(QStringLiteral("dir2/util.o"));

    for (const QString &file : {source1, source2, object1, object2}) {
        QFile f(file);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write("x");
        f.close();
    }

    // dir1/util.s: line 3 at offset 0, line 6 at 4.
    // dir2/util.s: line 5 at offset 0, line 9 at 0x20.
    listing1 = writeModuleListing(dir, QStringLiteral("util1.lst"), { {3, 0x0}, {6, 0x4} },
                                  source1);
    listing2 = writeModuleListing(dir, QStringLiteral("util2.lst"), { {5, 0x0}, {9, 0x20} },
                                  source2);
    return !listing1.isEmpty() && !listing2.isEmpty();
}

/// Load `mapPath` into a program map holding both modules of the pair and
/// resolve it against `live`.
bool placePair(ProgramLineMap &program, const SameNamedPair &pair, const QString &mapPath,
               const LineMap::SectionBases &live)
{
    QString error;
    if (!program.addModule(pair.source1, pair.object1, pair.listing1, &error))
        return false;
    if (!program.addModule(pair.source2, pair.object2, pair.listing2, &error))
        return false;

    LinkMap map;
    if (!map.parse(mapPath, &error))
        return false;
    program.setLinkMap(map);
    program.setLiveBases(live);
    return program.isResolved();
}

/// The program base both pair tests resolve against; the offsets the map adds on
/// top are what the tests are about.
LineMap::SectionBases pairLiveBases()
{
    LineMap::SectionBases live;
    live.text = 0x12596;
    live.data = 0x12600;
    live.bss = 0x12610;
    return live;
}

/// Run a tool to completion, quietly — the real-tool cases only care whether it
/// worked. A timeout is a failure, not a zero exit code: `waitForFinished`
/// returns false when the tool never finished, and reading exitCode() after that
/// would report whatever stale value the process object holds.
bool runTool(const QString &program, const QStringList &args)
{
    QProcess p;
    p.start(program, args);
    return p.waitForFinished(20000) && p.exitCode() == 0;
}

} // namespace

class TstLink : public QObject
{
    Q_OBJECT

private slots:
    void parsesModulePlacement();
    void ignoresTheFilesHeader();
    void rejectsAFileThatIsNotAMap();

    void mapsLinesAcrossModules();
    void refusesAddressesOutsideEveryModule();
    void singleModuleNeedsNoLinkMap();
    void reportsModulesTheMapDidNotPlace();
    void multiModuleWithoutLinkMapStaysUnplaced();

    /// Same-named modules in different directories: each keeps its own
    /// placement, and the map's refusals where it cannot say which is which.
    void separatesSameNamedModulesByPath();
    void refusesToGuessBetweenSameNamedModules();

    /// One module, two sections: the section asked about decides the entry.
    void doesNotConfuseAModulesOwnSections();

    /// The real thing, when vasm and a vlink are available: assemble two
    /// modules, link them, and check the mapping against the linked program's
    /// own symbol table.
    void endToEndAgainstARealLink();

    /// The same, for a same-named pair: what the tools actually put in the map,
    /// and what the mapping may therefore claim.
    void endToEndRefusesSameNamedModules();
};

void TstLink::parsesModulePlacement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    LinkMap map;
    QString error;
    QVERIFY2(map.parse(writeLinkMap(dir), &error), qPrintable(error));
    QVERIFY(!map.isEmpty());

    quint32 offset = 0;
    QVERIFY(map.moduleOffset(QStringLiteral("main.o"), QStringLiteral("CODE"), &offset));
    QCOMPARE(offset, 0u);

    QVERIFY(map.moduleOffset(QStringLiteral("lib.o"), QStringLiteral("CODE"), &offset));
    QCOMPARE(offset, 0x0au);   // exactly where main.o's code ended

    // An absolute path must match too: the map records whatever was handed to
    // the linker, which the build passes by path.
    QVERIFY(map.moduleOffset(QStringLiteral("/build/lib.o"), QStringLiteral("CODE"), &offset));
    QCOMPARE(offset, 0x0au);

    QVERIFY(!map.moduleOffset(QStringLiteral("other.o"), QStringLiteral("CODE"), &offset));
}

// The `Files:` header also lists modules with sizes in parentheses, so a loose
// parser would record them as placements at bogus addresses.
void TstLink::ignoresTheFilesHeader()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    LinkMap map;
    QVERIFY(map.parse(writeLinkMap(dir), nullptr));

    // Two modules, from the section-mapping block only — not four from both.
    QCOMPARE(map.placements().size(), 2);
    for (const LinkPlacement &p : map.placements())
        QVERIFY2(p.sectionType == QLatin1String("CODE"), qPrintable(p.sectionType));
}

void TstLink::rejectsAFileThatIsNotAMap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("notamap.map"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("this is not a linker map\n");
    f.close();

    LinkMap map;
    QString error;
    QVERIFY2(!map.parse(path, &error), "a file with no placements must be rejected");
    QVERIFY(!error.isEmpty());
}

// The behaviour this whole file exists for: `helper` lives in the second module
// and starts at offset 0 there, but must resolve to the address the linker put
// it at, not to the program's start.
void TstLink::mapsLinesAcrossModules()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString mainSource = QStringLiteral("main.s");
    const QString libSource = QStringLiteral("lib.s");

    // main.s: line 4 at offset 0, line 7 at offset 8.
    // lib.s:  line 3 at offset 0, line 6 at offset 4.
    const QString mainLst = writeModuleListing(
        dir, QStringLiteral("main.lst"), {{4, 0x0}, {7, 0x8}}, mainSource);
    const QString libLst = writeModuleListing(
        dir, QStringLiteral("lib.lst"), {{3, 0x0}, {6, 0x4}}, libSource);

    ProgramLineMap program;
    QString error;
    QVERIFY2(program.addModule(mainSource, QStringLiteral("main.o"), mainLst, &error),
             qPrintable(error));
    QVERIFY2(program.addModule(libSource, QStringLiteral("lib.o"), libLst, &error),
             qPrintable(error));

    LinkMap map;
    QVERIFY(map.parse(writeLinkMap(dir), &error));
    program.setLinkMap(map);

    // The program is loaded somewhere; the map's offsets are added to it.
    LineMap::SectionBases live;
    live.text = 0x12596;
    live.data = 0x125b0;
    live.bss = 0x125c0;
    program.setLiveBases(live);
    QVERIFY(program.isResolved());

    quint32 addr = 0;

    // Module one: offset 0, so its first line is at the live text base.
    QVERIFY(program.addressFor(mainSource, 4, &addr));
    QCOMPARE(addr, 0x12596u);
    QVERIFY(program.addressFor(mainSource, 7, &addr));
    QCOMPARE(addr, 0x12596u + 0x8u);

    // Module two: placed at 0xa, and its own offsets are relative to that.
    QVERIFY2(program.addressFor(libSource, 3, &addr),
             "a line in the second module must resolve through the link map");
    QCOMPARE(addr, 0x12596u + 0x0au);
    QVERIFY(program.addressFor(libSource, 6, &addr));
    QCOMPARE(addr, 0x12596u + 0x0eu);

    // And back again, which is what drives the editor highlight.
    LineMap::Address back;
    QVERIFY(program.lineFor(0x12596u + 0x0au, &back));
    QCOMPARE(back.line, 3);
    QVERIFY(LineMap::sameSource(back.file, libSource));

    QVERIFY(program.lineFor(0x12596u + 0x0u, &back));
    QCOMPARE(back.line, 4);
    QVERIFY(LineMap::sameSource(back.file, mainSource));
}

void TstLink::refusesAddressesOutsideEveryModule()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString mainLst = writeModuleListing(
        dir, QStringLiteral("main.lst"), {{4, 0x0}, {7, 0x8}}, QStringLiteral("main.s"));

    ProgramLineMap program;
    QVERIFY(program.addModule(QStringLiteral("main.s"), QStringLiteral("main.o"),
                              mainLst, nullptr));

    LinkMap map;
    QVERIFY(map.parse(writeLinkMap(dir), nullptr));
    program.setLinkMap(map);

    LineMap::SectionBases live;
    live.text = 0x12596;
    program.setLiveBases(live);

    // Far past both modules: ROM, the stack, or uninitialised memory. There is no
    // honest line to report, and claiming one would scroll the editor somewhere
    // unrelated.
    LineMap::Address back;
    QVERIFY2(!program.lineFor(0x12596u + 0x1000u, &back),
             "an address outside every module must not resolve to a line");
}

// A one-file project assembles straight to a .PRG with no linker, so there is no
// map and the single module owns the whole section.
void TstLink::singleModuleNeedsNoLinkMap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString lst = writeModuleListing(
        dir, QStringLiteral("only.lst"), {{2, 0x0}, {9, 0x20}}, QStringLiteral("only.s"));

    ProgramLineMap program;
    QVERIFY(program.addModule(QStringLiteral("only.s"), QStringLiteral("only.o"), lst, nullptr));

    LineMap::SectionBases live;
    live.text = 0x12596;
    program.setLiveBases(live);
    QVERIFY(program.isResolved());

    quint32 addr = 0;
    QVERIFY(program.addressFor(QStringLiteral("only.s"), 9, &addr));
    QCOMPARE(addr, 0x12596u + 0x20u);
}

void TstLink::reportsModulesTheMapDidNotPlace()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // This module is not in the map at all — the case when a source contributed
    // no code, or when a stale map is paired with a rebuilt program.
    const QString lst = writeModuleListing(
        dir, QStringLiteral("ghost.lst"), {{1, 0x0}}, QStringLiteral("ghost.s"));

    ProgramLineMap program;
    QVERIFY(program.addModule(QStringLiteral("ghost.s"), QStringLiteral("ghost.o"),
                              lst, nullptr));

    LinkMap map;
    QVERIFY(map.parse(writeLinkMap(dir), nullptr));
    program.setLinkMap(map);

    LineMap::SectionBases live;
    live.text = 0x12596;
    program.setLiveBases(live);

    const QStringList unplaced = program.unplacedModules();
    QCOMPARE(unplaced.size(), 1);
    QCOMPARE(unplaced.first(), QStringLiteral("ghost.s"));

    // And it must not be mapped to a guessed address.
    quint32 addr = 0;
    QVERIFY(!program.addressFor(QStringLiteral("ghost.s"), 1, &addr));
}

// A multi-module build whose link map is missing or failed to parse: the
// placement of every module is unknown, so none may be treated as placed.
// Giving them all the live section base is what arms a breakpoint at an
// address inside the first module and reports it as armed.
void TstLink::multiModuleWithoutLinkMapStaysUnplaced()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString mainLst = writeModuleListing(
        dir, QStringLiteral("main.lst"), {{2, 0x0}, {9, 0x20}}, QStringLiteral("main.s"));
    const QString libLst = writeModuleListing(
        dir, QStringLiteral("lib.lst"), {{3, 0x0}, {7, 0x40}}, QStringLiteral("lib.s"));

    ProgramLineMap program;
    QVERIFY(program.addModule(QStringLiteral("main.s"), QStringLiteral("main.o"),
                              mainLst, nullptr));
    QVERIFY(program.addModule(QStringLiteral("lib.s"), QStringLiteral("lib.o"),
                              libLst, nullptr));
    // Deliberately no setLinkMap: the map is absent or did not parse.

    LineMap::SectionBases live;
    live.text = 0x12596;
    program.setLiveBases(live);

    const QStringList unplaced = program.unplacedModules();
    QCOMPARE(unplaced.size(), 2);

    // Neither module may resolve to a guessed address — in particular the
    // second must not be handed the first one's base.
    quint32 addr = 0;
    QVERIFY(!program.addressFor(QStringLiteral("main.s"), 9, &addr));
    QVERIFY(!program.addressFor(QStringLiteral("lib.s"), 7, &addr));
}

// Two modules built from same-named sources in different directories must keep
// their own placements. Both were previously recorded as `util.o`, so both
// queried the map as `util.o` and both were answered with the *first* match:
// `dir2/util.s`'s lines resolved to `dir1`'s addresses, the second module's
// breakpoints armed in the first, and the linker diagnostic for `dir2/util.o`
// was shown against `dir1/util.s` — with `placed` true and nothing reported as
// unplaced anywhere.
void TstLink::separatesSameNamedModulesByPath()
{
    SameNamedPair pair;
    QVERIFY(pair.build());

    const QList<MapPlacement> placements{
        {pair.object1, 0x0, 0x6},
        {pair.object2, 0xa, 0x2c},
    };
    const QString mapPath =
        writeLinkMapNaming(pair.dir, QStringLiteral("prog.map"), placements, 0x2c);
    QVERIFY(!mapPath.isEmpty());

    ProgramLineMap program;
    QVERIFY2(placePair(program, pair, mapPath, pairLiveBases()), "the fixture must load");
    QVERIFY2(program.unplacedModules().isEmpty(), "both modules are named in the map");

    quint32 addr = 0;
    QVERIFY(program.addressFor(pair.source1, 3, &addr));
    QCOMPARE(addr, 0x12596u);
    QVERIFY(program.addressFor(pair.source1, 6, &addr));
    QCOMPARE(addr, 0x12596u + 0x4u);

    // Module two: its own placement (0xa), not the first module's (0).
    QVERIFY2(program.addressFor(pair.source2, 5, &addr),
             "a line in the second util.s must resolve through its own placement");
    QCOMPARE(addr, 0x12596u + 0xau);
    QVERIFY(program.addressFor(pair.source2, 9, &addr));
    QCOMPARE(addr, 0x12596u + 0x2au);

    // And back again, which is what moves the editor highlight and the PC bar.
    LineMap::Address back;
    QVERIFY(program.lineFor(0x12596u + 0xau, &back));
    QCOMPARE(back.line, 5);
    QCOMPARE(back.file, pair.source2);
    QVERIFY(program.lineFor(0x12596u + 0x4u, &back));
    QCOMPARE(back.line, 6);
    QCOMPARE(back.file, pair.source1);

    // A linker diagnostic names the module it is about, so `dir2/util.o` must
    // reach dir2's listing — offsets are per module, and these two modules have
    // an identically-shaped beginning.
    LineMap::Address resolved;
    QVERIFY2(program.lineForObjectOffset(pair.object2, QStringLiteral("CODE"), 0x2, &resolved),
             "a diagnostic against dir2/util.o must find dir2's module");
    QCOMPARE(resolved.file, pair.source2);
    QCOMPARE(resolved.line, 5);
    QVERIFY(program.lineForObjectOffset(pair.object1, QStringLiteral("CODE"), 0x2, &resolved));
    QCOMPARE(resolved.file, pair.source1);
    QCOMPARE(resolved.line, 3);

    // The shape mismatch the entry names: the build records one path, a caller
    // may hold the same file spelled another way. Canonicalising both sides is
    // what reaches dir2 here — raw comparison would miss, fall back to the name
    // both modules share, and refuse.
    const QString roundaboutSource = pair.dir.filePath(QStringLiteral("dir2/../dir2/util.s"));
    QVERIFY2(program.addressFor(roundaboutSource, 5, &addr),
             "the same file spelled differently is still the same module");
    QCOMPARE(addr, 0x12596u + 0xau);

    const QString roundaboutObject = pair.dir.filePath(QStringLiteral("dir1/../dir2/util.o"));
    QVERIFY2(program.lineForObjectOffset(roundaboutObject, QStringLiteral("CODE"), 0x2, &resolved),
             "a diagnostic's module path is the same file however it is spelled");
    QCOMPARE(resolved.file, pair.source2);

    // vlink writes only the base name, in its diagnostics exactly as in its map
    // (measured), so the name a diagnostic really carries — `util.o` — cannot
    // say which of the two it is about and must locate neither. The refusal is
    // the subject of the next test; what matters here is that the answer is not
    // dir1's listing.
    QVERIFY2(!program.lineForObjectOffset(QStringLiteral("util.o"), QStringLiteral("CODE"),
                                          0x2, &resolved),
             "the base name alone must not be read as the first module's");
}

// What happens when the map cannot tell the pair apart. vlink writes base names
// into its map and its diagnostics (measured: `-M` prints `util.o` for an
// absolute object path), so this is the usual shape for a same-named pair, and
// the honest answer is that neither module can be placed: one of the two
// placements is theirs and which one is unknowable. Handing both the first
// match is how the second module's breakpoints ended up at the first's
// addresses — silently, reporting themselves as armed.
void TstLink::refusesToGuessBetweenSameNamedModules()
{
    SameNamedPair pair;
    QVERIFY(pair.build());

    const LineMap::SectionBases live = pairLiveBases();

    const QString ambiguous = writeLinkMapNaming(
        pair.dir, QStringLiteral("prog.map"),
        { {QStringLiteral("util.o"), 0x0, 0x6}, {QStringLiteral("util.o"), 0xa, 0x2c} }, 0x2c);
    QVERIFY(!ambiguous.isEmpty());

    ProgramLineMap program;
    QVERIFY(placePair(program, pair, ambiguous, live));

    // Both modules are reported — and by path, because their shared file name is
    // what the map could not resolve: a log line naming `util.s` twice would
    // read as one file, not as the collision that left both unmapped.
    const QStringList unplaced = program.unplacedModules();
    QCOMPARE(unplaced.size(), 2);
    QCOMPARE(QFileInfo(unplaced.value(0)).canonicalFilePath(),
             QFileInfo(pair.source1).canonicalFilePath());
    QCOMPARE(QFileInfo(unplaced.value(1)).canonicalFilePath(),
             QFileInfo(pair.source2).canonicalFilePath());

    quint32 addr = 0;
    QVERIFY2(!program.addressFor(pair.source2, 5, &addr),
             "an ambiguous name must resolve to nothing, not to the first module's address");
    QVERIFY(!program.addressFor(pair.source1, 3, &addr));
    QVERIFY(!program.codeAddressFor(pair.source2, 5, &addr));

    LineMap::Address resolved;
    QVERIFY2(!program.lineForObjectOffset(QStringLiteral("util.o"), QStringLiteral("CODE"), 0x2,
                                          &resolved),
             "a diagnostic naming one of two same-named modules locates neither");

    // A single bare `util.o` is no better: it is one of our two modules, and the
    // name is exactly what cannot say which. Only a name that reaches one module
    // identifies a placement.
    const QString halfMap = writeLinkMapNaming(
        pair.dir, QStringLiteral("half.map"), { {QStringLiteral("util.o"), 0x0, 0x6} }, 0x6);
    ProgramLineMap half;
    QVERIFY(placePair(half, pair, halfMap, live));
    QCOMPARE(half.unplacedModules().size(), 2);
    QVERIFY2(!half.addressFor(pair.source2, 5, &addr),
             "the one placement belongs to one of the two, and guessing is the bug");
}

// Guard rather than a reproduction: the old lookup matched name and section in
// one pass, so this cannot fail before the fix. It pins the placement list being
// scoped to the section asked about — a module with both code and data has two
// entries under its name, and a lookup that considered both would find every
// such module ambiguous with itself and place it nowhere.
void TstLink::doesNotConfuseAModulesOwnSections()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("prog.map"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "\n"
           "Section mapping (numbers in hex):\n"
           "------------------------------\n"
           "  00000000 .text  (size c, allocated c)\n"
           "           00000000 - 0000000a main.o(CODE)\n"
           "           0000000a - 0000000c lib.o(CODE)\n"
           "  00000000 .data  (size 8, allocated 8)\n"
           "           00000000 - 00000004 main.o(DATA)\n"
           "           00000004 - 00000008 lib.o(DATA)\n";
    out.flush();
    f.close();

    LinkMap map;
    QString error;
    QVERIFY2(map.parse(path, &error), qPrintable(error));

    quint32 offset = 0;
    QVERIFY(map.moduleOffset(QStringLiteral("main.o"), QStringLiteral("CODE"), &offset));
    QCOMPARE(offset, 0u);
    QVERIFY(map.moduleOffset(QStringLiteral("lib.o"), QStringLiteral("CODE"), &offset));
    QCOMPARE(offset, 0xau);

    QVERIFY(map.moduleOffset(QStringLiteral("main.o"), QStringLiteral("DATA"), &offset));
    QCOMPARE(offset, 0u);
    QVERIFY(map.moduleOffset(QStringLiteral("lib.o"), QStringLiteral("DATA"), &offset));
    QCOMPARE(offset, 0x4u);
}

void TstLink::endToEndAgainstARealLink()
{
    const QString vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    const QString vlink = QStandardPaths::findExecutable(QStringLiteral("vlink"));
    if (vasm.isEmpty() || vlink.isEmpty())
        QSKIP("needs both vasmm68k_mot and vlink");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString mainSrc = dir.filePath(QStringLiteral("main.s"));
    const QString libSrc = dir.filePath(QStringLiteral("lib.s"));

    QFile m(mainSrc);
    QVERIFY(m.open(QIODevice::WriteOnly | QIODevice::Text));
    m.write("\ttext\n"
            "\txdef\tstart\n"
            "\txref\thelper\n"
            "start:\tmoveq\t#1,d0\n"
            "\tbsr\thelper\n"
            "loop:\tbra.s\tloop\n"
            "\tend\n");
    m.close();

    QFile l(libSrc);
    QVERIFY(l.open(QIODevice::WriteOnly | QIODevice::Text));
    l.write("\ttext\n"
            "\txdef\thelper\n"
            "helper:\tmoveq\t#3,d0\n"
            "\trts\n"
            "\teven\n"
            "msg:\tdc.b\t\"HI\",0\n"
            "\teven\n"
            "\tend\n");
    l.close();

    const QString mainObj = dir.filePath(QStringLiteral("main.o"));
    const QString libObj = dir.filePath(QStringLiteral("lib.o"));
    const QString mainLst = dir.filePath(QStringLiteral("main.lst"));
    const QString libLst = dir.filePath(QStringLiteral("lib.lst"));
    const QString mapFile = dir.filePath(QStringLiteral("prog.map"));
    const QString prg = dir.filePath(QStringLiteral("prog.prg"));

    QVERIFY(runTool(vasm, {QStringLiteral("-quiet"), QStringLiteral("-Fvobj"),
                           QStringLiteral("-L"), mainLst, QStringLiteral("-o"), mainObj,
                           mainSrc}));
    QVERIFY(runTool(vasm, {QStringLiteral("-quiet"), QStringLiteral("-Fvobj"),
                           QStringLiteral("-L"), libLst, QStringLiteral("-o"), libObj,
                           libSrc}));
    QVERIFY(runTool(vlink, {QStringLiteral("-b"), QStringLiteral("ataritos"),
                            QStringLiteral("-M") + mapFile,
                            QStringLiteral("-o"), prg, mainObj, libObj}));

    LinkMap map;
    QString error;
    QVERIFY2(map.parse(mapFile, &error), qPrintable(error));

    ProgramLineMap program;
    QVERIFY2(program.addModule(mainSrc, mainObj, mainLst, &error), qPrintable(error));
    QVERIFY2(program.addModule(libSrc, libObj, libLst, &error), qPrintable(error));
    program.setLinkMap(map);

    LineMap::SectionBases live;
    live.text = 0x12596;
    program.setLiveBases(live);
    QVERIFY(program.isResolved());
    QVERIFY2(program.unplacedModules().isEmpty(), "every module should be in the map");

    // `helper` is the first thing in the second module, so its address must be
    // past the end of the first module rather than at the program's start. This
    // is precisely what a single-base mapping would get wrong.
    quint32 startAddr = 0;
    quint32 helperAddr = 0;
    QVERIFY(program.addressFor(mainSrc, 4, &startAddr));
    QVERIFY(program.addressFor(libSrc, 3, &helperAddr));
    QCOMPARE(startAddr, 0x12596u);
    QVERIFY2(helperAddr > startAddr,
             "a symbol in the second module cannot live at the program's start");

    // Cross-check against the linked program's own symbol table, read the same
    // way Hatari reads it.
    const QString gst2ascii = QStandardPaths::findExecutable(QStringLiteral("gst2ascii"));
    if (gst2ascii.isEmpty())
        QSKIP("needs gst2ascii to cross-check the link");

    QProcess symbols;
    symbols.start(gst2ascii, {prg});
    QVERIFY(symbols.waitForFinished(15000));
    const QString listed = QString::fromUtf8(symbols.readAllStandardOutput());

    QVERIFY2(listed.contains(QLatin1String("helper")), qPrintable(listed));

    // The text-relative value from the symbol table, plus the live base, must
    // equal what the line map produced.
    QRegularExpression re(QStringLiteral(R"(^0x([0-9A-Fa-f]+)\s+\w\s+helper$)"),
                          QRegularExpression::MultilineOption);
    const auto match = re.match(listed);
    QVERIFY2(match.hasMatch(), qPrintable("no helper symbol in:\n" + listed));
    const quint32 fromSymbols = live.text + match.captured(1).toUInt(nullptr, 16);
    QCOMPARE(helperAddr, fromSymbols);
}

// The measured shape of a same-named pair, with the real tools: vlink's map
// names both modules `util.o` and says nothing about which directory either came
// from, however absolute the paths handed to it were. The mapping therefore
// cannot place them, and must say so; the linked program, meanwhile, really did
// put the two modules at different addresses, which is what made the old
// first-match placement wrong rather than merely imprecise.
void TstLink::endToEndRefusesSameNamedModules()
{
    const QString vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    const QString vlink = QStandardPaths::findExecutable(QStringLiteral("vlink"));
    if (vasm.isEmpty() || vlink.isEmpty())
        QSKIP("needs both vasmm68k_mot and vlink");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("dir1")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("dir2")));

    const QString source1 = dir.filePath(QStringLiteral("dir1/util.s"));
    const QString source2 = dir.filePath(QStringLiteral("dir2/util.s"));

    QFile m(source1);
    QVERIFY(m.open(QIODevice::WriteOnly | QIODevice::Text));
    m.write("\ttext\n"
            "\txdef\tone\n"
            "one:\tmoveq\t#1,d0\n"
            "\trts\n"
            "\teven\n"
            "\tend\n");
    m.close();

    QFile l(source2);
    QVERIFY(l.open(QIODevice::WriteOnly | QIODevice::Text));
    l.write("\ttext\n"
            "\txdef\ttwo\n"
            "two:\tmoveq\t#2,d0\n"
            "\trts\n"
            "\teven\n"
            "\tend\n");
    l.close();

    const QString object1 = dir.filePath(QStringLiteral("dir1/util.o"));
    const QString object2 = dir.filePath(QStringLiteral("dir2/util.o"));
    const QString listing1 = dir.filePath(QStringLiteral("dir1/util.lst"));
    const QString listing2 = dir.filePath(QStringLiteral("dir2/util.lst"));
    const QString mapFile = dir.filePath(QStringLiteral("prog.map"));
    const QString prg = dir.filePath(QStringLiteral("prog.prg"));

    // Absolute object paths, exactly as BuildService hands them over.
    QVERIFY(runTool(vasm, {QStringLiteral("-quiet"), QStringLiteral("-Fvobj"),
                           QStringLiteral("-L"), listing1, QStringLiteral("-o"), object1,
                           source1}));
    QVERIFY(runTool(vasm, {QStringLiteral("-quiet"), QStringLiteral("-Fvobj"),
                           QStringLiteral("-L"), listing2, QStringLiteral("-o"), object2,
                           source2}));
    QVERIFY(runTool(vlink, {QStringLiteral("-b"), QStringLiteral("ataritos"),
                            QStringLiteral("-M") + mapFile,
                            QStringLiteral("-o"), prg, object1, object2}));

    LinkMap map;
    QString error;
    QVERIFY2(map.parse(mapFile, &error), qPrintable(error));

    // The measurement: the map keeps no directory for either module.
    QCOMPARE(map.placements().size(), 2);
    for (const LinkPlacement &p : map.placements())
        QCOMPARE(p.objectFile, QStringLiteral("util.o"));

    ProgramLineMap program;
    QVERIFY2(program.addModule(source1, object1, listing1, &error), qPrintable(error));
    QVERIFY2(program.addModule(source2, object2, listing2, &error), qPrintable(error));
    program.setLinkMap(map);

    LineMap::SectionBases live;
    live.text = 0x12596;
    program.setLiveBases(live);
    QVERIFY(program.isResolved());

    QVERIFY2(program.unplacedModules().size() == 2,
             qPrintable("a map naming both modules `util.o` cannot place either: "
                        + program.unplacedModules().join(QLatin1String(", "))));

    quint32 addr = 0;
    QVERIFY2(!program.addressFor(source2, 3, &addr),
             "the second module must not be armed at the first module's address");

    // `two` is not where the old base-name match would have put it: the two
    // modules are at different addresses in the linked program, so one shared
    // placement for both was wrong, not merely unlucky.
    const QString gst2ascii = QStandardPaths::findExecutable(QStringLiteral("gst2ascii"));
    if (gst2ascii.isEmpty())
        QSKIP("needs gst2ascii to read the linked program");

    QProcess symbols;
    symbols.start(gst2ascii, {prg});
    QVERIFY(symbols.waitForFinished(15000));
    const QString listed = QString::fromUtf8(symbols.readAllStandardOutput());

    const auto offsetOf = [&listed](const QString &name) -> int {
        const QRegularExpression re(
            QStringLiteral(R"(^0x([0-9A-Fa-f]+)\s+\w\s+%1$)").arg(name),
            QRegularExpression::MultilineOption);
        const auto match = re.match(listed);
        return match.hasMatch() ? match.captured(1).toInt(nullptr, 16) : -1;
    };

    // `one` is the first module's first instruction, so its offset is legitimately
    // zero; -1 is what marks a name the table does not have.
    const int one = offsetOf(QStringLiteral("one"));
    const int two = offsetOf(QStringLiteral("two"));
    QVERIFY2(one >= 0 && two >= 0, qPrintable("no symbols in:\n" + listed));
    QVERIFY2(two != one, qPrintable("the two modules must not share an address:\n" + listed));
}

QTEST_MAIN(TstLink)
#include "tst_link.moc"
