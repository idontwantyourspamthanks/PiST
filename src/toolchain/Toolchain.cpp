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
constexpr const char *kLinkerName = "vlink";

/// Read a version string from a tool, tolerating either convention.
///
/// `--version` first (Hatari answers it), then no arguments at all: vasm prints
/// its banner and exits when given no input, and does not support `--version` at
/// all, so probing only the flag would report no version for the assembler.
/// Best-effort throughout — a missing version is not an error.
QString probeVersion(const QString &path)
{
    const QStringList attempts = {QStringLiteral("--version"), QString()};

    for (const QString &args : attempts) {
        QProcess p;
        p.start(path, args.isEmpty() ? QStringList{} : QStringList{args});
        if (!p.waitForStarted(3000))
            continue;
        if (!p.waitForFinished(5000)) {
            p.kill();
            p.waitForFinished(1000);
        }

        const QString output = QString::fromUtf8(p.readAllStandardOutput())
                             + QString::fromUtf8(p.readAllStandardError());
        static const QRegularExpression re(QStringLiteral(R"((\d+\.\d+[a-z]?(?:\.\d+)?))"));
        const auto match = re.match(output);
        if (match.hasMatch())
            return match.captured(1);

        // A tool with no version output on this invocation: try the next form
        // rather than reporting failure.
    }

    return {};
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

    // 2. Beside the application (release bundles), then the per-user tools
    //    directory a fetch would populate.
    //
    //    findExecutable is used with explicit directories rather than a direct
    //    QFileInfo check, because it applies the platform's own notion of an
    //    executable name: on Windows that includes the `.exe` suffix, so a
    //    bundled `vasmm68k_mot.exe` is found where a bare `vasmm68k_mot` lookup
    //    would silently miss it.
    const QStringList preferred = {QCoreApplication::applicationDirPath(),
                                   suggestedInstallDir()};
    const QString beside = QStandardPaths::findExecutable(program, preferred);
    if (!beside.isEmpty()) {
        info.path = beside;
        info.version = probeVersion(info.path);
        return info;
    }

    // 3. The system search path.
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

ToolInfo findLinker(const QString &overridePath)
{
    return locate(QString::fromLatin1(kLinkerName), overridePath);
}

QString suggestedInstallDir()
{
    // GenericDataLocation rather than AppLocalDataLocation: the latter is
    // <org>/<app>, and since both are "PiST" it yields ~/.local/share/PiST/PiST.
    // This gives ~/.local/share/PiST/tools, which also matches where the
    // per-user tools are documented to live.
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return base + QStringLiteral("/PiST/tools");
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

QString linkerInstallHint()
{
    return QObject::tr(
        "PiST links separately-assembled sources with vlink, which is not bundled "
        "with the source build.\n\n"
        "Get it from the author:\n\n"
        "    http://sun.hasenbraten.de/vlink/\n\n"
        "It builds with plain `make`. Alternatively set an explicit path in "
        "Project Settings, or place the binary in:\n\n    %1")
        .arg(suggestedInstallDir());
}

QString emulatorInstallHint()
{
    // The release archives do not all carry an emulator, so this is still
    // reachable — but three different situations land here and one message
    // cannot serve them: a release AppImage missing its copy (a packaging fault,
    // not the user's problem), a macOS or Windows archive or a source build
    // where the user genuinely has to supply one, and a system Hatari that is
    // present but unusable. The message names the version requirement, because
    // "install hatari" is what produced unusable 2.4.1 setups in the first place.
    return QObject::tr(
        "PiST runs programs under Hatari, and could not find a usable copy.\n\n"
        "The Linux AppImage normally carries Hatari inside it, so if you are "
        "running one, this is a packaging fault rather than something you can "
        "fix — please report it.\n\n"
        "The macOS and Windows archives, and builds from source, do not include "
        "it. Install it with your package manager, or set an explicit path in "
        "Project Settings, or place the executable in:\n\n    %1\n\n"
        "Version matters: use Hatari 2.5 or later. The debugger transport PiST "
        "relies on works reliably on 2.6.x, and the truncated register dumps "
        "returned by 2.4.1 break source-line debugging. Ubuntu 24.04 ships 2.4.1 "
        "and 22.04 ships 2.3.1, so a distribution package is often not enough — "
        "build 2.6.1 from https://www.hatari-emu.org/ if your distribution does "
        "not have a recent enough one.")
        .arg(suggestedInstallDir());
}

} // namespace toolchain
} // namespace pist
