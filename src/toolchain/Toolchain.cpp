// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "toolchain/Toolchain.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
#ifdef Q_OS_WIN
    // Only PE executables can answer on Windows: anything else (for example a
    // POSIX script with an .exe name) can never run there, so probing it is
    // wasted work — and worse, CreateProcess on such a file has been observed
    // to stall for minutes inside the OS on some systems (CI evidence: a
    // 300 s hang in QProcess::start that does not occur for real tools).
    // Check the magic instead of paying a process start.
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly) || probe.read(2) != "MZ")
        return {};
#endif
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

/// Whether a process probe can answer the version at all.
///
/// hatari is excluded on every platform, and the reason is not a Windows one:
/// `--version` on a GUI build prints into a console the process does not own and
/// then waits for Enter, and with no arguments at all it launches the full
/// emulator GUI. Either way the probe blocks its caller — the GUI thread here —
/// for the timeout, and pops a window on the user's screen on Linux and macOS
/// exactly as on Windows. Its version and its capabilities come from the content
/// probe (emu/HatariProbe) instead, which is why this was never noticed as a
/// platform-neutral rule.
bool canProbeByProcess(const QString &program)
{
    return program != QLatin1String("hatari");
}

/// The path an explicit override names, by the same rule the discovery steps
/// resolve a program: a path that is already an executable file is taken as it
/// is, and on Windows a name without a suffix also resolves as `<name>.exe` —
/// the rule `findExecutable` applies below, which a bare `QFileInfo` check does
/// not. Without it a perfectly good override of
/// `C:\\PiST\\tools\\vasmm68k_mot` reported the tool as missing on Windows while
/// the same name resolved through discovery.
QString resolveOverridePath(const QString &overridePath)
{
    const QFileInfo info(overridePath);
    if (info.isFile() && info.isExecutable())
        return info.absoluteFilePath();
#ifdef Q_OS_WIN
    const QFileInfo exe(overridePath + QStringLiteral(".exe"));
    if (exe.isFile() && exe.isExecutable())
        return exe.absoluteFilePath();
#endif
    return {};
}

ToolInfo locate(const QString &program, const QString &overridePath)
{
    ToolInfo info;
    info.name = program;

    // 1. An explicit path from settings wins outright, even if it does not
    //    resolve: pointing PiST at a binary that has moved is a mistake worth
    //    reporting rather than silently substituting a different one. `reason`
    //    is what lets the caller report that instead of showing the bare name.
    if (!overridePath.isEmpty()) {
        info.path = resolveOverridePath(overridePath);
        if (info.path.isEmpty()) {
            info.reason = QObject::tr("'%1' is not an executable file").arg(overridePath);
        }
        return info;
    }

    // 2. Every directory searchPaths() reports, in its order: beside the
    //    application (release bundles), the per-user tools directory a fetch
    //    populates, then the system search path. One list drives the lookup and
    //    the report — `pist --diagnose` prints what was searched, so a directory
    //    listed there is a promise that it is actually searched.
    //
    //    findExecutable is used with explicit directories rather than a direct
    //    QFileInfo check, because it applies the platform's own notion of an
    //    executable name: on Windows that includes the `.exe` suffix, so a
    //    bundled `vasmm68k_mot.exe` is found where a bare `vasmm68k_mot` lookup
    //    would silently miss it.
    const QString found = QStandardPaths::findExecutable(program, searchPaths());
    if (!found.isEmpty()) {
        info.path = found;
        if (canProbeByProcess(program))
            info.version = probeVersion(info.path);
    }

    return info;
}

} // namespace

QStringList searchPaths()
{
    // The directories locate() walks, in order — one list, so the report and the
    // lookup cannot drift apart. It used to list ApplicationsLocation as well,
    // which locate() never searched: those directories hold .desktop entries,
    // not executables, so `pist --diagnose` told the user to put a binary where
    // it would not be found.
    QStringList paths;
    paths << QCoreApplication::applicationDirPath();
    paths << suggestedInstallDir();

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
    // No package-manager sentence here any more: `apt install vasm` does not
    // exist (checked against both the Debian and the Ubuntu package index), and
    // it was the first thing this hint told a user whose assembler was missing.
    // What does exist is the pinned fetch the setup dialog runs, so that is what
    // the hint points at.
    return QObject::tr(
        "PiST uses vasm (vasmm68k_mot) as its assembler, and does not ship it.\n\n"
        "vasm is not free software: its licence permits redistribution unmodified "
        "for non-commercial use, which is why PiST never patches it and why it is "
        "not committed to this repository. Get it from the author:\n\n"
        "    http://sun.hasenbraten.de/vasm/\n\n"
        "PiST can fetch and build it for you — that is what the first-run setup "
        "offers — or set an explicit path in Project Settings, or place the binary "
        "in:\n\n    %1")
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
        "The macOS archive and builds from source do not include it (the Windows "
        "archive bundles the same fork the AppImage carries, so there it is a "
        "packaging fault too). Install it with your package manager, or set an "
        "explicit path in Project Settings, or place the executable in:\n\n    %1\n\n"
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
