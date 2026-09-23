// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/Paths.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>

#include <cerrno>
#include <csignal>

#ifdef Q_OS_WIN
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace pist {
namespace paths {

namespace {

void appendUnique(QStringList &list, const QString &path)
{
    if (path.isEmpty())
        return;
    const QString clean = QDir::cleanPath(path);
    // Must be a directory: an executable path (e.g. /usr/bin/hatari) exists but
    // is not somewhere ROMs live, and listing it in the "directories searched"
    // message would be misleading.
    if (!QFileInfo(clean).isDir())
        return;
    if (!list.contains(clean))
        list.append(clean);
}

/// Where extracted floppy documents live, without creating anything.
///
/// Split from `documentExtractDir()` so the sweep can look at the location
/// without creating it on every launch.
QString documentExtractPath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.cache");
    return base + QStringLiteral("/PiST/documents");
}

/// Newest modification time of `dir` or of anything directly inside it.
///
/// A directory's own mtime only advances when an entry is added or removed, so
/// ageing a per-image document directory by that alone would delete a document
/// that has been edited every day for a week — editing a file does not touch its
/// parent — which is exactly how an extracted document tab gets used. Each
/// extracted document is written directly into its per-image directory
/// (`extractedFloppyPath` replaces '/' in the disk entry path with '_'), so one
/// level of entries is what there is to inspect.
QDateTime lastActivity(const QFileInfo &dir)
{
    QDateTime newest = dir.lastModified();
    const QFileInfoList entries = QDir(dir.absoluteFilePath())
                                      .entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries
                                                     | QDir::Hidden);
    for (const QFileInfo &entry : entries)
        newest = qMax(newest, entry.lastModified());
    return newest;
}

/// Whether `pid` names a process that is still running.
///
/// A session directory is named `<pid>-<counter>`, so the process that owns it
/// can be recovered from the name — no lock file, no extra bookkeeping. This is
/// a heuristic, not a guarantee: a pid can be recycled by an unrelated process,
/// and then a stale directory survives one more prune. That is the safe
/// direction, because the alternative — deleting a live session's socket out
/// from under a running emulator — is unrecoverable.
///
/// On Unix signal 0 asks the kernel about the process without touching it; a
/// process owned by somebody else (EPERM) is still alive. On Windows opening
/// the process for a status query is the equivalent, and a process that has
/// exited cannot be opened or no longer reports STILL_ACTIVE.
bool processIsRunning(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    const HANDLE handle =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!handle)
        return false;
    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(handle, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM;
#endif
}

} // namespace

QStringList bundledDataSearchPaths(const QString &applicationDir)
{
    QStringList dirs;
    if (applicationDir.isEmpty())
        return dirs;

    // Four levels reaches `share` from every layout the release archives use:
    //   <bundle>/bin/pist                      -> ../share
    //   <mount>/usr/bin/pist                   -> ../../share
    //   <bundle>/pist.app/Contents/MacOS/pist  -> ../../../share
    // One level beyond the deepest of those costs a couple of stat calls and
    // tolerates an extra wrapper directory, so the walk is deliberately shallow
    // but not exactly fitted to today's layouts.
    QDir up(applicationDir);
    for (int level = 0; level < 4; ++level) {
        for (const char *sub : {"share/emutos", "share/hatari"}) {
            const QString candidate =
                QDir::cleanPath(up.absoluteFilePath(QLatin1String(sub)));
            if (QFileInfo(candidate).isDir() && !dirs.contains(candidate))
                dirs.append(candidate);
        }
        if (!up.cdUp())
            break;
    }
    return dirs;
}

QStringList tosSearchPaths()
{
    QStringList dirs;

    // 1. Explicit override. This is what the integration test and anyone with
    //    ROMs somewhere unusual should use.
    const QString override =
        QProcessEnvironment::systemEnvironment().value(QString::fromLatin1(kTosDirEnvVar));
    appendUnique(dirs, override);

    // 2. Data shipped beside the application. Every release archive puts the ROM
    //    in a `share/emutos` directory next to the executable's parent: a tarball
    //    unpacks to `bin/pist` beside `share/emutos`, and an AppImage mounts
    //    `usr/bin/pist` beside `usr/share/emutos`. Walking up covers both, and the
    //    macOS bundle (`Contents/MacOS/pist` beside `../share`) as well, without
    //    hard-coding any one of those layouts.
    //
    //    This deliberately precedes the system locations. The archive's own ROM is
    //    the one the release process verified against the GEMDOS-hard-disk
    //    autostart path, so preferring it is what makes a fresh download run with
    //    nothing else installed. Distribution ROMs are still found and listed
    //    afterwards and remain selectable, and an explicit PIST_TOS_DIR still
    //    wins over both.
    //
    //    Without this branch the ROM travelled inside every archive but was
    //    invisible: the archives unpack to `share/emutos`, and nothing searched
    //    there. The bug survived release checking because the verification step
    //    set PIST_TOS_DIR, which skipped the discovery logic being tested.
    for (const QString &dir : bundledDataSearchPaths(QCoreApplication::applicationDirPath()))
        appendUnique(dirs, dir);

    // 2.5. The per-user directory a first-run EmuTOS download installs into.
    //    After the bundled data deliberately: an archive's own ROM is the one
    //    its release process verified, so it stays the default when both exist.
    appendUnique(dirs, suggestedRomDir());

    // 3. The OS data location for Hatari. Covers /usr/share/hatari on Linux,
    //    /usr/local/share/hatari, and the Homebrew prefix on macOS.
    const QStringList dataLocs =
        QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &base : dataLocs)
        appendUnique(dirs, base + QStringLiteral("/hatari"));

    // 4. Platform conventions Hatari packages use on macOS.
#ifdef Q_OS_MACOS
    appendUnique(dirs, QStringLiteral("/opt/homebrew/share/hatari"));
    appendUnique(dirs, QStringLiteral("/usr/local/share/hatari"));
    appendUnique(dirs, QStringLiteral("/opt/local/share/hatari"));
#endif

    // 5. Custom install prefixes: <prefix>/share/hatari for a hatari at
    //    <prefix>/bin/hatari.
    const QString hatari = QStandardPaths::findExecutable(QStringLiteral("hatari"));
    if (!hatari.isEmpty()) {
        const QDir exeDir(QFileInfo(hatari).absolutePath());
        appendUnique(dirs, exeDir.absoluteFilePath(QStringLiteral("../share/hatari")));

        // 6. Beside the executable, which is how the Windows and macOS release
        //    bundles are laid out. On Linux this would mean scanning /usr/bin,
        //    which is not where data files live.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        appendUnique(dirs, exeDir.absolutePath());
#endif
    }

    return dirs;
}

QString suggestedRomDir()
{
    // GenericDataLocation, matching toolchain::suggestedInstallDir(): the two
    // sit side by side as ~/.local/share/PiST/{tools,roms}.
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/PiST/roms");
}

QString sessionBaseDir()
{
    // Platform temp location: /tmp on Linux, /var/folders/... on macOS, and
    // %LOCALAPPDATA%\Temp on Windows. A literal "/tmp" would resolve to
    // <drive>:\tmp on Windows, which normally cannot be created.
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    base = QDir(base).absoluteFilePath(QStringLiteral("pist"));
#ifdef Q_OS_MACOS
    // macOS $TMPDIR is /var/folders/<two>/.../T — 50+ characters before our
    // name — and a session's control socket sits another ~25 deeper still.
    // That overflows sockaddr_un's 104-byte sun_path (measured on CI:
    // QLocalServer::listen fails, the session never starts). /tmp is the
    // short, sanctioned macOS temp path.
    if (base.length() > 60)
        base = QStringLiteral("/tmp/pist");
#endif
    return base;
}

QString documentExtractDir()
{
    // GenericCacheLocation, beside the data directory suggestedRomDir() uses:
    // ~/.cache/PiST/documents on Linux and the equivalent elsewhere. A cache
    // location rather than the session temp tree because the files are
    // regenerable from the floppy image, but a *cache* rather than temp because
    // an open editor tab holds one and pruneStaleSessions() must never reach
    // them (it only walks sessionBaseDir()) — pruneExtractedDocuments() is what
    // collects them instead, on its own much longer clock.
    const QString dir = documentExtractPath();
    // Created on first use, including parents: MainWindow derives a file path
    // from this directory before it opens the containing directory itself.
    QDir().mkpath(dir);
    return dir;
}

bool ensureDirectory(const QString &dir, QString *error)
{
    if (dir.isEmpty()) {
        if (error)
            *error = QObject::tr("no directory given");
        return false;
    }
    if (QFileInfo(dir).isDir())
        return true;
    if (QDir().mkpath(dir))
        return true;
    if (error)
        *error = QObject::tr("cannot create directory '%1'").arg(dir);
    return false;
}

void removeSessionDir(const QString &dir)
{
    if (dir.isEmpty())
        return;
    const QFileInfo info(dir);
    if (!info.isDir())
        return;

    // Refuse to delete outside the session base: a bad path must not turn this
    // into a recursive delete somewhere important.
    const QString base = QDir(sessionBaseDir()).absolutePath();
    if (!info.absoluteFilePath().startsWith(base + QLatin1Char('/')))
        return;

    QDir(dir).removeRecursively();
}

void pruneStaleSessions(int maxAgeMinutes)
{
    // The extracted documents have their own lifetime rule, but the same
    // startup moment is when it runs; see pruneExtractedDocuments().
    pruneExtractedDocuments();

    const QDir base(sessionBaseDir());
    if (!base.exists())
        return;

    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-60LL * maxAgeMinutes);
    const QFileInfoList entries =
        base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

    for (const QFileInfo &entry : entries) {
        if (entry.lastModified() >= cutoff)
            continue;
        // Age alone is not enough, and age alone broke the invariant this
        // function exists to keep (Paths.h: another PiST instance may be
        // running, and its live session must not be removed from under it). A
        // session directory is not written to after launch — the bootstrap
        // script, control socket and config tree are all created at start — so
        // a debugger left stopped for hours has the same timestamp as a crash,
        // and pruning it unlinks the socket and the HOME tree Hatari is running
        // from. The name still carries the owning process
        // (`<pid>-<counter>`, MainWindow::makeSessionDir), so a directory whose
        // pid is still running belongs to a live instance and is left alone.
        // Non-numeric prefixes keep the old age-only behaviour.
        const QString name = entry.fileName();
        const int dash = name.indexOf(QLatin1Char('-'));
        bool parsed = false;
        const qint64 pid = dash > 0 ? name.left(dash).toLongLong(&parsed) : 0;
        if (parsed && processIsRunning(pid))
            continue;
        QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

void pruneExtractedDocuments(int maxAgeMinutes)
{
    const QDir base(documentExtractPath());
    if (!base.exists())
        return;

    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-60LL * maxAgeMinutes);
    // One subdirectory per image (documentExtractDir()), so removing a whole
    // entry is what removes a document; the directory itself stays. Age each
    // candidate by the newest thing inside it, not by the directory: a document
    // edited today under a directory that has not gained an entry in a week is
    // an open tab, not an orphan.
    const QFileInfoList entries =
        base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

    for (const QFileInfo &entry : entries) {
        if (lastActivity(entry) < cutoff)
            QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

} // namespace paths
} // namespace pist
