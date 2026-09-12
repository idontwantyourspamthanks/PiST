// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#include "emu/Paths.h"

#include <QCoreApplication>
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
    if (!QFileInfo::exists(clean))
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

    // 3. Platform-specific conventions Hatari packages use.
#ifdef Q_OS_MACOS
    appendUnique(dirs, QStringLiteral("/opt/homebrew/share/hatari"));
    appendUnique(dirs, QStringLiteral("/usr/local/share/hatari"));
    appendUnique(dirs, QStringLiteral("/opt/local/share/hatari"));
#endif
#ifdef Q_OS_WIN
    // Windows builds keep data files next to the executable; handled in step 4.
#endif

    // 4. Beside the Hatari executable, which is how the Windows and macOS
    //    bundles are laid out.
    const QString hatari = QStandardPaths::findExecutable(QStringLiteral("hatari"));
    if (!hatari.isEmpty()) {
        const QDir exeDir(QFileInfo(hatari).absolutePath());
        appendUnique(dirs, exeDir.absoluteFilePath(QStringLiteral("hatari")));
        appendUnique(dirs, exeDir.absolutePath());
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

} // namespace paths
} // namespace pist
