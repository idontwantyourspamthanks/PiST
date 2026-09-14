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
    return QDir(base).absoluteFilePath(QStringLiteral("pist"));
}

bool ensureDirectory(const QString &dir, QString *error)
{
    if (dir.isEmpty()) {
        if (error)
            *error = QStringLiteral("no directory given");
        return false;
    }
    if (QFileInfo(dir).isDir())
        return true;
    if (QDir().mkpath(dir))
        return true;
    if (error)
        *error = QStringLiteral("cannot create directory '%1'").arg(dir);
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
    const QDir base(sessionBaseDir());
    if (!base.exists())
        return;

    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-60LL * maxAgeMinutes);
    const QFileInfoList entries =
        base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);

    for (const QFileInfo &entry : entries) {
        if (entry.lastModified() < cutoff)
            QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

} // namespace paths
} // namespace pist
