// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/DebugBackend.h"
#include "emu/HatariProbe.h"
#include "project/ProjectSettings.h"

#include <QString>

#include <functional>
#include <utility>

namespace pist {

class DebugSessionController;
class IDebugBackend;

/// Launching an emulator session: ROM selection, the floppy-boot fallback, the
/// transport (backend) switch, the SessionConfig assembly and the refusals that
/// can stop a launch (MAJ-41).
///
/// This is the part of `run` that happens after the build finishes, and it is
/// where a quiet (remote-invoked) run and an interactive one diverge: a refusal
/// must answer the remote reply instead of opening a modal nobody can dismiss.
/// The window keeps the launch *intent* (the session controller's
/// launchAfterBuild) and calls launch() from the build-finished path, so the
/// async hop between "Run was asked for" and "the binary exists" stays where it
/// was.
///
/// Everything the launcher needs from the window arrives as the `Host`
/// operations below (MIN-89 — it used to be a `friend` of MainWindow, and it
/// used to write the window's backend pointer mid-launch).
class SessionLauncher : public QObject
{
public:
    /// What a launch reads from and does to the window. Callbacks rather than a
    /// `MainWindow *`: the two operations a launch really performs on the
    /// window — switching the debug transport and realizing the embedded
    /// display — are named here, so the mid-launch backend swap is one
    /// window-owned operation instead of a reach into a member.
    struct Host
    {
        /// The project configuration the session is assembled from.
        std::function<const ProjectSettings &()> settings;
        /// The source Build/Run acts on; its output directory holds the PRG.
        std::function<QString()> buildSourcePath;
        /// A fresh per-session working directory, kept short so the control
        /// socket path fits in sockaddr_un::sun_path.
        std::function<QString()> makeSessionDir;
        /// The debug transport in force.
        std::function<IDebugBackend *()> backend;
        /// Make the transport in force `wanted`: when the live backend is of
        /// another kind it is stopped, replaced and re-wired. A no-op when it
        /// already is.
        std::function<void(BackendKind)> selectBackend;
        /// Realize the embedded display container, when this session can embed
        /// one, and return the X11 window id Hatari must reparent into. Empty
        /// for a separate window, which is also the answer when embedding is
        /// off, unsupported, or the widget is absent.
        std::function<QString(const HatariCapabilities &)> embedDisplayWindowId;
        /// True while the run was started by the quiet (remote-invoked) entry
        /// point: then a refusal must not open a modal nobody can dismiss.
        std::function<bool()> quiet;
        /// Report a refused launch — title, reason, and whether it is critical:
        /// quiet reports on the console and the status bar and answers
        /// buildCompleted(false); interactive keeps the modal.
        std::function<void(const QString &, const QString &, bool)> refuseRun;
        /// One line on the console.
        std::function<void(const QString &)> log;
        /// One line on the status bar, for the given number of milliseconds.
        std::function<void(const QString &, int)> showStatus;
        /// The session controller, whose state a launch resets.
        std::function<DebugSessionController *()> session;
    };

    explicit SessionLauncher(Host host, QObject *parent = nullptr);

    /// Build a SessionConfig for the current project and start the session.
    /// Every refusal path reports through the window's refuseRun() (quiet:
    /// console, status bar and buildCompleted(false); interactive: the modal it
    /// has always shown).
    void launch();

private:
    Host m_host;
};

} // namespace pist
