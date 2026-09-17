// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/Paths.h"
#include "emu/HatariProbe.h"
#include "emu/TosRom.h"
#include "toolchain/Toolchain.h"
#include "ui/MainWindow.h"
#include "ui/Appearance.h"
#include "control/RemoteControl.h"

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
    if (emulator.found()) {
        // Probe the emulator's capabilities, so a release archive can prove
        // what its bundled emulator speaks (native prompt framing vs HRDB).
        const pist::HatariCapabilities caps = pist::probeHatari(emulator.path);
        out << "  transport: " << (caps.hasHrdb ? "hrdb" : "native") << "\n";
    }

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
#if defined(Q_OS_LINUX)
    // Embedded display needs PiST to be an X11 client: the container widget's
    // window ID is handed to Hatari as PARENT_WIN_ID, and that only exists on
    // xcb. On a Wayland session Qt would otherwise choose the wayland platform,
    // leaving no X11 window to embed into. Prefer xcb whenever an X display is
    // reachable (native X11, or XWayland under Wayland); when there is none we
    // stay native Wayland and the embedded option is simply unavailable. An
    // explicit QT_QPA_PLATFORM always wins.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")
        && !qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "xcb");
#endif

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PiST"));
    QApplication::setApplicationVersion(QStringLiteral(PIST_VERSION));
    QApplication::setOrganizationName(QStringLiteral("PiST"));

    // Before anything else touches the style: applyTheme() captures the
    // platform's own style and palette here so the "system" preference can
    // restore them later.
    pist::appearance::applyTheme();

    QApplication::setWindowIcon(pist::appearance::windowIcon());

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("An IDE for Atari ST assembly development"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption diagnose(
        QStringLiteral("diagnose"),
        QStringLiteral("Report the tools and ROMs PiST can find, then exit."));
    parser.addOption(diagnose);

    QCommandLineOption controlPort(
        QStringLiteral("control-port"),
        QStringLiteral("Listen on 127.0.0.1:<port> for remote-control commands "
                       "(open/build/run/step/screenshot/state/console/quit). "
                       "May also be set with the PIST_CONTROL_PORT environment "
                       "variable. Off by default."),
        QStringLiteral("port"));
    parser.addOption(controlPort);

    parser.addPositionalArgument(QStringLiteral("source"),
                                 QStringLiteral("Assembly source file to open."));
    parser.process(app);

    // Answered before any window exists, so it works headless and on a machine
    // with no display at all.
    if (parser.isSet(diagnose))
        return runDiagnose();

    pist::MainWindow window;
    window.show();

    // First-run convenience: when a required piece (assembler, emulator, ROM)
    // is missing, offer the guided setup rather than letting the first build
    // or run fail with it. Queued so the window is up first.
    QMetaObject::invokeMethod(&window, "showSetupIfNeeded", Qt::QueuedConnection);

    // The remote-control interface is strictly opt-in: an IDE that opens a
    // listening socket without being asked would be a surprise. --control-port
    // wins over the environment variable.
    pist::RemoteControl remoteControl(&window);
    {
        QString portText = parser.value(controlPort);
        if (portText.isEmpty())
            portText = qEnvironmentVariable("PIST_CONTROL_PORT");
        if (!portText.isEmpty()) {
            bool ok = false;
            const quint16 port = portText.toUShort(&ok);
            QString error;
            if (!ok || port == 0) {
                fprintf(stderr, "Invalid control port '%s'\n", qPrintable(portText));
            } else if (!remoteControl.listen(port, &error)) {
                fprintf(stderr, "Cannot listen for remote control on %d: %s\n",
                        int(port), qPrintable(error));
            } else {
                fprintf(stderr, "Listening for remote control on 127.0.0.1:%d\n",
                        int(remoteControl.boundPort()));
            }
        }
    }

    const QStringList args = parser.positionalArguments();
    if (!args.isEmpty()) {
        QMetaObject::invokeMethod(&window, "openPath", Qt::QueuedConnection,
                                  Q_ARG(QString, args.first()));
    }

    return app.exec();
}
