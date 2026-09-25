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

#include "model/Machine.h"
#include "emu/HostKey.h"
#include "emu/LibretroBackend.h"
#include "emu/Paths.h"
#include "emu/SessionConfig.h"
#include "emu/TosRom.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <functional>

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
    void initTestCase();
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

    // machine pairing
    void stAcceptsStTosAndRejectsSteTos();
    void steAcceptsSteTosAndRejectsStTos();
    void steAcceptsBothSteTosVersions();
    void unknownVersionIsNotExcluded();
    void machineModelRoundTripsThroughCliNames();

    // selection
    void prefersNewestCompatibleRom();
    void steSelectionAvoidsStOnlyRom();
    void fallbackPrefersMachineCompatibleRom();
    void fallbackPicksNewestCompatibleRom();

    // Bundled-ROM discovery. Every release archive ships its ROM in share/emutos
    // reached by walking up from the executable, but nothing searched there, so
    // the ROM travelled in every archive and was still invisible on a machine
    // without a system TOS. These pin the layouts that must resolve.
    void findsBundledRomInTarballLayout();
    void findsBundledRomInAppImageLayout();
    void findsBundledRomInMacBundleLayout();
    void libretroCoreSitsInFrameworks();
    void libretroCoreIsFoundInFrameworks();
    void libretroSessionUsesTheCoreOnMacWhenPresent();
    void libretroStartFailsWithoutTheCore();
    void libretroCoreDrivesTheEntryStop();
    void hostKeyMapsQtToSdl();
    void hostMouseScalesToVideoPixels();
    void reportsNoBundledDirWhenAbsent();
    void tosSearchPathsIncludesBundledDir();

private:
    /// Saved from the environment in initTestCase, then unset, so the
    /// "core is missing" case still sees the Frameworks search.
    QString m_libretroCore;
};

void TstTosRom::initTestCase()
{
    m_libretroCore = qEnvironmentVariable("PIST_LIBRETRO_CORE");
    qunsetenv("PIST_LIBRETRO_CORE");
}

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

    const TosRom chosen = selectPreferredRom(roms, Machine::St);
    QVERIFY2(chosen.supportsAutostart(), "selection must skip the 1.02 image");
    QCOMPARE(chosen.versionCode, 0x0104);
}

// --- machine pairing -----------------------------------------------------
//
// These encode what Hatari itself enforces, verified by running each pairing and
// reading its override messages:
//
//   TOS 1.04 + --machine ste -> "TOS versions <= 1.4 work only in" -> switches to ST
//   TOS 1.06 + --machine st  -> "1.06 and 1.62 are for Atari STE only" -> switches to STE
//
// PiST must model this itself, because Hatari resolves a mismatch by silently
// changing the machine.

void TstTosRom::stAcceptsStTosAndRejectsSteTos()
{
    QVERIFY(machineAcceptsTos(Machine::St, 0x0100, false));
    QVERIFY(machineAcceptsTos(Machine::St, 0x0102, false));
    QVERIFY(machineAcceptsTos(Machine::St, 0x0104, false));
    QVERIFY(machineAcceptsTos(Machine::MegaSt, 0x0104, false));

    QVERIFY(!machineAcceptsTos(Machine::St, 0x0106, false));
    QVERIFY(!machineAcceptsTos(Machine::St, 0x0162, false));
}

void TstTosRom::steAcceptsSteTosAndRejectsStTos()
{
    QVERIFY(machineAcceptsTos(Machine::Ste, 0x0106, false));
    QVERIFY(machineAcceptsTos(Machine::Ste, 0x0162, false));

    // An STe cannot run an ST-only TOS at all; Hatari switches the machine.
    QVERIFY(!machineAcceptsTos(Machine::Ste, 0x0104, false));
    QVERIFY(!machineAcceptsTos(Machine::Ste, 0x0102, false));
}

// 1.06 and 1.62 are both STe ROMs; 1.62 is the later release with bug fixes.
void TstTosRom::steAcceptsBothSteTosVersions()
{
    const QList<Machine> for106 = machinesForTos(0x0106, false);
    const QList<Machine> for162 = machinesForTos(0x0162, false);

    QVERIFY(for106.contains(Machine::Ste));
    QVERIFY(for162.contains(Machine::Ste));
    QVERIFY(for106.contains(Machine::MegaSte));
    QVERIFY(for162.contains(Machine::MegaSte));

    // Neither belongs on a plain ST.
    QVERIFY(!for106.contains(Machine::St));
    QVERIFY(!for162.contains(Machine::St));
}

void TstTosRom::unknownVersionIsNotExcluded()
{
    // No version read means no evidence of incompatibility, so every machine must
    // still accept it rather than the file being silently dropped.
    for (Machine machine : allMachines())
        QVERIFY(machineAcceptsTos(machine, 0, true)); // EmuTOS adapts itself

    TosRom unknown;
    unknown.versionKnown = false;
    QVERIFY(unknown.supportsMachine(Machine::St));
    QVERIFY(unknown.supportsMachine(Machine::Falcon));
}

void TstTosRom::machineModelRoundTripsThroughCliNames()
{
    for (Machine machine : allMachines()) {
        Machine parsed = Machine::St;
        QVERIFY(machineFromCliName(machineCliName(machine), &parsed));
        QCOMPARE(parsed, machine);
        QVERIFY(!machineDisplayName(machine).isEmpty());
    }
    QVERIFY(!machineFromCliName(QStringLiteral("amiga"), nullptr));
}

void TstTosRom::prefersNewestCompatibleRom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Directory order deliberately puts the older ROMs first.
    QVERIFY(!writeRom(dir.path(), QStringLiteral("a-102.img"), 0x0102).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("b-104.img"), 0x0104).isEmpty());

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 2);

    // On an ST, 1.04 is the newest usable ROM.
    const TosRom st = selectPreferredRom(roms, Machine::St);
    QCOMPARE(st.versionCode, 0x0104);
    QVERIFY(st.supportsMachine(Machine::St));
}

// The regression this guards: an STe project must not be handed an ST-only ROM
// just because it sorts first, and where two STe ROMs exist the later bugfix
// release (1.62) is the better default.
void TstTosRom::steSelectionAvoidsStOnlyRom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(!writeRom(dir.path(), QStringLiteral("a-104.img"), 0x0104).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("b-106.img"), 0x0106).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("c-162.img"), 0x0162).isEmpty());

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 3);

    const TosRom chosen = selectPreferredRom(roms, Machine::Ste);
    QVERIFY2(chosen.supportsMachine(Machine::Ste),
             "an STe must not be given an ST-only ROM");
    QCOMPARE(chosen.versionCode, 0x0162);
    QVERIFY(chosen.supportsAutostart());
}

// With nothing that fully suits the machine, the fallback must still prefer a
// ROM the machine can run. An incompatible one is worse than useless: Hatari
// resolves the pairing by overriding --machine (reporting only an ERROR line),
// so the session silently runs a different machine than the project selected —
// exactly what the ROM/machine model exists to prevent. A compatible ROM, even
// a too-old one, boots on the requested machine through the AUTO-folder floppy
// fallback the launch path already implements.
void TstTosRom::fallbackPrefersMachineCompatibleRom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("a-102.img"), 0x0102).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("b-404.img"), 0x0404).isEmpty());

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 2);

    // Neither ROM is both compatible with an ST and autostart-capable, so this is
    // the fallback path. The 1.02 is the one Hatari will actually boot on an ST;
    // the newer 4.04 Falcon ROM would make Hatari switch the machine.
    const TosRom chosen = selectPreferredRom(roms, Machine::St);
    QCOMPARE(chosen.versionCode, 0x0102);
    QVERIFY2(chosen.supportsMachine(Machine::St),
             "the fallback must not hand an ST a ROM that makes Hatari override --machine");

    // When nothing on disk suits the machine at all the fallback still names a
    // ROM: an STe with only a 1.04 ST image gets that image, because the run path
    // has to say *which* ROM will make Hatari override the machine. An invalid
    // entry here would turn a mismatch into "no TOS ROM found", which is false.
    QTemporaryDir onlySt;
    QVERIFY(onlySt.isValid());
    QVERIFY(!writeRom(onlySt.path(), QStringLiteral("a-104.img"), 0x0104).isEmpty());
    const QList<TosRom> stRoms = scanTosRoms(onlySt.path());
    QCOMPARE(stRoms.size(), 1);

    const TosRom named = selectPreferredRom(stRoms, Machine::Ste);
    QVERIFY2(!named.path.isEmpty(), "the fallback must still name a ROM");
    QCOMPARE(named.versionCode, 0x0104);
}

// Compatible-but-old beats incompatible-new, and among compatible ROMs the newest
// wins. Mega ST accepts the 1.x ST ROMs and nothing newer; both 1.00 and 1.02 are
// compatible but too old to autostart, so the fallback has a real choice: 1.02,
// not the first entry and not the newest overall.
void TstTosRom::fallbackPicksNewestCompatibleRom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("a-100.img"), 0x0100).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("b-102.img"), 0x0102).isEmpty());
    QVERIFY(!writeRom(dir.path(), QStringLiteral("c-404.img"), 0x0404).isEmpty());

    const QList<TosRom> roms = scanTosRoms(dir.path());
    QCOMPARE(roms.size(), 3);

    const TosRom chosen = selectPreferredRom(roms, Machine::MegaSt);
    QCOMPARE(chosen.versionCode, 0x0102);
    QVERIFY(chosen.supportsMachine(Machine::MegaSt));
}

// ---------------------------------------------------------------------------
// Bundled-ROM discovery.
//
// The layouts below are not hypothetical: each one is what a release artifact
// actually unpacks to. The discovery walk is exercised directly with a
// fabricated application directory, because tosSearchPaths() always starts from
// the running test executable and so could never be pointed at a fixture — which
// is precisely how an archive shipped a ROM that nothing searched.
// ---------------------------------------------------------------------------

namespace {

/// Create <root>/<appDir>/ with a ROM in <root>/share/emutos, and return the
/// fabricated executable directory. Returns an empty string on failure.
QString makeBundle(QTemporaryDir &tmp, const QString &appDir, const QString &romName)
{
    const QString romDir = QDir(tmp.path()).filePath(QStringLiteral("share/emutos"));
    if (!QDir().mkpath(romDir))
        return {};
    if (writeRom(romDir, romName, 0x0104, /*emuTos=*/true).isEmpty())
        return {};

    const QString binDir = QDir(tmp.path()).filePath(appDir);
    if (!QDir().mkpath(binDir))
        return {};
    return binDir;
}

/// The share/emutos directory is a suffix of one of the reported paths.
bool reportsEmutosDir(const QStringList &paths, QTemporaryDir &tmp)
{
    const QString expected =
        QDir::cleanPath(QDir(tmp.path()).filePath(QStringLiteral("share/emutos")));
    return paths.contains(expected);
}

} // namespace

void TstTosRom::findsBundledRomInTarballLayout()
{
    // A tarball unpacks to bin/pist beside share/emutos.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString binDir = makeBundle(tmp, QStringLiteral("bin"), QStringLiteral("etos1024k.img"));
    QVERIFY(!binDir.isEmpty());

    QVERIFY(reportsEmutosDir(paths::bundledDataSearchPaths(binDir), tmp));
}

void TstTosRom::findsBundledRomInAppImageLayout()
{
    // An AppImage mounts usr/bin/pist beside usr/share/emutos.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString binDir = makeBundle(tmp, QStringLiteral("usr/bin"), QStringLiteral("etos1024k.img"));
    QVERIFY(!binDir.isEmpty());

    QVERIFY(reportsEmutosDir(paths::bundledDataSearchPaths(binDir), tmp));
}

void TstTosRom::findsBundledRomInMacBundleLayout()
{
    // The disk image ships only the app, so the ROM sits inside it at
    // Contents/share — one level above Contents/MacOS. A share directory beside
    // the .app would be left on the disk image when the app is dragged to
    // Applications, and this layout is the one that has to resolve.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString romDir =
        QDir(tmp.path()).filePath(QStringLiteral("PiST.app/Contents/share/emutos"));
    QVERIFY(QDir().mkpath(romDir));
    QVERIFY(!writeRom(romDir, QStringLiteral("etos1024k.img"), 0x0104, true).isEmpty());
    const QString binDir =
        QDir(tmp.path()).filePath(QStringLiteral("PiST.app/Contents/MacOS"));
    QVERIFY(QDir().mkpath(binDir));

    const QString expected = QDir::cleanPath(romDir);
    QVERIFY(paths::bundledDataSearchPaths(binDir).contains(expected));
}

void TstTosRom::libretroCoreSitsInFrameworks()
{
    // The disk image copies the core to Contents/Frameworks. From the
    // executable in Contents/MacOS that is one directory up, then Frameworks.
    const QString binDir = QStringLiteral("/PiST.app/Contents/MacOS");
    const QStringList candidates = libretroCoreCandidates(binDir);
    QCOMPARE(candidates.size(), 2);
    QCOMPARE(candidates.at(0),
             QDir::cleanPath(binDir + QStringLiteral("/../Frameworks/") + libretroCoreFileName()));
    QCOMPARE(candidates.at(1),
             QDir::cleanPath(binDir + QLatin1Char('/') + libretroCoreFileName()));
    QVERIFY(libretroCoreCandidates(QString()).isEmpty());
}

void TstTosRom::libretroCoreIsFoundInFrameworks()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString binDir = QDir(tmp.path()).filePath(QStringLiteral("PiST.app/Contents/MacOS"));
    const QString coreDir = QDir(tmp.path()).filePath(QStringLiteral("PiST.app/Contents/Frameworks"));
    QVERIFY(QDir().mkpath(binDir));
    QVERIFY(QDir().mkpath(coreDir));
    QVERIFY(findLibretroCore(binDir).isEmpty());

    const QString core = QDir(coreDir).filePath(libretroCoreFileName());
    QFile file(core);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();
    QCOMPARE(findLibretroCore(binDir), QDir::cleanPath(core));
}

void TstTosRom::libretroSessionUsesTheCoreOnMacWhenPresent()
{
    // The launch decision, without a Mac and without a real dylib: present on
    // macOS with an empty Hatari path, absent otherwise. A named path is the
    // subprocess escape hatch even when the file exists.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString binDir = QDir(tmp.path()).filePath(QStringLiteral("PiST.app/Contents/MacOS"));
    const QString coreDir = QDir(tmp.path()).filePath(QStringLiteral("PiST.app/Contents/Frameworks"));
    QVERIFY(QDir().mkpath(binDir));
    QVERIFY(QDir().mkpath(coreDir));

    QVERIFY(!sessionUsesInProcessCore(true, QString(), binDir));
    QVERIFY(!sessionUsesInProcessCore(false, QString(), binDir));

    const QString core = QDir(coreDir).filePath(libretroCoreFileName());
    QFile file(core);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    QVERIFY(sessionUsesInProcessCore(true, QString(), binDir));
    QVERIFY(!sessionUsesInProcessCore(false, QString(), binDir));
    QVERIFY(!sessionUsesInProcessCore(true, QStringLiteral("/usr/local/bin/hatari"), binDir));
}

void TstTosRom::libretroStartFailsWithoutTheCore()
{
    // A sealed app that forgot the dylib must say so, naming the Frameworks
    // path. Launch never reaches start() in that case: the decision above is
    // false when the file is absent.
    LibretroBackend backend;
    QString error;
    QVERIFY(!backend.start(SessionConfig(), &error));
    QVERIFY(error.contains(libretroCoreFileName()));
    QVERIFY(error.contains(QStringLiteral("Frameworks")));
    QVERIFY(!backend.isRunning());
}

void TstTosRom::hostKeyMapsQtToSdl()
{
    QCOMPARE(qtKeyToSdlSym(Qt::Key_A), 97);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_Z), 122);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_1), 49);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_Exclam), 49);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_Return), 13);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_Left), 1073741904);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_F5), 1073741886);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_Shift), 1073742049);
    QCOMPARE(qtKeyToSdlSym(Qt::Key_unknown), 0);
    QCOMPARE(qtModifiersToSdlMod(Qt::ShiftModifier | Qt::AltModifier), 0x0001 | 0x0100);
    QCOMPARE(qtModifiersToSdlMod(Qt::ControlModifier), 0x0040);
}

void TstTosRom::hostMouseScalesToVideoPixels()
{
    QCOMPARE(qtButtonsToHost(Qt::LeftButton | Qt::RightButton), 3);
    QCOMPARE(qtButtonsToHost(Qt::MiddleButton), 0);

    int x = 0;
    int y = 0;
    // A 640x400 picture stretched to 1280x800: two widget pixels are one ST pixel.
    widgetToVideo256(2, 4, 0, 0, 1280, 800, 640, 400, &x, &y);
    QCOMPARE(x, 256);
    QCOMPARE(y, 512);

    int originX = 0;
    int originY = 0;
    int dx = -1;
    int dy = -1;
    bool have = false;
    takePointerDelta(x, y, &originX, &originY, &have, &dx, &dy);
    QCOMPARE(dx, 0);
    QCOMPARE(dy, 0);
    takePointerDelta(x + 256, y + 200, &originX, &originY, &have, &dx, &dy);
    QCOMPARE(dx, 1);
    QCOMPARE(dy, 0);
    takePointerDelta(x + 256, y + 512, &originX, &originY, &have, &dx, &dy);
    QCOMPARE(dx, 0);
    QCOMPARE(dy, 2);
}

void TstTosRom::libretroCoreDrivesTheEntryStop()
{
    // The owner thread against a real core: boot, stop in RAM, read the
    // bases and registers, arm the instruction we stopped on, resume onto
    // that breakpoint, and step. Skips where the core or a ROM is absent.
    if (m_libretroCore.isEmpty() || !QFileInfo::exists(m_libretroCore))
        QSKIP("needs hatari_libretro ($PIST_LIBRETRO_CORE)");

    QString tos = qEnvironmentVariable("PIST_TOS");
    if (tos.isEmpty()) {
        const QList<TosRom> roms = findTosRoms();
        if (!roms.isEmpty())
            tos = roms.first().path;
    }
    if (tos.isEmpty() || !QFileInfo::exists(tos))
        QSKIP("needs a TOS image ($PIST_TOS or $PIST_TOS_DIR)");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString prgPath = QDir(dir.path()).filePath(QStringLiteral("HELLO.PRG"));
    // Header, four bytes of text, and a four-byte empty relocation table.
    // Without the table Hatari refuses the program and the entry stop never comes.
    QByteArray prg(0x1c + 4 + 4, '\0');
    prg[0] = 0x60;
    prg[1] = 0x1a;
    prg[5] = 4;
    prg[0x1c] = 0x4e;
    prg[0x1d] = 0x71; // nop
    prg[0x1e] = 0x60;
    prg[0x1f] = static_cast<char>(0xfe); // bra.s to itself
    QFile prgFile(prgPath);
    QVERIFY(prgFile.open(QIODevice::WriteOnly));
    QCOMPARE(prgFile.write(prg), prg.size());
    prgFile.close();

    qputenv("PIST_LIBRETRO_CORE", m_libretroCore.toLocal8Bit());
    const struct CoreEnv {
        ~CoreEnv() { qunsetenv("PIST_LIBRETRO_CORE"); }
    } coreEnv;
    LibretroBackend backend;
    // Connect before the action. A stop emitted from the owner thread is
    // queued, and a connection made after that emit misses it.
    auto waitUntilStopped = [&backend](const std::function<void()> &action) {
        QEventLoop loop;
        bool got = false;
        const QMetaObject::Connection conn = connect(
            &backend, &IDebugBackend::stoppedChanged, &loop, [&](bool stopped) {
                if (stopped) {
                    got = true;
                    loop.quit();
                }
            });
        action();
        QTimer::singleShot(20000, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(conn);
        return got;
    };

    SessionConfig config;
    config.tosPath = tos;
    config.programPath = prgPath;
    config.gemdosDir = dir.path();
    config.machine = QStringLiteral("st");
    config.memSizeMiB = 1;
    QString error;
    QVERIFY(waitUntilStopped([&] {
        QVERIFY2(backend.start(config, &error), qPrintable(error));
    }));

    MachineState state;
    {
        QEventLoop loop;
        const QMetaObject::Connection conn = connect(
            &backend, &IDebugBackend::stateUpdated, &loop, [&](const MachineState &got) {
                state = got;
                loop.quit();
            });
        backend.readBasepage();
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        loop.exec();
        disconnect(conn);
    }
    QVERIFY(state.regs.valid);
    QVERIFY(state.hasBases());
    QVERIFY(state.pc < 0xe00000u);
    QCOMPARE(state.pc, state.textBase + 2);

    backend.armBreakpoint(QStringLiteral("b pc = $%1").arg(state.pc, 0, 16));
    QVERIFY(waitUntilStopped([&] { backend.resume(); }));
    QVERIFY(backend.isStopped());

    QVERIFY(waitUntilStopped([&] { backend.step(); }));
    QVERIFY(backend.isStopped());

    backend.stop();
    qunsetenv("PIST_LIBRETRO_CORE");
}

void TstTosRom::reportsNoBundledDirWhenAbsent()
{
    // A source build has no share/emutos, and must not invent one. Reported
    // paths are shown to the user as "directories searched", so a fabricated
    // entry would be a lie in the diagnostics.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString binDir = QDir(tmp.path()).filePath(QStringLiteral("bin"));
    QVERIFY(QDir().mkpath(binDir));

    QVERIFY(paths::bundledDataSearchPaths(binDir).isEmpty());
    QVERIFY(paths::bundledDataSearchPaths(QString()).isEmpty());
}

void TstTosRom::tosSearchPathsIncludesBundledDir()
{
    // The wiring test, and the one that matters: the helper above can be correct
    // while tosSearchPaths() never calls it, which is exactly the shape of the bug
    // that shipped — a ROM in every archive that nothing looked for. This drives
    // the real function against a directory laid out the way an archive unpacks.
    //
    // It has to create the directory beside the running test binary, because
    // tosSearchPaths() always starts from the executable's own location. The path
    // is removed again on every exit path; a leftover would be a directory in the
    // build tree, which is generated and disposable.
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString shareDir = QDir(appDir).filePath(QStringLiteral("share/emutos"));

    if (QFileInfo::exists(shareDir)) {
        QSKIP("a share/emutos already exists beside the test binary; not touching it");
    }
    QVERIFY(QDir().mkpath(shareDir));

    const QString rom = writeRom(shareDir, QStringLiteral("etos1024k.img"), 0x0104,
                                /*emuTos=*/true);
    QVERIFY(!rom.isEmpty());

    const QStringList found = paths::tosSearchPaths();

    // Unwind in the only order that works: the file, then its directory, then the
    // one created for it. rmdir refuses a non-empty directory, so doing this the
    // other way round would silently leave both behind.
    QFile::remove(rom);
    QDir().rmdir(shareDir);
    QDir(appDir).rmdir(QStringLiteral("share"));

    const QString expected = QDir::cleanPath(shareDir);
    QVERIFY2(found.contains(expected),
             qPrintable(QStringLiteral("share/emutos beside the executable was not searched; "
                                       "searched: %1").arg(found.join(QStringLiteral(", ")))));
}

QTEST_MAIN(TstTosRom)
#include "tst_tosrom.moc"
