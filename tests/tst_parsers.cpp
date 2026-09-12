// SPDX-License-Identifier: GPL-2.0-or-later
//
// Regression tests for the two parsers whose correctness the IDE depends on:
// vasm diagnostics and the listing line map. Both formats were captured from
// real vasm 2.0f output.

#include "build/Diagnostic.h"
#include "build/LineMap.h"

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

    void lineMapMapsLinesToOffsets();
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

static QString writeListing(QTemporaryDir &dir)
{
    // Verbatim structure of `vasmm68k_mot -L`, including the section table.
    const QString path = dir.filePath(QStringLiteral("ok.lst"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return {}; // empty path: the caller's parse will fail visibly
    QTextStream out(&f);
    out << "Sections:\n"
           "00: \"text\" (0-C)\n"
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
           "00: \"text\" (0-C)\n"
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
