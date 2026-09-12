// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "toolchain/Toolchain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

namespace pist {
namespace toolchain {

namespace {

constexpr const char *kAssemblerName = "vasmm68k_mot";
constexpr const char *kEmulatorName = "hatari";

/// Read a version string from `--version`, tolerating tools that print to either
/// stream or exit non-zero for it. Best-effort: a missing version is not an error.
QString probeVersion(const QString &path)
{
    QProcess p;
    p.start(path, {QStringLiteral("--version")});
    if (!p.waitForStarted(3000) || !p.waitForFinished(5000))
        return {};

    const QString output = QString::fromUtf8(p.readAllStandardOutput())
                         + QString::fromUtf8(p.readAllStandardError());
    static const QRegularExpression re(QStringLiteral(R"((\d+\.\d+[a-z]?(?:\.\d+)?))"));
    const auto match = re.match(output);
    return match.hasMatch() ? match.captured(1) : QString();
}

ToolInfo locate(const QString &program, const QString &overridePath)
{
    ToolInfo info;
    info.name = program;

    // 1. An explicit path from settings wins outright, even if it does not
    //    resolve: pointing PiST at a binary that has moved is a mistake worth
    //    reporting rather than silently substituting a different one.
    if (!overridePath.isEmpty()) {
        const QFileInfo override(overridePath);
        if (override.isFile() && override.isExecutable())
            info.path = override.absoluteFilePath();
        return info;
    }

    // 2. Beside the application, which is how release bundles are laid out.
    const QString beside = QCoreApplication::applicationDirPath()
                         + QLatin1Char('/') + program;
    if (QFileInfo(beside).isExecutable()) {
        info.path = beside;
        info.version = probeVersion(info.path);
        return info;
    }

    // 3. The suggested install directory, where a first-run fetch would place it.
    const QString installed = suggestedInstallDir() + QLatin1Char('/') + program;
    if (QFileInfo(installed).isExecutable()) {
        info.path = installed;
        info.version = probeVersion(info.path);
        return info;
    }

    // 4. The system search path.
    const QString onPath = QStandardPaths::findExecutable(program);
    if (!onPath.isEmpty()) {
        info.path = onPath;
        info.version = probeVersion(info.path);
    }

    return info;
}

} // namespace

QStringList searchPaths()
{
    QStringList paths;
    paths << QCoreApplication::applicationDirPath();
    paths << suggestedInstallDir();

    const QStringList standard = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const QString &p : standard)
        paths << p;

    const QString envPath = QProcessEnvironment::systemEnvironment()
                                .value(QStringLiteral("PATH"));
#if defined(Q_OS_WIN)
    const QChar sep = QLatin1Char(';');
#else
    const QChar sep = QLatin1Char(':');
#endif
    for (const QString &p : envPath.split(sep, Qt::SkipEmptyParts))
        paths << p;

    paths.removeDuplicates();
    return paths;
}

ToolInfo findAssembler(const QString &overridePath)
{
    return locate(QString::fromLatin1(kAssemblerName), overridePath);
}

ToolInfo findEmulator(const QString &overridePath)
{
    return locate(QString::fromLatin1(kEmulatorName), overridePath);
}

QString suggestedInstallDir()
{
    // A per-user data location rather than a system one, so no elevation is needed
    // and the app stays usable from a plain unpacked archive.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.pist");
    return base + QStringLiteral("/tools");
}

QString assemblerInstallHint()
{
    return QObject::tr(
        "PiST uses vasm (vasmm68k_mot) as its assembler, and does not ship it.\n\n"
        "vasm is not free software: its licence permits redistribution unmodified "
        "for non-commercial use, which is why PiST never patches it and why it is "
        "not committed to this repository. Get it from the author:\n\n"
        "    http://sun.hasenbraten.de/vasm/\n\n"
        "Some distributions also package it (for example `apt install vasm` on "
        "Debian and Ubuntu). Alternatively set an explicit path in Project "
        "Settings, or place the binary in:\n\n    %1")
        .arg(suggestedInstallDir());
}

QString emulatorInstallHint()
{
    return QObject::tr(
        "PiST runs programs under Hatari, which is not bundled.\n\n"
        "Install it with your package manager (for example `apt install hatari` "
        "or `brew install hatari`), or set an explicit path in Project Settings, "
        "or place the executable in:\n\n    %1")
        .arg(suggestedInstallDir());
}

} // namespace toolchain
} // namespace pist
