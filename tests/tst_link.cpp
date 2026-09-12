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
        << QStringLiteral("00: \"text\" (0-%1)\n").arg(extent, 1, 16).toUpper()
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

    /// The real thing, when vasm and a vlink are available: assemble two
    /// modules, link them, and check the mapping against the linked program's
    /// own symbol table.
    void endToEndAgainstARealLink();
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

    auto run = [](const QString &program, const QStringList &args) {
        QProcess p;
        p.start(program, args);
        p.waitForFinished(20000);
        return p.exitCode() == 0;
    };

    const QString mainObj = dir.filePath(QStringLiteral("main.o"));
    const QString libObj = dir.filePath(QStringLiteral("lib.o"));
    const QString mainLst = dir.filePath(QStringLiteral("main.lst"));
    const QString libLst = dir.filePath(QStringLiteral("lib.lst"));
    const QString mapFile = dir.filePath(QStringLiteral("prog.map"));
    const QString prg = dir.filePath(QStringLiteral("prog.prg"));

    QVERIFY(run(vasm, {QStringLiteral("-quiet"), QStringLiteral("-Fvobj"),
                       QStringLiteral("-L"), mainLst, QStringLiteral("-o"), mainObj, mainSrc}));
    QVERIFY(run(vasm, {QStringLiteral("-quiet"), QStringLiteral("-Fvobj"),
                       QStringLiteral("-L"), libLst, QStringLiteral("-o"), libObj, libSrc}));
    QVERIFY(run(vlink, {QStringLiteral("-b"), QStringLiteral("ataritos"),
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

QTEST_MAIN(TstLink)
#include "tst_link.moc"
