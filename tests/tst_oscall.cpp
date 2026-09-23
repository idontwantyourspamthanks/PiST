// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include <QtTest>

#include "editor/AsmLex.h"
#include "editor/IncludeNav.h"
#include "editor/OsCallBinding.h"
#include "editor/OsCallRef.h"
#include "editor/OsCallScan.h"

using namespace pist;

// The OS-call reference table (OsCallRef) and the source-line scanner
// (OsCallScan) that resolves a `trap #1/#13/#14` sequence to the call it
// makes. Facts are verified against tos.hyp and Atari's 1986 GEMDOS manual;
// the tests pin the table's shape and the scanner's idiom coverage, not the
// prose.
class TstOsCall : public QObject
{
    Q_OBJECT

private slots:
    void tableCoversThreeLayers();
    void opcodesAreUniquePerLayer();
    void lookupFindsTheCanonicalCalls();
    void lookupRejectsGapsAndForeignLayers();
    void lookupByNameIsCaseInsensitiveAndLayerScoped();
    void stackLayoutsMatchTheBindings();
    void availabilityIsRecordedWhereTiered();

    void trapLineResolvesNumericPush();
    void trapLineResolvesHexAndSymbolicPush();
    void clrWordPushIsFunctionZero();
    void registerPushStaysUnresolvedButInContext();
    void unknownNumberStaysUnresolvedButInContext();
    void aLabelSharingItsLineStillResolves();
    void pushLineResolvesThroughTheTrapBelow();
    void argumentPushesAreCollectedInSourceOrder();
    void commentsAndBlanksRideInsideASequence();
    void aQuotedSemicolonStaysInTheOperand();
    void aLabelEndsTheSequence();
    void nonOsTrapsAndDistantPushesAreIgnored();

    void bindingShapeIsCanonical();
    void bindingSpecialCasesKeepTheirDocumentedShape();
    void everyBindingAddsUpToItsStackBytes();
    void everyBindingScansBackToItsOwnCall();
    void declaredStackLayoutMatchesTheData();
    void aRenamedReservedArgumentStillPushesItsValue();

    void commentsAndQuotesFollowVasm();
    void labelsMayBeIndentedAndAreAscii();
    void navigationFindsLabelDefinitions();
    void navigationParsesIncludeTargets();
    void navigationResolvesTheWordAtTheCursor();
};

void TstOsCall::tableCoversThreeLayers()
{
    // The table is the reference's listing order, so a layer is one run of rows
    // — GEMDOS, then XBIOS, then BIOS — and every row belongs to exactly one of
    // the three layers. That is the shape the panel depends on (it lists the
    // table in order, so an interleaved layer reads as a manual with its pages
    // shuffled). The exact sizes (53/46/12, 111 in total) used to be pinned
    // here and were pure maintenance tax — a new call forced a test edit —
    // while a truncated or duplicated table is what opcodesAreUniquePerLayer
    // and the lookups below actually catch (NIT-14).
    QStringList layers;
    for (const OsCallInfo &info : osCallTable()) {
        switch (info.trap) {
        case 1:
        case 13:
        case 14:
            break;
        default:
            QFAIL("entry with a trap number that is not an OS layer");
        }
        const QString layer = osCallLayerKey(info.trap);
        if (layers.isEmpty() || layers.last() != layer)
            layers << layer;
    }
    // Three runs, in the documented order: any other list means a layer is
    // missing, empty, or picked up again after another one.
    QCOMPARE(layers, QStringList({QStringLiteral("gemdos"), QStringLiteral("xbios"),
                                  QStringLiteral("bios")}));
}

void TstOsCall::opcodesAreUniquePerLayer()
{
    QSet<QString> seen;
    QSet<QString> named;
    for (const OsCallInfo &info : osCallTable()) {
        const QString key = QStringLiteral("%1:%2").arg(info.trap).arg(info.opcode);
        QVERIFY2(!seen.contains(key), qPrintable(QStringLiteral("duplicate %1 (%2)").arg(key, info.name)));
        seen.insert(key);
        // Both look-ups are keyed on exactly these two pairs, so a repeated name
        // in one layer would leave one of the rows unreachable through
        // osCallRefByName: the table would still list it and the lookup would
        // answer with its twin, silently. (Same hazard as the opcode above.)
        const QString nameKey = QStringLiteral("%1:%2").arg(info.trap).arg(info.name.toLower());
        QVERIFY2(!named.contains(nameKey), qPrintable(QStringLiteral("duplicate name %1").arg(nameKey)));
        named.insert(nameKey);
        // ...and every row the table lists is the one each lookup answers with.
        QCOMPARE(osCallRef(info.trap, info.opcode), &info);
        QCOMPARE(osCallRefByName(info.trap, info.name), &info);
        // Every entry names itself and its layer, so a row or a detail can
        // never render empty.
        QVERIFY(!info.name.isEmpty());
        QVERIFY(!info.summary.isEmpty());
        QVERIFY(!osCallLayerName(info.trap).isEmpty());
        QCOMPARE(osCallLayerKey(info.trap), osCallLayerName(info.trap).toLower());
        QVERIFY(info.stackBytes >= 2);
    }
}

void TstOsCall::lookupFindsTheCanonicalCalls()
{
    const OsCallInfo *cconws = osCallRef(1, 9);
    QVERIFY(cconws);
    QCOMPARE(cconws->name, QStringLiteral("Cconws"));

    const OsCallInfo *supexec = osCallRef(14, 38);
    QVERIFY(supexec);
    QCOMPARE(supexec->name, QStringLiteral("Supexec"));

    const OsCallInfo *bconin = osCallRef(13, 2);
    QVERIFY(bconin);
    QCOMPARE(bconin->name, QStringLiteral("Bconin"));

    // Function 0 exists in all three layers and must not alias.
    QCOMPARE(osCallRef(1, 0)->name, QStringLiteral("Pterm0"));
    QCOMPARE(osCallRef(14, 0)->name, QStringLiteral("Initmouse"));
    QCOMPARE(osCallRef(13, 0)->name, QStringLiteral("Getmpb"));
}

void TstOsCall::lookupRejectsGapsAndForeignLayers()
{
    // 0x0C is a GEMDOS gap; 88 is beyond the TOS set; trap 2 (AES/VDI) is not
    // an OS-call layer the table covers.
    QCOMPARE(osCallRef(1, 12), nullptr);
    QCOMPARE(osCallRef(1, 88), nullptr);
    QCOMPARE(osCallRef(2, 9), nullptr);
}

void TstOsCall::lookupByNameIsCaseInsensitiveAndLayerScoped()
{
    const OsCallInfo *cconws = osCallRefByName(1, QStringLiteral("cconws"));
    QVERIFY(cconws);
    QCOMPARE(cconws->opcode, 9);

    // Names resolve only inside their own layer: Cconws is not an XBIOS call,
    // and a same-named entry elsewhere must not bleed across.
    QCOMPARE(osCallRefByName(14, QStringLiteral("Cconws")), nullptr);
    QCOMPARE(osCallRefByName(1, QStringLiteral("Supexec")), nullptr);
    QCOMPARE(osCallRefByName(1, QStringLiteral("NotACall")), nullptr);
}

void TstOsCall::stackLayoutsMatchTheBindings()
{
    // Spot-check the layouts the tos.hyp binding blocks pin, including the
    // three whose cleanup exceeds the C prototype's apparent size.
    QCOMPARE(osCallRef(1, 9)->stackBytes, 6);   // Cconws: pea + word
    QCOMPARE(osCallRef(1, 74)->stackBytes, 12); // Mshrink: reserved zero word
    QCOMPARE(osCallRef(1, 86)->stackBytes, 12); // Frename: reserved zero word
    QCOMPARE(osCallRef(1, 75)->stackBytes, 16); // Pexec: mode + three longs
    QCOMPARE(osCallRef(13, 4)->stackBytes, 18); // Rwabs: five words + two longs
    QCOMPARE(osCallRef(14, 10)->stackBytes, 26); // Flopfmt: the widest call
    QCOMPARE(osCallRef(1, 0)->stackBytes, 2);   // Pterm0: function word only
}

void TstOsCall::availabilityIsRecordedWhereTiered()
{
    QVERIFY(osCallRef(1, 20)->availability.contains(QStringLiteral("GEMDOS 0.19"))); // Maddalt
    QVERIFY(osCallRef(14, 41)->availability.contains(QStringLiteral("1.04")));       // Floprate
    QVERIFY(osCallRef(14, 47)->availability.contains(QStringLiteral("ST-Book")));    // Waketime
    // Untiered calls say nothing rather than inventing a floor.
    QVERIFY(osCallRef(1, 9)->availability.isEmpty());
    QVERIFY(osCallRef(13, 2)->availability.isEmpty());
}

// A scan fixture: the Cconws sequence from demo/hello.s, one idiom per test.
static QStringList cconwsSequence()
{
    return {QStringLiteral("start:"),
            QStringLiteral("\tmove.l\t#msg,-(a7)"),
            QStringLiteral("\tmove.w\t#9,-(a7)"),
            QStringLiteral("\ttrap\t#1"),
            QStringLiteral("\taddq.l\t#6,a7")};
}

void TstOsCall::trapLineResolvesNumericPush()
{
    const OsCallMatch match = osCallAt(cconwsSequence(), 3);
    QVERIFY(match.trapContext);
    QVERIFY(match.call);
    QCOMPARE(match.call->name, QStringLiteral("Cconws"));
    QCOMPARE(match.args, QStringList{QStringLiteral("#msg")});
}

void TstOsCall::trapLineResolvesHexAndSymbolicPush()
{
    const QStringList hex = {QStringLiteral("\tmove.w\t#$0b,-(sp)"), QStringLiteral("\ttrap\t#1")};
    const OsCallMatch hexMatch = osCallAt(hex, 1);
    QVERIFY(hexMatch.call);
    QCOMPARE(hexMatch.call->name, QStringLiteral("Cconis"));

    const QStringList sym = {QStringLiteral("\tmove.w\t#Cconws,-(sp)"), QStringLiteral("\ttrap\t#1")};
    const OsCallMatch symMatch = osCallAt(sym, 1);
    QVERIFY(symMatch.call);
    QCOMPARE(symMatch.call->opcode, 9);
}

void TstOsCall::clrWordPushIsFunctionZero()
{
    const QStringList lines = {QStringLiteral("\tclr.w\t-(a7)"), QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 1);
    QVERIFY(match.call);
    QCOMPARE(match.call->name, QStringLiteral("Pterm0"));
}

void TstOsCall::registerPushStaysUnresolvedButInContext()
{
    const QStringList lines = {QStringLiteral("\tmove.w\td0,-(sp)"), QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 1);
    QVERIFY(match.trapContext); // the panel shows the generic trap entry
    QCOMPARE(match.call, nullptr);
}

void TstOsCall::unknownNumberStaysUnresolvedButInContext()
{
    const QStringList lines = {QStringLiteral("\tmove.w\t#$59,-(sp)"), QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 1);
    QVERIFY(match.trapContext);
    QCOMPARE(match.call, nullptr);
}

void TstOsCall::aLabelSharingItsLineStillResolves()
{
    // vasm lets a label share its line with an instruction; the push after
    // the colon is still the call's argument (this shape comes from real
    // demo sources).
    const QStringList lines = {QStringLiteral("start:\tmove.l\t#msg,-(a7)"),
                               QStringLiteral("\tmove.w\t#9,-(a7)"),
                               QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 2);
    QVERIFY(match.call);
    QCOMPARE(match.call->name, QStringLiteral("Cconws"));
    QCOMPARE(match.args, QStringList{QStringLiteral("#msg")});
}

void TstOsCall::pushLineResolvesThroughTheTrapBelow()
{
    // Cursor on the function-number push and on an argument push both resolve.
    const QStringList lines = cconwsSequence();
    const OsCallMatch fnPush = osCallAt(lines, 2);
    QVERIFY(fnPush.call);
    QCOMPARE(fnPush.call->name, QStringLiteral("Cconws"));

    const OsCallMatch argPush = osCallAt(lines, 1);
    QVERIFY(argPush.call);
    QCOMPARE(argPush.call->name, QStringLiteral("Cconws"));
}

void TstOsCall::argumentPushesAreCollectedInSourceOrder()
{
    const QStringList lines = {QStringLiteral("\tmove.l\t#buf,-(a7)"),
                               QStringLiteral("\tmove.l\t#100,-(a7)"),
                               QStringLiteral("\tmove.w\t#3,-(a7)"),
                               QStringLiteral("\tmove.w\t#$3F,-(a7)"),
                               QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 4);
    QVERIFY(match.call);
    QCOMPARE(match.call->name, QStringLiteral("Fread"));
    QCOMPARE(match.args,
             (QStringList{QStringLiteral("#buf"), QStringLiteral("#100"), QStringLiteral("#3")}));

    // A pea counts, and clr.l displays as the zero it pushes (Super's idiom).
    const QStringList super = {QStringLiteral("\tclr.l\t-(sp)"),
                               QStringLiteral("\tmove.w\t#$20,-(sp)"),
                               QStringLiteral("\ttrap\t#1")};
    const OsCallMatch superMatch = osCallAt(super, 2);
    QVERIFY(superMatch.call);
    QCOMPARE(superMatch.call->name, QStringLiteral("Super"));
    QCOMPARE(superMatch.args, QStringList{QStringLiteral("0")});
}

void TstOsCall::commentsAndBlanksRideInsideASequence()
{
    const QStringList lines = {QStringLiteral("\tpea\tsuper_fn"),
                               QStringLiteral("* a whole-line comment"),
                               QStringLiteral("\tmove.w\t#38,-(sp)\t; Supexec"),
                               QStringLiteral(""),
                               QStringLiteral("\ttrap\t#14")};
    const OsCallMatch match = osCallAt(lines, 4);
    QVERIFY(match.call);
    QCOMPARE(match.call->name, QStringLiteral("Supexec"));
    QCOMPARE(match.args, QStringList{QStringLiteral("super_fn")});
}

void TstOsCall::aQuotedSemicolonStaysInTheOperand()
{
    // The scanner reads its lines through AsmLex, so a `;` inside a quoted
    // operand is part of the operand rather than the start of a comment: the
    // push is recognised, and the panel shows the text the source wrote. A
    // character constant is the way a `;` reaches an argument.
    const QStringList lines = {QStringLiteral("\tmove.w\t#';',-(a7)"),
                               QStringLiteral("\tmove.w\t#2,-(a7)"),
                               QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 2);
    QVERIFY(match.call);
    QCOMPARE(match.call->name, QStringLiteral("Cconout"));
    QCOMPARE(match.args, QStringList{QStringLiteral("#';'")});
}

void TstOsCall::aLabelEndsTheSequence()
{
    // A label between the push and the trap means the push is not this trap's
    // function number: still a trap context, but no call is claimed.
    const QStringList lines = {QStringLiteral("\tmove.w\t#9,-(sp)"),
                               QStringLiteral("done:"),
                               QStringLiteral("\ttrap\t#1")};
    const OsCallMatch match = osCallAt(lines, 2);
    QVERIFY(match.trapContext);
    QCOMPARE(match.call, nullptr);
}

void TstOsCall::nonOsTrapsAndDistantPushesAreIgnored()
{
    // trap #2 is the AES/VDI dispatcher: not an OS-call layer here.
    const QStringList aes = {QStringLiteral("\tmove.w\t#9,-(sp)"), QStringLiteral("\ttrap\t#2")};
    const OsCallMatch aesMatch = osCallAt(aes, 1);
    QVERIFY(!aesMatch.trapContext);
    QCOMPARE(aesMatch.call, nullptr);

    // A push more than a few lines above its trap is not the call's number.
    QStringList distant = {QStringLiteral("\tmove.w\t#9,-(sp)")};
    for (int i = 0; i < 10; ++i)
        distant << QStringLiteral("\tnop");
    distant << QStringLiteral("\ttrap\t#1");
    const OsCallMatch farMatch = osCallAt(distant, distant.size() - 1);
    QCOMPARE(farMatch.call, nullptr);

    // Ordinary lines carry no context at all.
    const QStringList plain = {QStringLiteral("\tmoveq\t#0,d0")};
    const OsCallMatch plainMatch = osCallAt(plain, 0);
    QVERIFY(!plainMatch.trapContext);
}

// The byte size of one generated push line, 0 for trap/cleanup lines. Match
// the opcode, never a substring: "#repeat" contains "pea".
static int pushSize(const QString &line)
{
    const QString t = line.trimmed();
    if (t.startsWith(QStringLiteral("pea\t")) || t.startsWith(QStringLiteral("move.l\t")))
        return 4;
    if (t.startsWith(QStringLiteral("move.w\t")))
        return 2;
    return 0;
}

void TstOsCall::bindingShapeIsCanonical()
{
    // The shape a user reads after inserting: argument pushes, function word
    // with its comment, trap, cleanup.
    QCOMPARE(osCallBinding(*osCallRef(1, 9)),
             QStringLiteral("\tpea\tbuf\n"
                            "\tmove.w\t#9,-(sp)\t; GEMDOS Cconws\n"
                            "\ttrap\t#1\n"
                            "\taddq.l\t#6,sp\n"));

    // No arguments: just the function word, and addq covers the cleanup.
    QCOMPARE(osCallBinding(*osCallRef(1, 0)),
             QStringLiteral("\tmove.w\t#0,-(sp)\t; GEMDOS Pterm0\n"
                            "\ttrap\t#1\n"
                            "\taddq.l\t#2,sp\n"));
}

void TstOsCall::bindingSpecialCasesKeepTheirDocumentedShape()
{
    // Mshrink and Frename: the reserved zero word rides between the arguments
    // and the function number.
    const QString mshrink = osCallBinding(*osCallRef(1, 74));
    QVERIFY(mshrink.contains(QStringLiteral("\tpea\tblock\n\tmove.w\t#0,-(sp)\n\tmove.w\t#74,-(sp)")));

    // Pexec: the varargs prototype still gets its fixed three-long layout.
    const QString pexec = osCallBinding(*osCallRef(1, 75));
    QVERIFY(pexec.startsWith(QStringLiteral("\tpea\tenv\n\tpea\tcmdline\n\tpea\tname\n")));
    QVERIFY(pexec.contains(QStringLiteral("\tlea\t16(sp),sp\n")));

    // Dbmsg: the reserved word is the literal 5, not a placeholder.
    const QString dbmsg = osCallBinding(*osCallRef(14, 11));
    QVERIFY(dbmsg.contains(QStringLiteral("\tmove.w\t#5,-(sp)")));
    QVERIFY(!dbmsg.contains(QStringLiteral("rsrvd")));
}

void TstOsCall::everyBindingAddsUpToItsStackBytes()
{
    // The pushes a binding emits must total the entry's stack layout: this
    // catches prototype-parse size/order bugs across the whole table, not
    // just the entries someone hand-checked.
    for (const OsCallInfo &info : osCallTable()) {
        const QStringList lines = osCallBinding(info).split(QLatin1Char('\n'));
        int bytes = 0;
        for (const QString &line : lines)
            bytes += pushSize(line);
        QVERIFY2(bytes == info.stackBytes,
                 qPrintable(QStringLiteral("%1 %2: pushes total %3, table says %4")
                                .arg(osCallLayerName(info.trap), info.name)
                                .arg(bytes)
                                .arg(info.stackBytes)));
    }
}

void TstOsCall::everyBindingScansBackToItsOwnCall()
{
    // Round-trip: the text the generator emits must resolve through the
    // scanner to the call it came from, with every argument push collected.
    for (const OsCallInfo &info : osCallTable()) {
        const QStringList lines = osCallBinding(info).split(QLatin1Char('\n'));
        int trapIndex = -1, pushLines = 0;
        for (int i = 0; i < lines.size(); ++i) {
            if (lines.at(i).startsWith(QStringLiteral("\ttrap")))
                trapIndex = i;
            else if (pushSize(lines.at(i)) > 0)
                ++pushLines;
        }
        QVERIFY2(trapIndex > 0, qPrintable(info.name));
        const OsCallMatch match = osCallAt(lines, trapIndex);
        QVERIFY2(match.call, qPrintable(info.name));
        QVERIFY2(match.call->trap == info.trap && match.call->opcode == info.opcode,
                 qPrintable(QStringLiteral("%1 resolved to %2").arg(info.name, match.call->name)));
        QCOMPARE(match.args.size(), pushLines - 1); // minus the fn-word push
    }
}

// The byte size of one prototype parameter, read from its declaration alone: a
// pointer or a 32-bit type is a long, anything else a word. Written out here
// rather than shared with the generator on purpose — the test's job is that the
// table, the prototype and the generator all say the same number.
static int prototypeBytes(const QString &prototype)
{
    const int open = prototype.indexOf(QLatin1Char('('));
    const int close = prototype.lastIndexOf(QLatin1Char(')'));
    const QString inside = prototype.mid(open + 1, close - open - 1).trimmed();
    if (inside.isEmpty() || inside == QLatin1String("void"))
        return 0;

    int bytes = 0;
    const QStringList parts = inside.split(QLatin1Char(','));
    for (const QString &raw : parts) {
        const QString part = raw.trimmed();
        if (part == QLatin1String("..."))
            continue; // Pexec's varargs: the entry's fixed block covers them
        const bool isLong = part.contains(QLatin1Char('*'))
                            || part.contains(QStringLiteral("32_t"))
                            || part.contains(QLatin1String("long"), Qt::CaseInsensitive);
        bytes += isLong ? 4 : 2;
    }
    return bytes;
}

void TstOsCall::declaredStackLayoutMatchesTheData()
{
    // What the caller pops is a function of the entry: the function word, the
    // prototype's arguments, the reserved words the binding adds and Pexec's
    // fixed block. It used to be a second hand-written number that nothing kept
    // in step with the first; now the listed value, the derivation the binding
    // generates its cleanup from, and the prototype must agree entry by entry.
    for (const OsCallInfo &info : osCallTable()) {
        const int derived = 2 + prototypeBytes(info.prototype) + 2 * info.reservedWords
                            + (info.fixedArgBlock ? 3 * 4 : 0);
        const QString what = QStringLiteral("%1 %2").arg(osCallLayerName(info.trap), info.name);
        QVERIFY2(info.stackBytes == derived,
                 qPrintable(QStringLiteral("%1: listed %2 bytes, its prototype and layout say %3")
                                .arg(what)
                                .arg(info.stackBytes)
                                .arg(derived)));
        QVERIFY2(osCallStackBytes(info) == derived,
                 qPrintable(QStringLiteral("%1: the binding derives %2 bytes, its prototype and "
                                           "layout say %3")
                                .arg(what)
                                .arg(osCallStackBytes(info))
                                .arg(derived)));
    }
}

void TstOsCall::aRenamedReservedArgumentStillPushesItsValue()
{
    // Dbmsg's reserved word is fixed by the call, and the entry says which
    // argument that is: renaming the parameter cannot turn the literal 5 into a
    // placeholder the caller would have to fill in.
    OsCallInfo renamed = *osCallRef(14, 11);
    renamed.prototype =
        QStringLiteral("void Dbmsg(int16_t unused, int16_t msg_num, int32_t msg_arg)");
    const QString binding = osCallBinding(renamed);
    QVERIFY(binding.contains(QStringLiteral("\tmove.w\t#5,-(sp)")));
    QVERIFY(!binding.contains(QStringLiteral("unused")));
    QCOMPARE(osCallStackBytes(renamed), 10);
}

// --- AsmLex: the token rules the highlighter, the scanner and navigation share

void TstOsCall::commentsAndQuotesFollowVasm()
{
    // `;` starts a comment anywhere on the line.
    QCOMPARE(asmlex::commentStart(QStringLiteral("\tmove.w\t#9,-(sp)\t; Cconws")), 17);
    QCOMPARE(asmlex::codePart(QStringLiteral("\tmove.w\t#9,-(sp)\t; Cconws")),
             QStringLiteral("move.w\t#9,-(sp)"));

    // …but not inside a string: both quote styles are strings in Motorola
    // syntax, and a `;` in one is only a character. The code after the closing
    // quote still counts.
    QCOMPARE(asmlex::commentStart(QStringLiteral("\tdc.b\t';',0")), -1);
    QCOMPARE(asmlex::commentStart(QStringLiteral("\tdc.b\t\";\",0")), -1);
    QCOMPARE(asmlex::codePart(QStringLiteral("\tdc.b\t';'  ; done")), QStringLiteral("dc.b\t';'"));
    // An escaped quote does not close the string, so the `;` behind it is still
    // inside it.
    QCOMPARE(asmlex::commentStart(QStringLiteral("\tdc.b\t'it\\'s ; here'")), -1);
    // An unterminated quote swallows the rest of the line: no comment opens.
    QCOMPARE(asmlex::commentStart(QStringLiteral("\tdc.b\t'hi ; there")), -1);

    // `*` in the first column makes the whole line a comment; anywhere else it
    // is an operand, and indenting it does not make it a comment.
    QCOMPARE(asmlex::commentStart(QStringLiteral("* hello.s")), 0);
    QVERIFY(asmlex::codePart(QStringLiteral("* hello.s")).isEmpty());
    QCOMPARE(asmlex::codePart(QStringLiteral("\tmove.w\t#2*3,-(sp)")),
             QStringLiteral("move.w\t#2*3,-(sp)"));
}

void TstOsCall::labelsMayBeIndentedAndAreAscii()
{
    // vasm reads the first field as a label wherever it starts, so an indented
    // definition is one — the demos indent their local labels.
    QString symbol;
    QVERIFY(asmlex::isLabelDefinition(QStringLiteral("count:"), &symbol));
    QCOMPARE(symbol, QStringLiteral("count"));
    QVERIFY(asmlex::isLabelDefinition(QStringLiteral("\tcount:"), &symbol));
    QCOMPARE(symbol, QStringLiteral("count"));
    QVERIFY(asmlex::isLabelDefinition(QStringLiteral("  start:\tmoveq\t#0,d0"), &symbol));
    QCOMPARE(symbol, QStringLiteral("start"));

    // The instruction sharing the label's line is what the scanner reads, and a
    // line holding only a label has none — which is how a call sequence ends.
    QCOMPARE(asmlex::instructionPart(QStringLiteral("\tstart:\tmove.w\t#9,-(sp)")),
             QStringLiteral("move.w\t#9,-(sp)"));
    QVERIFY(asmlex::instructionPart(QStringLiteral("\tdone:")).isEmpty());

    // Not definitions: a reference, a directive's operand, and a name mentioned
    // in a comment or inside a string.
    QVERIFY(!asmlex::isLabelDefinition(QStringLiteral("\tbsr count"), &symbol));
    QVERIFY(!asmlex::isLabelDefinition(QStringLiteral("\tmoveq\t#0,d0\t; count:"), &symbol));
    QVERIFY(!asmlex::isLabelDefinition(QStringLiteral("\tdc.b\t'count:'"), &symbol));

    // The alphabet is ASCII, and `_`, `.` and `$` are part of it; a Unicode
    // letter is not a name, and a leading digit is a number rather than a label.
    QVERIFY(asmlex::isLabelDefinition(QStringLiteral("\t.data_1$:"), &symbol));
    QCOMPARE(symbol, QStringLiteral(".data_1$"));
    QVERIFY(!asmlex::isWordChar(QChar(0x00E9))); // é
    QVERIFY2(!asmlex::isLabelDefinition(QStringLiteral("caf") + QChar(0x00E9) + QLatin1Char(':'),
                                        &symbol),
             "a Unicode letter is not part of a name");
    QVERIFY(!asmlex::isWordStart(QLatin1Char('1')));
    QVERIFY(!asmlex::isLabelDefinition(QStringLiteral("1up:"), &symbol));
}

// IncludeNav is QtCore-only and now reads its lines through AsmLex, so its
// first suite coverage lives here beside the scanner's: the definition,
// include-target and word-at-cursor rules Ctrl+click depends on. (Ported from
// the throwaway /tmp harness that pinned the MAJ-30 rewrite's behaviour
// byte-for-byte; see the code-quality report's MAJ-30 row.)
void TstOsCall::navigationFindsLabelDefinitions()
{
    // A definition is a name and a colon at the start of the code field,
    // indented or not (vasm's rule — the demos indent their local labels), and
    // the assignment forms are definitions too.
    const QString doc = QStringLiteral("* comment\n"
                                      "start:\n"
                                      "\tmoveq\t#0,d0\n"
                                      "count:\taddq.w\t#1,d0\n"
                                      "\tkMaxX equ 40\n"
                                      "\trows set 25\n"
                                      "\twidth = 320\n"
                                      "\t; start: mentioned in a comment\n"
                                      "\tbsr count\n");
    QCOMPARE(labelLine(doc, QStringLiteral("START")), 2);  // case-insensitively
    QCOMPARE(labelLine(doc, QStringLiteral("count")), 4);  // shared with an instruction
    QCOMPARE(labelLine(doc, QStringLiteral("kMaxX")), 5);  // `equ`
    QCOMPARE(labelLine(doc, QStringLiteral("rows")), 6);   // `set`
    QCOMPARE(labelLine(doc, QStringLiteral("width")), 7);  // `=`
    QCOMPARE(labelLine(doc, QStringLiteral("bsr")), 0);    // a reference is not a definition
    QCOMPARE(labelLine(doc, QStringLiteral("nope")), 0);   // an unknown symbol is nowhere
    QCOMPARE(labelLine(QStringLiteral("\tstart:\tnop\n"), QStringLiteral("start")), 1);
}

void TstOsCall::navigationParsesIncludeTargets()
{
    QCOMPARE(includeTargetAt(QStringLiteral("\tinclude\t'lib.s'")), QStringLiteral("lib.s"));
    QCOMPARE(includeTargetAt(QStringLiteral("Include \"lib.s\"")), QStringLiteral("lib.s"));
    QVERIFY(includeTargetAt(QStringLiteral("\t; include 'lib.s'")).isEmpty());
    QVERIFY(includeTargetAt(QStringLiteral("\t* include 'lib.s'")).isEmpty());
}

void TstOsCall::navigationResolvesTheWordAtTheCursor()
{
    const QString line = QStringLiteral("\tmove.w\t#kMaxX+1,d4\t; note");
    QCOMPARE(wordAtCursor(line, line.indexOf(QLatin1Char('#'))), QStringLiteral("kMaxX"));
    QCOMPARE(wordAtCursor(line, line.indexOf(QLatin1Char('+')) + 1), QStringLiteral("1"));
    QCOMPARE(wordAtCursor(line, line.indexOf(QLatin1Char(','))), QStringLiteral("1"));
    QCOMPARE(wordAtCursor(line, line.indexOf(QLatin1String("kMaxX")) + 2), QStringLiteral("kMaxX"));
    QCOMPARE(wordAtCursor(QStringLiteral("\tmove.w\t#kMaxX"), 999), QStringLiteral("kMaxX"));
}

QTEST_MAIN(TstOsCall)
#include "tst_oscall.moc"
