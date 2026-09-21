// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include <QtTest>

#include "emu/ProfileData.h"

using namespace pist;

// The `profile save` parser: the header fields PiST consumes and, since the
// profiler's TOS/ROM row, the memory-area lines the header carries. Fixtures
// follow the Hatari 2.6.1 save shape documented in ProfileData.h.
class TstProfile : public QObject
{
    Q_OBJECT

private slots:
    void headerAndEntriesParse();
    void regionsAreCapturedWithTheirSpans();
    void aSaveWithoutRegionsStillParses();
    void aSaveWithNoExecutedInstructionsIsAnError();
};

namespace {

QString saveText(const QString &areas)
{
    return QStringLiteral("Hatari CPU profile (Hatari v2.6.1)\n"
                          "Cycles/second:\t8021247\n"
                          "Field names:\tExecuted instructions, Used cycles, "
                          "Instruction cache misses, Data cache hits\n"
                          "Field regexp:\t^\\$?([0-9A-Fa-f]+) .*% \\(([^)]*)\\)$\n")
           + areas
           + QStringLiteral("# disassembly with profile data: <instructions percentage>% (...)\n"
                            "start:\n"
                            "$12596 :   moveq #0,d0     0.10% (1, 4, 0, 0)\n"
                            "$12598 :   addq.w #1,d0    97.00% (100, 400, 0, 0)\n");
}

} // namespace

void TstProfile::headerAndEntriesParse()
{
    ProfileData data;
    QString error;
    QVERIFY2(parseProfileText(saveText(QStringLiteral("ST_RAM:\t\t0x000000-0x100000\n")),
                              &data, &error),
             qPrintable(error));
    QCOMPARE(data.processor, QStringLiteral("CPU"));
    QCOMPARE(data.emulator, QStringLiteral("Hatari v2.6.1"));
    QCOMPARE(data.clockHz, 8021247u);
    QCOMPARE(data.lines.size(), 2);
    QCOMPARE(data.lines.at(1).address, 0x12598u);
    QCOMPARE(data.lines.at(1).count, 100ull);
    QCOMPARE(data.lines.at(1).cycles, 400ull);
    QCOMPARE(data.totalCount, 101ull);
    QCOMPARE(data.totalCycles, 404ull);
}

void TstProfile::regionsAreCapturedWithTheirSpans()
{
    // The TOS/ROM row is fed by these: a dropped or misread area line silently
    // sends ROM time back to "unmapped".
    ProfileData data;
    QString error;
    QVERIFY2(parseProfileText(saveText(QStringLiteral("ST_RAM:\t\t0x000000-0x100000\n"
                                                      "ROM_TOS:\t\t0xfc0000-0x1000000\n"
                                                      "CARTRIDGE:\t0xfa0000-0xfc0000\n"
                                                      "PROGRAM_TEXT:\t0x0012554-0x0012600\n")),
                              &data, &error),
             qPrintable(error));
    QCOMPARE(data.regions.size(), 4);
    QCOMPARE(data.regions.at(1).name, QStringLiteral("ROM_TOS"));
    QCOMPARE(data.regions.at(1).first, 0xfc0000u);
    QCOMPARE(data.regions.at(1).last, 0x1000000u);
    QCOMPARE(data.regions.at(2).name, QStringLiteral("CARTRIDGE"));
    QCOMPARE(data.regions.at(3).first, 0x12554u);
}

void TstProfile::aSaveWithoutRegionsStillParses()
{
    // An older save without area lines: no regions, and the ROM-floor fallback
    // in the view carries the OS share instead.
    ProfileData data;
    QString error;
    QVERIFY2(parseProfileText(saveText(QString()), &data, &error), qPrintable(error));
    QVERIFY(data.regions.isEmpty());
    QCOMPARE(data.lines.size(), 2);
}

void TstProfile::aSaveWithNoExecutedInstructionsIsAnError()
{
    const QString empty = QStringLiteral("Hatari CPU profile (Hatari v2.6.1)\n"
                                         "Cycles/second:\t8021247\n"
                                         "Field names:\tExecuted instructions, Used cycles\n"
                                         "Field regexp:\t^\\$?([0-9A-Fa-f]+) .*% \\(([^)]*)\\)$\n"
                                         "# disassembly with profile data: <instructions percentage>% (...)\n");
    ProfileData data;
    QString error;
    QVERIFY(!parseProfileText(empty, &data, &error));
    QVERIFY(error.contains(QStringLiteral("no profiled instructions")));
}

QTEST_MAIN(TstProfile)
#include "tst_profile.moc"
