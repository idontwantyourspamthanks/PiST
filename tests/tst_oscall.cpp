// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include <QtTest>

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
    void aLabelEndsTheSequence();
    void nonOsTrapsAndDistantPushesAreIgnored();
};

void TstOsCall::tableCoversThreeLayers()
{
    int gemdos = 0, bios = 0, xbios = 0;
    for (const OsCallInfo &info : osCallTable()) {
        switch (info.trap) {
        case 1: ++gemdos; break;
        case 13: ++bios; break;
        case 14: ++xbios; break;
        default: QFAIL("entry with a trap number that is not an OS layer");
        }
    }
    QCOMPARE(gemdos, 53);
    QCOMPARE(xbios, 46);
    QCOMPARE(bios, 12);
    QCOMPARE(osCallTable().size(), 111);
}

void TstOsCall::opcodesAreUniquePerLayer()
{
    QSet<QString> seen;
    for (const OsCallInfo &info : osCallTable()) {
        const QString key = QStringLiteral("%1:%2").arg(info.trap).arg(info.opcode);
        QVERIFY2(!seen.contains(key), qPrintable(QStringLiteral("duplicate %1 (%2)").arg(key, info.name)));
        seen.insert(key);
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

QTEST_MAIN(TstOsCall)
#include "tst_oscall.moc"
