// SPDX-License-Identifier: GPL-2.0-or-later
//
// Regression tests for the parsers whose correctness the IDE depends on: vasm
// and vlink diagnostics and the listing line map. The formats were captured from
// real vasm 2.0f / vlink 0.18a output.

#include "build/BuildService.h"
#include "build/Diagnostic.h"
#include "build/LineMap.h"
#include "build/ProgramLineMap.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

using namespace pist;

class TstParsers : public QObject
{
    Q_OBJECT

private slots:
    void diagnosticShapes_data();
    void diagnosticShapes();

    void linkerDiagnosticShapes_data();
    void linkerDiagnosticShapes();

    void lineMapMapsLinesToOffsets();
    void programMapTextExtent();
    void lineMapResolvesAgainstLiveBases();
    void lineMapRejectsUnknownLine();
    void lineMapMatchesAbsoluteListingPaths();
};

// The two diagnostic shapes vasm produces. The second has no file or line, and
// must still parse so it can be reported against the build log.
void TstParsers::diagnosticShapes_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<int>("code");
    QTest::addColumn<int>("lineNo");
    QTest::addColumn<QString>("file");

    QTest::newRow("located")
        << QStringLiteral("error 10 in line 2 of \"bad.s\": number or identifier expected")
        << 10 << 2 << QStringLiteral("bad.s");
    QTest::newRow("located-warning")
        << QStringLiteral("warning 51 in line 4 of \"warn.s\": instruction has been auto-aligned")
        << 51 << 4 << QStringLiteral("warn.s");
    QTest::newRow("unlocated")
        << QStringLiteral("error 3004: section attributes <r> not supported")
        << 3004 << 0 << QString();
    QTest::newRow("unlocated-fatal")
        << QStringLiteral(
               "fatal error 3008: output module doesn't allow multiple sections of the same type ()")
        << 3008 << 0 << QString();
}

void TstParsers::diagnosticShapes()
{
    QFETCH(QString, line);
    QFETCH(int, code);
    QFETCH(int, lineNo);
    QFETCH(QString, file);

    static const QRegularExpression located(QStringLiteral(
        "^(error|warning|fatal error)\\s+(\\d+)\\s+in line\\s+(\\d+)\\s+of\\s+\"([^\"]+)\":\\s*(.*)$"));
    static const QRegularExpression unlocated(
        QStringLiteral("^(error|warning|fatal error)\\s+(\\d+):\\s*(.*)$"));

    Diagnostic d;
    auto m = located.match(line);
    if (m.hasMatch()) {
        d.code = m.captured(2).toInt();
        d.line = m.captured(3).toInt();
        d.file = m.captured(4);
    } else {
        auto u = unlocated.match(line);
        QVERIFY2(u.hasMatch(), qPrintable("neither pattern matched: " + line));
        d.code = u.captured(2).toInt();
        d.line = 0;
    }

    QCOMPARE(d.code, code);
    QCOMPARE(d.line, lineNo);
    QCOMPARE(d.file, file);
}

// vlink's two diagnostic shapes. The severity word must survive parsing: it was
// once matched only to be discarded, which made `captured(1)` the numeric code
// and recorded every located warning as an error (docs/code-review-glm-001.md,
// P3). The located-warning row is the case that was missing.
void TstParsers::linkerDiagnosticShapes_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<int>("severity");
    QTest::addColumn<int>("code");
    QTest::addColumn<QString>("objectFile");
    QTest::addColumn<QString>("section");
    QTest::addColumn<uint>("offset");
    QTest::addColumn<QString>("message");

    QTest::newRow("located-error")
        << QStringLiteral("Error 36: main.o (CODE+0x4): Reference to undefined symbol helper.")
        << int(Diagnostic::Error) << 36 << QStringLiteral("main.o") << QStringLiteral("CODE")
        << 0x4u << QStringLiteral("Reference to undefined symbol helper.");
    QTest::newRow("located-warning")
        << QStringLiteral("Warning 1013: main.o (CODE+0x4): 8bit code reference to "
                          "`helper' (value to write: 0x12596) out of range")
        << int(Diagnostic::Warning) << 1013 << QStringLiteral("main.o") << QStringLiteral("CODE")
        << 0x4u
        << QStringLiteral("8bit code reference to `helper' (value to write: 0x12596) out of "
                          "range");
    QTest::newRow("unlocated-warning")
        << QStringLiteral("Warning 122: Relocation table format not supported by selected "
                          "output format - reverting to ataritos's standard.")
        << int(Diagnostic::Warning) << 122 << QString() << QString() << 0u
        << QStringLiteral("Relocation table format not supported by selected output format - "
                          "reverting to ataritos's standard.");
    QTest::newRow("unlocated-fatal")
        << QStringLiteral("Fatal error 9: Invalid target format \"atari\".")
        << int(Diagnostic::Error) << 9 << QString() << QString() << 0u
        << QStringLiteral("Invalid target format \"atari\".");
}

void TstParsers::linkerDiagnosticShapes()
{
    QFETCH(QString, line);
    QFETCH(int, severity);
    QFETCH(int, code);
    QFETCH(QString, objectFile);
    QFETCH(QString, section);
    QFETCH(uint, offset);
    QFETCH(QString, message);

    Diagnostic d;
    QVERIFY2(parseLinkerDiagnostic(line, &d), qPrintable("not a diagnostic: " + line));
    QCOMPARE(int(d.severity), severity);
    QCOMPARE(d.code, code);
    QCOMPARE(d.objectFile, objectFile);
    QCOMPARE(d.section, section);
    QCOMPARE(d.sectionOffset, quint32(offset));
    QCOMPARE(d.message, message);
    QCOMPARE(d.hasObjectOffset, !objectFile.isEmpty());

    // Ordinary linker chatter must be reported verbatim by the caller instead.
    Diagnostic ignored;
    QVERIFY(!parseLinkerDiagnostic(QStringLiteral("Aborting."), &ignored));
}

static QString writeListing(QTemporaryDir &dir)
{
    // Verbatim structure of `vasmm68k_mot -L`, including the section table.
    const QString path = dir.filePath(QStringLiteral("ok.lst"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {}; // empty path: the caller's parse will fail visibly
    QTextStream out(&f);
    out << "Sections:\n"
           "00: \"text\" (0-E)\n"
           "01: \"data\" (0-4)\n"
           "02: \"bss\" (0-10)\n"
           "\n\n"
           "Source: \"ok.s\"\n"
           "                            \t     1: \ttext\n"
           "00:00000000 58425241        \t     2: \tdc.b\t\"XBRA\"\n"
           "00:00000004 00000000        \t     3: \tdc.l\t0\n"
           "00:00000008 702A            \t     4: start:\tmove.l\t#42,d0\n"
           "00:0000000A 4E75            \t     5: \trts\n"
           "                            \t     6: \teven\n"
           "                            \t     7: \tdata\n"
           "01:00000000 6869            \t     8: msg_ptr:\tdc.b\t\"hi\",0\n";
    out.flush();
    f.close();
    return path;
}

void TstParsers::lineMapMapsLinesToOffsets()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeListing(dir);

    LineMap map;
    QString error;
    QVERIFY2(map.parseListing(path, &error), qPrintable(error));
    QVERIFY(!map.isEmpty());

    // `start` is at text offset 0x08, `msg_ptr` at data offset 0.
    LineMap::SectionBases bases;
    bases.text = 0x12596;
    bases.data = 0x125ac;
    bases.bss = 0x125b0;

    quint32 addr = 0;
    QVERIFY(map.addressFor(QStringLiteral("ok.s"), 4, bases, &addr));
    QCOMPARE(addr, 0x12596u + 0x08u);

    QVERIFY(map.addressFor(QStringLiteral("ok.s"), 8, bases, &addr));
    QCOMPARE(addr, 0x125acu);

    // Line 1 emitted nothing, so it has no address.
    QVERIFY(!map.addressFor(QStringLiteral("ok.s"), 1, bases, &addr));
}

// The stack view's "return address?" mark needs the program's text end, not the
// data base. With no data section an empty range silently drops every return
// address, so the extent comes from the listing's own `(start-end)` header
// (docs/code-review-glm-001.md, P3).
void TstParsers::programMapTextExtent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ProgramLineMap program;
    QVERIFY(program.addModule(QStringLiteral("ok.s"), QStringLiteral("ok.o"),
                              writeListing(dir), nullptr));

    // No live bases: there is no address to report yet.
    QCOMPARE(program.textEnd(), 0u);

    LineMap::SectionBases live;
    live.text = 0x12596;
    live.data = 0x125ac;
    live.bss = 0x125b0;
    program.setLiveBases(live);

    // The fixture's text is (0-E), so the extent is base + 0xE — neither the data
    // base (which only accidentally works when data follows text) nor the last
    // entry's offset.
    QCOMPARE(program.textEnd(), 0x12596u + 0xEu);
}

void TstParsers::lineMapResolvesAgainstLiveBases()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeListing(dir);

    LineMap map;
    QVERIFY(map.parseListing(path, nullptr));

    LineMap::SectionBases bases;
    bases.text = 0x12596;
    bases.data = 0x125ac;
    bases.bss = 0x125b0;

    // The PC inside `start` must resolve back to line 4.
    LineMap::Address a;
    QVERIFY(map.lineFor(0x12596u + 0x08u, bases, &a));
    QCOMPARE(a.line, 4);
    QCOMPARE(a.file, QStringLiteral("ok.s"));

    // Mid-instruction still resolves to the instruction that is executing:
    // `move.l #42,d0` occupies 0x08-0x09.
    QVERIFY(map.lineFor(0x12596u + 0x09u, bases, &a));
    QCOMPARE(a.line, 4);

    // 0x0A is the start of the *next* instruction (`rts`), which is line 5.
    QVERIFY(map.lineFor(0x12596u + 0x0Au, bases, &a));
    QCOMPARE(a.line, 5);

    // Past the end of the text section there is no line to report. This
    // expectation was previously the opposite — the lookup resolved to line 5
    // wherever the PC landed — which meant a PC in ROM, on the stack, or anywhere
    // past the program highlighted an unrelated source line.
    QVERIFY2(!map.lineFor(0x12596u + 0x10u, bases, &a),
             "an address past the section extent must not resolve to a line");

    // The bound comes from the listing's own `(0-C)` extent, so the last valid
    // offset still resolves.
    QVERIFY(map.lineFor(0x12596u + 0x0Au, bases, &a));
    QCOMPARE(a.line, 5);

    // Uninitialised bases must not resolve.
    LineMap::SectionBases empty;
    QVERIFY(!map.lineFor(0x12596u, empty, &a));
}

void TstParsers::lineMapRejectsUnknownLine()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeListing(dir);

    LineMap map;
    QVERIFY(map.parseListing(path, nullptr));

    LineMap::SectionBases bases;
    bases.text = 0x1000;

    quint32 addr = 0;
    QVERIFY(!map.addressFor(QStringLiteral("ok.s"), 9999, bases, &addr));
    QVERIFY(!map.addressFor(QStringLiteral("other.s"), 4, bases, &addr));
}

// vasm records the source path exactly as passed on the command line. The IDE
// builds with an absolute path but identifies the open file by name, so a literal
// comparison silently matches nothing — which would break both breakpoint arming
// and the PC-to-line highlight, with no error anywhere.
void TstParsers::lineMapMatchesAbsoluteListingPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("prog.lst"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "Sections:\n"
           "00: \"text\" (0-E)\n"
           "\n"
           "Source: \"" << dir.filePath(QStringLiteral("prog.s")) << "\"\n"
           "00:0000000C 60FE            \t     6: loop:\tbra.s\tloop\n";
    out.flush();
    f.close();

    LineMap map;
    QString error;
    QVERIFY2(map.parseListing(path, &error), qPrintable(error));

    LineMap::SectionBases bases;
    bases.text = 0x12596;

    // Queried by base name, as the IDE does.
    quint32 addr = 0;
    QVERIFY2(map.addressFor(QStringLiteral("prog.s"), 6, bases, &addr),
             "a bare file name must match an absolute path in the listing");
    QCOMPARE(addr, 0x12596u + 0x0Cu);

    // And the reverse, which drives the editor highlight.
    LineMap::Address back;
    QVERIFY(map.lineFor(addr, bases, &back));
    QCOMPARE(back.line, 6);
    QVERIFY(LineMap::sameSource(back.file, QStringLiteral("prog.s")));

    // A genuinely different file must still not match.
    QVERIFY(!map.addressFor(QStringLiteral("other.s"), 6, bases, &addr));
    QVERIFY(!LineMap::sameSource(back.file, QStringLiteral("other.s")));
}

QTEST_MAIN(TstParsers)
#include "tst_parsers.moc"
