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

QStringList tosSearchPaths()
{
    QStringList dirs;

    // 1. Explicit override. This is what the integration test and anyone with
    //    ROMs somewhere unusual should use.
    const QString override =
        QProcessEnvironment::systemEnvironment().value(QString::fromLatin1(kTosDirEnvVar));
    appendUnique(dirs, override);

    // 2. The OS data location for Hatari. Covers /usr/share/hatari on Linux,
    //    /usr/local/share/hatari, and the Homebrew prefix on macOS.
    const QStringList dataLocs =
        QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const QString &base : dataLocs)
        appendUnique(dirs, base + QStringLiteral("/hatari"));

    // 3. Platform conventions Hatari packages use on macOS.
#ifdef Q_OS_MACOS
    appendUnique(dirs, QStringLiteral("/opt/homebrew/share/hatari"));
    appendUnique(dirs, QStringLiteral("/usr/local/share/hatari"));
    appendUnique(dirs, QStringLiteral("/opt/local/share/hatari"));
#endif

    // 4. Custom install prefixes: <prefix>/share/hatari for a hatari at
    //    <prefix>/bin/hatari.
    const QString hatari = QStandardPaths::findExecutable(QStringLiteral("hatari"));
    if (!hatari.isEmpty()) {
        const QDir exeDir(QFileInfo(hatari).absolutePath());
        appendUnique(dirs, exeDir.absoluteFilePath(QStringLiteral("../share/hatari")));

        // 5. Beside the executable, which is how the Windows and macOS release
        //    bundles are laid out. On Linux this would mean scanning /usr/bin,
        //    which is not where data files live.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        appendUnique(dirs, exeDir.absolutePath());
#endif
    }

    return dirs;
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
