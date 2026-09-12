// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Project settings round-trip. The important property is that everything a user
// configures survives save/load, because the alternative is silent loss of build
// configuration between sessions.

#include "emu/Paths.h"
#include "project/ProjectSettings.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <filesystem>

using namespace pist;

class TstSettings : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsEverything();
    void rejectsMalformedFile();
    void projectFileSitsBesideSource();
    void appliesDefaultsForMissingKeys();

    // Session directory lifecycle. These matter because removing one is a
    // recursive delete, so the guard against deleting outside the session base
    // is as important as the removal itself.
    void removesSessionDirInsideBase();
    void refusesToRemoveOutsideSessionBase();
    void prunesOnlyOldSessions();
};

void TstSettings::roundTripsEverything()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("proj.pistproject"));

    ProjectSettings original;
    original.sourceFile = QStringLiteral("/tmp/proj.s");
    original.includePaths = {QStringLiteral("/inc/one"), QStringLiteral("/inc/two")};
    original.defines = {QStringLiteral("DEBUG"), QStringLiteral("VERSION=2")};
    original.cpu = QStringLiteral("68020");
    original.extraBuildArgs = {QStringLiteral("-align"), QStringLiteral("-spaces")};
    original.machine = Machine::Ste;
    original.monitor = QStringLiteral("rgb");
    original.memSizeMiB = 4;
    original.tosPath = QStringLiteral("/roms/tos162.img");
    original.hardDiskImage = QStringLiteral("/disks/hd.img");
    original.fastForward = false;
    original.extraEmulatorArgs = {QStringLiteral("--force-bpp"), QStringLiteral("1")};

    QString error;
    QVERIFY2(settings::save(original, path, &error), qPrintable(error));

    ProjectSettings loaded;
    QVERIFY2(settings::load(&loaded, path, &error), qPrintable(error));

    QCOMPARE(loaded.includePaths, original.includePaths);
    QCOMPARE(loaded.defines, original.defines);
    QCOMPARE(loaded.cpu, original.cpu);
    QCOMPARE(loaded.extraBuildArgs, original.extraBuildArgs);
    QCOMPARE(loaded.machine, original.machine);
    QCOMPARE(loaded.monitor, original.monitor);
    QCOMPARE(loaded.memSizeMiB, original.memSizeMiB);
    QCOMPARE(loaded.tosPath, original.tosPath);
    QCOMPARE(loaded.hardDiskImage, original.hardDiskImage);
    QCOMPARE(loaded.fastForward, original.fastForward);
    QCOMPARE(loaded.extraEmulatorArgs, original.extraEmulatorArgs);
}

void TstSettings::rejectsMalformedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("broken.pistproject"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{ this is not json");
    f.close();

    ProjectSettings settings;
    QString error;
    QVERIFY2(!settings::load(&settings, path, &error),
             "a malformed project file must be reported, not silently ignored");
    QVERIFY(!error.isEmpty());
}

void TstSettings::projectFileSitsBesideSource()
{
    const QString project = settings::projectFileFor(QStringLiteral("/home/x/tests/prog.s"));
    QCOMPARE(project, QStringLiteral("/home/x/tests/prog.pistproject"));
    QVERIFY(settings::projectFileFor(QString()).isEmpty());
}

// A project file written by an older version, or hand-edited, must not produce an
// unusable configuration.
void TstSettings::appliesDefaultsForMissingKeys()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("minimal.pistproject"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"version\":1}");
    f.close();

    ProjectSettings settings;
    QString error;
    QVERIFY2(settings::load(&settings, path, &error), qPrintable(error));

    QCOMPARE(settings.machine, Machine::St);
    QCOMPARE(settings.cpu, QStringLiteral("68000"));
    QCOMPARE(settings.monitor, QStringLiteral("mono"));
    QVERIFY(settings.includePaths.isEmpty());
    QVERIFY(settings.fastForward);
}

void TstSettings::removesSessionDirInsideBase()
{
    const QString dir = paths::sessionBaseDir() + QStringLiteral("/test-remove-me");
    QVERIFY(QDir().mkpath(dir + QStringLiteral("/nested")));
    QFile f(dir + QStringLiteral("/nested/file.txt"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    QVERIFY(QFileInfo::exists(dir));
    paths::removeSessionDir(dir);
    QVERIFY2(!QFileInfo::exists(dir), "a session directory must be removed completely");
}

// The removal is recursive, so a path outside the session base must be refused
// outright rather than trusted. This is the guard that stops a bad path turning
// into a delete somewhere important.
void TstSettings::refusesToRemoveOutsideSessionBase()
{
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    const QString victim = outside.filePath(QStringLiteral("precious"));
    QVERIFY(QDir().mkpath(victim));
    QFile f(victim + QStringLiteral("/keep.txt"));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    paths::removeSessionDir(victim);
    QVERIFY2(QFileInfo::exists(victim),
             "a directory outside the session base must not be deleted");

    // Empty and non-existent paths are no-ops rather than errors.
    paths::removeSessionDir(QString());
    paths::removeSessionDir(QStringLiteral("/"));
    paths::removeSessionDir(outside.filePath(QStringLiteral("does-not-exist")));
}

void TstSettings::prunesOnlyOldSessions()
{
    const QString base = paths::sessionBaseDir();
    QVERIFY(QDir().mkpath(base));

    const QString oldDir = base + QStringLiteral("/test-old-session");
    const QString newDir = base + QStringLiteral("/test-new-session");
    QVERIFY(QDir().mkpath(oldDir));
    QVERIFY(QDir().mkpath(newDir));

    // Backdate one directory well past the cutoff. The prune reads the
    // *directory's* modification time, and QFile cannot set that, so use
    // std::filesystem, which handles directories.
    std::error_code ec;
    std::filesystem::last_write_time(
        oldDir.toStdString(),
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(4), ec);
    QVERIFY2(!ec, "could not backdate the test directory");

    paths::pruneStaleSessions(120);

    QVERIFY2(!QFileInfo::exists(oldDir), "a stale session must be pruned");
    QVERIFY2(QFileInfo::exists(newDir),
             "a recent session must survive: another instance may own it");

    QDir(newDir).removeRecursively();
}

QTEST_MAIN(TstSettings)
#include "tst_settings.moc"
