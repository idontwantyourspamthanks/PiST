// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/LineMap.h"
#include "build/ProgramLineMap.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "emu/MachineState.h"
#include "emu/MemoryDump.h"

#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <utility>

namespace pist {

class BreakpointWatchpointModel;
class IDebugBackend;
class ProfilerController;

/// The debug session's lifecycle state, in one owner (MAJ-41).
///
/// MainWindow used to carry the session's in-flight flags as six ad-hoc
/// booleans spread over the class, and both lifecycle defects the review found
/// there were flag-lifecycle defects: CRIT-4's poisoned
/// `m_breakpointsArmedThisSession` (set before the bases that give it meaning
/// arrived, so the entry stop never armed anything again) and MAJ-16's step-out
/// and profile-save flags surviving into the next session. Keeping them — and
/// the code that resets, sets and consumes them — in one class is what makes
/// "who clears this" answerable.
///
/// It also owns the two entry points those flags guard: the entry-stop attach
/// (`onDebuggerStopped`) and the one arming gate (`armBreakpoints`, with
/// `canReArmBreakpoints` deciding when an edit re-arms at all). The window
/// stays the seam's caller and the owner of the views; this class owns when
/// the session's state changes hands, and reaches the window only through the
/// `Host` operations below (MIN-89 — it used to be a `friend` of MainWindow).
class DebugSessionController : public QObject
{
public:
    /// What the controller does to the window it serves: the arming gate's
    /// inputs, the console, the panel rows and the one session-end reset. Each
    /// entry is a callback rather than a `MainWindow *` — the controller owns
    /// the session's state and the window owns its views, so they meet at
    /// these operations instead of at a pointer into another class's privates.
    struct Host
    {
        /// The debug transport in force. Read per use, because a launch
        /// selects it (SessionLauncher replaces a backend of the other kind).
        std::function<IDebugBackend *()> backend;
        /// The profiling seam, whose mode the session's end clears.
        std::function<ProfilerController *()> profiler;
        /// The breakpoint and watchpoint models: what the arming gate arms and
        /// what the panel lists.
        std::function<BreakpointWatchpointModel *()> breakpoints;
        /// The program map, which turns a source line into an address.
        std::function<const ProgramLineMap &()> programMap;
        /// The section bases the last `info basepage` installed. The arming
        /// gate is whether they are live yet (CRIT-4).
        std::function<const LineMap::SectionBases &()> bases;
        /// The cached machine state of the last stop (the step-out read).
        std::function<const MachineState &()> lastState;
        /// One line on the console.
        std::function<void(const QString &)> log;
        /// Show the watchpoints: the pre-bases arm path, which lists them
        /// before the program's own addresses mean anything.
        std::function<void(const QList<Watchpoint> &)> showWatchpoints;
        /// Show an armed set. The panel marks these resolvable, so a
        /// breakpoint that could not be placed is visible as such.
        std::function<void(const QList<Breakpoint> &, const QList<Watchpoint> &)>
            showArmedBreakpoints;
        /// Drop everything on screen that described the session that just
        /// ended: the stop location, the resolved bases, the cached machine
        /// state, any pending remote command, and every editor's execution
        /// line.
        std::function<void()> clearSessionViews;
    };

    explicit DebugSessionController(Host host, QObject *parent = nullptr);

    /// Set once the entry stop has been handled for the current session, so the
    /// arming sequence runs exactly once.
    bool sessionArmed() const { return m_sessionArmed; }
    void setSessionArmed(bool on) { m_sessionArmed = on; }

    /// Set once breakpoints have been armed against the live bases this session.
    /// Arming has to wait for the basepage response (not a timer, which always
    /// loses that race), so this guards doing it exactly once, when they arrive.
    bool breakpointsArmedThisSession() const { return m_breakpointsArmedThisSession; }

    /// A stack dump requested by stepOut() is in flight; the next
    /// stackDumpReady arms the return-address breakpoint instead of only
    /// feeding the stack view. Consuming it is one operation, so the flag
    /// cannot be read and left set.
    bool consumeStepOutPending();
    /// Is a step-out dump in flight? (For the stack view, which answers the
    /// dump regardless.)
    bool stepOutPending() const { return m_stepOutPending; }

    /// A stop was announced; the next state batch carries its PC, so the
    /// stopped event is published from onStateUpdated with the detail filled.
    bool stopEventPending() const { return m_stopEventPending; }
    void setStopEventPending(bool on) { m_stopEventPending = on; }
    bool consumeStopEventPending();

    /// A `profile save` is in flight; its commandFinished parses the file.
    bool profileSavePending() const { return m_profileSavePending; }
    void setProfileSavePending(bool on) { m_profileSavePending = on; }
    bool consumeProfileSavePending();

    /// Set by Run, consumed by onBuildFinished. Needed because the build is
    /// asynchronous: the launch has to wait for it, not run alongside it.
    bool launchAfterBuild() const { return m_launchAfterBuild; }
    void setLaunchAfterBuild(bool on) { m_launchAfterBuild = on; }
    /// Takes the launch intent and clears it, so a build that refused cannot
    /// leave it armed for the next one.
    bool consumeLaunchAfterBuild();

    /// Whether a breakpoint or watchpoint edit should re-arm the host now: the
    /// one gate all six edit paths share. A live session is the requirement and
    /// the whole requirement — armBreakpoints() holds the bases gate itself —
    /// and a re-arm issued while the machine is *running* is still worth
    /// issuing, because the host holds the commands until the debugger is back.
    bool canReArmBreakpoints() const;

    /// Can the host be told about an edit right now? Then tell it.
    void reArmIfLive();

    /// The ARMING side of the same gate, and it is the live bases: breakpoint
    /// and watchpoint edits call this too, so it is reachable in the window
    /// between the emulator starting and the entry stop reporting
    /// `info basepage`. Until the bases arrive source lines cannot be turned
    /// into addresses at all, and marking the session armed there is what
    /// CRIT-4 was.
    void armBreakpoints();

    /// Called once the debugger stops. Past the entry stop this is the
    /// refresh-plus-guided-profile teardown; on the entry stop it runs the
    /// two-phase attach (symbols, basepage, registers, disassembly) exactly
    /// once per session.
    void onDebuggerStopped();

    /// Step out of the current subroutine: one-shot breakpoint at the return
    /// address read from the stack, then resume (no Hatari primitive exists).
    /// The read is asynchronous — this method's other half is
    /// handleStackDumpForStepOut(), which the stack dump calls.
    void stepOut();

    /// Complete a stepOut() from a stack dump: the flag saying one is in
    /// flight lives here, so the arming that answers it does too. The dump's
    /// first long is the return address; a one-shot breakpoint is armed there
    /// and the program resumed.
    ///
    /// False means the dump was that gesture's answer alone and the caller has
    /// nothing left to show: an implausible top of stack (not inside a
    /// subroutine) is logged and the gesture ends there — the stack view is
    /// left as it was, exactly as before the extraction.
    bool handleStackDumpForStepOut(const QList<MemoryRow> &rows);

    /// Reset everything that must not leak from one debug session into the
    /// next: the arming state and resolved bases (GEMDOS relocates the program
    /// on every run), the cached machine state (the remote `state` command must
    /// not answer for a dead session), any pending remote command, and the
    /// in-flight step-out / profile-save / stop-event flags, whose answers
    /// belonged to the session that ended. Called at launch and again when the
    /// session ends, so a session's state has exactly one lifetime and one
    /// owner.
    void resetSessionState();

private:
    Host m_host;

    bool m_sessionArmed = false;
    bool m_breakpointsArmedThisSession = false;
    bool m_stepOutPending = false;
    bool m_stopEventPending = false;
    bool m_profileSavePending = false;
    bool m_launchAfterBuild = false;
};

} // namespace pist
