// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SessionLauncher.h"

// For MainWindow::tr alone: the launcher's messages are written in the window's
// translation context. The include grants no access to the class — the launcher
// reaches the window only through its Host (MIN-89).
#include "ui/MainWindow.h"

#include "ui/DebugSessionController.h"
#include "ui/UiText.h"
#include "build/FloppyImage.h"
#include "emu/EmulatorHost.h"
#include "emu/DebugBackend.h"
#include "emu/HatariProbe.h"
#include "emu/LibretroBackend.h"
#include "model/Machine.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "toolchain/Toolchain.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QWidget>

namespace pist {

namespace {

/// The widget a modal is parented to: the seam's own QObject parent, which is
/// the window it was built for. Not a Host entry — parenting a dialog to the
/// window you serve is a fact about Qt, not an operation on the window.
QWidget *dialogParent(const QObject *seam)
{
    return qobject_cast<QWidget *>(seam->parent());
}

} // namespace

SessionLauncher::SessionLauncher(Host host, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
{
}

void SessionLauncher::launch()
{
    const QFileInfo info(m_host.buildSourcePath());
    const QString prg = settings::outputPathsFor(info.absoluteFilePath()).program;
    if (!QFileInfo::exists(prg)) {
        m_host.refuseRun(MainWindow::tr("Run"),
                         MainWindow::tr("The build did not produce %1.").arg(prg),
                         /*critical=*/false);
        return;
    }

    // An empty Hatari path with the core library present: the core runs
    // in-process and arms its own entry stop, so there is no Hatari to find,
    // probe, or bootstrap. A named path keeps the subprocess.
    const bool inProcess = sessionUsesInProcessCore(
        m_host.settings().hatariPath, QCoreApplication::applicationDirPath());

    ToolInfo emulator;
    HatariCapabilities launchedCaps;
    if (!inProcess) {
        emulator = toolchain::findEmulator(m_host.settings().hatariPath);
        if (!emulator.found()) {
            m_host.refuseRun(MainWindow::tr("Emulator not found"), toolchain::emulatorInstallHint(),
                             /*critical=*/true);
            return;
        }

        // Probe the binary actually being launched (not the default-path one the
        // status bar probed at startup): the transport selection and the option
        // gating below must match this emulator.
        launchedCaps = probeHatari(emulator.path);
    }

    const QString sessionDir = m_host.makeSessionDir();

    SessionConfig config;
    config.hatariPath = emulator.path;
    config.programPath = prg;
    config.sessionDir = sessionDir;
    config.gemdosDir = info.absolutePath();

    config.machine = machineCliName(m_host.settings().machine);
    config.monitor = m_host.settings().monitor;
    config.memSizeMiB = m_host.settings().memSizeMiB;
    config.extraArgs = m_host.settings().extraEmulatorArgs;
    if (!m_host.settings().hardDiskImage.isEmpty()) {
        // ACSI is the safe default: it exists on every ST-family machine, unlike
        // IDE which is STe-and-later only.
        config.acsiImage = m_host.settings().hardDiskImage;
        config.acsiId = 0;
    }

    config.floppyImages = m_host.settings().floppyImages;

    QString error;

    if (inProcess) {
        // No subprocess window, no control socket, no bootstrap script. The
        // core arms `b pc = TEXT && pc < $e00000 :once` itself.
        config.controlSocketPath.clear();
        config.parentWindowId.clear();
        config.debugExceptions.clear();
        m_host.selectBackend(BackendKind::Libretro);
    } else {
        // The control socket is compiled into Hatari only under
        // HAVE_UNIX_DOMAIN_SOCKETS. Passing the option to a build without it makes
        // Hatari exit with "Unrecognized option", so it must be gated rather than
        // passed unconditionally (docs/PLAN.md §5 rule 12). The debugger commands
        // never travel over it: stdin (native) or HRDB's TCP channel carry those.
        if (launchedCaps.hasControlSocket)
            config.controlSocketPath = sessionDir + QStringLiteral("/ctl.sock");
        else
            config.controlSocketPath.clear();

        // Embedded display: name the container's X11 window so Hatari reparents its
        // SDL window into it. Gated on the platform and the control socket, so a
        // Wayland-native session or a socket-less build falls back to a separate
        // window even when the option is on. The dock is shown and the widget
        // realized by the window, because winId() has to be a live X11 window
        // before Hatari starts or there is nothing to reparent into.
        config.parentWindowId = m_host.embedDisplayWindowId(launchedCaps);

        if (!launchedCaps.hasDebugExcept)
            config.debugExceptions.clear();

        // Backend selection: an explicit per-project setting wins; the default
        // ("auto") follows the *launched* binary's HRDB capability, probed by
        // content because the fork is version-identical to upstream and adds no
        // CLI option (docs/PLAN.md §5 rule 5). The bundled emulator is the fork,
        // so a fresh install lands on HRDB; a user-supplied stock Hatari lands on
        // native. Only the control socket is native-transport machinery; the
        // bootstrap script runs on the fork too (--parse is upstream), and there
        // its entry breakpoint fires into the remote break loop and waits for our
        // HRDB connect — no race with TOS boot, unlike a socket-armed bp.
        // A forced transport that mismatches the binary is a dead session, not a
        // degradation: the fork's stdin debugger is never read once its listener
        // binds (§9), and stock Hatari has no HRDB listener at all. Refuse at
        // launch, naming the mismatch, rather than hanging the user.
        BackendKind wanted;
        if (m_host.settings().debugBackend == QLatin1String("hrdb")) {
            if (!launchedCaps.hasHrdb) {
                m_host.refuseRun(MainWindow::tr("Run"),
                                 MainWindow::tr("The debug transport is set to HRDB, but %1 is a stock Hatari, which "
                                    "has no HRDB listener. Set the transport to Native/Auto, or use the "
                                    "hrdb-main fork.").arg(emulator.path),
                                 /*critical=*/true);
                return;
            }
            wanted = BackendKind::Hrdb;
        } else if (m_host.settings().debugBackend == QLatin1String("native")) {
            if (launchedCaps.hasHrdb) {
                m_host.refuseRun(MainWindow::tr("Run"),
                                 MainWindow::tr("The debug transport is set to Native, but %1 is the hrdb-main fork, "
                                    "whose stdin debugger is never read once its listener binds. Set the "
                                    "transport to HRDB/Auto.").arg(emulator.path),
                                 /*critical=*/true);
                return;
            }
            wanted = BackendKind::Native;
        } else {
            wanted = launchedCaps.hasHrdb ? BackendKind::Hrdb : BackendKind::Native;
        }
        m_host.selectBackend(wanted);

        config.bootstrapScriptPath =
            EmulatorHost::writeBootstrapScript(sessionDir, launchedCaps, &error);
        if (config.bootstrapScriptPath.isEmpty()) {
            m_host.refuseRun(MainWindow::tr("Run"), error, /*critical=*/true);
            return;
        }
    }

    // A TOS ROM is required. Hatari ships none and original ROMs remain
    // proprietary, so this must be user-supplied (docs/PLAN.md §7).
    //
    // Selection matters: autostart needs TOS >= 1.04, and picking the
    // alphabetically first image would select TOS 1.02 and produce a session
    // that never reaches the entry breakpoint (docs/PLAN.md §5 rule 3).
    const QList<TosRom> roms = findTosRoms();

    // An explicitly configured ROM wins; otherwise pick the best for the machine.
    // Choosing for the machine matters because the machines accept different TOS
    // versions outright: an STe needs 1.06 or 1.62, and would otherwise be handed
    // a 1.04 that Hatari rejects.
    TosRom rom;
    if (!m_host.settings().tosPath.isEmpty()) {
        for (const TosRom &candidate : roms) {
            if (candidate.path == m_host.settings().tosPath) {
                rom = candidate;
                break;
            }
        }
        if (rom.path.isEmpty()) {
            if (m_host.quiet()) {
                // The fallback below is the answer either way; nobody is there
                // to read a warning about it.
                m_host.log(MainWindow::tr("[run] configured TOS ROM '%1' is missing — using the best ROM "
                                          "for the %2.")
                               .arg(m_host.settings().tosPath,
                                    machineDisplayName(m_host.settings().machine)));
            } else {
                QMessageBox::warning(
                    dialogParent(this), MainWindow::tr("Run"),
                    MainWindow::tr("The configured TOS ROM is missing:\n\n%1\n\nFalling back to the best "
                       "ROM for the %2.")
                        .arg(m_host.settings().tosPath,
                             machineDisplayName(m_host.settings().machine)));
            }
        }
    }
    // Whether the ROM below was picked for the user rather than configured by
    // them. Only the auto-selection is checked for a machine mismatch: an
    // explicitly selected ROM is the user's own decision, already marked
    // "(not for <machine>)" where it is chosen.
    bool autoSelected = false;
    if (rom.path.isEmpty()) {
        autoSelected = true;
        rom = selectPreferredRom(roms, m_host.settings().machine);
    }
    if (rom.path.isEmpty()) {
        const QStringList searched = paths::tosSearchPaths();
        m_host.refuseRun(MainWindow::tr("Run"),
                         MainWindow::tr("No TOS ROM image found.\n\n"
                            "Original TOS images cannot be bundled with PiST, so one has to be supplied "
                            "separately. Place a ROM image in one of these directories, or point "
                            "$PIST_TOS_DIR at the directory containing it:\n\n%1")
                             .arg(searched.isEmpty() ? MainWindow::tr("(no searchable directory found)")
                                                     : searched.join(QLatin1Char('\n'))),
                         /*critical=*/true);
        return;
    }

    // Auto-selection can still come up with a ROM this machine cannot run: with
    // nothing suitable on disk, selectPreferredRom returns the newest anyway so
    // that messages can name it. Hatari then resolves the pairing itself, by
    // overriding `--machine` and logging only an ERROR line — so the session runs
    // a different machine than the one selected, silently. SettingsDialog says so
    // plainly when the user picks such a ROM; the auto-selected one gets the same
    // warning here. It is a warning, not a refusal: the ROM does boot, it is just
    // the wrong machine.
    if (autoSelected && !rom.supportsMachine(m_host.settings().machine)) {
        const QString mismatch =
            MainWindow::tr("The auto-selected TOS ROM '%1' (TOS %2) is not for the %3: Hatari will override "
               "the machine to match the ROM, so the emulated machine will not be the one "
               "selected. Add a %3 ROM, or choose a machine this ROM supports.")
                .arg(rom.fileName, rom.versionText(),
                     machineDisplayName(m_host.settings().machine));
        m_host.log(QStringLiteral("[run] ") + mismatch);
        m_host.showStatus(firstLine(mismatch), 15000);
    }
    // Three distinct cases, because collapsing them produces either a false
    // error or the silent hang this check exists to prevent:
    //
    //   known too old  -> AUTO-folder floppy fallback
    //   known good     -> the GEMDOS-HD path
    //   unknown        -> warn and ask; if the user proceeds — and always on a
    //                     quiet (remote) run, which has nobody to ask — the
    //                     same floppy fallback, which works on every TOS
    //                     version
    bool floppyBoot = false;
    if (rom.knownTooOldForAutostart()) {
        // GEMDOS HD does not exist below TOS 1.04 (Hatari refuses it), but
        // every TOS executes AUTO/*.PRG from the boot floppy — the fallback
        // instead of the old refusal (docs/PLAN.md §5 rule 3).
        floppyBoot = true;
        m_host.log(MainWindow::tr("[run] TOS %1 has no GEMDOS-HD autostart; booting from an "
                                  "AUTO-folder floppy instead.").arg(rom.versionText()));
    }

    if (!floppyBoot && !rom.supportsAutostart()) {
        QString detail;
        if (rom.versionKnown) {
            // A version was read from the filename only. Hatari never consults
            // filenames, so this is not evidence about what it will do.
            detail = MainWindow::tr("Its filename suggests TOS %1, but the version field in the image "
                                    "header could not be read, so this cannot be confirmed.")
                         .arg(rom.versionText());
        } else {
            detail = MainWindow::tr("Its TOS version could not be determined.");
        }

        if (m_host.quiet()) {
            // Nobody can answer the question remotely. Proceed with the path the
            // dialog describes as the one that works on every TOS version, and
            // say why in the console.
            m_host.log(MainWindow::tr("[run] %1 Booting from an AUTO-folder floppy instead.")
                           .arg(detail));
            floppyBoot = true;
        } else {
            const auto answer = QMessageBox::warning(
                dialogParent(this), MainWindow::tr("Run"),
                MainWindow::tr("This ROM cannot be confirmed to support autostarting a program from a "
                   "GEMDOS hard disk (that needs TOS 1.04 or later).\n\n%1\n\n"
                   "If you proceed, the program boots from an AUTO-folder floppy instead, "
                   "which works on every TOS version.\n\nTry to run anyway?")
                    .arg(detail),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes)
                return;
            // An unverifiable ROM gets the path that works on every TOS version.
            floppyBoot = true;
        }
    }

    if (floppyBoot) {
        // Build the session's AUTO-folder floppy (AUTO/PROG.PRG plus an empty
        // EMUDESK.INF for the debug-except deferral), and route the session
        // around the GEMDOS HD: --disk-a, no positional, no -d, and --debug to
        // arm the exception mask the INF path would have armed.
        config.bootFloppyPath = sessionDir + QStringLiteral("/auto.st");
        QString floppyError;
        if (!floppy::writeAutoFolderImage(config.bootFloppyPath, prg, &floppyError)) {
            m_host.refuseRun(MainWindow::tr("Run"),
                             MainWindow::tr("Could not build the AUTO-folder floppy:\n%1")
                                 .arg(floppyError),
                             /*critical=*/true);
            return;
        }
        config.gemdosDir.clear();
        config.debugToggle = true;

        // TOS < 1.04 boots from A:. A user image already in A: would be
        // overwritten by auto.st (Hatari's last --disk-a wins) — the sidebar
        // would still list the magazine while Hatari ran the AUTO floppy.
        const QString userA = config.floppyImages.value(0);
        if (!userA.isEmpty()) {
            while (config.floppyImages.size() < 2)
                config.floppyImages.append(QString());
            if (config.floppyImages.at(1).isEmpty()) {
                config.floppyImages[1] = userA;
                m_host.log(MainWindow::tr("[run] TOS %1 autostarts from drive A:, so '%2' is in B:.")
                               .arg(rom.versionText(), QFileInfo(userA).fileName()));
            } else {
                m_host.log(MainWindow::tr("[run] TOS %1 needs drive A: to autostart; '%2' was not mounted.")
                               .arg(rom.versionText(), QFileInfo(userA).fileName()));
            }
            config.floppyImages[0].clear();
        }
    }
    config.tosPath = rom.path;

    // A new session relocates the program, so previously resolved addresses are
    // meaningless and arming must happen again after the next entry stop.
    m_host.session()->resetSessionState();

    if (!m_host.backend()->start(config, &error)) {
        m_host.refuseRun(MainWindow::tr("Run"), error, /*critical=*/true);
        return;
    }

    m_host.log(MainWindow::tr("Session started in %1").arg(sessionDir));
    const QString diskA = !config.floppyImages.value(0).isEmpty()
        ? config.floppyImages.at(0)
        : config.bootFloppyPath;
    if (!diskA.isEmpty())
        m_host.log(MainWindow::tr("[run] Floppy A: %1").arg(diskA));
    if (!config.floppyImages.value(1).isEmpty())
        m_host.log(MainWindow::tr("[run] Floppy B: %1").arg(config.floppyImages.at(1)));
}

} // namespace pist
