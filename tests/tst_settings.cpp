// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Project settings round-trip. The important property is that everything a user
// configures survives save/load, because the alternative is silent loss of build
// configuration between sessions.

#include "emu/Paths.h"
#include "project/ProjectSettings.h"
#include "toolchain/Toolchain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <chrono>
#include <filesystem>

#ifndef Q_OS_WIN
#  include <csignal>
#  include <sys/resource.h>
#endif

using namespace pist;

namespace {

#ifndef Q_OS_WIN
/// Make every file write in this process fail, the way a full disk does.
///
/// The point is to fail *after* the destination has been opened: the old
/// implementation opened the project file with Truncate and only then wrote, so
/// a write that cannot complete leaves the file at zero bytes. RLIMIT_FSIZE
/// reproduces that exactly (write() fails with EFBIG), and is not affected by
/// whether the tests happen to run as root, unlike making a directory read-only.
///
/// Exceeding the limit raises SIGXFSZ, whose default action is to kill the
/// process before write() can return, so the signal is ignored for the duration.
class WriteLimit
{
public:
    explicit WriteLimit(rlim_t bytes)
    {
        if (::getrlimit(RLIMIT_FSIZE, &m_old) != 0)
            return;
        rlimit limited = m_old;
        limited.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limited) != 0)
            return;
        m_oldHandler = std::signal(SIGXFSZ, SIG_IGN);
        m_armed = true;
    }

    ~WriteLimit()
    {
        if (!m_armed)
            return;
        ::setrlimit(RLIMIT_FSIZE, &m_old);
        std::signal(SIGXFSZ, m_oldHandler);
    }

    bool armed() const { return m_armed; }

private:
    rlimit m_old{};
    void (*m_oldHandler)(int) = SIG_DFL;
    bool m_armed = false;
};
#endif

/// A pid that no process can be running as: well past Linux's pid_max and every
/// other supported platform's pid_t range.
qint64 deadPid()
{
    return 0x7fffffff;
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

/// Move the process to another directory for the length of a test, and put it
/// back on the way out. A bare QDir::setCurrent() would leak the change into
/// every later slot, because a failing QCOMPARE/QVERIFY only returns from the
/// test function — it does not unwind past it.
class CwdGuard
{
public:
    explicit CwdGuard(const QString &dir)
        : m_old(QDir::currentPath())
    {
        QDir::setCurrent(dir);
    }

    ~CwdGuard() { QDir::setCurrent(m_old); }

private:
    QString m_old;
};

} // namespace

class TstSettings : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsEverything();
    void rejectsMalformedFile();
    void projectFileSitsBesideSource();
    void appliesDefaultsForMissingKeys();

    // A relative path in the file means "beside the project", so it has to be
    // read against the project file's directory, not the process's.
    void resolvesRelativePathsAgainstProjectFile();

    // A file written by a newer PiST must be refused, not silently downgraded and
    // then overwritten, and a malformed entry must fail the load by name.
    void refusesNewerProjectVersion();
    void rejectsMalformedEntries();

    // A hand-edited or third-party project file must not reach the emulator
    // command line with an impossible monitor, RAM size or machine.
    void clampsHostileEmulatorValues();

    // Session directory lifecycle. These matter because removing one is a
    // recursive delete, so the guard against deleting outside the session base
    // is as important as the removal itself.
    void removesSessionDirInsideBase();
    void refusesToRemoveOutsideSessionBase();
    void prunesOnlyOldSessions();
    void pruneKeepsLiveSession();
    void documentExtractDirOutsideSessions();
    void prunesOldExtractedDocuments();
    void keepsRecentlyEditedExtractedDocument();

    // Saving a project must never be able to destroy the copy already on disk.
    void saveFailureKeepsPreviousFile();

    // Toolchain resolution. A missing tool must be reported as missing rather
    // than silently falling back to a bare name, which surfaced later as an
    // opaque process-start failure.
    void findsToolsOnPath();
    void reportsMissingToolAsMissing();
    void explicitPathWinsOverDiscovery();
    void missingExplicitPathIsReported();
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
    original.additionalSources = {QStringLiteral("/p/utils.s"), QStringLiteral("/p/data.s")};
    original.assemblerPath = QStringLiteral("/opt/vasm/vasmm68k_mot");
    original.linkerPath = QStringLiteral("/usr/local/bin/vlink");
    original.machine = Machine::Ste;
    original.monitor = QStringLiteral("rgb");
    original.memSizeMiB = 4;
    original.tosPath = QStringLiteral("/roms/tos162.img");
    original.hatariPath = QStringLiteral("/opt/hatari/bin/hatari");
    original.hardDiskImage = QStringLiteral("/disks/hd.img");
    // Both drives, and a second entry on B with an empty slot: the schema
    // stores the list as written, so a dropped key loses the user's mounts.
    original.floppyImages = {QStringLiteral("/disks/boot.st"), QStringLiteral("/disks/data.msa")};
    original.fastForward = false;
    original.extraEmulatorArgs = {QStringLiteral("--force-bpp"), QStringLiteral("1")};
    original.debugBackend = QStringLiteral("hrdb");

    QString error;
    QVERIFY2(settings::save(original, path, &error), qPrintable(error));

    ProjectSettings loaded;
    QVERIFY2(settings::load(&loaded, path, &error), qPrintable(error));

    QCOMPARE(loaded.includePaths, original.includePaths);
    QCOMPARE(loaded.defines, original.defines);
    QCOMPARE(loaded.cpu, original.cpu);
    QCOMPARE(loaded.extraBuildArgs, original.extraBuildArgs);
    QCOMPARE(loaded.additionalSources, original.additionalSources);
    QCOMPARE(loaded.assemblerPath, original.assemblerPath);
    QCOMPARE(loaded.linkerPath, original.linkerPath);
    QCOMPARE(loaded.machine, original.machine);
    QCOMPARE(loaded.monitor, original.monitor);
    QCOMPARE(loaded.memSizeMiB, original.memSizeMiB);
    QCOMPARE(loaded.tosPath, original.tosPath);
    QCOMPARE(loaded.hatariPath, original.hatariPath);
    QCOMPARE(loaded.hardDiskImage, original.hardDiskImage);
    QCOMPARE(loaded.floppyImages, original.floppyImages);
    QCOMPARE(loaded.fastForward, original.fastForward);
    QCOMPARE(loaded.extraEmulatorArgs, original.extraEmulatorArgs);
    QCOMPARE(loaded.debugBackend, original.debugBackend);
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
    // Asserted from a real path: "/home/x/tests/prog.s" is not absolute on
    // Windows (no drive), so a hardcoded expectation fails there for reasons
    // unrelated to the behaviour under test.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("prog.s"));
    const QString project = settings::projectFileFor(source);

    QVERIFY2(QFileInfo(project).isAbsolute(), qPrintable(project));
    QVERIFY(QFileInfo(project).absolutePath() == QFileInfo(source).absolutePath());
    QCOMPARE(QFileInfo(project).fileName(), QStringLiteral("prog.pistproject"));

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

// A relative `-I` or additional source in the file means "beside the project",
// which is what makes the project portable. It used to be read verbatim and then
// resolved against PiST's working directory, so the same project assembled
// differently depending on where the IDE happened to be started.
void TstSettings::resolvesRelativePathsAgainstProjectFile()
{
    QTemporaryDir projectDir;
    QTemporaryDir elsewhere;
    QVERIFY(projectDir.isValid());
    QVERIFY(elsewhere.isValid());

    const QString path = projectDir.filePath(QStringLiteral("proj.pistproject"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArrayLiteral(
        "{\"version\":1,\"build\":{\"includePaths\":[\"inc\",\"/abs/inc\"],"
        "\"additionalSources\":[\"src/extra.s\"]}}"));
    f.close();

    // Load from somewhere else entirely: the result must not depend on this.
    CwdGuard guard(elsewhere.path());

    ProjectSettings loaded;
    QString error;
    QVERIFY2(settings::load(&loaded, path, &error), qPrintable(error));

    // Relative entries are rebased onto the project directory; an absolute entry
    // is left exactly as written.
    QCOMPARE(loaded.includePaths, QStringList({projectDir.filePath(QStringLiteral("inc")),
                                               QStringLiteral("/abs/inc")}));
    QCOMPARE(loaded.additionalSources,
             QStringList({projectDir.filePath(QStringLiteral("src/extra.s"))}));
    QVERIFY2(QFileInfo(loaded.additionalSources.first()).isAbsolute(),
             qPrintable(loaded.additionalSources.first()));

    // save() writes the strings it is handed, verbatim: it must not re-relativise
    // them against the project, which would rewrite the file behind the user.
    ProjectSettings relative;
    relative.includePaths = {QStringLiteral("inc")};
    relative.additionalSources = {QStringLiteral("src/extra.s")};
    QVERIFY2(settings::save(relative, path, &error), qPrintable(error));

    QFile written(path);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QJsonObject build = QJsonDocument::fromJson(written.readAll())
                                  .object()
                                  .value(QStringLiteral("build"))
                                  .toObject();
    QCOMPARE(build.value(QStringLiteral("includePaths")).toArray().at(0).toString(),
             QStringLiteral("inc"));
    QCOMPARE(build.value(QStringLiteral("additionalSources")).toArray().at(0).toString(),
             QStringLiteral("src/extra.s"));
}

// `version` was written but never read, so a file from a newer PiST loaded with
// every key this build does not understand dropped, and the next save wrote that
// truncated project back over it. It must be refused instead — naming what it
// found and what this build supports — and refusing must not touch the file.
void TstSettings::refusesNewerProjectVersion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("future.pistproject"));

    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArrayLiteral("{\"version\":2,\"build\":{\"cpu\":\"68030\"},"
                              "\"somedayKey\":{\"a\":1}}"));
    f.close();

    const QByteArray before = readBytes(path);
    QVERIFY2(!before.isEmpty(), "the fixture must have a project file to preserve");

    ProjectSettings settings;
    QString error;
    QVERIFY2(!settings::load(&settings, path, &error),
             "a project file from a newer PiST must not load");
    QVERIFY2(error.contains(QStringLiteral("project version 2")), qPrintable(error));
    QVERIFY2(error.contains(QStringLiteral("version 1")), qPrintable(error));

    // Refusing is not the same as rewriting: this file is the only copy of that
    // project, and nothing may have saved over it.
    QCOMPARE(readBytes(path), before);

    // The current version, and the unversioned files older builds wrote, both
    // still load exactly as before.
    const auto loadFromText = [&dir](const char *name, const QByteArray &json,
                                     ProjectSettings *out, QString *error) {
        const QString projectPath = dir.filePath(QLatin1String(name));
        QFile file(projectPath);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(json);
        file.close();
        return settings::load(out, projectPath, error);
    };

    ProjectSettings current;
    QVERIFY2(loadFromText("current.pistproject",
                          QByteArrayLiteral("{\"version\":1,\"build\":{\"cpu\":\"68000\"}}"),
                          &current, &error),
             qPrintable(error));

    ProjectSettings unversioned;
    QVERIFY2(loadFromText("legacy.pistproject",
                          QByteArrayLiteral("{\"build\":{\"cpu\":\"68010\"}}"), &unversioned,
                          &error),
             qPrintable(error));
    QCOMPARE(unversioned.cpu, QStringLiteral("68010"));
}

// A malformed entry used to be coerced: a non-array list, or a non-string element
// inside one, became an empty `-I`/`-D` argument that reached the assembler with
// nothing to explain where it came from. Each case must now fail the load and
// name the offending key, so the file can be fixed rather than silently halved.
void TstSettings::rejectsMalformedEntries()
{
    const auto loadText = [](const QByteArray &json, QString *error) {
        QTemporaryDir dir;
        if (!dir.isValid())
            return false;
        const QString path = dir.filePath(QStringLiteral("malformed.pistproject"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(json);
        f.close();
        ProjectSettings settings;
        return settings::load(&settings, path, error);
    };

    const QList<QPair<QByteArray, QString>> cases = {
        // A list where a scalar belongs, and its mirror.
        {QByteArrayLiteral("{\"version\":1,\"build\":{\"includePaths\":\"inc\"}}"),
         QStringLiteral("build.includePaths")},
        {QByteArrayLiteral("{\"version\":1,\"build\":{\"cpu\":3}}"), QStringLiteral("build.cpu")},
        // A non-string element inside a list, named by index.
        {QByteArrayLiteral("{\"version\":1,\"build\":{\"defines\":[\"OK\",7]}}"),
         QStringLiteral("build.defines[1]")},
        {QByteArrayLiteral("{\"version\":1,\"emulator\":{\"floppyImages\":[\"/b.st\",null]}}"),
         QStringLiteral("emulator.floppyImages[1]")},
        // Scalars of the wrong type, and a container of the wrong type.
        {QByteArrayLiteral("{\"version\":1,\"emulator\":{\"memSizeMiB\":\"lots\"}}"),
         QStringLiteral("emulator.memSizeMiB")},
        {QByteArrayLiteral("{\"version\":1,\"emulator\":{\"fastForward\":\"yes\"}}"),
         QStringLiteral("emulator.fastForward")},
        {QByteArrayLiteral("{\"version\":1,\"build\":[\"not\",\"an\",\"object\"]}"),
         QStringLiteral("build")},
    };

    for (const auto &entry : cases) {
        QString error;
        QVERIFY2(!loadText(entry.first, &error),
                 qPrintable(QStringLiteral("must reject: %1").arg(QString::fromUtf8(entry.first))));
        QVERIFY2(error.contains(entry.second), qPrintable(error));
    }

    // The well-typed equivalent still loads: the strictness is about shape, not
    // about the values, which the emulator guards still clamp.
    QString error;
    QVERIFY2(loadText(QByteArrayLiteral("{\"version\":1,\"build\":{\"includePaths\":[\"inc\"]}}"),
                      &error),
             qPrintable(error));
}

// A hand-edited or third-party file is not trusted: a monitor Hatari does not
// know, an impossible RAM size or an unknown machine must never reach the
// emulator command line, where it aborts startup with nothing to explain it.
void TstSettings::clampsHostileEmulatorValues()
{
    const auto loadWith = [](const QByteArray &json, ProjectSettings *out) {
        QTemporaryDir dir;
        if (!dir.isValid())
            return false;
        const QString path = dir.filePath(QStringLiteral("hostile.pistproject"));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(json);
        f.close();
        QString error;
        return settings::load(out, path, &error);
    };

    // The file itself is well-formed: these are out-of-range values, not a
    // parse error, so load() succeeds and the guards decide what to keep.
    ProjectSettings settings;
    QVERIFY(loadWith(QByteArrayLiteral(
                         "{\"version\":1,\"build\":{\"cpu\":\"68020\"},"
                         "\"emulator\":{\"monitor\":\"xvga\",\"memSizeMiB\":9999,"
                         "\"machine\":\"amiga-500\"}}"),
                     &settings));
    // Each hostile value is refused by the value it would have to be: the
    // monitor falls back, the RAM clamps to the 14 MiB ceiling, and the machine
    // is not one this front end can launch.
    QCOMPARE(settings.monitor, QStringLiteral("mono"));
    QCOMPARE(settings.memSizeMiB, 14);
    QCOMPARE(settings.machine, Machine::St);

    // The other end of the RAM range: a negative size clamps to Hatari's
    // 0 = its own default rather than underflowing.
    ProjectSettings negative;
    QVERIFY(loadWith(QByteArrayLiteral("{\"version\":1,\"emulator\":{\"memSizeMiB\":-7}}"),
                     &negative));
    QCOMPARE(negative.memSizeMiB, 0);
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

// Age cannot tell a crashed session from one that is merely quiet: nothing
// writes to a session directory after launch, so an instance sitting in a
// debugger for hours has the same mtime as a crash. The name carries the owning
// pid, and a live one must keep its directory — deleting it would unlink the
// control socket and the HOME tree Hatari is running from.
void TstSettings::pruneKeepsLiveSession()
{
    const QString base = paths::sessionBaseDir();
    QVERIFY(QDir().mkpath(base));

    const QString liveDir =
        base + QStringLiteral("/%1-1").arg(QCoreApplication::applicationPid());
    const QString deadDir =
        base + QStringLiteral("/%1-2").arg(deadPid());
    QVERIFY(QDir().mkpath(liveDir));
    QVERIFY(QDir().mkpath(deadDir));

    // Both well past the cutoff, so age alone would remove either.
    std::error_code ec;
    const auto stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(4);
    std::filesystem::last_write_time(liveDir.toStdString(), stale, ec);
    QVERIFY2(!ec, "could not backdate the live session directory");
    std::filesystem::last_write_time(deadDir.toStdString(), stale, ec);
    QVERIFY2(!ec, "could not backdate the dead session directory");

    paths::pruneStaleSessions(120);

    QVERIFY2(QFileInfo::exists(liveDir),
             "a session directory owned by a running process must not be pruned");
    QVERIFY2(!QFileInfo::exists(deadDir),
             "a session directory owned by a process that is gone must be pruned");

    QDir(liveDir).removeRecursively();
}

// Extracted floppy documents are editable and are written back to the image, so
// they must not live anywhere pruneStaleSessions() walks.
void TstSettings::documentExtractDirOutsideSessions()
{
    const QString docs = paths::documentExtractDir();
    QVERIFY2(!docs.isEmpty(), "the document directory must resolve on every platform");
    QVERIFY2(QFileInfo(docs).isDir(),
             "the extracted-document directory must be created on demand");

    const QString base = QDir(paths::sessionBaseDir()).absolutePath();
    const QString absolute = QDir(docs).absolutePath();
    QVERIFY2(absolute != base && !absolute.startsWith(base + QLatin1Char('/')),
             qPrintable(QStringLiteral("'%1' is inside the pruned session tree '%2'")
                            .arg(absolute, base)));
}

// Moving the extracted documents out of the session tree also took them out of
// pruneStaleSessions()' reach, so they need their own lifetime rule: without one
// the orphaned per-image directories accumulate for good. The rule is an
// age-based sweep run from the same startup call.
void TstSettings::prunesOldExtractedDocuments()
{
    const QString base = paths::documentExtractDir();
    QVERIFY(QDir().mkpath(base));

    const QString oldDir = base + QStringLiteral("/test-old-doc-0000abcd");
    const QString newDir = base + QStringLiteral("/test-new-doc-0000abcd");
    QVERIFY(QDir().mkpath(oldDir));
    QVERIFY(QDir().mkpath(newDir));
    QFile doc(oldDir + QStringLiteral("/readme.txt"));
    QVERIFY(doc.open(QIODevice::WriteOnly));
    doc.write("x");
    doc.close();

    // Eight days back, directory *and* the document inside it: a genuinely
    // orphaned document, not one that was edited recently (see the sibling
    // case below, where only the directory is old).
    std::error_code ec;
    const auto stale = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8);
    std::filesystem::last_write_time(oldDir.toStdString(), stale, ec);
    QVERIFY2(!ec, "could not backdate the extracted document directory");
    std::filesystem::last_write_time(
        (oldDir + QStringLiteral("/readme.txt")).toStdString(), stale, ec);
    QVERIFY2(!ec, "could not backdate the extracted document");

    // The real startup call, so this covers the sweep being wired in as well as
    // the sweep itself.
    paths::pruneStaleSessions(120);

    QVERIFY2(!QFileInfo::exists(oldDir),
             "an orphaned extracted document must not accumulate forever");
    QVERIFY2(QFileInfo::exists(newDir),
             "a recent extracted document may back an open tab and must survive");
    QVERIFY2(QFileInfo(base).isDir(), "the document directory itself must survive");

    QDir(newDir).removeRecursively();
}

// The complement of the case above, and the one that matters for open tabs:
// editing a document does not touch its directory, because a directory's mtime
// moves only when an entry is added or removed. Ageing the directory alone
// therefore deletes a document that has been edited every day for a week.
void TstSettings::keepsRecentlyEditedExtractedDocument()
{
    const QString base = paths::documentExtractDir();
    QVERIFY(QDir().mkpath(base));

    const QString dir = base + QStringLiteral("/test-edited-doc-0000abcd");
    QVERIFY(QDir().mkpath(dir));
    const QString docPath = dir + QStringLiteral("/notes.txt");
    QFile doc(docPath);
    QVERIFY(doc.open(QIODevice::WriteOnly));
    doc.write("edited today");
    doc.close();

    // Only the directory is backdated; the document keeps today's timestamp,
    // exactly as a long-running edit leaves it.
    std::error_code ec;
    std::filesystem::last_write_time(
        dir.toStdString(),
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8), ec);
    QVERIFY2(!ec, "could not backdate the extracted document directory");
    QVERIFY2(QFileInfo(docPath).lastModified() > QDateTime::currentDateTime().addSecs(-3600),
             "the fixture depends on the document looking recently edited");

    paths::pruneStaleSessions(120);

    QVERIFY2(QFileInfo::exists(docPath),
             "an extracted document edited recently must survive even though its "
             "directory has not gained an entry in a week");

    QDir(dir).removeRecursively();
}

// The old save() opened the destination with Truncate and wrote into it, so any
// failure after the open destroyed the only copy of the project. This makes the
// write itself fail and checks the previous file is still there, byte for byte.
void TstSettings::saveFailureKeepsPreviousFile()
{
#ifdef Q_OS_WIN
    QSKIP("failing a write after the open needs POSIX RLIMIT_FSIZE");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("proj.pistproject"));

    ProjectSettings original;
    original.sourceFile = QStringLiteral("/tmp/keep.s");
    original.defines = {QStringLiteral("KEEP")};
    QString error;
    QVERIFY2(settings::save(original, path, &error), qPrintable(error));

    const QByteArray before = readBytes(path);
    QVERIFY2(!before.isEmpty(), "the fixture must have a project file to preserve");

    ProjectSettings changed = original;
    changed.defines = {QStringLiteral("CLOBBERED")};

    bool saved = true;
    {
        WriteLimit limit(0);
        QVERIFY2(limit.armed(), "could not arm the write limit");
        saved = settings::save(changed, path, &error);
    }

    // The file that was already there is the only copy of the project, so it is
    // what must survive. Pre-fix this is the failing assertion: the destination
    // was opened with Truncate and left at zero bytes.
    QVERIFY2(readBytes(path) == before,
             "a failed save must leave the previous project file intact");
    QVERIFY2(!saved, "a save that cannot reach the disk must report failure");
    QVERIFY2(!error.isEmpty(), "the failure must carry a reason");

    // And the file left behind must still be a usable project.
    ProjectSettings reloaded;
    QVERIFY2(settings::load(&reloaded, path, &error), qPrintable(error));
    QCOMPARE(reloaded.defines, original.defines);
#endif
}

void TstSettings::findsToolsOnPath()
{
    // vasm and hatari are not guaranteed present, so use a program that is: the
    // shell. The point is that discovery through PATH works at all.
    if (QStandardPaths::findExecutable(QStringLiteral("sh")).isEmpty())
        QSKIP("no sh on PATH");

    const QStringList paths = toolchain::searchPaths();
    QVERIFY(!paths.isEmpty());
    // The application directory and the per-user tools directory come first, so a
    // bundled copy takes precedence over whatever happens to be installed.
    QCOMPARE(paths.first(), QCoreApplication::applicationDirPath());
}

void TstSettings::reportsMissingToolAsMissing()
{
    // A name that cannot exist. Crucially this must yield an *empty* path, not a
    // bare name: the caller decides how to report it, and the preflight checks
    // rely on found() being honest.
    const ToolInfo info = toolchain::findAssembler(
        QStringLiteral("/nonexistent/definitely-not-here/vasmm68k_mot"));
    QVERIFY2(!info.found(), "a missing tool must report as not found");
    QVERIFY(info.path.isEmpty());
}

// An explicit path wins outright, including over a working system install: a user
// pointing PiST at a specific vasm must get that one, not the packaged one.
void TstSettings::explicitPathWinsOverDiscovery()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // Windows decides executability from the file's suffix, not its permission
    // bits, so a bare `vasmm68k_mot` is not an executable there. Use the
    // platform's own name for one.
#ifdef Q_OS_WIN
    const QString fake = dir.filePath(QStringLiteral("vasmm68k_mot.exe"));
#else
    const QString fake = dir.filePath(QStringLiteral("vasmm68k_mot"));
#endif

    QFile f(fake);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("#!/bin/sh\nexit 0\n");
    f.close();
    QVERIFY(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                             | QFileDevice::ExeOwner));
    QVERIFY2(QFileInfo(fake).isExecutable(), "the fixture must look executable");

    const ToolInfo info = toolchain::findAssembler(fake);
    QVERIFY(info.found());
    QCOMPARE(info.path, fake);
    QCOMPARE(info.name, QStringLiteral("vasmm68k_mot"));
}

// A configured path that no longer resolves must be reported, not quietly
// substituted with a different binary from PATH, which would be confusing.
void TstSettings::missingExplicitPathIsReported()
{
    const ToolInfo info = toolchain::findEmulator(QStringLiteral("/gone/hatari"));
    QVERIFY2(!info.found(), "a stale configured path must not silently fall back");
}

/// The session-directory cases delete recursively, and their subject is real
/// user state: `<temp>/pist` holds every running PiST instance's session tree
/// (control socket, staged floppies, Hatari's HOME), and the extracted-document
/// cache holds documents an editor tab may have open. Running this suite must
/// touch neither — measured before this redirect: a stale-named directory in the
/// real session base was deleted by `prunesOnlyOldSessions`, and the extracted
/// document cases create and prune directories in the real cache.
///
/// Both locations are redirected through the environment *before*
/// QCoreApplication exists, because QStandardPaths resolves and caches them on
/// first use. QStandardPaths::setTestModeEnabled() is deliberately not used: it
/// would also move the data root the toolchain cases resolve tools from, exactly
/// as tst_gui records for the same redirect.
int main(int argc, char *argv[])
{
    // macOS's own temp root is long enough that sessionBaseDir() falls back to
    // the literal /tmp/pist when a session path would overflow sockaddr_un, so
    // the sandbox has to be short as well: /tmp is the root that fallback uses.
#ifdef Q_OS_MACOS
    const QString tempRoot = QStringLiteral("/tmp");
#else
    const QString tempRoot = QDir::tempPath();
#endif
    QTemporaryDir sandbox(tempRoot + QStringLiteral("/pist-tst-settings-XXXXXX"));
    if (sandbox.isValid()) {
        const QByteArray path = QFile::encodeName(sandbox.path());
        // TempLocation: TMPDIR on Unix, TEMP/TMP on Windows.
        qputenv("TMPDIR", path);
        qputenv("TEMP", path);
        qputenv("TMP", path);
#ifdef Q_OS_LINUX
        // GenericCacheLocation is only environment-selectable on Linux
        // (XDG_CACHE_HOME); macOS and Windows fix it to the user's library or
        // appdata directory.
        qputenv("XDG_CACHE_HOME",
                QFile::encodeName(sandbox.filePath(QStringLiteral("cache"))));
#endif
    }

    QCoreApplication app(argc, argv);
    TstSettings testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_settings.moc"
