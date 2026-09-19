// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Tests for breakpoint resolution and memory-dump parsing. Both are pure
// functions, deliberately, so the rules that matter are testable without an
// emulator.

#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "build/LineMap.h"
#include "emu/MemoryDump.h"
#include "project/ProjectSettings.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace pist;

namespace {

/// A listing matching the structure vasm actually emits.
QString writeListing(QTemporaryDir &dir)
{
    const QString path = dir.filePath(QStringLiteral("prog.lst"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {};
    QTextStream out(&f);
    out << "Sections:\n"
           "00: \"text\" (0-E)\n"
           "01: \"data\" (0-4)\n"
           "\n"
           "Source: \"prog.s\"\n"
           "                            \t     1: \ttext\n"
           "00:00000000 487A000C        \t     2: start:\tpea\tmsg(pc)\n"
           "00:00000004 3F3C0009        \t     3: \tmove.w\t#9,-(sp)\n"
           "00:00000008 4E41            \t     4: \ttrap\t#1\n"
           "00:0000000A 5C8F            \t     5: \taddq.l\t#6,sp\n"
           "00:0000000C 60FE            \t     6: loop:\tbra.s\tloop\n"
           "                            \t     7: \teven\n"
           "01:00000000 4849            \t     8: msg:\tdc.b\t\"HI\"\n";
    out.flush();
    f.close();
    return path;
}

LineMap::SectionBases liveBases()
{
    LineMap::SectionBases b;
    b.text = 0x12596;
    b.data = 0x125ac;
    b.bss = 0x125b0;
    return b;
}

} // namespace

class TstDebug : public QObject
{
    Q_OBJECT

private slots:
    // --- breakpoint resolution -------------------------------------------

    void resolvesLineToAddress();
    void breakpointOnDataLineIsUnresolved();
    void emitsConditionExpressionNotBareAddress();
    void appendsUserCondition();
    void reportsLinesWithNoCode();
    void skipsDisabledBreakpoints();
    void refusesToResolveWithoutBases();
    void refusesAddressPastTheEndOfItsSection();

    // --- memory dump parsing ---------------------------------------------

    void parsesByteDump();
    void parsesWordDump();
    void parsesLongDump();
    void ignoresNonDumpText();
    void ignoresAHexLookingCharacterColumn();
    void rendersCharacterColumn();
    void readsBigEndianLong();
    void rejectsOutOfRangeLong();
    void classifiesPointerValues();

    // Watchpoints: Hatari has no data watchpoints, so they are self-inequality
    // breakpoints, and the exact command text is what makes the change-tracking
    // fire. Pin it so a drift in the format cannot silently stop them working.
    void watchpointCommandIsSelfInequality();
    void watchpointWidthsFormatCorrectly();
    void watchpointLabelShowsAddressAndWidth();

    // paths
    void outputPathsDeriveFromSource();
};

void TstDebug::resolvesLineToAddress()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    map.setLiveBases(liveBases());

    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 6, QString(), true, 0, false}); // loop:

    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 1);
    QCOMPARE(plan.armed.size(), 1);
    QCOMPARE(plan.armed.first().address, 0x12596u + 0x0Cu);
    QVERIFY(plan.armed.first().resolved);
}

void TstDebug::breakpointOnDataLineIsUnresolved()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    map.setLiveBases(liveBases());

    // Line 8 is `msg: dc.b "HI"` — a data-section line. planBreakpoints must
    // report it unresolved rather than arm `b pc = $<data>`, which can never
    // fire (finding B10).
    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 8, QString(), true, 0, false});
    const ArmPlan plan = planBreakpoints(bps, map);
    QVERIFY(plan.commands.isEmpty());
    QVERIFY(plan.armed.isEmpty());
    QCOMPARE(plan.unresolved.size(), 1);

    // But the general resolver still resolves the data line — watchpoints arm on
    // data addresses, so the code/data filter belongs to the breakpoint consumer,
    // not addressFor.
    quint32 addr = 0;
    QVERIFY(map.addressFor(QStringLiteral("prog.s"), 8, &addr));
    QCOMPARE(addr, 0x125acu);
}

// Hatari's `b` takes a condition: a bare symbol or address produces
// "condition comparison missing". The command must therefore be an expression.
void TstDebug::emitsConditionExpressionNotBareAddress()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    map.setLiveBases(liveBases());

    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 4, QString(), true, 0, false});

    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 1);
    const QString cmd = plan.commands.first();
    QVERIFY2(cmd.startsWith(QLatin1String("b pc = $")), qPrintable(cmd));
    QVERIFY(cmd.contains(QStringLiteral("1259e"), Qt::CaseInsensitive));
}

// Upstream Hatari has no memory watchpoints, so a conditional breakpoint is the
// only way to watch a value. The user's expression must be preserved and ANDed.
void TstDebug::appendsUserCondition()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    map.setLiveBases(liveBases());

    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 4, QStringLiteral("d0 = $1234"), true, 0, false});

    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 1);
    const QString cmd = plan.commands.first();
    QVERIFY(cmd.contains(QLatin1String("&& d0 = $1234")));
    QVERIFY(cmd.startsWith(QLatin1String("b pc = ")));
}

void TstDebug::reportsLinesWithNoCode()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    map.setLiveBases(liveBases());

    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 1, QString(), true, 0, false}); // `text`
    bps.append(Breakpoint{QStringLiteral("prog.s"), 7, QString(), true, 0, false}); // `even`

    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 0);
    QCOMPARE(plan.unresolved.size(), 2);
}

void TstDebug::skipsDisabledBreakpoints()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    map.setLiveBases(liveBases());

    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 6, QString(), false, 0, false});

    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 0);
    // Disabled is not the same as unresolvable, so it must not be reported as
    // a problem either.
    QCOMPARE(plan.unresolved.size(), 0);
}

// Without a running program there is no load address, so nothing may be armed.
// This is the guard that stops a breakpoint binding to whatever happened to be
// at that address.
void TstDebug::refusesToResolveWithoutBases()
{
    QTemporaryDir dir;
    ProgramLineMap map;
    QString error;
    QVERIFY(map.addModule(QStringLiteral("prog.s"), QStringLiteral("prog.o"),
                          writeListing(dir), &error));
    // No live bases: nothing is resolved yet, so nothing may be armed.

    QList<Breakpoint> bps;
    bps.append(Breakpoint{QStringLiteral("prog.s"), 6, QString(), true, 0, false});

    const ArmPlan plan = planBreakpoints(bps, map);
    QCOMPARE(plan.commands.size(), 0);
}

// Without a span bound, any address past the last entry — ROM, stack, or the
// tail of a section — resolved to the program's final source line, so the editor
// highlighted and scrolled to a line unrelated to the PC.
void TstDebug::refusesAddressPastTheEndOfItsSection()
{
    QTemporaryDir dir;
    LineMap map;
    QVERIFY(map.parseListing(writeListing(dir), nullptr));

    LineMap::SectionBases bases = liveBases();

    // Last text entry is line 6 at offset 0x0C; line 8 is data at offset 0.
    // A text address well beyond the final text entry belongs to no line.
    LineMap::Address a;
    const quint32 pastText = bases.text + 0x40;
    QVERIFY2(!map.lineFor(pastText, bases, &a),
             "an address past the section's last entry must not resolve to a line");

    // An address inside the last text instruction still resolves.
    QVERIFY(map.lineFor(bases.text + 0x0Cu, bases, &a));
    QCOMPARE(a.line, 6);

    // And one inside a later section resolves within that section, not to the
    // last line of the previous one.
    QVERIFY(map.lineFor(bases.data, bases, &a));
    QCOMPARE(a.line, 8);
}

// --- paths --------------------------------------------------------------

// The build writes <base>.prg and the launch runs it. Deriving those twice was a
// hazard: a change to one would silently break Run, or worse, run a stale binary.
void TstDebug::outputPathsDeriveFromSource()
{
    // Built from a real path in the temp directory rather than a hardcoded POSIX
    // one: "/home/x/proj/main.s" is not an absolute path on Windows (it has no
    // drive), so expectations written that way fail there for reasons that have
    // nothing to do with the behaviour under test.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("main.s"));

    const settings::OutputPaths paths = settings::outputPathsFor(source);
    QVERIFY(paths.isValid());

    // The outputs are siblings of the source, with the expected suffixes.
    const QFileInfo sourceInfo(source);
    for (const QString &derived : {paths.program, paths.listing, paths.project}) {
        QVERIFY2(QFileInfo(derived).isAbsolute(), qPrintable(derived));
        QCOMPARE(QFileInfo(derived).absolutePath(), sourceInfo.absolutePath());
    }
    QCOMPARE(QFileInfo(paths.program).fileName(), QStringLiteral("main.prg"));
    QCOMPARE(QFileInfo(paths.listing).fileName(), QStringLiteral("main.lst"));
    QCOMPARE(QFileInfo(paths.project).fileName(), QStringLiteral("main.pistproject"));

    // A source with dots in the name keeps only the final extension stripped, so
    // the program path cannot collide with a differently-named sibling.
    const settings::OutputPaths dotted =
        settings::outputPathsFor(dir.filePath(QStringLiteral("game.v2.s")));
    QCOMPARE(QFileInfo(dotted.program).fileName(), QStringLiteral("game.v2.prg"));

    QVERIFY(!settings::outputPathsFor(QString()).isValid());
}

// --- memory --------------------------------------------------------------

void TstDebug::parsesByteDump()
{
    const QString dump =
        QStringLiteral("00012596: 48 7a 00 0c 3f 3c 00 09 4e 41 5c 8f 60 fe 4f 4b  Hz..?<..NA\\OK\n"
                       "000125A6: 0d 0a 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................\n");

    const QList<MemoryRow> rows = parseMemoryDump(dump);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].address, 0x12596u);
    QCOMPARE(rows[0].bytes.size(), 16);
    QCOMPARE(rows[0].bytes[0], quint8(0x48));
    QCOMPARE(rows[0].bytes[15], quint8(0x4b));
    QCOMPARE(rows[1].address, 0x125A6u);
}

void TstDebug::parsesWordDump()
{
    // `m w` emits 4-digit groups; these must still normalise to bytes so the
    // view renders identically regardless of the requested width.
    const QString dump = QStringLiteral(
        "00012596: 487a 000c 3f3c 0009 4e41 5c8f 60fe 4f4b  Hz..?<..NA\\OK\n");
    const QList<MemoryRow> rows = parseMemoryDump(dump);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].bytes.size(), 16);
    QCOMPARE(rows[0].bytes[0], quint8(0x48));
    QCOMPARE(rows[0].bytes[1], quint8(0x7a));
    QCOMPARE(rows[0].bytes[2], quint8(0x00));
    QCOMPARE(rows[0].bytes[3], quint8(0x0c));
}

void TstDebug::parsesLongDump()
{
    const QString dump =
        QStringLiteral("00012596: 487a000c 3f3c0009 4e415c8f 60fe4f4b  Hz..?<..NA\\OK\n");
    const QList<MemoryRow> rows = parseMemoryDump(dump);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].bytes.size(), 16);
    QCOMPARE(rows[0].bytes[3], quint8(0x0c));
}

void TstDebug::ignoresNonDumpText()
{
    // A register dump and a symbol listing must not be mistaken for memory.
    const QString noise = QStringLiteral(
        "D0 00003D50   D1 00000000   D2 00000000   D3 00000000\n"
        "SR=0300 T=00 S=0 M=0 X=0 N=0 Z=0 V=0 C=0 IM=3\n"
        "0x00000008 T start\n"
        "Loaded 3 symbols (3 for code)\n");
    QCOMPARE(parseMemoryDump(noise).size(), 0);
}

void TstDebug::ignoresAHexLookingCharacterColumn()
{
    // Hatari separates the hex field from its character column with two
    // spaces, and the row pattern's `\s+` crosses them: memory holding
    // "0123456789ABCDEF" — exactly what a pane pointed at text shows — used to
    // parse the character column as four more bytes, breaking the documented
    // one-row-per-16-bytes contract and rendering the absorbed digits in the
    // view's ASCII column.
    const QString dump = QStringLiteral(
        "00012596: 30 31 32 33 34 35 36 37 38 39 41 42 43 44 45 46  0123456789ABCDEF\n");
    const QList<MemoryRow> rows = parseMemoryDump(dump);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].bytes.size(), 16);
    QCOMPARE(rows[0].bytes[0], quint8(0x30));
    QCOMPARE(rows[0].bytes[15], quint8(0x46));
}

void TstDebug::rendersCharacterColumn()
{
    QVector<quint8> bytes;
    bytes << 0x48 << 0x7a << 0x00 << 0x7f << 0x20 << 0xff;
    QCOMPARE(renderMemoryChars(bytes), QStringLiteral("Hz.. ."));
}

// A pointer in memory is a big-endian long: the 68000 fetches the high byte
// first. Reading it the other way round would follow the wrong address.
void TstDebug::readsBigEndianLong()
{
    QCOMPARE(readLongBE(QByteArray::fromHex("00012596"), 0), 0x00012596u);
    // The offset selects position within the buffer, not just its start.
    QCOMPARE(readLongBE(QByteArray::fromHex("ff487a000c"), 1), 0x487a000cu);
}

void TstDebug::rejectsOutOfRangeLong()
{
    const QByteArray bytes = QByteArray::fromHex("00012596");
    // Fewer than four bytes from the offset cannot make a long, so rather than
    // reading past the dump it reports nothing.
    QCOMPARE(readLongBE(bytes, 4), 0u);
    QCOMPARE(readLongBE(bytes, 1), 0u);
    QCOMPARE(readLongBE(bytes, -1), 0u);
    QCOMPARE(readLongBE(QByteArray(), 0), 0u);
}

void TstDebug::classifiesPointerValues()
{
    // Zero points nowhere.
    QVERIFY(!looksLikeAddress(0u));
    // Odd addresses raise an address error on a long access, so they are data.
    QVERIFY(!looksLikeAddress(0x00012597u));
    // Past the 24-bit bus; the ST does not decode those bits, so the top byte is
    // register junk rather than part of an address.
    QVERIFY(!looksLikeAddress(0x01000000u));
    QVERIFY(!looksLikeAddress(0xFFFFFFFFu));
    // The highest even address the bus can reach, and a normal program pointer.
    QVERIFY(looksLikeAddress(0x00FFFFFEu));
    QVERIFY(looksLikeAddress(0x00012596u));
}

void TstDebug::watchpointCommandIsSelfInequality()
{
    Watchpoint wp;
    wp.address = 0x12345;
    wp.width = 'w';
    // The whole mechanism depends on this exact form: a self-inequality the
    // debugger's change-tracking reads as "fire when it changes".
    QCOMPARE(wp.command(), QStringLiteral("b ($12345).w ! ($12345).w"));
}

void TstDebug::watchpointWidthsFormatCorrectly()
{
    Watchpoint wp;
    wp.address = 0x4ba;
    for (const char *width : {"b", "w", "l"}) {
        wp.width = width[0];
        QVERIFY(wp.command().contains(QStringLiteral("($4ba).%1").arg(width[0])));
    }
}

void TstDebug::watchpointLabelShowsAddressAndWidth()
{
    Watchpoint wp;
    wp.address = 0xff8201;
    wp.width = 'b';
    QVERIFY(wp.label().contains(QStringLiteral("ff8201")));
    QVERIFY(wp.label().contains(QStringLiteral("b")));
}

QTEST_MAIN(TstDebug)
#include "tst_debug.moc"
