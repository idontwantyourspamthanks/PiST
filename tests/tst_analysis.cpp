// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// TOS executables and the 68000 listing a walk from the entry point produces.
// The byte fixtures are fixed encodings, so this suite does not need vasm, a
// display, or an emulator.

#include "analysis/M68kDisasm.h"
#include "analysis/ProgramTrace.h"
#include "analysis/TosProgram.h"

#include <QFile>
#include <QTest>

using namespace pist;

namespace {

QByteArray be16(quint16 value)
{
    const char bytes[2] = {char(value >> 8), char(value)};
    return QByteArray(bytes, 2);
}

QByteArray be32(quint32 value)
{
    const char bytes[4] = {char(value >> 24), char(value >> 16), char(value >> 8), char(value)};
    return QByteArray(bytes, 4);
}

QByteArray tos(const QByteArray &text, const QByteArray &data = {}, quint32 bss = 0,
               const QByteArray &symbols = {}, bool relocatable = true,
               const QByteArray &relocations = {})
{
    QByteArray bytes;
    bytes += be16(0x601A);
    bytes += be32(quint32(text.size()));
    bytes += be32(quint32(data.size()));
    bytes += be32(bss);
    bytes += be32(quint32(symbols.size()));
    bytes += be32(0);
    bytes += be32(0);
    bytes += be16(relocatable ? 0 : 1);
    bytes += text;
    bytes += data;
    bytes += symbols;
    if (relocatable)
        bytes += relocations.isEmpty() ? be32(0) : relocations;
    return bytes;
}

QString rowText(const ProgramListing &listing, quint32 address)
{
    for (const ListingRow &row : listing.rows) {
        if (row.address == address)
            return row.text;
    }
    return {};
}

const TosSymbol *symbolNamed(const TosProgram &program, const QString &name)
{
    for (const TosSymbol &symbol : program.symbols) {
        if (symbol.name == name)
            return &symbol;
    }
    return nullptr;
}

} // namespace

class tst_analysis : public QObject
{
    Q_OBJECT

private slots:
    void rejectsAFileThatIsNotTos();
    void rejectsOddSectionSizes();
    void rejectsATruncatedImage();
    void relocationSkipOf254();
    void helloSymbolsAndRelocations();
    void decodesKnownEncodings_data();
    void decodesKnownEncodings();
    void namesARelocatedImmediate();
    void helloListingStopsAtPterm();
    void indirectJumpDoesNotEnterFollowingBytes();
    void ptermEndsTheWalk();
    void aReturningTrapDoesNotEndTheWalk();
    void aBranchSkipsBytesThatLookLikeCode();
    void dataSegmentStaysData();
    void lineAFallsThrough();
    void fullExtensionStopsTheWalk();
};

void tst_analysis::rejectsAFileThatIsNotTos()
{
    TosProgram program;
    QString error;
    QVERIFY(!parseTosProgram(QByteArray("not a program"), &program, &error));
    QCOMPARE(error, QStringLiteral("not a TOS executable"));
}

void tst_analysis::rejectsOddSectionSizes()
{
    TosProgram program;
    QString error;
    QVERIFY(!parseTosProgram(tos(QByteArray(1, '\0')), &program, &error));
    QCOMPARE(error, QStringLiteral("text or data size is odd"));
}

void tst_analysis::rejectsATruncatedImage()
{
    TosProgram program;
    QString error;
    QByteArray bytes = be16(0x601A);
    bytes += be32(4);
    bytes += be32(0);
    bytes += be32(0);
    bytes += be32(0);
    bytes += be32(0);
    bytes += be32(0);
    bytes += be16(0);
    QVERIFY(!parseTosProgram(bytes, &program, &error));
    QCOMPARE(error, QStringLiteral("truncated TOS executable"));

    error.clear();
    // An empty relocation argument still writes the terminating long, so this
    // one is valid. A relocatable image with no table at all is not.
    QVERIFY(parseTosProgram(tos(QByteArray("\x4E\x75", 2)), &program, &error));
    QVERIFY(error.isEmpty());
    QByteArray cut = tos(QByteArray("\x4E\x75", 2));
    cut.chop(4);
    QVERIFY(!parseTosProgram(cut, &program, &error));
    QCOMPARE(error, QStringLiteral("relocation table is truncated"));
}

void tst_analysis::relocationSkipOf254()
{
    QByteArray text(300, '\0');
    QByteArray relocs = be32(2);
    relocs.append(char(1));
    relocs.append(char(2));
    relocs.append(char(0));
    TosProgram program;
    QString error;
    QVERIFY(parseTosProgram(tos(text, {}, 0, {}, true, relocs), &program, &error));
    QCOMPARE(program.relocations, QVector<quint32>({2, 258}));
}

void tst_analysis::helloSymbolsAndRelocations()
{
    QFile file(QStringLiteral(PIST_SOURCE_DIR "/demo/hello.prg"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    TosProgram program;
    QString error;
    QVERIFY2(parseTosProgram(file.readAll(), &program, &error), qPrintable(error));
    QCOMPARE(program.textSize, 192u);
    QCOMPARE(program.dataSize, 4u);
    QCOMPARE(program.bssSize, 0u);
    QCOMPARE(program.symbols.size(), 7);
    QCOMPARE(program.relocations, QVector<quint32>({2, 36, 76}));

    const auto expect = [&](const char *name, TosSymbol::Kind kind, quint32 value) {
        const TosSymbol *symbol = symbolNamed(program, QString::fromLatin1(name));
        QVERIFY2(symbol, name);
        QCOMPARE(symbol->kind, kind);
        QCOMPARE(symbol->value, value);
    };
    expect("_start", TosSymbol::Kind::Text, 0);
    expect("count", TosSymbol::Kind::Text, 0x20);
    expect("banner", TosSymbol::Kind::Text, 0x74);
    expect("done", TosSymbol::Kind::Text, 0xA0);
    expect("rounds", TosSymbol::Kind::Equate, 3);
    expect("counter", TosSymbol::Kind::Data, 0xC0);
    expect("hook_calls", TosSymbol::Kind::Data, 0xC2);
}

void tst_analysis::decodesKnownEncodings_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<quint32>("address");
    QTest::addColumn<QString>("text");

    const auto add = [](const char *name, const char *hex, quint32 address, const char *text) {
        QTest::newRow(name) << QByteArray::fromHex(hex) << address << QString::fromLatin1(text);
    };
    add("bra.s self", "60FE", 0, "bra.s $0");
    add("bra.w self", "6000FFFE", 0, "bra.w $0");
    add("movem to stack", "48E7FFFE", 0, "movem.l d0-d7/a0-a6,-(sp)");
    add("movem from stack", "4CDF7FFF", 0, "movem.l (sp)+,d0-d7/a0-a6");
    add("add.l", "D280", 0, "add.l d0,d1");
    add("addx.l", "D380", 0, "addx.l d0,d1");
    add("clr.w predec", "4267", 0, "clr.w -(sp)");
    add("jsr pc", "4EBAFFEA", 0x18, "jsr $4(pc)");
    add("lea pc", "41FAFFFE", 0, "lea $0(pc),a0");
    add("move.l immediate", "2F3C00000074", 0, "move.l #$74,-(sp)");
    add("rts", "4E75", 0, "rts");
    add("addq.l", "5C8F", 0, "addq.l #6,sp");
    add("moveq", "70FF", 0, "moveq #-1,d0");
    add("muls", "C3C0", 0, "muls.w d0,d1");
    add("divu", "80FC000A", 0, "divu.w #$a,d0");
    add("asl", "E540", 0, "asl.w #2,d0");
    add("lsr", "E22A", 0, "lsr.b d1,d2");
    add("rol", "E19B", 0, "rol.l #8,d3");
    add("asr memory", "E0D1", 0, "asr.w (a1)");
    add("btst immediate", "08000003", 0, "btst #3,d0");
    add("btst register", "0300", 0, "btst d1,d0");
    add("abcd", "C300", 0, "abcd d0,d1");
    add("exg data", "C141", 0, "exg d0,d1");
    add("exg address", "C149", 0, "exg a0,a1");
    add("exg mixed", "C188", 0, "exg d0,a0");
    add("movep to mem", "01890000", 0, "movep.w d0,0(a1)");
    add("movep to reg", "01490000", 0, "movep.l 0(a1),d0");
    add("cmpm", "B308", 0, "cmpm.b (a0)+,(a1)+");
    add("and.l", "C280", 0, "and.l d0,d1");
    add("and immediate", "C0BC000000FF", 0, "and.l #$ff,d0");
    add("or immediate", "803C0001", 0, "or.b #$1,d0");
    add("eor", "B350", 0, "eor.w d1,(a0)");
    add("sub.l", "9280", 0, "sub.l d0,d1");
    add("suba", "93C8", 0, "suba.l a0,a1");
    add("cmp.w", "B07C0003", 0, "cmp.w #$3,d0");
    add("cmpa", "B2C8", 0, "cmpa.w a0,a1");
    add("neg", "4480", 0, "neg.l d0");
    add("ext", "48C0", 0, "ext.l d0");
    add("swap", "4840", 0, "swap d0");
    add("pea", "4850", 0, "pea (a0)");
    add("link", "4E56FFF8", 0, "link a6,#-8");
    add("nop", "4E71", 0, "nop");
    add("illegal", "4AFC", 0, "illegal");
    add("line-a", "A000", 0, "dc.w $a000");
    add("scc", "54C0", 0, "scc d0");
    add("st", "50C1", 0, "st d1");
    add("dbf", "51C8FFFE", 0, "dbf d0,$0");
}

void tst_analysis::decodesKnownEncodings()
{
    QFETCH(QByteArray, bytes);
    QFETCH(quint32, address);
    QFETCH(QString, text);
    QByteArray image(int(address), '\0');
    image += bytes;
    const DecodedInsn insn = decode68k(image, address, quint32(image.size()));
    QCOMPARE(insn.text, text);
    QCOMPARE(insn.length, bytes.size());
}

void tst_analysis::namesARelocatedImmediate()
{
    const QByteArray image = QByteArray::fromHex("2F3C00000074");
    DisasmNames names;
    names.labels.insert(0x74, QStringLiteral("banner"));
    names.relocations.insert(2);
    const DecodedInsn insn = decode68k(image, 0, quint32(image.size()), names);
    QCOMPARE(insn.text, QStringLiteral("move.l #banner,-(sp)"));
}

void tst_analysis::helloListingStopsAtPterm()
{
    QFile file(QStringLiteral(PIST_SOURCE_DIR "/demo/hello.prg"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const ProgramListing listing = listProgram(file.readAll());
    QVERIFY2(listing.error.isEmpty(), qPrintable(listing.error));
    QCOMPARE(listing.summary,
             QStringLiteral("192 bytes of code, 4 bytes of data, 0 bytes of bss, 7 symbols, 3 relocations"));
    QCOMPARE(listing.rows.first().label, QStringLiteral("_start"));
    QCOMPARE(listing.rows.first().text, QStringLiteral("move.l #banner,-(sp)"));
    QVERIFY(listing.rows.first().note.contains(QStringLiteral("reloc")));

    bool sawTrap = false;
    bool sawRounds = false;
    bool sawBannerData = false;
    bool sawCounter = false;
    for (const ListingRow &row : listing.rows) {
        if (row.text == QStringLiteral("trap #1"))
            sawTrap = true;
        if (row.text.contains(QStringLiteral("rounds")))
            sawRounds = true;
        if (row.label == QStringLiteral("banner")) {
            QVERIFY(row.text.startsWith(QStringLiteral("dc.b ")));
            sawBannerData = true;
        }
        if (row.label == QStringLiteral("counter")) {
            QVERIFY(row.address >= 192);
            QVERIFY(row.text.startsWith(QStringLiteral("dc.b ")));
            sawCounter = true;
        }
        if (row.address >= 192)
            QVERIFY2(row.text.startsWith(QStringLiteral("dc.")), qPrintable(row.text));
    }
    QVERIFY(sawTrap);
    QVERIFY(sawRounds);
    QVERIFY(sawBannerData);
    QVERIFY(sawCounter);
}

void tst_analysis::indirectJumpDoesNotEnterFollowingBytes()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("4ED04E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("jmp (a0)"));
    QCOMPARE(listing.rows.first().note, QStringLiteral("unresolved"));
    QCOMPARE(rowText(listing, 2), QStringLiteral("dc.b $4e,$71"));
}

void tst_analysis::ptermEndsTheWalk()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("42674E414E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("clr.w -(sp)"));
    QCOMPARE(rowText(listing, 2), QStringLiteral("trap #1"));
    QCOMPARE(rowText(listing, 4), QStringLiteral("dc.b $4e,$71"));
}

void tst_analysis::aReturningTrapDoesNotEndTheWalk()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("4E414E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("trap #1"));
    QCOMPARE(rowText(listing, 2), QStringLiteral("nop"));
}

void tst_analysis::aBranchSkipsBytesThatLookLikeCode()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("60024E714E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("bra.s $4"));
    QCOMPARE(rowText(listing, 2), QStringLiteral("dc.b $4e,$71"));
    QCOMPARE(rowText(listing, 4), QStringLiteral("nop"));
}

void tst_analysis::dataSegmentStaysData()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("4E75"), QByteArray::fromHex("4E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("rts"));
    QCOMPARE(rowText(listing, 2), QStringLiteral("dc.b $4e,$71"));
}

void tst_analysis::lineAFallsThrough()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("A0004E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("dc.w $a000"));
    QCOMPARE(rowText(listing, 2), QStringLiteral("nop"));
}

void tst_analysis::fullExtensionStopsTheWalk()
{
    const ProgramListing listing = listProgram(tos(QByteArray::fromHex("41F001004E71")));
    QCOMPARE(rowText(listing, 0), QStringLiteral("dc.w $41f0,$0100"));
    QCOMPARE(rowText(listing, 4), QStringLiteral("dc.b $4e,$71"));
}

QTEST_MAIN(tst_analysis)
#include "tst_analysis.moc"
