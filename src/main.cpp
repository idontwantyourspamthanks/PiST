// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "toolchain/Toolchain.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QTextStream>

#ifdef Q_OS_WIN
#  include <cstdio>
#  include <windows.h>
#endif

namespace {

/// Give the diagnostic mode somewhere to print on Windows.
///
/// PiST is a GUI-subsystem application, so it has no console of its own and
/// stdout goes nowhere — `--diagnose` would produce no output at all when run
/// from a terminal, which is exactly when someone would use it. Attaching to the
/// console that launched us and reopening the standard streams restores the
/// expected behaviour without giving the GUI a console window on normal startup.
void attachParentConsole()
{
#ifdef Q_OS_WIN
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#endif
}

/// Print what PiST found and how it resolved it.
///
/// Exists for two reasons. For a user, a bundled or configured tool that is not
/// being picked up is otherwise invisible — PiST would simply report the tool
/// missing with no indication of where it looked. For release verification, this
/// is how a packaged archive proves that the assembler travelling beside the
/// binary is actually the one being used, rather than whatever happens to be on
/// the machine.
int runDiagnose()
{
    attachParentConsole();
    QTextStream out(stdout);

    out << "PiST " << QApplication::applicationVersion() << "\n";
    out << "Qt " << QT_VERSION_STR << "\n";
    out << "Executable: " << QCoreApplication::applicationDirPath() << "\n\n";

    const pist::ToolInfo assembler = pist::toolchain::findAssembler();
    out << "Assembler (vasmm68k_mot): "
        << (assembler.found() ? assembler.path : QStringLiteral("NOT FOUND")) << "\n";
    if (assembler.found() && !assembler.version.isEmpty())
        out << "  version: " << assembler.version << "\n";

    const pist::ToolInfo emulator = pist::toolchain::findEmulator();
    out << "Emulator (hatari): "
        << (emulator.found() ? emulator.path : QStringLiteral("NOT FOUND")) << "\n";
    if (emulator.found() && !emulator.version.isEmpty())
        out << "  version: " << emulator.version << "\n";

    // ROMs and where they were looked for, since "no ROM" is the most common
    // first-run problem and the search spans several directories.
    const QList<pist::TosRom> roms = pist::findTosRoms();
    out << "\nTOS ROMs found: " << roms.size() << "\n";
    for (const pist::TosRom &rom : roms) {
        out << "  " << rom.versionText() << "\t" << rom.path;
        if (!rom.supportsAutostart())
            out << "   (cannot autostart from GEMDOS HD)";
        out << "\n";
    }

    out << "\nSearch paths:\n";
    for (const QString &p : pist::toolchain::searchPaths())
        out << "  " << p << "\n";
    out << "\nROM search paths:\n";
    for (const QString &p : pist::paths::tosSearchPaths())
        out << "  " << p << "\n";

    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PiST"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));
    QApplication::setOrganizationName(QStringLiteral("PiST"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("An IDE for Atari ST assembly development"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption diagnose(
        QStringLiteral("diagnose"),
        QStringLiteral("Report the tools and ROMs PiST can find, then exit."));
    parser.addOption(diagnose);

    parser.addPositionalArgument(QStringLiteral("source"),
                                 QStringLiteral("Assembly source file to open."));
    parser.process(app);

    // Answered before any window exists, so it works headless and on a machine
    // with no display at all.
    if (parser.isSet(diagnose))
        return runDiagnose();

    pist::MainWindow window;
    window.show();

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        QMetaObject::invokeMethod(&window, "openPath", Qt::QueuedConnection,
                                  Q_ARG(QString, args.first()));
    }

    return app.exec();
}
