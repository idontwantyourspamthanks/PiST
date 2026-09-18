#include "build/FloppyImage.h"
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
    void floppyImageGeometry();
    void floppyListsAutoFolder();
    void floppyWritesAndListsFiles();
    void floppyMsaRoundTrip();
    void floppyReadsFilesFromImage();
    void floppyUpdateAddsAndRemoves();
    void floppyUpdateRefusesDim();
    void floppyRejectsOversizedExport();
    void floppyRejectsOversizedAutoFolderProgram();
    void floppyListingSurvivesDirectoryCycles();
    void floppyNamesAreSanitizedForHostPaths();
    void floppyRejectsAbsurdGeometry();
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

// The AUTO-folder image must match mkfs.vfat's canonical 720 KiB layout
// exactly — pinned against a real image so a geometry mistake fails here
// rather than as a TOS boot failure: 512-byte sectors, 2 sectors per cluster,
// two 3-sector FATs, a 7-sector root directory, data at sector 14.
void TstParsers::floppyImageGeometry()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString prgPath = tmp.path() + QStringLiteral("/hello.prg");
    {
        QFile f(prgPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        // 1500 bytes: bigger than the 4096-byte prg-size guard, spanning two
        // clusters (2 * 1024) so the FAT chain has a real link to verify.
        QByteArray data(1500, '\x41');
        QCOMPARE(f.write(data), qint64(data.size()));
    }
    const QString imgPath = tmp.path() + QStringLiteral("/auto.st");
    QString error;
    QVERIFY(pist::floppy::writeAutoFolderImage(imgPath, prgPath, &error));
    QFile img(imgPath);
    QVERIFY(img.open(QIODevice::ReadOnly));
    const QByteArray image = img.readAll();
    QCOMPARE(image.size(), 720 * 1024);
    auto u8 = [&](int off) { return quint8(image.at(off)); };
    auto u16 = [&](int off) { return quint16(u8(off) | (u8(off + 1) << 8)); };

    // BPB: the mkfs.vfat geometry.
    QCOMPARE(u16(11), quint16(512));   // bytes per sector
    QCOMPARE(u8(13), quint8(2));       // sectors per cluster
    QCOMPARE(u16(14), quint16(1));     // reserved sectors
    QCOMPARE(u8(16), quint8(2));       // FATs
    QCOMPARE(u16(17), quint16(112));   // root entries
    QCOMPARE(u16(19), quint16(1440));  // total sectors
    QCOMPARE(u8(21), quint8(0xF0));    // media descriptor
    QCOMPARE(u16(22), quint16(3));     // sectors per FAT

    // FAT start (both copies share the layout): media F0, EOC, then the
    // cluster chain 2 -> 3 -> ... -> EOC for AUTO (2) and PROG.PRG (3,4).
    auto fat12 = [&](int cluster) {
        const int off = 512 + cluster + cluster / 2; // FAT 1 at sector 1
        const int v = u16(off);
        return quint16(cluster & 1 ? v >> 4 : v & 0xFFF);
    };
    QCOMPARE(fat12(0), quint16(0xFF0));
    QCOMPARE(fat12(1), quint16(0xFFF));
    QCOMPARE(fat12(2), quint16(0xFFF)); // AUTO: single cluster
    QCOMPARE(fat12(3), quint16(4));     // PROG.PRG: cluster 3 continues
    QCOMPARE(fat12(4), quint16(0xFFF)); // PROG.PRG: ends at cluster 4
    QCOMPARE(fat12(5), quint16(0));     // nothing beyond

    // Root directory at sector 7: AUTO directory, EMUDESK.INF file.
    const int root = 7 * 512;
    QVERIFY(memcmp(image.constData() + root, "AUTO       ", 11) == 0);
    QCOMPARE(u8(root + 11), quint8(0x10));          // directory attribute
    QCOMPARE(u16(root + 26), quint16(2));           // start cluster
    QVERIFY(memcmp(image.constData() + root + 32, "EMUDESK INF", 11) == 0);
    QCOMPARE(u8(root + 43), quint8(0x20));          // archive attribute
    QCOMPARE(u16(root + 32 + 28), quint32(0));      // empty file

    // AUTO's data cluster at sector 14: ".", "..", PROG.PRG with size 1500.
    const int autoDir = 14 * 512;
    QVERIFY(memcmp(image.constData() + autoDir, ".          ", 11) == 0);
    QVERIFY(memcmp(image.constData() + autoDir + 32, "..         ", 11) == 0);
    QVERIFY(memcmp(image.constData() + autoDir + 64, "PROG    PRG", 11) == 0);
    QCOMPARE(u16(autoDir + 64 + 26), quint16(3));      // start cluster
    QCOMPARE(u16(autoDir + 64 + 28) | (u16(autoDir + 64 + 30) << 16),
             quint32(1500));                           // file size

    // PRG content lands at cluster 3 (sector 16) and reads back whole.
    QCOMPARE(image.mid(16 * 512, 1500), QByteArray(1500, '\x41'));
}

static bool entryNamed(const QVector<floppy::Entry> &entries, const QString &path, bool isDir)
{
    for (const floppy::Entry &e : entries) {
        if (e.path.compare(path, Qt::CaseInsensitive) == 0 && e.isDirectory == isDir)
            return true;
    }
    return false;
}

void TstParsers::floppyListsAutoFolder()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString prgPath = tmp.path() + QStringLiteral("/hello.prg");
    {
        QFile f(prgPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(QByteArray(64, 'A')), qint64(64));
    }
    const QString imgPath = tmp.path() + QStringLiteral("/auto.st");
    QString error;
    QVERIFY(floppy::writeAutoFolderImage(imgPath, prgPath, &error));
    const QVector<floppy::Entry> entries = floppy::listImage(imgPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entryNamed(entries, QStringLiteral("AUTO"), true));
    QVERIFY(entryNamed(entries, QStringLiteral("AUTO/PROG.PRG"), false));
    QVERIFY(entryNamed(entries, QStringLiteral("EMUDESK.INF"), false));
}

void TstParsers::floppyWritesAndListsFiles()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item dir;
    dir.destPath = QStringLiteral("DATA");
    dir.isDirectory = true;
    items.append(dir);
    floppy::Item file;
    file.destPath = QStringLiteral("DATA/HELLO.TXT");
    file.data = QByteArrayLiteral("hello");
    items.append(file);
    floppy::Item root;
    root.destPath = QStringLiteral("readme.md");
    root.data = QByteArrayLiteral("readme");
    items.append(root);

    const QString img = tmp.path() + QStringLiteral("/out.st");
    QString error;
    QVERIFY2(floppy::writeImage(img, items, &error), qPrintable(error));
    QCOMPARE(QFileInfo(img).size(), qint64(720 * 1024));

    const QVector<floppy::Entry> entries = floppy::listImage(img, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entryNamed(entries, QStringLiteral("DATA"), true));
    QVERIFY(entryNamed(entries, QStringLiteral("DATA/HELLO.TXT"), false));
    QVERIFY(entryNamed(entries, QStringLiteral("README.MD"), false));
}

void TstParsers::floppyMsaRoundTrip()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("A.PRG");
    file.data = QByteArray(200, '\x42');
    items.append(file);

    const QString st = tmp.path() + QStringLiteral("/disk.st");
    const QString msa = tmp.path() + QStringLiteral("/disk.msa");
    QString error;
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));
    QVERIFY2(floppy::writeImage(msa, items, &error), qPrintable(error));
    QVERIFY(QFileInfo(msa).size() > 10);
    QVERIFY(QFileInfo(msa).size() < QFileInfo(st).size());

    QByteArray rawSt;
    QByteArray rawMsa;
    QVERIFY2(floppy::loadRaw(st, &rawSt, &error), qPrintable(error));
    QVERIFY2(floppy::loadRaw(msa, &rawMsa, &error), qPrintable(error));
    QCOMPARE(rawMsa.size(), rawSt.size());
    QCOMPARE(rawMsa, rawSt);

    const QVector<floppy::Entry> entries = floppy::listImage(msa, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entryNamed(entries, QStringLiteral("A.PRG"), false));
}

void TstParsers::floppyReadsFilesFromImage()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item dir;
    dir.destPath = QStringLiteral("STUFF");
    dir.isDirectory = true;
    items.append(dir);
    floppy::Item nested;
    nested.destPath = QStringLiteral("STUFF/DATA.BIN");
    // 1500 bytes spans two 1 KiB clusters, so the read must follow the chain.
    QByteArray payload(1500, '\0');
    for (int i = 0; i < payload.size(); ++i)
        payload[i] = char(i & 0xff);
    nested.data = payload;
    items.append(nested);
    floppy::Item root;
    root.destPath = QStringLiteral("hello.txt");
    root.data = QByteArrayLiteral("hi there");
    items.append(root);

    const QString img = tmp.path() + QStringLiteral("/read.st");
    QString error;
    QVERIFY2(floppy::writeImage(img, items, &error), qPrintable(error));
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(img, &raw, &error), qPrintable(error));

    QByteArray data;
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("HELLO.TXT"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArrayLiteral("hi there"));
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("hello.txt"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArrayLiteral("hi there"));
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("STUFF/DATA.BIN"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, payload);

    QVERIFY(!floppy::readFileRaw(raw, QStringLiteral("STUFF"), &data, &error));
    QVERIFY(!floppy::readFileRaw(raw, QStringLiteral("NOPE.TXT"), &data, &error));
    QVERIFY(!error.isEmpty());
}

void TstParsers::floppyUpdateAddsAndRemoves()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item dir;
    dir.destPath = QStringLiteral("DATA");
    dir.isDirectory = true;
    items.append(dir);
    floppy::Item keep;
    keep.destPath = QStringLiteral("DATA/KEEP.TXT");
    keep.data = QByteArrayLiteral("kept");
    items.append(keep);
    floppy::Item drop;
    drop.destPath = QStringLiteral("DROP.TXT");
    drop.data = QByteArrayLiteral("dropped");
    items.append(drop);

    QString error;
    const QString st = tmp.path() + QStringLiteral("/work.st");
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));

    floppy::Item added;
    added.destPath = QStringLiteral("DATA/NEW.BIN");
    added.data = QByteArray(1200, '\x7f');
    QVERIFY2(floppy::updateImage(st, {added}, {QStringLiteral("DROP.TXT")}, &error),
             qPrintable(error));

    const QVector<floppy::Entry> entries = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entryNamed(entries, QStringLiteral("DATA"), true));
    QVERIFY(entryNamed(entries, QStringLiteral("DATA/KEEP.TXT"), false));
    QVERIFY(entryNamed(entries, QStringLiteral("DATA/NEW.BIN"), false));
    QVERIFY(!entryNamed(entries, QStringLiteral("DROP.TXT"), false));

    QByteArray raw;
    QVERIFY2(floppy::loadRaw(st, &raw, &error), qPrintable(error));
    QByteArray data;
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("DATA/KEEP.TXT"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArrayLiteral("kept"));
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("DATA/NEW.BIN"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArray(1200, '\x7f'));

    // Adding a name that already exists duplicates it with a numbered name
    // rather than failing, so pasting the same file onto a disk twice keeps
    // both copies.
    floppy::Item dup;
    dup.destPath = QStringLiteral("DROP.TXT");
    dup.data = QByteArrayLiteral("dropped again");
    QVERIFY2(floppy::updateImage(st, {dup, dup}, {}, &error), qPrintable(error));
    const QVector<floppy::Entry> withDups = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entryNamed(withDups, QStringLiteral("DROP.TXT"), false));
    QVERIFY(entryNamed(withDups, QStringLiteral("DROP1.TXT"), false));

    // Removing a folder removes everything under it, and nothing beside it.
    QVERIFY2(floppy::updateImage(st, {}, {QStringLiteral("DATA")}, &error),
             qPrintable(error));
    const QVector<floppy::Entry> after = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(after.size(), 2);
    QVERIFY(entryNamed(after, QStringLiteral("DROP.TXT"), false));
    QVERIFY(entryNamed(after, QStringLiteral("DROP1.TXT"), false));

    // The same operations must keep a compressed .msa a working .msa.
    const QString msa = tmp.path() + QStringLiteral("/work.msa");
    QVERIFY2(floppy::writeImage(msa, items, &error), qPrintable(error));
    floppy::Item small;
    small.destPath = QStringLiteral("A.PRG");
    small.data = QByteArrayLiteral("\x4e\x75");
    QVERIFY2(floppy::updateImage(msa, {small}, {}, &error), qPrintable(error));
    const QVector<floppy::Entry> msaEntries = floppy::listImage(msa, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entryNamed(msaEntries, QStringLiteral("DATA/KEEP.TXT"), false));
    QVERIFY(entryNamed(msaEntries, QStringLiteral("A.PRG"), false));

    // The replace dance must clean up after itself: no staged or backup
    // file may survive a successful update.
    const QStringList litter = QDir(tmp.path()).entryList(
        {QStringLiteral(".pist-*")}, QDir::Hidden | QDir::Files);
    QCOMPARE(litter.size(), 0);
}

void TstParsers::floppyUpdateRefusesDim()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dim = tmp.path() + QStringLiteral("/disk.dim");
    {
        QFile f(dim);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(32 + 720 * 1024, '\0'));
    }
    floppy::Item file;
    file.destPath = QStringLiteral("X.TXT");
    file.data = QByteArrayLiteral("x");
    QString error;
    QVERIFY(!floppy::updateImage(dim, {file}, {}, &error));
    QVERIFY(!error.isEmpty());
}

void TstParsers::floppyRejectsOversizedExport()
{
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("HUGE.BIN");
    file.data = QByteArray(800 * 1024, 'X');
    items.append(file);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString img = tmp.path() + QStringLiteral("/huge.st");
    QString error;
    QVERIFY(!floppy::writeImage(img, items, &error));
    QVERIFY(!error.isEmpty());
}

void TstParsers::floppyRejectsOversizedAutoFolderProgram()
{
    // The AUTO folder takes cluster 2 and the program runs contiguously from
    // cluster 3, so 712 clusters — 729,088 bytes — is the capacity. The
    // boundary itself must succeed; one byte past it must be refused rather
    // than written past the end of the image (writeImage's allocateClusters
    // guards the generic export path the same way).
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString prgExact = tmp.path() + QStringLiteral("/exact.prg");
    {
        QFile f(prgExact);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(729088, 'X'));
    }
    const QString exactImg = tmp.path() + QStringLiteral("/exact.st");
    QString error;
    QVERIFY2(floppy::writeAutoFolderImage(exactImg, prgExact, &error),
             qPrintable(error));

    const QString prgHuge = tmp.path() + QStringLiteral("/huge.prg");
    {
        QFile f(prgHuge);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(729089, 'X'));
    }
    const QString hugeImg = tmp.path() + QStringLiteral("/huge.st");
    error.clear();
    QVERIFY(!floppy::writeAutoFolderImage(hugeImg, prgHuge, &error));
    QVERIFY(error.contains(QLatin1String("fit")));
    // The refused write must not leave a truncated image behind.
    QVERIFY(!QFile::exists(hugeImg));
}


namespace {
/// Patch a 32-byte directory entry into the image at `at` (canonical 720 KiB
/// geometry: the first allocated cluster is 2, at sector 14).
void putDirEntry(QFile &f, int at, const QByteArray &name11, quint8 attr, quint16 cluster,
                 quint32 size)
{
    QByteArray e(32, '\0');
    e.replace(0, 11, QByteArray(name11).left(11).leftJustified(11, ' '));
    e[11] = char(attr);
    e[26] = char(cluster & 0xff);
    e[27] = char(cluster >> 8);
    e[28] = char(size & 0xff);
    e[29] = char((size >> 8) & 0xff);
    e[30] = char((size >> 16) & 0xff);
    e[31] = char((size >> 24) & 0xff);
    QVERIFY(f.seek(at));
    QVERIFY(f.write(e) == 32);
}
} // namespace

void TstParsers::floppyListingSurvivesDirectoryCycles()
{
    // A crafted image whose directory entries point back at the cluster they
    // live in: two entries per level double per depth step, and a walk with
    // no shared visited set expands without bound (the per-chain cycle set in
    // readChain dies with each call). The listing must terminate, bounded.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item folder;
    folder.destPath = QStringLiteral("F");
    folder.isDirectory = true;
    items.append(folder);
    const QString st = tmp.path() + QStringLiteral("/cycle.st");
    QString error;
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));

    QFile f(st);
    QVERIFY(f.open(QIODevice::ReadWrite));
    // The folder is the only allocation, so its directory is cluster 2, at
    // sector 14 of the canonical geometry.
    putDirEntry(f, 14 * 512, QByteArrayLiteral("LOOPA      "), 0x10, 2, 0);
    putDirEntry(f, 14 * 512 + 32, QByteArrayLiteral("LOOPB      "), 0x10, 2, 0);
    f.close();

    const QVector<floppy::Entry> entries = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(entries.size() <= 1000);
    QVERIFY(entryNamed(entries, QStringLiteral("F"), true));
    QVERIFY(entryNamed(entries, QStringLiteral("F/LOOPA"), true));
    QVERIFY(entryNamed(entries, QStringLiteral("F/LOOPB"), true));
}

void TstParsers::floppyNamesAreSanitizedForHostPaths()
{
    // A crafted 8.3 name carrying path separators and traversal dots: the
    // listed path is what the copy-out destination is joined from, so every
    // component of it must be safe to use as a host file name.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item folder;
    folder.destPath = QStringLiteral("F");
    folder.isDirectory = true;
    items.append(folder);
    const QString st = tmp.path() + QStringLiteral("/evil.st");
    QString error;
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));

    QFile f(st);
    QVERIFY(f.open(QIODevice::ReadWrite));
    putDirEntry(f, 14 * 512, QByteArrayLiteral("../evil     "), 0x20, 0, 0);
    putDirEntry(f, 14 * 512 + 32, QByteArrayLiteral("SLASH/NAME  "), 0x20, 0, 0);
    f.close();

    const QVector<floppy::Entry> entries = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    for (const floppy::Entry &entry : entries) {
        const QStringList components = entry.path.split(QLatin1Char('/'));
        for (const QString &component : components) {
            QVERIFY2(!component.isEmpty(), qPrintable(entry.path));
            QVERIFY2(component != QLatin1String("..") && component != QLatin1String("."),
                     qPrintable(entry.path));
            QVERIFY2(!component.contains(QLatin1Char('\\'))
                         && !component.contains(QLatin1Char('/')),
                     qPrintable(entry.path));
        }
    }
}

void TstParsers::floppyRejectsAbsurdGeometry()
{
    // A crafted BPB whose data region starts far past the end of the image.
    // Computed in 32-bit, firstDataSector * sectorSize overflows a signed int
    // (UB) and the wrapped — often negative — product sails through the bounds
    // check written to contain it, so the geometry is accepted and describes
    // nothing. In 64-bit the check sees the real value and refuses the image.
    QByteArray img(1024, '\0');
    const auto put16 = [&img](int at, quint16 v) {
        img[at] = char(v & 0xff);
        img[at + 1] = char((v >> 8) & 0xff);
    };
    put16(11, 512);   // bytes per sector
    img[13] = 2;      // sectors per cluster
    put16(14, 1);     // reserved sectors
    img[16] = char(255);  // FAT count
    put16(17, 112);   // root directory entries
    put16(19, 2);     // total sectors
    put16(22, 65535); // sectors per FAT

    QString error;
    QVERIFY(floppy::listRaw(img, &error).isEmpty());
    QVERIFY2(!error.isEmpty(), "a geometry that describes nothing must be refused");

    error.clear();
    QByteArray data;
    QVERIFY(!floppy::readFileRaw(img, QStringLiteral("A.TXT"), &data, &error));
    QVERIFY2(!error.isEmpty(), "the reader must refuse it too");
}
QTEST_MAIN(TstParsers)
#include "tst_parsers.moc"
