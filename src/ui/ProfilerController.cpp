// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ProfilerController.h"

// For MainWindow::tr alone: the controller's messages are written in the
// window's translation context. The include grants no access to the class — the
// controller reaches the window only through its Host (MIN-89).
#include "ui/MainWindow.h"

#include "ui/DebugSessionController.h"
#include "editor/CodeEditor.h"
#include "emu/DebugBackend.h"
#include "emu/ProfileData.h"

namespace pist {

ProfilerController::ProfilerController(Host host, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
{
}

void ProfilerController::resetSession()
{
    // A session's profiling mode does not cross into the next one: the flags
    // say what the *live* session is collecting, and the console/dock state the
    // session-end teardown clears is the transcript of the session that ended.
    m_guided = false;
    m_active = false;
    // The results belong to the session that produced them, like the dock's
    // copy of them: the remote `profile results` verb must not answer with a
    // dead session's hot lines after a reset.
    m_results = AttributedProfile();
}

void ProfilerController::start()
{
    // Hatari starts collecting on continue (DebugCpu_SetDebugging) and zeroes
    // the counters if any breakpoint command is issued mid-run — including
    // Pause's one-shot — so profiling is armed while stopped and the stopping
    // breakpoint must exist before this. The flow that works: set a
    // breakpoint, Profile Start here, continue, the breakpoint stops the run.
    if (!m_host.backend()->isStopped()) {
        const QString hint = MainWindow::tr("Needs a stopped session — run (F5), stop at a "
                                "breakpoint, then Profile Start");
        m_host.log(QStringLiteral("[profile] ") + hint);
        m_host.showMessage(hint);
        return;
    }
    m_host.backend()->profileOn();
    m_active = true;
    syncActions();
    m_host.log(MainWindow::tr("[profile] on — continue to collect, then use "
                              "Profile Stop at the next breakpoint stop"));
    m_host.showMessage(MainWindow::tr("Collecting — continue, then Profile Stop at the next stop"));
}

bool ProfilerController::stop()
{
    if (!m_host.backend()->isStopped() || m_host.sessionDir().isEmpty()) {
        const QString hint = MainWindow::tr("Needs a stopped session — the save command "
                                "needs the debugger");
        m_host.log(QStringLiteral("[profile] ") + hint);
        m_host.showMessage(hint);
        return false;
    }
    // The UAE disassembler core (the default with PiST's isolated config)
    // writes profile disassembly to the trace file instead of the save file,
    // so the save would contain only "[...]" gap markers — switch engines for
    // the save. Without Capstone the save stays empty and the parser's error
    // says so plainly.
    m_host.session()->setProfileSavePending(true);
    m_host.backend()->setDisasmEngine(IDebugBackend::DisasmEngine::Ext);
    m_host.backend()->profileSave(m_host.sessionDir() + QStringLiteral("/profile.txt"));
    m_host.backend()->profileOff();
    // The commands run in order, so the save is complete before this restores
    // the session's default engine — the Disassembly pane must not silently
    // keep the external renderer for the rest of the run.
    m_host.backend()->setDisasmEngine(IDebugBackend::DisasmEngine::Uae);
    m_active = false;
    syncActions();
    m_host.showMessage(MainWindow::tr("Saving — results appear when the save lands"));
    return true;
}

void ProfilerController::toCursor()
{
    // The whole ritual in one gesture: a one-shot at the cursor line (armed
    // BEFORE `profile on`, since any arm after it would zero the counters),
    // collection on, continue — onDebuggerStopped saves and shows.
    CodeEditor *const editor = m_host.editor();
    if (!m_host.backend()->isStopped() || !editor || editor->filePath().isEmpty()) {
        const QString hint = MainWindow::tr("Needs a stopped session — run (F5) and stop at "
                                "a breakpoint first");
        m_host.log(QStringLiteral("[profile] ") + hint);
        m_host.showMessage(hint);
        return;
    }
    const int line = editor->textCursor().blockNumber() + 1;
    quint32 address = 0;
    if (!m_host.programMap().codeAddressFor(editor->filePath(), line, &address)) {
        const QString hint = MainWindow::tr("No code on %1:%2 — put the cursor on an instruction line")
                                 .arg(editor->filePath())
                                 .arg(line);
        m_host.log(QStringLiteral("[profile] ") + hint);
        m_host.showMessage(hint);
        return;
    }
    m_guided = true;
    m_active = true;
    syncActions();
    m_host.backend()->breakAtAddressOnce(address);
    m_host.backend()->profileOn();
    const QString collecting = MainWindow::tr("Collecting to %1:%2 — results show on the stop")
                                   .arg(editor->filePath())
                                   .arg(line);
    m_host.log(QStringLiteral("[profile] ") + collecting);
    m_host.showMessage(collecting);
    m_host.backend()->resume();
}

void ProfilerController::showResults()
{
    ProfileData data;
    QString error;
    if (!parseProfile(m_host.sessionDir() + QStringLiteral("/profile.txt"), &data, &error)) {
        m_host.log(QStringLiteral("[profile] ") + error);
        m_host.resultsReady(false);
        return;
    }
    // The attribution is a free function over the parse, the line map and the
    // symbols (MAJ-44). Its result is held here, and both the gutter heat and
    // the remote JSON are read from this copy — the dock used to hold the only
    // one, so a programmatic caller depended on a dock existing and being
    // populated, and the analysis itself was unreachable from a test.
    m_results = attributedProfile(data, m_host.programMap(),
                                  m_host.editor() ? m_host.editor()->filePath() : QString(),
                                  m_host.symbols());
    m_host.showResults(m_results);
    m_host.log(MainWindow::tr("[profile] %1 instructions, %2 cycles at %3 Hz")
                   .arg(data.totalCount)
                   .arg(data.totalCycles)
                   .arg(data.clockHz));
    m_host.resultsReady(true);
}

void ProfilerController::syncActions()
{
    // Profiling is a mode: once collecting, Start and Profile-to-cursor make
    // no sense until Stop; with nothing collecting, Stop has nothing to save.
    // The tooltip always says WHY, so a disabled button is never a riddle.
    const bool stopped = m_host.backend() && m_host.backend()->isStopped();
    const QString needStopped = MainWindow::tr("Needs a stopped session — run (F5) and stop at a breakpoint");
    const QString alreadyCollecting =
        MainWindow::tr("Already collecting — Profile Stop and Show ends the run");

    m_host.setProfileAction(
        ProfileAction::Start, stopped && !m_active,
        m_active ? alreadyCollecting
                 : stopped ? MainWindow::tr("Start collecting CPU profile counts from here")
                           : needStopped);
    m_host.setProfileAction(
        ProfileAction::Stop, stopped && m_active,
        !m_active ? MainWindow::tr("Nothing is collecting — Profile Start begins a run")
                  : stopped ? MainWindow::tr("Save the profile, then show hot lines and gutter heat")
                            : needStopped);
    m_host.setProfileAction(
        ProfileAction::ToCursor, stopped && !m_active,
        m_active ? alreadyCollecting
                 : stopped ? MainWindow::tr("Collect profile counts to the cursor line, then show the results")
                           : needStopped);
}

} // namespace pist
