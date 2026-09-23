// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/DebugSessionController.h"

// For MainWindow::tr alone: a seam's messages are written in the window's
// translation context, so an entry in a catalogue does not move when the code
// that says it does. The include grants no access to the class — the seam
// reaches the window only through its Host (MIN-89).
#include "ui/MainWindow.h"

#include "ui/BreakpointWatchpointModel.h"
#include "ui/BreakpointPanel.h"
#include "ui/ProfilerController.h"
#include "editor/CodeEditor.h"
#include "emu/DebugBackend.h"
#include "debug/Breakpoint.h"

namespace pist {

DebugSessionController::DebugSessionController(Host host, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
{
}

bool DebugSessionController::consumeStepOutPending()
{
    const bool pending = m_stepOutPending;
    m_stepOutPending = false;
    return pending;
}

bool DebugSessionController::consumeStopEventPending()
{
    const bool pending = m_stopEventPending;
    m_stopEventPending = false;
    return pending;
}

bool DebugSessionController::consumeProfileSavePending()
{
    const bool pending = m_profileSavePending;
    m_profileSavePending = false;
    return pending;
}

bool DebugSessionController::consumeLaunchAfterBuild()
{
    const bool wanted = m_launchAfterBuild;
    m_launchAfterBuild = false;
    return wanted;
}

void DebugSessionController::reArmIfLive()
{
    if (canReArmBreakpoints())
        armBreakpoints();
}

void DebugSessionController::resetSessionState()
{
    // GEMDOS relocates the program on every run, so resolved addresses and
    // armed breakpoints are meaningless; the cached machine state and any
    // pending remote command must not answer for a session that no longer
    // exists. The in-flight flags are the same kind of leftover: a step-out
    // still waiting on its stack dump, a profile save waiting on its response,
    // and a stop event waiting for the register batch that would carry its PC
    // all belong to the session that is gone. Left set, the next session's own
    // entry-stop stack dump is consumed by the stale step-out flag — arming a
    // one-shot breakpoint at the old stack's idea of a return address and
    // resuming the new session away from its entry stop — and its `profile
    // save` response runs showProfileResults against a deleted session
    // directory. Both call sites (launch, session end) are idempotent resets
    // (MAJ-16).
    //
    // The window's half of the same reset — the stop location, the bases, the
    // cached state, the pending command and the editors' PC bars — is the one
    // Host operation, because it is the window's state to clear and not this
    // class's.
    m_sessionArmed = false;
    m_host.clearSessionViews();
    m_host.profiler()->resetSession();
    m_profileSavePending = false;
    m_breakpointsArmedThisSession = false;
    m_stepOutPending = false;
    m_stopEventPending = false;
}

void DebugSessionController::onDebuggerStopped()
{
    // Every stop refreshes the views and the editor's execution line, so the
    // display always shows where the machine actually stopped — on a step, on a
    // breakpoint, or on an exception. The entry stop additionally runs the
    // two-phase attach below, once per session.
    if (m_sessionArmed) {
        m_host.backend()->refresh();
        if (m_host.profiler()->guided()) {
            m_host.profiler()->setGuided(false);
            // Any stop ends the guided run: the one-shot's, or a user
            // breakpoint that won the race (results are then partial, which is
            // what "composes" means). When the one-shot itself stopped the run
            // it is already consumed — `:once` removes it on the hit — but when
            // another breakpoint won, it is still armed at the cursor line and
            // would stop the program there later for no reason.
            //
            // Rebuilding the set is how it is dropped. Hatari has no
            // delete-breakpoint-by-address — `b <index>` removes by index and
            // `b all` removes everything — and armBreakpoints() is the
            // clear-then-re-arm this program already uses after a rebuild, so
            // the stray one-shot goes and the planned breakpoints and
            // watchpoints come back. (The command this replaces was a raw
            // `db pc = $X :once`: `db` is dspbreak, a *DSP* breakpoint command,
            // so on an ST it printed "DSP isn't present or initialized." and
            // deleted nothing.)
            m_host.profiler()->stop();
            armBreakpoints();
        }
        return;
    }
    m_sessionArmed = true;

    // The two-phase attach, in order. The program's load address is only known
    // once it has been executed, so:
    //
    //   1. stop at entry (armed at launch via --parse, using the TEXT variable,
    //      which needs no symbols)
    //   2. load symbols, which relocates them against the live base page
    //   3. read the base page, because the line map needs the section addresses
    //
    // Only then can a source line be turned into an address
    // (docs/PLAN.md §5 rules 5 and 6).
    m_host.backend()->loadSymbols();
    m_host.backend()->readBasepage();
    m_host.backend()->dumpRegisters();
    m_host.backend()->readDisassembly();
    // Arming happens in onStateUpdated, when the bases those commands report
    // actually arrive — not here, where they have not been read yet.
}

bool DebugSessionController::canReArmBreakpoints() const
{
    // The gate every breakpoint and watchpoint edit goes through (MIN-13). A
    // live session is the requirement and the whole requirement: with no
    // process each queued command would only log "No emulator session is
    // running.", while armBreakpoints() holds the second gate itself — source
    // breakpoints need live bases, and until they arrive it re-arms the
    // watchpoints without marking the session armed (CRIT-4).
    //
    // Deliberately not `isStopped()`, which is what the source-breakpoint
    // callers used to require: the host holds stdin commands while emulation
    // runs (EmulatorHost::dispatchNextOnce — nothing reads stdin at the
    // debugger's prompt otherwise), so a re-arm issued during a run is
    // delivered at the next stop. Requiring a stop here is what dropped a
    // breakpoint added mid-run for the rest of the session — silently, with the
    // panel and the gutter showing it — while a watchpoint added the same way
    // was armed.
    return m_host.backend()->isRunning();
}

void DebugSessionController::armBreakpoints()
{
    // Arming has one gate, and it is the live bases. Breakpoint and watchpoint
    // edits call this too, so it is reachable in the window between the emulator
    // starting and the entry stop reporting `info basepage`, where a source line
    // cannot be turned into an address at all: the program map is unresolved, so
    // every breakpoint lands in `unresolved` and no `b` command is produced.
    //
    // Marking the session armed there is what made that fatal. The entry-stop
    // gate in onStateUpdated() arms only while the flag is clear, so the session
    // spent the rest of its life with no source breakpoint armed and no second
    // chance to arm one. The invariant is the documented one — info basepage ->
    // setLiveBases -> armBreakpoints — and this is where it is enforced, for
    // every caller rather than for the three that happened to check it.
    //
    // Watchpoints are address-based, so unlike source lines they mean something
    // before the program starts: they arm here, and the panel lists them.
    // Clear first, exactly like the armed path below, so repeated pre-base
    // edits re-arm the full set from the models instead of stacking duplicates.
    if (!m_host.bases().isValid()) {
        m_host.backend()->clearBreakpoints();
        for (const Watchpoint &wp : m_host.breakpoints()->watchpoints())
            m_host.backend()->armBreakpoint(wp.command());
        m_host.showWatchpoints(m_host.breakpoints()->watchpoints());
        return;
    }

    m_breakpointsArmedThisSession = true;
    const ArmPlan plan = planBreakpoints(m_host.breakpoints()->breakpoints(), m_host.programMap());

    // Clear before arming. A rebuilt program can occupy different addresses, so
    // leaving old breakpoints in place would silently break on whatever code now
    // lives at those addresses.
    m_host.backend()->clearBreakpoints();

    for (const QString &command : plan.commands)
        m_host.backend()->armBreakpoint(command);

    // Watchpoints arm alongside the source breakpoints. They are address-based,
    // so unlike source lines they are valid before the program even starts.
    for (const Watchpoint &wp : m_host.breakpoints()->watchpoints())
        m_host.backend()->armBreakpoint(wp.command());

    for (int i = 0; i < plan.unresolved.size(); ++i) {
        m_host.log(MainWindow::tr("[breakpoints] %1: %2")
                       .arg(plan.unresolved.at(i), plan.unresolvedReasons.value(i)));
    }

    if (!plan.commands.isEmpty()) {
        m_host.log(MainWindow::tr("[breakpoints] armed %1 of %2")
                       .arg(plan.commands.size())
                       .arg(m_host.breakpoints()->breakpoints().size()));
    }

    // Show the resolved addresses and flag any breakpoint that could not be
    // placed, so "why did my breakpoint not fire" is answerable at a glance.
    m_host.showArmedBreakpoints(m_host.breakpoints()->mergeResolved(plan),
                                m_host.breakpoints()->watchpoints());
}

void DebugSessionController::stepOut()
{
    // Hatari has no step-out primitive, so: read the return address from the
    // top of the stack (valid inside a jsr/bsr subroutine that has not
    // adjusted A7), arm a one-shot breakpoint there and resume. The read is
    // asynchronous — handleStackDumpForStepOut does the arming.
    if (!m_host.backend()->isStopped() || !m_host.lastState().regs.valid) {
        m_host.log(MainWindow::tr("[step out] needs a stopped program with known registers"));
        return;
    }
    if (m_stepOutPending)
        return;
    m_stepOutPending = true;
    m_host.backend()->requestStackDump(m_host.lastState().regs.a[7], 16);
}

bool DebugSessionController::handleStackDumpForStepOut(const QList<MemoryRow> &rows)
{
    if (!consumeStepOutPending())
        return true;

    // The dump is at A7, and a jsr/bsr leaves the return address as the long
    // on top: the first row's first long. The rows are the backend's parse of
    // the dump (MAJ-45), so this and the stack view read one parse, not two.
    const quint32 returnAddress = rows.isEmpty() ? 0 : readLongBE(rows.first().bytes, 0);
    if (!looksLikeAddress(returnAddress)) {
        m_host.log(MainWindow::tr("[step out] no plausible return address at A7 "
                                  "(0x%1) — not inside a subroutine?")
                       .arg(returnAddress, 0, 16));
        return false;
    }
    m_host.log(MainWindow::tr("[step out] to 0x%1").arg(returnAddress, 0, 16));
    m_host.backend()->breakAtAddressOnce(returnAddress);
    m_host.backend()->resume();
    return true;
}

} // namespace pist
