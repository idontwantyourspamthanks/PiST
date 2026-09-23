// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Regression tests for the parsers whose correctness the IDE depends on: vasm
// and vlink diagnostics and the listing line map. The formats were captured from
// real vasm 2.0f / vlink 0.18a output.

#include "build/BuildService.h"
#include "build/Diagnostic.h"
#include "build/FloppyImage.h"
#include "build/LineMap.h"
#include "build/ProgramLineMap.h"
#include "build/SymbolTable.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

#include <algorithm>

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
    void lineMapRejectsUnparseableListing();
    void lineMapMatchesAbsoluteListingPaths();
    void buildServiceFlushesFinalUnterminatedLine();
    void buildServiceWithoutALinkerSaysSo();
    void buildServiceRefusesCollidingObjectPaths();
    void symbolTableReadsTheTablesInEitherOrder();
    void floppyImageGeometry();
    void floppyListsAutoFolder();
    void floppyWritesAndListsFiles();
    void floppyMsaRoundTrip();
    void floppyReadsFilesFromImage();
    void floppyUpdateAddsAndRemoves();
    void floppyUpdatePreservesDuplicateNames();
    void floppyNestedDirectoriesPointAtTheirParent();
    void floppyUpdateKeepsForeignNamesByteIdentical();
    void floppyUpdateRefusesDim();
    void floppyRejectsOversizedExport();
    void floppyRejectsOversizedAutoFolderProgram();
    void floppyAutoFolderImageToleratesNoErrorSink();
    void floppyListingSurvivesDirectoryCycles();
    void floppyNamesAreSanitizedForHostPaths();
    void floppyRejectsAbsurdGeometry();
    void floppyReadRefusesADamagedChain();
    void floppyUpdateRefusesADamagedImage();
    void floppyCanonicalLayoutIsRecognised();
    void floppyReadsAForeignMkfsImage();
    void floppyReadsAForeignSpecMsa();
    void msaEncodingMatchesTheDocumentedFormat();
    void floppyRejectsATruncatedMsaRun();
};

// vasm's diagnostic shapes, parsed by parseVasmDiagnostic itself. An earlier
// version of this test carried its own copy of the two patterns, so a change to
// BuildService.cpp — a dropped capture group, a widened quote class — left it
// green while the Problems pane quietly misreported diagnostics. Severity is
// pinned because that is precisely the drift that already shipped once on the
// linker side, where the severity word was matched but never captured
// (docs/code-review-glm-001.md P3).
void TstParsers::diagnosticShapes_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("isDiagnostic");
    QTest::addColumn<int>("severity");
    QTest::addColumn<int>("code");
    QTest::addColumn<int>("lineNo");
    QTest::addColumn<QString>("file");
    QTest::addColumn<QString>("message");

    QTest::newRow("located")
        << QStringLiteral("error 10 in line 2 of \"bad.s\": number or identifier expected")
        << true << int(Diagnostic::Error) << 10 << 2 << QStringLiteral("bad.s")
        << QStringLiteral("number or identifier expected");
    QTest::newRow("located-warning")
        << QStringLiteral("warning 51 in line 4 of \"warn.s\": instruction has been auto-aligned")
        << true << int(Diagnostic::Warning) << 51 << 4 << QStringLiteral("warn.s")
        << QStringLiteral("instruction has been auto-aligned");
    QTest::newRow("unlocated")
        << QStringLiteral("error 3004: section attributes <r> not supported")
        << true << int(Diagnostic::Error) << 3004 << 0 << QString()
        << QStringLiteral("section attributes <r> not supported");
    QTest::newRow("unlocated-fatal")
        << QStringLiteral(
               "fatal error 3008: output module doesn't allow multiple sections of the same type ()")
        << true << int(Diagnostic::Error) << 3008 << 0 << QString()
        << QStringLiteral("output module doesn't allow multiple sections of the same type ()");
    // Ordinary build output must not become a diagnostic, or the Problems pane
    // fills with noise and the build looks failed.
    QTest::newRow("not-a-diagnostic")
        << QStringLiteral("vasm v1.9 (compiled 2024.01.01)")
        << false << int(Diagnostic::Error) << 0 << 0 << QString() << QString();
}

void TstParsers::diagnosticShapes()
{
    QFETCH(QString, line);
    QFETCH(bool, isDiagnostic);
    QFETCH(int, severity);
    QFETCH(int, code);
    QFETCH(int, lineNo);
    QFETCH(QString, file);
    QFETCH(QString, message);

    Diagnostic d;
    const bool parsed = parseVasmDiagnostic(line, &d);
    QVERIFY2(parsed == isDiagnostic,
             qPrintable(QStringLiteral("%1: %2")
                            .arg(isDiagnostic ? QStringLiteral("not parsed")
                                              : QStringLiteral("misread as a diagnostic"), line)));
    if (!isDiagnostic)
        return;

    QCOMPARE(int(d.severity), severity);
    QCOMPARE(d.code, code);
    QCOMPARE(d.line, lineNo);
    QCOMPARE(d.file, file);
    QCOMPARE(d.message, message);
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

void TstParsers::lineMapRejectsUnparseableListing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad.lst"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    // A real, readable file that maps nothing — no section table, no entry lines.
    f.write("not a listing at all\n");
    f.close();

    LineMap map;
    QString error;
    // parseListing used to return true here, so an empty map masqueraded as a
    // good one and breakpoints silently never resolved (finding B11).
    QVERIFY(!map.parseListing(path, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(map.isEmpty());
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


void TstParsers::buildServiceFlushesFinalUnterminatedLine()
{
#ifdef Q_OS_WIN
    // The fake assembler is a /bin/sh script and Windows has no /bin/sh; the
    // finish-time flush is platform-independent, so POSIX coverage suffices.
    QSKIP("fake assembler needs a POSIX shell");
#endif

    // A fake assembler that emits a diagnostic with NO trailing newline, then
    // fails. The readyRead loop only consumes lines up to '\n', so without the
    // finish-time flush the last line never reaches handleStderrLine and its
    // output is silently dropped (finding B12).
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString script = dir.filePath(QStringLiteral("fakeasm.sh"));
    {
        QFile f(script);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("#!/bin/sh\nprintf 'hello.s:1: error: boom' >&2\nexit 1\n");
        f.close();
    }
    QFile::setPermissions(script,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    const QString src = dir.filePath(QStringLiteral("hello.s"));
    {
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\ttext\nstart:\trts\n\tend\n");
        f.close();
    }

    BuildService bs;
    bs.setAssemblerPath(script);
    bs.setSourceFile(src);
    bs.setOutputFile(dir.filePath(QStringLiteral("hello.prg")));
    QSignalSpy output(&bs, &BuildService::outputLine);
    QSignalSpy done(&bs, &BuildService::finished);
    bs.build();
    QVERIFY(done.wait(5000));

    bool sawBoom = false;
    for (const auto &args : output)
        if (args.at(0).toString().contains(QStringLiteral("boom")))
            sawBoom = true;
    QVERIFY2(sawBoom, "the final unterminated diagnostic line must be flushed and surfaced");
}

// A linked build with no linker configured has no step that can run. The plan
// used to append the link step anyway, so the failure surfaced from
// `QProcess::start("")` as "Could not run ''. Is it installed?" and the
// diagnostic written for exactly this case — the `m_steps.isEmpty()` guard — was
// unreachable (finding MIN-36).
void TstParsers::buildServiceWithoutALinkerSaysSo()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString main = dir.filePath(QStringLiteral("main.s"));
    const QString helper = dir.filePath(QStringLiteral("helper.s"));
    for (const QString &src : {main, helper}) {
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\ttext\n");
    }

    BuildService bs;
    bs.setSourceFile(main);
    bs.setAdditionalSources({helper});
    bs.setOutputFile(dir.filePath(QStringLiteral("out.prg")));
    QVERIFY(bs.usesLinker());
    QVERIFY(bs.linkerPath().isEmpty());

    QSignalSpy done(&bs, &BuildService::finished);
    QStringList messages;
    bool succeeded = true;
    connect(&bs, &BuildService::finished, &bs,
            [&](bool success, const QList<Diagnostic> &diags) {
                succeeded = success;
                for (const Diagnostic &d : diags)
                    messages.append(d.message);
            });
    bs.build();
    if (done.isEmpty())
        QVERIFY(done.wait(5000));

    QCOMPARE(done.count(), 1);
    QVERIFY(!succeeded);
    QVERIFY2(messages.contains(
                 QStringLiteral("More than one source needs a linker, and none is configured.")),
             qPrintable(messages.join(QStringLiteral(" | "))));
    // The empty-program start must not happen at all: its message is what the
    // caller used to see instead.
    QVERIFY2(!messages.join(QLatin1Char(' ')).contains(QStringLiteral("Could not run")),
             qPrintable(messages.join(QStringLiteral(" | "))));
    QVERIFY(bs.objectFiles().isEmpty());
    QVERIFY(bs.listingFiles().isEmpty());
    QVERIFY(!bs.isRunning());
}

// `a.s` and `a.asm` in one directory both derive `a.o`/`a.lst`: the second
// assembly overwrote the first's object and the link received the same path
// twice, so one module was linked twice, the other was missing, and no
// diagnostic said anything (finding MIN-40).
void TstParsers::buildServiceRefusesCollidingObjectPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString first = dir.filePath(QStringLiteral("a.s"));
    const QString second = dir.filePath(QStringLiteral("a.asm"));
    const QString other = dir.filePath(QStringLiteral("b.s"));
    for (const QString &src : {first, second, other}) {
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\ttext\n");
    }

    BuildService bs;
    bs.setSourceFile(first);
    bs.setAdditionalSources({second, other});
    bs.setOutputFile(dir.filePath(QStringLiteral("out.prg")));
    // Steps that cannot fail on their own, so a build that runs them instead of
    // refusing is visible as a success rather than as an unrelated error.
    bs.setAssemblerPath(QStringLiteral("/bin/true"));
    bs.setLinkerPath(QStringLiteral("/bin/true"));

    QSignalSpy done(&bs, &BuildService::finished);
    QStringList messages;
    bool succeeded = true;
    connect(&bs, &BuildService::finished, &bs,
            [&](bool success, const QList<Diagnostic> &diags) {
                succeeded = success;
                for (const Diagnostic &d : diags)
                    messages.append(d.message);
            });
    bs.build();
    if (done.isEmpty())
        QVERIFY(done.wait(5000));

    QCOMPARE(done.count(), 1);
    // Pre-fix this "succeeded": `a.o` was planned twice and the link was handed
    // the same path for both modules.
    QVERIFY2(!succeeded,
             qPrintable(QStringLiteral("planned object files: %1")
                            .arg(bs.objectFiles().join(QLatin1Char(' ')))));
    const QString message = messages.join(QStringLiteral(" | "));
    QVERIFY2(message.contains(QStringLiteral("a.s")) && message.contains(QStringLiteral("a.asm")),
             qPrintable(message));
    QVERIFY2(message.contains(QStringLiteral("a.o")), qPrintable(message));
    // A refused plan leaves nothing behind for the caller to read back.
    QVERIFY(bs.objectFiles().isEmpty());
    QVERIFY(bs.listingFiles().isEmpty());

    // The same two sources through different spellings of one directory are one
    // module, not two: the collision is on the resolved object path.
    BuildService spelled;
    spelled.setSourceFile(first);
    spelled.setAdditionalSources({dir.path() + QStringLiteral("/./a.s")});
    spelled.setOutputFile(dir.filePath(QStringLiteral("out.prg")));
    spelled.setAssemblerPath(QStringLiteral("/bin/true"));
    spelled.setLinkerPath(QStringLiteral("/bin/true"));
    QSignalSpy spelledDone(&spelled, &BuildService::finished);
    QStringList spelledMessages;
    connect(&spelled, &BuildService::finished, &spelled,
            [&](bool, const QList<Diagnostic> &diags) {
                for (const Diagnostic &d : diags)
                    spelledMessages.append(d.message);
            });
    spelled.build();
    if (spelledDone.isEmpty())
        QVERIFY(spelledDone.wait(5000));
    QCOMPARE(spelledDone.count(), 1);
    QVERIFY2(spelledMessages.join(QLatin1Char(' ')).contains(QStringLiteral("a.o")),
             qPrintable(spelledMessages.join(QStringLiteral(" | "))));
}

// vasm appends `Symbols by name` then `Symbols by value`, and the parser used to
// depend on that order: the `section != Body` branch recognised only the
// by-value header, so a listing carrying the tables the other way round kept
// reading the by-name rows with the by-value shape. A by-name row whose label is
// hex-shaped (`DEADBEEF` — a plausible ST label) starts with eight hex digits, so
// the offset beside it was recorded as a symbol and reached console completion
// (finding MIN-39).
void TstParsers::symbolTableReadsTheTablesInEitherOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("value-first.lst"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&f);
    out << "Sections:\n"
           "00: \"text\" (0-2)\n"
           "\n\n"
           "Source: \"sym.s\"\n"
           "                            \t     1: \ttext\n"
           "00:00000000 4E71            \t     2: DEADBEEF:\tnop\n"
           "                            \t     3: \tcount\tequ\t2\n"
           "\n"
           "Symbols by value:\n"
           "00000000 DEADBEEF\n"
           "00000002 count\n"
           "0000000A CLI_DEFINE\n"
           "Symbols by name:\n"
           "CLI_DEFINE                      external EXP\n"
           "DEADBEEF                        00:00000000\n"
           "count                           E:00000002\n";
    out.flush();
    f.close();

    const QVector<SymbolEntry> symbols = symbolsFromListing(path, QString());
    QStringList names;
    for (const SymbolEntry &s : symbols)
        names.append(s.name);

    // The by-name table's rows are read as by-name rows wherever they appear:
    // no column of either table is ever reported as a symbol.
    QVERIFY2(!names.contains(QStringLiteral("00:00000000")),
             qPrintable(names.join(QStringLiteral(" | "))));
    QVERIFY2(!names.contains(QStringLiteral("E:00000002")),
             qPrintable(names.join(QStringLiteral(" | "))));

    // Both tables' names are reported, the hex-shaped one once, at its body
    // definition: its own value-table row reads exactly like the offset column
    // beside a by-name row, so the guard refuses that capture rather than
    // duplicating the symbol.
    QCOMPARE(names.count(QStringLiteral("DEADBEEF")), 1);
    QVERIFY(names.contains(QStringLiteral("count")));
    QVERIFY(names.contains(QStringLiteral("CLI_DEFINE")));
    for (const SymbolEntry &s : symbols) {
        if (s.name == QLatin1String("DEADBEEF")) {
            QCOMPARE(s.file, QStringLiteral("sym.s"));
            QCOMPARE(s.line, 2);
        }
    }
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

void TstParsers::floppyUpdatePreservesDuplicateNames()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item a;
    a.destPath = QStringLiteral("A.TXT");
    a.data = QByteArrayLiteral("first");
    items.append(a);
    floppy::Item b;
    b.destPath = QStringLiteral("B.TXT");
    b.data = QByteArrayLiteral("second");
    items.append(b);
    QString error;
    const QString st = tmp.path() + QStringLiteral("/dup.st");
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));

    // Craft a duplicate 8.3 name: rewrite B.TXT's root-dir entry name to A.TXT,
    // keeping its own cluster and "second" bytes. writeImage's uniqueName11 never
    // produces a duplicate, so edit the raw bytes. 720k root dir begins at sector
    // (1 reserved + 2 FATs * 3) = 7, 32-byte entries.
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(st, &raw, &error), qPrintable(error));
    const auto name11 = [](char c) {
        return QByteArray(1, c) + QByteArray(7, ' ') + QByteArrayLiteral("TXT");
    };
    const int rootStart = (1 + 2 * 3) * 512;
    int pos = -1;
    for (int i = rootStart; i + 32 <= rootStart + 7 * 512; i += 32) {
        if (raw.mid(i, 11) == name11('B')) {
            pos = i;
            break;
        }
    }
    QVERIFY2(pos >= 0, "B.TXT directory entry not found to duplicate");
    raw.replace(pos, 11, name11('A'));
    {
        QFile f(st);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(raw), raw.size());
    }

    // Update: add C.TXT. updateImage re-reads each existing entry; a name-only
    // read gives both A.TXT entries the FIRST's bytes ("first"), silently losing
    // "second". Reading by cluster preserves both (finding B9).
    floppy::Item c;
    c.destPath = QStringLiteral("C.TXT");
    c.data = QByteArrayLiteral("third");
    QVERIFY2(floppy::updateImage(st, {c}, {}, &error), qPrintable(error));

    QByteArray raw2;
    QVERIFY2(floppy::loadRaw(st, &raw2, &error), qPrintable(error));
    const QVector<floppy::Entry> entries = floppy::listImage(st, &error);
    bool sawFirst = false;
    bool sawSecond = false;
    for (const floppy::Entry &e : entries) {
        if (e.isDirectory)
            continue;
        QByteArray data;
        if (!floppy::readFileRaw(raw2, e.path, &data, &error))
            continue;
        if (data == QByteArrayLiteral("first"))
            sawFirst = true;
        else if (data == QByteArrayLiteral("second"))
            sawSecond = true;
    }
    QVERIFY2(sawFirst, "the first A.TXT's bytes must survive");
    QVERIFY2(sawSecond, "the duplicate-named entry must keep its own bytes, not collapse to the first");
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

// `error` is optional across this namespace — every sibling diagnostic goes
// through the null-safe `setError` — and writeAutoFolderImage was the one place
// that dereferenced the pointer unconditionally, on all three of its failure
// paths. A caller with no error sink got a crash instead of false (finding
// MIN-37).
void TstParsers::floppyAutoFolderImageToleratesNoErrorSink()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString prg = tmp.path() + QStringLiteral("/hello.prg");
    {
        QFile f(prg);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(QByteArray(64, 'A')), qint64(64));
    }

    // The program cannot be read, and the image cannot be written (no such
    // directory): both must report failure without an error sink.
    QVERIFY(!floppy::writeAutoFolderImage(tmp.path() + QStringLiteral("/out.st"),
                                          tmp.path() + QStringLiteral("/missing.prg"), nullptr));
    QVERIFY(!floppy::writeAutoFolderImage(tmp.path() + QStringLiteral("/nodir/out.st"), prg,
                                          nullptr));
    QVERIFY(!QFile::exists(tmp.path() + QStringLiteral("/out.st")));

    // The same calls with a sink still report the reason.
    QString error;
    QVERIFY(!floppy::writeAutoFolderImage(tmp.path() + QStringLiteral("/out.st"),
                                          tmp.path() + QStringLiteral("/missing.prg"), &error));
    QVERIFY(error.contains(QLatin1String("missing.prg")));
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

/// Rewrite one entry of the image's first FAT copy — the one the readers
/// follow (FAT 1, sector 1 of the canonical 720 KiB geometry). A FAT12 entry is
/// 12 bits packed two to three bytes with the entry's parity choosing the
/// halves, so the byte pair is read, masked and written back rather than
/// stamped, leaving the neighbouring entry alone.
void patchFat12(QFile &f, int cluster, quint16 value)
{
    const int at = 512 + cluster + cluster / 2;
    QVERIFY(f.seek(at));
    const QByteArray pair = f.read(2);
    QVERIFY(pair.size() == 2);
    const quint16 packed = quint16(quint8(pair.at(0)) | (quint16(quint8(pair.at(1))) << 8));
    const quint16 patched = cluster & 1
        ? quint16((packed & 0x000f) | ((value << 4) & 0xfff0))
        : quint16((packed & 0xf000) | (value & 0x0fff));
    QByteArray out(2, '\0');
    out[0] = char(patched & 0xff);
    out[1] = char((patched >> 8) & 0xff);
    QVERIFY(f.seek(at));
    QVERIFY(f.write(out) == 2);
}

/// The listed entry's own first cluster: `Entry::cluster` is what a reader keys
/// the chain off, not a re-resolved name (finding B9), and damaging a chain
/// means damaging that cluster's FAT link.
quint16 clusterOfEntry(const QVector<floppy::Entry> &entries, const QString &path, bool isDir)
{
    for (const floppy::Entry &e : entries) {
        if (e.path.compare(path, Qt::CaseInsensitive) == 0 && e.isDirectory == isDir)
            return e.cluster;
    }
    return 0;
}

/// A committed image written by foreign tools — `mkfs.fat 4.2` plus mtools,
/// carrying a volume label, an empty file, a subdirectory and a two-cluster
/// file. Stored under Qt's compression framing (`qUncompress`) so 720 KiB of
/// mostly-zero FAT12 costs about a kilobyte in the repository.
QByteArray foreignMkfsImage()
{
    QFile f(QStringLiteral(PIST_SOURCE_DIR "/tests/data/floppy-foreign-mkfs.st.qz"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return qUncompress(f.readAll());
}

/// An 8.3 name field: both columns padded to their fixed width with `pad` —
/// space for the padding a disk normally carries, NUL for the padding a
/// third-party imaging tool writes.
QByteArray name83(const QByteArray &base, const QByteArray &ext = QByteArray(), char pad = ' ')
{
    return base.leftJustified(8, pad) + ext.leftJustified(3, pad);
}

/// Rewrite the 11-byte name field `oldName` into `newName` wherever the image
/// holds it as a directory entry. Directories start on sector boundaries and
/// their slots are 32 bytes apart, so a match at a 32-byte-aligned offset is an
/// entry. `writeImage` alone can never produce the crafted fields these tests
/// need — it upper-cases, and it pads with spaces — so they are patched in.
void renameEntry(QFile &f, const QByteArray &oldName, const QByteArray &newName)
{
    QCOMPARE(oldName.size(), 11);
    QCOMPARE(newName.size(), 11);
    QVERIFY(f.seek(0));
    const QByteArray raw = f.readAll();
    const int at = raw.indexOf(oldName);
    QVERIFY2(at >= 0 && at % 32 == 0, qPrintable(QString::fromLatin1(oldName)));
    QVERIFY(f.seek(at));
    QCOMPARE(f.write(newName), qint64(11));
}
} // namespace

// A written subdirectory's `..` entry must hold the start cluster of the
// directory that *contains* it; 0 is correct only for a child of the root, which
// owns no cluster. Every subdirectory used to claim the root as its parent, so a
// depth-2 tree exported from the project files (SUB/INNER) sent TOS's "open
// parent" straight to the root. PiST's own reader never noticed — it skips `.`
// and `..` and resolves top-down — which is why this reads the raw entries
// (finding MAJ-39).
void TstParsers::floppyNestedDirectoriesPointAtTheirParent()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item sub;
    sub.destPath = QStringLiteral("SUB");
    sub.isDirectory = true;
    items.append(sub);
    floppy::Item inner;
    inner.destPath = QStringLiteral("SUB/INNER");
    inner.isDirectory = true;
    items.append(inner);
    floppy::Item file;
    file.destPath = QStringLiteral("SUB/INNER/DEEP.TXT");
    file.data = QByteArrayLiteral("deep");
    items.append(file);

    const QString st = tmp.path() + QStringLiteral("/nested.st");
    QString error;
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(st, &raw, &error), qPrintable(error));
    const QVector<floppy::Entry> entries = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const quint16 subCluster = clusterOfEntry(entries, QStringLiteral("SUB"), true);
    const quint16 innerCluster = clusterOfEntry(entries, QStringLiteral("SUB/INNER"), true);
    QVERIFY(subCluster >= 2);
    QVERIFY(innerCluster >= 2);
    QVERIFY(subCluster != innerCluster);

    // Canonical 720 KiB geometry: the first data cluster (2) starts at sector
    // 14 and holds two sectors, so a directory's own bytes start here. Slot 0 is
    // ".", slot 1 "..", and both record a start cluster at offset 26.
    const auto dirOffset = [](quint16 cluster) {
        return (14 + (int(cluster) - 2) * 2) * 512;
    };
    const auto startClusterOf = [&raw, &dirOffset](quint16 cluster, int slot) -> quint16 {
        const int at = dirOffset(cluster) + slot * 32 + 26;
        return quint16(quint8(raw.at(at)) | (quint8(raw.at(at + 1)) << 8));
    };

    QCOMPARE(raw.mid(dirOffset(subCluster) + 32, 11), QByteArrayLiteral("..         "));
    QCOMPARE(raw.mid(dirOffset(innerCluster) + 32, 11), QByteArrayLiteral("..         "));

    // "." names the directory itself...
    QCOMPARE(startClusterOf(subCluster, 0), subCluster);
    QCOMPARE(startClusterOf(innerCluster, 0), innerCluster);
    // ...and ".." names the directory that contains it: INNER lives inside SUB...
    QCOMPARE(startClusterOf(innerCluster, 1), subCluster);
    // ...while SUB's `..` is 0, the root directory having no cluster to name.
    QCOMPARE(startClusterOf(subCluster, 1), quint16(0));

    // The tree itself still reads: the `..` fix must not disturb the entries.
    QByteArray deep;
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("SUB/INNER/DEEP.TXT"), &deep, &error),
             qPrintable(error));
    QCOMPARE(deep, QByteArrayLiteral("deep"));
}

// A disk's 8.3 name fields are not ours to rewrite. `updateImage` re-derived
// every carried entry's name from its listed path, so copying one file onto a
// disk rewrote the names beside it: lower-case names came back upper-cased,
// Latin-1 accents were mangled (`0xE9` → `0xC9`) and a field a third-party tool
// padded with NULs became literal underscores. The fields must come back byte
// for byte (finding MIN-38).
void TstParsers::floppyUpdateKeepsForeignNamesByteIdentical()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item dir;
    dir.destPath = QStringLiteral("SUB");
    dir.isDirectory = true;
    items.append(dir);
    floppy::Item nested;
    nested.destPath = QStringLiteral("SUB/KEEP.TXT");
    nested.data = QByteArrayLiteral("kept");
    items.append(nested);
    for (const char *name : {"LOWER.TXT", "CAFE.TXT", "PADDED.TXT"}) {
        floppy::Item file;
        file.destPath = QLatin1String(name);
        file.data = QByteArrayLiteral("data");
        items.append(file);
    }

    const QString st = tmp.path() + QStringLiteral("/foreign-names.st");
    QString error;
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));

    // What a real disk may hold and our own writer cannot produce: lower case,
    // a Latin-1 accent, and NUL padding.
    {
        QFile f(st);
        QVERIFY(f.open(QIODevice::ReadWrite));
        renameEntry(f, name83("LOWER", "TXT"), name83("lower", "txt"));
        renameEntry(f, name83("CAFE", "TXT"), name83("caf\xE9", "txt"));
        renameEntry(f, name83("PADDED", "TXT"), name83("PADDED", "TXT", '\0'));
        renameEntry(f, name83("SUB"), name83("sub"));
        renameEntry(f, name83("KEEP", "TXT"), name83("keep", "txt"));
    }

    const QVector<floppy::Entry> before = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY2(entryNamed(before, QStringLiteral("lower.txt"), false),
             "a lower-case field must list as it reads");
    // The cafe expectation is spelled QString::fromLatin1, not
    // QStringLiteral: QStringLiteral pastes its narrow text after `u""`
    // (qstring.h's QT_UNICODE_LITERAL), which hands the UTF-16 meaning of
    // escaped byte 0xE9 to the compiler's mixed-literal interpretation —
    // MSVC's reading of it is not U+00E9, which is exactly why this line
    // failed on the Windows leg while GCC and clang passed. fromLatin1 maps
    // byte 0xE9 to U+00E9 on every compiler, the same decode the listing
    // itself applies to the field (FloppyImage's latin1Field/display83 go
    // through QString::fromLatin1). The name83 expectations need no such
    // care: those are raw bytes, identical on every compiler.
    QVERIFY2(entryNamed(before, QString::fromLatin1("caf\xE9.txt"), false),
             "a Latin-1 field must list as one accented character");
    QVERIFY2(entryNamed(before, QStringLiteral("PADDED.TXT"), false),
             "NUL padding must be padding, not name characters");
    QVERIFY(entryNamed(before, QStringLiteral("sub/keep.txt"), false));

    // One unrelated file onto the disk, nothing removed.
    floppy::Item added;
    added.destPath = QStringLiteral("NEW.BIN");
    added.data = QByteArrayLiteral("new");
    QVERIFY2(floppy::updateImage(st, {added}, {}, &error), qPrintable(error));

    const QVector<floppy::Entry> after = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const auto name11Of = [&after](const QString &path) {
        for (const floppy::Entry &e : after) {
            if (e.path == path)
                return e.name11;
        }
        return QByteArray();
    };
    QCOMPARE(name11Of(QStringLiteral("LOWER.TXT")), QByteArray());   // never re-emitted upper-cased
    QCOMPARE(name11Of(QStringLiteral("lower.txt")), name83("lower", "txt"));
    QCOMPARE(name11Of(QString::fromLatin1("caf\xE9.txt")), name83("caf\xE9", "txt"));
    QCOMPARE(name11Of(QStringLiteral("PADDED.TXT")), name83("PADDED", "TXT", '\0'));
    QCOMPARE(name11Of(QStringLiteral("sub")), name83("sub"));
    QCOMPARE(name11Of(QStringLiteral("sub/keep.txt")), name83("keep", "txt"));
    // The name the operation added is the one it derives from the host name.
    QCOMPARE(name11Of(QStringLiteral("NEW.BIN")), name83("NEW", "BIN"));
}

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

// A cluster chain that cannot deliver what its directory entry declares — an
// early end-of-chain, a loop back onto its own cluster, a link past the end of
// the image — used to read as a short buffer reported as success, so copying
// the file out silently truncated it. Each shape must be refused with an error
// instead (finding CRIT-3).
void TstParsers::floppyReadRefusesADamagedChain()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("PIECE.BIN");
    // 1500 bytes: two 1 KiB clusters, so the chain has a real link to damage.
    file.data = QByteArray(1500, '\x5a');
    items.append(file);

    QString error;
    const QString base = tmp.path() + QStringLiteral("/intact.st");
    QVERIFY2(floppy::writeImage(base, items, &error), qPrintable(error));
    const quint16 start =
        clusterOfEntry(floppy::listImage(base, &error), QStringLiteral("PIECE.BIN"), false);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(start >= 2);

    struct Damage { const char *name; quint16 fatValue; };
    const Damage damages[] = {
        {"truncated", 0xfff},  // end-of-chain after the first of two clusters
        {"cyclic", start},     // the link loops back onto its own cluster
        {"outside", 0xfe0},    // a cluster far past the end of the image
    };
    for (const Damage &d : damages) {
        const QString path = tmp.path() + QStringLiteral("/%1.st").arg(QLatin1String(d.name));
        QVERIFY2(QFile::copy(base, path), qPrintable(path));
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::ReadWrite));
            patchFat12(f, start, d.fatValue);
        }
        QByteArray raw;
        QVERIFY2(floppy::loadRaw(path, &raw, &error), qPrintable(error));
        QByteArray data;
        error.clear();
        QVERIFY2(!floppy::readFileRaw(raw, QStringLiteral("PIECE.BIN"), &data, &error),
                 qPrintable(QStringLiteral("a %1 chain was reported as a successful read")
                                .arg(QLatin1String(d.name))));
        QVERIFY2(!error.isEmpty(), "the refusal must say what is damaged");
    }

    // The intact image still reads back whole: the guard refuses damage, not
    // chained files.
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(base, &raw, &error), qPrintable(error));
    QByteArray data;
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("PIECE.BIN"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArray(1500, '\x5a'));
}

// updateImage reads every entry back and renames the rebuilt image over the
// user's original, so a source whose chains cannot deliver what its entries
// declare must be refused before anything is staged. Carrying a truncation (or
// a folder the walk could not expand) into the replacement image would destroy
// exactly the clusters a repair needs, which is the hazard the MSA reader
// already fails closed on (finding CRIT-3).
void TstParsers::floppyUpdateRefusesADamagedImage()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("KEEP.BIN");
    file.data = QByteArray(1500, '\x33');
    items.append(file);
    floppy::Item folder;
    folder.destPath = QStringLiteral("DATA");
    folder.isDirectory = true;
    items.append(folder);
    floppy::Item inner;
    inner.destPath = QStringLiteral("DATA/INNER.TXT");
    inner.data = QByteArrayLiteral("inner");
    items.append(inner);

    QString error;
    const QString image = tmp.path() + QStringLiteral("/work.st");
    QVERIFY2(floppy::writeImage(image, items, &error), qPrintable(error));
    const QVector<floppy::Entry> listed = floppy::listImage(image, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const quint16 fileCluster = clusterOfEntry(listed, QStringLiteral("KEEP.BIN"), false);
    const quint16 dirCluster = clusterOfEntry(listed, QStringLiteral("DATA"), true);
    QVERIFY(fileCluster >= 2 && dirCluster >= 2);

    const auto imageBytes = [](const QString &path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    };
    floppy::Item added;
    added.destPath = QStringLiteral("NEW.TXT");
    added.data = QByteArrayLiteral("new");

    // The file's chain ends after its first cluster: the second cluster is
    // still on the disk, but the entry still declares 1500 bytes.
    {
        QFile f(image);
        QVERIFY(f.open(QIODevice::ReadWrite));
        patchFat12(f, fileCluster, 0xfff);
    }
    const QByteArray beforeFile = imageBytes(image);
    error.clear();
    QVERIFY2(!floppy::updateImage(image, {added}, {}, &error),
             "an image whose file chain cannot yield the declared size must not be rewritten");
    QVERIFY2(!error.isEmpty(), "the refusal must say what is damaged");
    QCOMPARE(imageBytes(image), beforeFile);

    // The same hazard one level up: the listing tolerates a folder with a
    // damaged chain by leaving it unexpanded, so a rewrite that carried the
    // listing at face value would drop the folder's whole subtree.
    QVERIFY2(floppy::writeImage(image, items, &error), qPrintable(error));
    error.clear();
    const quint16 damagedDir =
        clusterOfEntry(floppy::listImage(image, &error), QStringLiteral("DATA"), true);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(damagedDir >= 2);
    {
        QFile f(image);
        QVERIFY(f.open(QIODevice::ReadWrite));
        patchFat12(f, damagedDir, 0xfe0);
    }
    const QByteArray beforeFolder = imageBytes(image);
    error.clear();
    QVERIFY2(!floppy::updateImage(image, {added}, {}, &error),
             "an image whose folder chain leaves the image must not be rewritten");
    QVERIFY2(!error.isEmpty(), "the refusal must say what is damaged");
    QCOMPARE(imageBytes(image), beforeFolder);

    // A refusal must not leave a staged or backup image beside the original.
    QCOMPARE(QDir(tmp.path()).entryList({QStringLiteral(".pist-*")}, QDir::Hidden | QDir::Files)
                 .size(),
             0);
}

void TstParsers::floppyCanonicalLayoutIsRecognised()
{
    // updateImage rebuilds whatever it writes with PiST's canonical layout, so
    // the caller asks first when the image is not already that shape. The
    // predicate has to tell PiST's own images from a disk that would lose
    // something.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("A.TXT");
    file.data = QByteArrayLiteral("x");
    items.append(file);
    const QString st = tmp.path() + QStringLiteral("/own.st");
    QString error;
    QVERIFY2(floppy::writeImage(st, items, &error), qPrintable(error));
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(st, &raw, &error), qPrintable(error));
    QVERIFY(floppy::looksLikeCanonical720k(raw.left(512), raw.size()));

    // A foreign boot sector at the same size: rewriting it replaces that boot
    // code, so it must not pass as canonical.
    QByteArray foreign = raw;
    foreign.replace(3, 8, QByteArrayLiteral("GAMEDSK "));
    QVERIFY(!floppy::looksLikeCanonical720k(foreign.left(512), foreign.size()));

    // Neither may a larger image, whose upper half the rewrite would drop.
    QByteArray big(1474560, '\0');
    big.replace(0, 512, raw.left(512));
    QVERIFY(!floppy::looksLikeCanonical720k(big.left(512), big.size()));
}

// Every other floppy test reads back something our own writer produced, so a
// mismatch between what we emit and what the formats actually specify stays
// invisible. These two fixtures came from outside: mkfs.fat + mtools for the
// .st, and a packer written against the documented MSA layout (quoted in
// Hatari's src/floppies/msa.c) for the .msa.
void TstParsers::floppyReadsAForeignMkfsImage()
{
    const QByteArray image = foreignMkfsImage();
    QCOMPARE(image.size(), 720 * 1024);

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString st = tmp.path() + QStringLiteral("/foreign.st");
    {
        QFile f(st);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(image), qint64(image.size()));
    }

    QString error;
    const QVector<floppy::Entry> entries = floppy::listImage(st, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    // The volume label is metadata rather than an entry, the empty file owns no
    // cluster at all, and AUTO/PROG.PRG spans two clusters with a real link.
    QVERIFY(entryNamed(entries, QStringLiteral("HELLO.TXT"), false));
    QVERIFY(entryNamed(entries, QStringLiteral("EMUDESK.INF"), false));
    QVERIFY(entryNamed(entries, QStringLiteral("AUTO"), true));
    QVERIFY(entryNamed(entries, QStringLiteral("AUTO/PROG.PRG"), false));
    QVERIFY(!entryNamed(entries, QStringLiteral("PISTTEST"), false));
    QCOMPARE(entries.size(), 4);

    QByteArray data;
    QVERIFY2(floppy::readFileRaw(image, QStringLiteral("HELLO.TXT"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArrayLiteral("hello from mkfs\n"));
    QVERIFY2(floppy::readFileRaw(image, QStringLiteral("AUTO/PROG.PRG"), &data, &error),
             qPrintable(error));
    QCOMPARE(data, QByteArray(1500, 'A'));
    QVERIFY2(floppy::readFileRaw(image, QStringLiteral("EMUDESK.INF"), &data, &error),
             qPrintable(error));
    QVERIFY2(data.isEmpty(), "an empty file must read back empty");
}

void TstParsers::floppyReadsAForeignSpecMsa()
{
    QFile src(QStringLiteral(PIST_SOURCE_DIR "/tests/data/floppy-foreign-spec.msa"));
    QVERIFY(src.open(QIODevice::ReadOnly));
    const QByteArray packed = src.readAll();

    // loadRaw dispatches on the suffix, so the fixture needs a real .msa name.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString msa = tmp.path() + QStringLiteral("/foreign.msa");
    {
        QFile out(msa);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(packed), qint64(packed.size()));
    }

    QString error;
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(msa, &raw, &error), qPrintable(error));
    // Decoding an independently packed archive must reproduce the mkfs.vfat
    // image byte for byte. Our own round-trip test cannot make that claim: its
    // encoder and decoder are the same code, so a shared wrong convention —
    // transposed run fields, say — passes it and interoperates with nothing.
    QCOMPARE(raw, foreignMkfsImage());

    // The fixture must exercise both track forms the format allows: a length
    // equal to the track size means the bytes are stored verbatim (spec-legal,
    // and what an uncompressed MSA is made of entirely), anything shorter is an
    // RLE stream. One of each, or the raw branch goes untested.
    const int trackSize = 9 * 512;
    int rawTracks = 0, rleTracks = 0;
    for (int at = 10; at + 2 <= packed.size(); at += 2) {
        const int length = (quint16(quint8(packed.at(at))) << 8) | quint8(packed.at(at + 1));
        (length == trackSize ? rawTracks : rleTracks)++;
        at += length;   // the loop's +2 steps over the next header
    }
    QVERIFY2(rawTracks >= 1, "the fixture stores no uncompressed track");
    QVERIFY2(rleTracks >= 1, "the fixture stores no compressed track");

    const QVector<floppy::Entry> entries = floppy::listImage(msa, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(entries.size(), 4);
}

void TstParsers::floppyRejectsATruncatedMsaRun()
{
    // Two different truncations, caught by two different guards: a track whose
    // declared length runs past the file, and a run header that stops inside
    // its own four bytes (marker, value, 16-bit length). Both must be refused
    // with an error rather than decoded into a partly-formed track, because
    // loadRaw success is what gates updateImage rewriting the user's image.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.path() + QStringLiteral("/truncated.msa");

    const auto refuses = [&path](const QByteArray &msa) -> bool {
        {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            f.write(msa);
        }
        QByteArray raw;
        QString error;
        return !floppy::loadRaw(path, &raw, &error) && !error.isEmpty();
    };

    QByteArray header;
    header.append(char(0x0E)); header.append(char(0x0F));  // signature
    header.append(char(0x00)); header.append(char(9));    // sectors per track
    header.append(char(0x00)); header.append(char(1));    // last side
    header.append(char(0x00)); header.append(char(0));    // first track
    header.append(char(0x00)); header.append(char(79));   // last track

    // A run header that stops inside itself: the declared track length is
    // honest, so the outer per-track bound passes and the decoder itself has
    // to refuse it.
    QByteArray midRun = header;
    midRun.append(char(0x00)); midRun.append(char(4));      // four payload bytes
    midRun.append(char(0x01)); midRun.append(char(0x02));   // two literals
    midRun.append(char(0xE5)); midRun.append(char(0xAA));   // header cut short
    QVERIFY2(refuses(midRun), "a run truncated inside its header was accepted");

    // One length byte short of a full header. Pinned for the behaviour rather
    // than as a guard test: checked by mutation, the pre-widening bound
    // refuses this too, reading the terminating NUL as a zero length and
    // failing the count check on the way instead.
    QByteArray oneShort = header;
    oneShort.append(char(0x00)); oneShort.append(char(5));
    oneShort.append(char(0x01)); oneShort.append(char(0x02));
    oneShort.append(char(0xE5)); oneShort.append(char(0xAA)); oneShort.append(char(0x00));
    QVERIFY2(refuses(oneShort), "a run header one byte short was accepted");

    // A track that promises more bytes than the file holds.
    QByteArray overrun = header;
    overrun.append(char(0x00)); overrun.append(char(9));
    overrun.append(char(0x01)); overrun.append(char(0x02));
    QVERIFY2(refuses(overrun), "a track length past the end of the file was accepted");
}

void TstParsers::msaEncodingMatchesTheDocumentedFormat()
{
    // The format our exporter writes is what Hatari and every ST archiver
    // reads, so the bytes on disk are the contract: a run is
    // $E5 <value> <16-bit big-endian length> (six $AA bytes are $E5 $AA $00
    // $06), and a literal $E5 is escaped as a run rather than emitted bare.
    QByteArray raw(720 * 1024, '\0');
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString msa = tmp.path() + QStringLiteral("/out.msa");

    const auto firstRun = [&]() -> QByteArray {
        QString error;
        if (!floppy::saveRaw(msa, raw, &error))
            return error.toUtf8();
        QFile f(msa);
        if (!f.open(QIODevice::ReadOnly))
            return QByteArrayLiteral("<unreadable>");
        // 10-byte header, then a 2-byte track length for track 0 side 0.
        return f.readAll().mid(12, 4).toHex();
    };

    for (int i = 0; i < 6; ++i)
        raw[i] = char(0xAA);
    QCOMPARE(firstRun(), QByteArrayLiteral("e5aa0006"));

    raw[0] = char(0xE5);
    raw[1] = char(0xE5);
    for (int i = 2; i < 6; ++i)
        raw[i] = '\0';
    QCOMPARE(firstRun(), QByteArrayLiteral("e5e50002"));

    // A track that cannot be compressed is stored verbatim rather than as a
    // stream that merely grows: alternating byte pairs contain no run of four
    // and no $E5, so their "RLE" would be exactly as long as the track.
    for (int i = 0; i < 9 * 512; ++i)
        raw[i] = char(i % 2 ? 0x22 : 0x11);
    const auto firstTrackLength = [&]() -> int {
        QString error;
        if (!floppy::saveRaw(msa, raw, &error))
            return -1;
        QFile f(msa);
        if (!f.open(QIODevice::ReadOnly))
            return -1;
        const QByteArray bytes = f.readAll();
        return (int(quint8(bytes.at(10))) << 8) | quint8(bytes.at(11));
    };
    QCOMPARE(firstTrackLength(), 9 * 512);
}
QTEST_MAIN(TstParsers)
#include "tst_parsers.moc"
