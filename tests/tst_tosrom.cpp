// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Tests for TOS ROM identification.
//
// The version must come from the image header, not the filename. A filename
// heuristic would classify a genuinely old ROM (or one with an unusual name) as
// "unknown", and the application only rejects ROMs whose version is *known* to
// be too old — so the guard against the silent-hang case would be skippable.

#include "emu/TosRom.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

namespace {

/// Write a synthetic ROM image with a given version field.
QString writeRom(const QString &dir, const QString &name, quint16 version,
                 bool emuTos = false, quint16 marker208 = 0)
{
    QByteArray image(196608, '\0'); // 192 KiB, a real TOS 1.0x size
    image[0] = '\x60';
    image[1] = '\x2e';
    image[2] = static_cast<char>((version >> 8) & 0xff);
    image[3] = static_cast<char>(version & 0xff);
    // TosAddress = 0x00e00000
    image[8] = '\x00';
    image[9] = '\xe0';
    image[10] = '\x00';
    image[11] = '\x00';
    if (marker208) {
        image[30] = static_cast<char>((marker208 >> 8) & 0xff);
        image[31] = static_cast<char>(marker208 & 0xff);
    }
    if (emuTos) {
        image[0x2c] = 'E';
        image[0x2d] = 'T';
        image[0x2e] = 'O';
        image[0x2f] = 'S';
    }

    const QString path = QDir(dir).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {}; // empty path: callers assert on it
    f.write(image);
    f.close();
    return path;
}

} // namespace

class TstTosRom : public QObject
{
    Q_OBJECT

private slots:
    void readsVersionFromHeader();
    void versionComesFromHeaderNotFilename();
    void detectsEmuTos();
    void handlesTos208Adjustment();
    void versionsRenderInHex();
    void filenameFallbackUsesHexBytes();
    void rejectsTruncatedImage();
    void autostartRequires104();
    void filenameClaimIsNotEnoughForAutostart();
    void selectsAutostartCapableRom();
};

void TstTosRom::readsVersionFromHeader()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeRom(dir.path(), QStringLiteral("anything.img"), 0x0104);
    QVERIFY(!path.isEmpty());

    TosRom rom;
    rom.path = path;
    QVERIFY(readTosHeader(path, &rom));
    QCOMPARE(rom.versionCode, 0x0104);
    QVERIFY(rom.versionKnown);
    QVERIFY(rom.versionFromHeader);
    QCOMPARE(rom.versionText(), QStringLiteral("1.04"));
}

// The regression this file exists for: a TOS 1.02 image whose name reveals
// nothing must still be recognised as 1.02, so the autostart guard applies.
void TstTosRom::versionComesFromHeaderNotFilename()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("mytos.img"), 0x0102).isEmpty());

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 1);
    const TosRom &rom = roms.first();

    QVERIFY2(rom.versionKnown, "version must be known despite an uninformative filename");
    QCOMPARE(rom.versionCode, 0x0102);
    QVERIFY(rom.versionFromHeader);
    QVERIFY2(!rom.supportsAutostart(), "TOS 1.02 must not be treated as autostart-capable");
}

void TstTosRom::detectsEmuTos()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeRom(dir.path(), QStringLiteral("etos.img"), 0x0104, true);
    QVERIFY(!path.isEmpty());

    TosRom rom;
    rom.path = path;
    QVERIFY(readTosHeader(path, &rom));
    QVERIFY(rom.isEmuTos);
    QVERIFY(rom.supportsAutostart());
    QCOMPARE(rom.versionText(), QStringLiteral("EmuTOS (TOS 1.04)"));
}

void TstTosRom::handlesTos208Adjustment()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A 2.06 field with the 0x186A marker is really TOS 2.08, exactly as
    // Hatari adjusts it.
    const QString path = writeRom(dir.path(), QStringLiteral("t208.img"), 0x0206, false, 0x186A);
    QVERIFY(!path.isEmpty());

    TosRom rom;
    rom.path = path;
    QVERIFY(readTosHeader(path, &rom));
    QCOMPARE(rom.versionCode, 0x0208);
}

// Version bytes are hex by convention: 0x0162 is TOS 1.62, not 1.98.
void TstTosRom::versionsRenderInHex()
{
    TosRom rom;
    rom.versionKnown = true;

    rom.versionCode = 0x0104;
    QCOMPARE(rom.versionText(), QStringLiteral("1.04"));
    rom.versionCode = 0x0106;
    QCOMPARE(rom.versionText(), QStringLiteral("1.06"));
    rom.versionCode = 0x0162;
    QCOMPARE(rom.versionText(), QStringLiteral("1.62"));
    rom.versionCode = 0x0206;
    QCOMPARE(rom.versionText(), QStringLiteral("2.06"));
    rom.versionCode = 0x0404;
    QCOMPARE(rom.versionText(), QStringLiteral("4.04"));

    rom.versionKnown = false;
    QCOMPARE(rom.versionText(), QStringLiteral("unknown"));
}

// Only used when the header cannot be read, but it must agree with the header
// convention or a 1.62 ROM would be recorded as 0x013E.
void TstTosRom::filenameFallbackUsesHexBytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("TOS v1.62 (1990)(Atari Corp)(STE).img"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(16, '\0')); // too small to hold a header
    f.close();

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 1);
    QCOMPARE(roms.first().versionCode, 0x0162);
    QVERIFY(!roms.first().versionFromHeader);
    QCOMPARE(roms.first().versionText(), QStringLiteral("1.62"));
}

void TstTosRom::rejectsTruncatedImage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("tiny.img"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(16, '\0'));
    f.close();

    TosRom rom;
    rom.path = path;
    QVERIFY2(!readTosHeader(path, &rom), "a 16-byte file has no TOS header");
    QVERIFY(!rom.versionKnown);
}

void TstTosRom::autostartRequires104()
{
    TosRom rom;
    rom.versionKnown = true;
    rom.versionFromHeader = true;

    rom.versionCode = 0x0100;
    QVERIFY(!rom.supportsAutostart());
    QVERIFY(rom.knownTooOldForAutostart());
    rom.versionCode = 0x0102;
    QVERIFY(!rom.supportsAutostart());
    QVERIFY(rom.knownTooOldForAutostart());
    rom.versionCode = 0x0104;
    QVERIFY(rom.supportsAutostart());
    QVERIFY(!rom.knownTooOldForAutostart());
    rom.versionCode = 0x0106;
    QVERIFY(rom.supportsAutostart());

    // Unknown versions are not claimed to be capable, but are not claimed to be
    // too old either: the caller asks rather than refusing.
    rom.versionKnown = false;
    QVERIFY(!rom.supportsAutostart());
    QVERIFY(!rom.knownTooOldForAutostart());
}

// A version inferred from a filename is not evidence about what Hatari will do,
// because Hatari reads the image header and never looks at names. An image whose
// header cannot be read must therefore not be treated as autostart-capable, even
// when its name claims a suitable version.
void TstTosRom::filenameClaimIsNotEnoughForAutostart()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString path = dir.filePath(QStringLiteral("TOS v1.04 (1989)(Atari Corp).img"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray(16, '\0')); // header unreadable
    f.close();

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 1);
    const TosRom &rom = roms.first();

    QVERIFY(rom.versionKnown);          // the name told us something
    QVERIFY(!rom.versionFromHeader);    // but not from the image
    QVERIFY2(!rom.supportsAutostart(),
             "a filename claim must not authorise the autostart run path");
    QVERIFY2(!rom.knownTooOldForAutostart(),
             "it is unknown, not known-bad; the caller must ask");
}

void TstTosRom::selectsAutostartCapableRom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Deliberately created so the alphabetical order is wrong: 1.02 sorts first.
    QVERIFY(!writeRom(dir.path(), QStringLiteral("a-tos102.img"), 0x0102).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("b-tos104.img"), 0x0104).isEmpty());

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 2);

    const TosRom chosen = selectPreferredRom(roms);
    QVERIFY2(chosen.supportsAutostart(), "selection must skip the 1.02 image");
    QCOMPARE(chosen.versionCode, 0x0104);
}

QTEST_MAIN(TstTosRom)
#include "tst_tosrom.moc"
