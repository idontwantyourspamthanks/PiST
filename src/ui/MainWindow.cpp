// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
#include "ui/BinaryView.h"
#include "ui/MainWindowPrivate.h"
#include "emu/HexFormat.h"
#include "support/CrashReport.h"
#include "support/DocumentWatcher.h"
#include "support/FileWrite.h"

#include "ui/BreakpointWatchpointModel.h"
#include "ui/DebugSessionController.h"
#include "ui/ProfilerController.h"
#include "ui/RemoteStateAdapter.h"
#include "ui/SessionLauncher.h"
#include "hooks/StopHookRunner.h"
#include "ui/StopHooksDialog.h"
#include "ui/SymbolPaletteDialog.h"
#include "ui/UiText.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/HatariTextParse.h"
#include "emu/EmulatorHost.h"
#include "emu/DebugBackend.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"
#include "ui/InstructionRefView.h"
#include "build/FloppyImage.h"
#include "editor/CursorReference.h"
#include "editor/IncludeNav.h"
#include "editor/InstrRef.h"
#include "editor/OsCallBinding.h"
#include "editor/OsCallRef.h"
#include "editor/OsCallScan.h"
#include "ui/ConsoleInput.h"
#include "ui/SymbolsView.h"
#include "control/RemoteControl.h"
#include "ui/ProfilerView.h"
#include "emu/ProfileData.h"
#include "build/SymbolTable.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "analysis/ProgramTrace.h"
#include "image/ImageDocument.h"
#include "build/FloppyImage.h"
#include "image/BinaryScan.h"
#include "image/StFormats.h"
#include "toolchain/Toolchain.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "ui/BreakpointPanel.h"
#include "ui/BitplaneExportDialog.h"
#include "ui/DisassemblyView.h"
#include "ui/EmulatorDisplayWidget.h"
#include "ui/EmuAudio.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include "ui/FileBrowser.h"
#include "ui/FindInFilesView.h"
#include "ui/GitPanel.h"
#include "ui/ImageEditor.h"
#include "ui/NewImageDialog.h"
#include "ui/NewFileDialog.h"
#include "ui/WelcomeView.h"
#include "ui/AboutDialog.h"
#include "ui/MemoryView.h"
#include "ui/PcHistoryView.h"
#include "ui/HardwareView.h"
#include "ui/SettingsDialog.h"
#include "ui/SetupDialog.h"
#include "ui/StackView.h"
#include "ui/RegistersView.h"

#include <algorithm>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QTextBlock>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QSet>
#include <QSize>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolTip>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtEndian>

namespace pist {

/// The source a project file implies: `<project>.pistproject` sits beside
/// `<project>.s`. The source path is deliberately not stored in the file — it
/// would go stale the moment either moved — so the derivation lives here once, and
/// the doorways that can be handed a project (the welcome rows, the Demo button)
/// all ask for it rather than opening the JSON as a document.
QString sourceForProject(const QString &projectPath)
{
    const QString suffix = QLatin1String(settings::kProjectSuffix);
    if (!projectPath.endsWith(suffix, Qt::CaseInsensitive))
        return {};
    const QString source =
        projectPath.left(projectPath.size() - suffix.size()) + QStringLiteral(".s");
    return QFileInfo::exists(source) ? source : QString();
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // The assembler's own vocabulary is available before anything is assembled: an
    // editor opened in a project that has never been built still completes
    // mnemonics and directives, and a build only *adds* label and equate
    // candidates on top (rebuildProgramMap). Seeding it here rather than at the
    // first build is what makes completion work in a fresh session — before this
    // line existed, so did nothing.
    m_completionItems = CompletionModel::vocabulary();

    // The MAJ-41 seams, before anything they own: the session controller holds
    // the lifecycle flags, the launcher the run path, the models the
    // breakpoint/watchpoint lists, the profiler its collecting mode and the
    // adapter the remote-control verbs. Qt parent-child owns all five.
    //
    // Each is handed a Host — the operations it performs on this window — rather
    // than a pointer back into the class (MIN-89; the friend declarations that
    // stood in for this are gone). The aggregates below are therefore the whole
    // of what a seam can reach, in one place. They are built out of lambdas
    // because the members they read are assigned later in this constructor: a
    // Host is only ever *called* once the window is up.
    const auto backendNow = [this]() -> IDebugBackend * { return m_host; };
    const auto sessionNow = [this]() -> DebugSessionController * { return m_session; };
    const auto editorNow = [this]() -> CodeEditor * { return m_editor; };
    const auto programMapNow = [this]() -> const ProgramLineMap & { return m_programMap; };
    const auto symbolsNow = [this]() -> const QVector<SymbolEntry> & { return m_symbols; };
    const auto logLine = [this](const QString &line) { m_log->appendPlainText(line); };

    // Showing the breakpoint models in the dock's panel. Each is a no-op before
    // the panel exists, which is how the seams can call them unconditionally.
    const auto showBreakpoints = [this](const QList<Breakpoint> &breakpoints) {
        if (m_breakpointPanel)
            m_breakpointPanel->setBreakpoints(breakpoints);
    };
    const auto showWatchpoints = [this](const QList<Watchpoint> &watchpoints) {
        if (m_breakpointPanel)
            m_breakpointPanel->setWatchpoints(watchpoints);
    };
    const auto showArmedBreakpoints = [this](const QList<Breakpoint> &breakpoints,
                                             const QList<Watchpoint> &watchpoints) {
        if (!m_breakpointPanel)
            return;
        m_breakpointPanel->setResolvable(true);
        m_breakpointPanel->setBreakpoints(breakpoints);
        m_breakpointPanel->setWatchpoints(watchpoints);
    };

    // The window's half of a session's end (the controller's resetSessionState):
    // what the last session left on screen. The PC bar goes with it — it was
    // painted from that stop's PC and nothing else cleared it (only
    // locationFromPc's failure paths did), so after Stop it stayed on a line the
    // session no longer runs at, and a wait for "the program counter moved to
    // line N" matched the stale highlight instantly instead of the new
    // session's stop. Same shape as the session-end teardown's setLineHeat({})
    // for the profiler heat.
    const auto clearSessionViews = [this] {
        m_stoppedFile.clear();
        m_stoppedLine = 0;
        // A session ends with the caret wherever the user left it, and with no
        // claim on it: the next session's first stop is a placement again, not
        // a movement (cursor-plan §3.1).
        setPcFollow(false);
        m_pcPlaced = false;
        m_pcFollowInProgress = false;
        m_bases = LineMap::SectionBases();
        m_lastState = MachineState();
        m_pendingDebugCommand.clear();
        for (CodeEditor *editor : openEditors())
            editor->clearCurrentExecutionLine();
    };

    DebugSessionController::Host sessionHost;
    sessionHost.backend = backendNow;
    sessionHost.profiler = [this] { return m_profiling; };
    sessionHost.breakpoints = [this] { return m_bpModel; };
    sessionHost.programMap = programMapNow;
    sessionHost.symbols = symbolsNow;
    sessionHost.bases = [this]() -> const LineMap::SectionBases & { return m_bases; };
    // The session boots with fast-forward on either way (a real-speed TOS
    // boot is ~30 s); the project's preference is applied at the entry stop.
    // The audio queue must shed the boot's chopped debris at the same moment,
    // or it plays over the program's first real-time instants.
    sessionHost.bootTurboToDrop = [this] { return !m_settings.fastForward; };
    sessionHost.bootTurboDropped = [this] {
        if (m_audio)
            m_audio->reset();
    };
    sessionHost.lastState = [this]() -> const MachineState & { return m_lastState; };
    sessionHost.log = logLine;
    sessionHost.showWatchpoints = showWatchpoints;
    sessionHost.showArmedBreakpoints = showArmedBreakpoints;
    sessionHost.clearSessionViews = clearSessionViews;
    m_session = new DebugSessionController(sessionHost, this);

    SessionLauncher::Host launcherHost;
    launcherHost.settings = [this]() -> const ProjectSettings & { return m_settings; };
    launcherHost.buildSourcePath = [this] { return buildSourcePath(); };
    launcherHost.makeSessionDir = [this] { return makeSessionDir(); };
    launcherHost.backend = backendNow;
    // The transport switch a launch may need. It used to be the launcher
    // reaching in and re-wiring m_host by hand — the leaky half of the
    // shell/launcher boundary (MIN-89); the window owns its backend, so the
    // window performs the swap.
    launcherHost.selectBackend = [this](BackendKind wanted) {
        if (m_host->kind() == wanted)
            return;
        m_host->stop();
        delete m_host;
        m_host = createBackend(wanted, this);
        wireBackend();
    };
    // A launch that runs without the media display (a non-hatari-pist
    // emulator) ends any media mode a previous session engaged.
    launcherHost.leaveMediaDisplay = [this] {
        m_mediaDisplay = false;
        m_lastMediaFrame = QImage();
    };
    // The media display (docs/PLAN.md §12). A media session must not embed, so
    // this is where the panel is shown for it, and the port it returns is what
    // the emulator's argv will name. Every backend carries a media server:
    // the hatari-pist fork speaks --pist-media no matter which debug
    // transport the session runs on.
    launcherHost.engageMediaDisplay = [this](const HatariCapabilities &caps)
        -> SessionLauncher::Host::MediaEngagement {
        if (!caps.hasPistMedia || !m_display)
            return {};
        const int port = m_host->mediaListen();
        if (port <= 0)
            return {};
        m_mediaDisplay = true;
        m_lastMediaFrame = QImage();
        m_displayDock->setVisible(true);
        m_display->setVisible(true);
        return {port, m_host->mediaServer().token()};
    };
    launcherHost.quiet = [this] { return m_quietDialogs; };
    launcherHost.refuseRun = [this](const QString &title, const QString &reason, bool critical) {
        refuseRun(title, reason, critical);
    };
    launcherHost.log = logLine;
    launcherHost.showStatus = [this](const QString &text, int milliseconds) {
        statusBar()->showMessage(text, milliseconds);
    };
    launcherHost.session = sessionNow;
    m_launcher = new SessionLauncher(launcherHost, this);

    BreakpointWatchpointModel::Host breakpointHost;
    breakpointHost.session = sessionNow;
    breakpointHost.programMap = programMapNow;
    breakpointHost.symbols = symbolsNow;
    breakpointHost.editor = editorNow;
    breakpointHost.editors = [this] { return openEditors(); };
    breakpointHost.backend = backendNow;
    breakpointHost.log = logLine;
    breakpointHost.showBreakpoints = showBreakpoints;
    breakpointHost.showWatchpoints = showWatchpoints;
    m_bpModel = new BreakpointWatchpointModel(breakpointHost, this);

    ProfilerController::Host profilerHost;
    profilerHost.backend = backendNow;
    profilerHost.session = sessionNow;
    profilerHost.editor = editorNow;
    profilerHost.programMap = programMapNow;
    profilerHost.symbols = symbolsNow;
    profilerHost.sessionDir = [this] { return m_currentSessionDir; };
    profilerHost.log = logLine;
    profilerHost.showMessage = [this](const QString &hint) { m_profiler->showMessage(hint); };
    // One attributed value, three presentations (MAJ-44): the dock's table, the
    // dock brought forward, and the current editor's gutter heat.
    profilerHost.showResults = [this](const AttributedProfile &profile) {
        m_profiler->setProfile(profile);
        if (m_profilerDock) {
            m_profilerDock->show();
            m_profilerDock->raise();
        }
        if (m_editor)
            m_editor->setLineHeat(lineCounts(profile));
    };
    profilerHost.resultsReady = [this](bool ok) { emit profileResultsReady(ok); };
    profilerHost.setProfileAction = [this](ProfilerController::ProfileAction action, bool enabled,
                                           const QString &tooltip) {
        QAction *const target = action == ProfilerController::ProfileAction::Start
            ? m_actProfileStart
            : action == ProfilerController::ProfileAction::Stop ? m_actProfileStop
                                                                : m_actProfileToCursor;
        if (!target)
            return;
        target->setEnabled(enabled);
        target->setToolTip(tooltip);
    };
    m_profiling = new ProfilerController(profilerHost, this);

    // The stop hook (docs/stop-hooks-plan.md). A seam like the others: it owns
    // the script's run and what the script may reach, and gets the window's
    // operations as callbacks. `resume` is the window's own slot, deliberately,
    // so a script's continue is the Continue action — arms queued at the entry
    // stop survive it (invariant 5).
    StopHookRunner::Host hookHost;
    hookHost.backend = backendNow;
    hookHost.log = logLine;
    hookHost.scriptPath = [this] { return m_settings.stopHookPath; };
    // `sym()` in a script: the same rule the remote `breakpoint <label>` verb
    // uses, through the model that owns the symbols and the program map.
    hookHost.symbol = [this](const QString &name, quint32 *address, QString *error) {
        return m_bpModel->resolveLabel(name, address, error);
    };
    hookHost.enabled = [this] { return m_settings.stopHookEnabled && !m_stopHookRefused; };
    hookHost.autoContinue = [this] { return m_settings.stopHookAutoContinue; };
    hookHost.resume = [this] { resume(); };
    hookHost.refresh = [this] { m_host->refresh(); };
    // Held for the duration of a run: with no command of the user's in flight,
    // the completion that arrives is the one the hook issued — so a script's
    // write cannot be pruned by the resume it asks for (R5). The console's
    // enabled state is otherwise the session's (MainWindow's stoppedChanged
    // handler), hence the same expression when releasing it.
    hookHost.holdConsole = [this](bool hold) {
        if (m_consoleInput)
            m_consoleInput->setEnabled(hold ? false : m_host->isRunning());
    };
    m_hooks = new StopHookRunner(hookHost, this);

    // What the hook did reaches a watcher here rather than as part of `stopped`:
    // that event is published *before* the script runs, because the run needs the
    // stop's state to be real first (D1). Emitted before the continue policy, so
    // the wire order is stopped → hook → running.
    connect(m_hooks, &StopHookRunner::runFinished, this,
            [this](const StopHookRunner::LastRun &run) {
                if (m_eventSink)
                    m_eventSink->publishEvent(QStringLiteral("hook"), hookEventDetail(run));
            });

    RemoteStateAdapter::Host stateHost;
    stateHost.editor = [this]() -> const CodeEditor * { return m_editor; };
    stateHost.backend = [this]() -> const IDebugBackend * { return m_host; };
    stateHost.lastState = [this]() -> const MachineState & { return m_lastState; };
    stateHost.problems = [this]() -> const QList<RemoteStateAdapter::ProblemRow> & {
        return m_problemEntries;
    };
    stateHost.tabs = [this] { return m_tabs; };
    stateHost.symbols = symbolsNow;
    stateHost.programMap = programMapNow;
    stateHost.profile = [this]() -> const AttributedProfile & { return m_profiling->results(); };
    stateHost.hookEnabled = [this] { return m_settings.stopHookEnabled && !m_stopHookRefused; };
    stateHost.hookPath = [this] { return m_settings.stopHookPath; };
    stateHost.hookAutoContinue = [this] { return m_settings.stopHookAutoContinue; };
    stateHost.hookLastRun = [this]() -> const StopHookRunner::LastRun & {
        return m_hooks->lastRun();
    };
    m_remoteState = new RemoteStateAdapter(stateHost, this);

    // Documents live in tabs. openPath() dispatches `.pim` / ST still-images
    // to addImageTab() and everything else to addEditorTab(). A binary file
    // opened for hex is a BinaryView tab, read-only. m_editor tracks the
    // current *text* editor and is null on an image or binary tab; m_image is
    // the reverse. Build/Run use buildSourcePath() so a focused image or hex
    // tab does not strand the assembler.
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("documentTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);

    // The instruction strip sits under the tabs, not in the dock: the dock is
    // one tab among the debug group, and a mnemonic the user is reading should
    // not depend on that tab being selected.
    auto *documents = new QWidget(this);
    auto *column = new QVBoxLayout(documents);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(m_tabs, 1);
    m_instrStrip = new QPushButton(documents);
    m_instrStrip->setObjectName(QStringLiteral("instructionStrip"));
    m_instrStrip->setFlat(true);
    m_instrStrip->setFocusPolicy(Qt::NoFocus);
    m_instrStrip->setCursor(Qt::PointingHandCursor);
    m_instrStrip->setToolTip(tr("Open the instruction reference"));
    appearance::markMono(m_instrStrip);
    connect(m_instrStrip, &QPushButton::clicked, this, &MainWindow::raiseInstructionRef);
    column->addWidget(m_instrStrip);
    m_registerStrip = new QLabel(documents);
    m_registerStrip->setObjectName(QStringLiteral("registerStrip"));
    m_registerStrip->setVisible(false);
    appearance::markMono(m_registerStrip);
    column->addWidget(m_registerStrip);

    // The central area is two pages: the documents, and the welcome view that
    // stands in for them when there are none. Zero documents is a real state
    // now — a document is a file on disk, and an empty "untitled" buffer was
    // only ever a document in appearance (docs/untitled-plan.md §5).
    m_center = new QStackedWidget(this);
    m_center->setObjectName(QStringLiteral("centralPages"));
    m_center->addWidget(documents);
    m_welcome = new WelcomeView(m_center);
    m_welcome->setObjectName(QStringLiteral("welcomeView"));
    m_center->addWidget(m_welcome);
    setCentralWidget(m_center);

    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // The welcome view's doors.
    connect(m_welcome, &WelcomeView::openRequested, this,
            [this](const QString &path) {
                // A doorway may name a project rather than a source — the demo rows
                // do — and opening the project file itself would show its JSON as a
                // document. The source beside it is what the demo is.
                const QString source = sourceForProject(path);
                openPath(source.isEmpty() ? path : source);
            });
    connect(m_welcome, &WelcomeView::browseRequested, this, &MainWindow::openFile);
    connect(m_welcome, &WelcomeView::newFileRequested, this, &MainWindow::newFile);
    connect(m_welcome, &WelcomeView::newImageRequested, this, &MainWindow::newImage);
    connect(m_welcome, &WelcomeView::setupRequested, this, &MainWindow::showToolSetup);

    // Every open document's file is watched, so a deletion or a rename outside
    // the IDE is noticed rather than discovered on the next save
    // (docs/untitled-plan.md §7.1). The sidebar's own rename and delete gestures
    // come through their own signals; this covers everything else.
    m_watcher = new DocumentWatcher(this);
    connect(m_watcher, &DocumentWatcher::deleted, this,
            [this](const QString &path) { documentVanished(path, /*deliberate=*/false); });

    // No pristine tab is created here: a document is a file on disk, so the
    // window opens on the welcome view and something real replaces it (the
    // command line's file, or the reopen below).

    m_build = new BuildService(this);
    connect(m_build, &BuildService::finished, this, &MainWindow::onBuildFinished);
    connect(m_build, &BuildService::outputLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });


    // EmuAudio is a plain QObject with no widget dependency, and it must
    // exist before wireBackend() connects the backend's mediaAudioReceived —
    // created down in createDocks() it would never exist in time, which is
    // exactly where it started life (and why no audio was routed).
    m_audio = new EmuAudio(this);
    applyAudioOutputDevice();
    m_host = createBackend(BackendKind::Native, this);
    wireBackend();

    createActions();
    createMenus();
    createDocks();
    createToolBar();
    createStatusBar();
    applyAppearance();

    // Sessions left by a crash or a kill, once they are old enough that no live
    // instance could still own them.
    paths::pruneStaleSessions();

    // The lifetime counters (docs/untitled-plan.md §6): this is a session, and
    // the first one also records the date the installation started counting.
    settings::noteSessionStarted();

    // Reopen where the user left off when nothing was passed on the command
    // line. A last source that no longer exists opens nothing, and the welcome
    // view is what remains — which is the honest answer to "where was I?".
    QTimer::singleShot(0, this, [this] {
        if (m_tabs->count() == 0)
            openRecentSource();
        updateCentralPage();
    });

    refreshToolchain();

    updateModifiedState();
    // The actions exist only now, so run the per-tab pass once, or Find would
    // stay greyed out until the first tab switch.
    onTabChanged(m_tabs->currentIndex());
    finalizeLayout();
}

void MainWindow::refreshToolchain()
{
    // Run at construction (after the status bar exists) and after the setup
    // dialog closes: a piece the dialog just installed (e.g. vasm in the
    // per-user tools directory, which is not on PATH) must take effect without
    // a restart, or the first build after a successful setup would still fail.
    const ToolInfo assembler = toolchain::findAssembler(m_settings.assemblerPath);
    m_build->setAssemblerPath(assembler.found() ? assembler.path
                                                : QStringLiteral("vasmm68k_mot"));
    // Probe the emulator the session would actually run: with a project
    // override pointing elsewhere, the discovered binary's capabilities are
    // the wrong answer for the status bar.
    m_caps = probeHatari(toolchain::findEmulator(m_settings.hatariPath).path);
    // A configured path that resolves to the wrong tool is reported as not
    // found (toolchain::locate), which would otherwise leave the chip naming a
    // bare fallback while the reason — the only sentence that says what is
    // actually misconfigured — went unread. Put it where the eye already is.
    if (!assembler.reason.isEmpty())
        qWarning("toolchain: %s", qPrintable(assembler.reason));
    m_statusToolchain->setText(assembler.reason.isEmpty()
                                   ? QStringLiteral("vasm: %1").arg(
                                         QFileInfo(m_build->assemblerPath()).fileName())
                                   : assembler.reason);
    updateSessionChip();
}

MainWindow::~MainWindow()
{
    // A live session has to be stopped while the window is still whole. stop()
    // announces the session end (EmulatorHost does it from its own destructor
    // too) and that handler touches sibling widgets — the hardware and PC
    // history transcripts, the profiler, every open editor's gutter heat — which
    // QObject::deleteChildren is by then taking down in its own order, so a
    // window destroyed with an emulator running reads freed widgets: measured
    // as a SIGSEGV/SIGABRT inside the session-end handler, with a real Hatari
    // left behind because the teardown never finished. closeEvent stops the
    // session for the ordinary quit; this is the same guarantee for every other
    // way the window dies (a test's early return, a delete, a crash path).
    if (m_host->isRunning())
        m_host->stop();

    // The previous-session summary, written once and only here
    // (docs/untitled-plan.md §6). The counters are already on disk as they
    // happen, so a crash costs this line and nothing the user did.
    settings::LastSession summary;
    summary.builds = m_sessionBuilds;
    summary.lastBuildOk = m_lastBuildOk;
    summary.lastFile = settings::lastSourcePath();
    settings::flushLastSession(summary);
}

QString MainWindow::makeSessionDir()
{
    // The session directory hosts the generated bootstrap script, the control
    // socket, and an isolated Hatari config. It must be short on Unix, because
    // the socket path has to fit in sockaddr_un::sun_path (108 bytes on Linux).
    // Windows uses named pipes and has no equivalent limit.
    // Starting a new session supersedes the previous one, so drop it now rather
    // than leaving a directory (and its config tree) behind on every run.
    if (!m_currentSessionDir.isEmpty()) {
        paths::removeSessionDir(m_currentSessionDir);
        m_currentSessionDir.clear();
    }

    static int counter = 0;
    const QString dir = paths::sessionBaseDir()
                      + QStringLiteral("/%1-%2")
                            .arg(QCoreApplication::applicationPid())
                            .arg(++counter);
    QString error;
    if (!paths::ensureDirectory(dir, &error)) {
        m_log->appendPlainText(QStringLiteral("[session] ") + error);
        return {};
    }
    m_currentSessionDir = dir;
    return dir;
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_restoredSize.isValid()
        && !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
        resize(m_restoredSize);
        m_restoredSize = QSize();
    }
    if (!m_applyFactorySizes)
        return;
    m_applyFactorySizes = false;
    applyFactoryDockSizes();
    // Reset layout should return to this sized arrangement, not the one
    // captured before the window had a real size.
    m_defaultLayoutState = saveState();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }

    // The panel arrangement and the window size persist across runs, so a
    // layout the user has arranged to taste is there next time
    // (docs/FUTURE.md §7).
    QSettings settings;
    settings.setValue(layoutGeometryKey(), saveGeometry());
    settings.setValue(layoutWidthKey(), width());
    settings.setValue(layoutHeightKey(), height());
    settings.setValue(layoutStateKey(), saveState());

    // Leaving this to the age-based prune would mean a directory survives every
    // ordinary quit, not just a crash.
    m_host->stop();
    if (!m_currentSessionDir.isEmpty()) {
        paths::removeSessionDir(m_currentSessionDir);
        m_currentSessionDir.clear();
    }
    QMainWindow::closeEvent(event);
}

QJsonArray MainWindow::symbolsJson(const QString &filter) const
{
    return m_remoteState->symbolsJson(filter);
}

QJsonObject MainWindow::hookJson() const
{
    return m_remoteState->hookJson();
}

QJsonObject MainWindow::documentJson() const
{
    return m_remoteState->documentJson();
}

QJsonObject MainWindow::stateJson() const
{
    return m_remoteState->stateJson();
}

QJsonArray MainWindow::problemsJson() const
{
    return m_remoteState->problemsJson();
}

QJsonArray MainWindow::tabsJson() const
{
    return m_remoteState->tabsJson();
}

bool MainWindow::saveCurrentDocument()
{
    // An image tab may still have no path of its own (an imported sheet); a
    // source document always does, so the old "no path yet" refusal is gone —
    // what survives is "nothing is open".
    if (m_image)
        return !m_image->filePath().isEmpty() && saveImage(m_image, m_image->filePath());
    if (!m_editor)
        return false;
    return saveEditor(m_editor, m_editor->filePath());
}

QJsonArray MainWindow::profilerResultsJson() const
{
    return m_remoteState->profilerResultsJson();
}

void MainWindow::showFindInFiles()
{
    if (!m_findInFilesDock)
        return;
    // The scope can have moved since it was last searched (another file opened,
    // another directory chosen), and the dock re-runs its query when it has.
    syncFindInFilesScope();
    m_findInFilesDock->show();
    m_findInFilesDock->raise();
    m_findInFiles->focusQuery();
}

QString MainWindow::debugConsoleText() const
{
    return m_log ? m_log->toPlainText() : QString();
}

QString MainWindow::stateSummary() const
{
    return m_remoteState->stateSummary();
}

bool MainWindow::writeMemoryByte(quint32 address, quint32 value)
{
    // The remote `setmem`: no view asked for it, so with no pane to refresh the
    // first one is (setMemoryByte's own rule for a null pane).
    return setMemoryByte(address, value);
}

bool MainWindow::ripImage(const QString &path, quint32 offset, int width, int height,
                          const QString &outPath, bool hasPalette, quint32 paletteOffset,
                          QString *error)
{
    const bool sizeOk = width >= 16 && width <= kStScreenWidth && width % 16 == 0 && height >= 1
                        && height <= kStScreenHeight;
    if (!sizeOk) {
        ImageDocument ignored;
        QString decodeError;
        imageFromLowResBitplanes(QByteArray(), width, height, QByteArray(), &ignored, &decodeError);
        if (error)
            *error = decodeError;
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.seek(offset)) {
        if (error)
            *error = QStringLiteral("offset is past the end");
        return false;
    }
    const int need = (width / 16) * 8 * height;
    const QByteArray planes = file.read(need);
    if (planes.size() != need) {
        if (error)
            *error = QStringLiteral("short read");
        return false;
    }
    QByteArray palette;
    if (hasPalette) {
        if (!file.seek(paletteOffset)) {
            if (error)
                *error = QStringLiteral("palette is short");
            return false;
        }
        palette = file.read(32);
        if (palette.size() != 32) {
            if (error)
                *error = QStringLiteral("palette is short");
            return false;
        }
    }
    ImageDocument doc;
    QString decodeError;
    if (!imageFromLowResBitplanes(planes, width, height, palette, &doc, &decodeError)) {
        if (error)
            *error = decodeError;
        return false;
    }
    const QByteArray json = doc.toJson();
    if (json.isEmpty()) {
        if (error)
            *error = QStringLiteral("the document produced no JSON");
        return false;
    }
    return files::write(outPath, json, error);
}

bool MainWindow::ripSample(const QString &path, quint32 offset, int length, int rate,
                           bool signedSamples, const QString &outPath, QString *error)
{
    if (length < 1) {
        wavFromPcm8(QByteArray(), rate, signedSamples, error);
        return false;
    }
    if (length > 65536) {
        wavFromPcm8(QByteArray(65537, '\0'), rate, signedSamples, error);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.seek(offset)) {
        if (error)
            *error = QStringLiteral("offset is past the end");
        return false;
    }
    const QByteArray samples = file.read(length);
    if (samples.size() != length) {
        if (error)
            *error = QStringLiteral("short read");
        return false;
    }
    const QByteArray wav = wavFromPcm8(samples, rate, signedSamples, error);
    if (wav.isEmpty())
        return false;
    return files::write(outPath, wav, error);
}

bool MainWindow::writeBinaryFile(const QString &path, const QByteArray &bytes, QString *error)
{
    if (bytes.isEmpty()) {
        if (error)
            *error = QStringLiteral("nothing to write");
        return false;
    }
    return files::write(path, bytes, error);
}

bool MainWindow::writeCapturedScreen(const QString &outPath, const QByteArray &resolutionByte,
                                    const QByteArray &paletteWords, const QByteArray &planes,
                                    QString *error)
{
    if (resolutionByte.isEmpty()) {
        if (error)
            *error = QStringLiteral("short read");
        return false;
    }
    ImageDocument doc;
    if (!imageFromLowResScreen(planes, paletteWords, int(uchar(resolutionByte.at(0))), &doc, error))
        return false;
    const QByteArray json = doc.toJson();
    if (json.isEmpty()) {
        if (error)
            *error = QStringLiteral("the document produced no JSON");
        return false;
    }
    return files::write(outPath, json, error);
}

bool MainWindow::writeCapturedDma(const QString &outPath, int rateHz, const QByteArray &samples,
                                 QString *error)
{
    const QByteArray wav = wavFromPcm8(samples, rateHz, false, error);
    if (wav.isEmpty())
        return false;
    return files::write(outPath, wav, error);
}

namespace {

QString waitForInfo(MainWindow *window, const QString &command)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QString response;
    QMetaObject::Connection connection = window->onDebugCommandFinished(
        &loop, [&](const QString &finished, const QString &text) {
            if (finished != command)
                return;
            response = text;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    window->debugCommand(command);
    loop.exec();
    QObject::disconnect(connection);
    return response;
}

bool readExactMemory(MainWindow *window, quint32 address, int length, QByteArray *out, QString *error)
{
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QList<MemoryRow> rows;
    bool arrived = false;
    QMetaObject::Connection connection = window->onDebugReadMemoryFinished(
        &loop, [&](quint32 at, const QList<MemoryRow> &got) {
            if (at != address)
                return;
            rows = got;
            arrived = true;
            loop.quit();
        });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    window->debugReadMemory(address, length);
    loop.exec();
    QObject::disconnect(connection);
    QByteArray bytes;
    if (arrived) {
        for (const MemoryRow &row : rows) {
            for (quint8 byte : row.bytes)
                bytes.append(char(byte));
        }
    }
    if (!arrived || bytes.size() != length) {
        if (error)
            *error = QStringLiteral("short read %1").arg(address, 8, 16, QLatin1Char('0'));
        return false;
    }
    *out = bytes;
    return true;
}

} // namespace

bool MainWindow::captureScreenTo(const QString &outPath, QString *error)
{
    const QString video = waitForInfo(this, QStringLiteral("info video"));
    const HardwareSummary hardware = hataritext::parseHardwareInfo(video);
    if (video.isEmpty() || !hardware.hasScreenBase) {
        if (error)
            *error = QStringLiteral("no screen base");
        return false;
    }
    QByteArray resolution;
    QByteArray palette;
    QByteArray planes;
    if (!readExactMemory(this, 0xff8260, 1, &resolution, error)
        || !readExactMemory(this, 0xff8240, 32, &palette, error)
        || !readExactMemory(this, hardware.screenBase, 32000, &planes, error))
        return false;
    return writeCapturedScreen(outPath, resolution, palette, planes, error);
}

bool MainWindow::captureDmaTo(const QString &outPath, QString *error)
{
    const QString dma = waitForInfo(this, QStringLiteral("info dmasnd"));
    const hataritext::DmaSoundInfo sound = hataritext::parseDmaSoundInfo(dma);
    if (!sound.present) {
        if (error)
            *error = QStringLiteral("no DMA sound");
        return false;
    }
    if (sound.stereo) {
        if (error)
            *error = QStringLiteral("DMA frame is stereo");
        return false;
    }
    const quint32 length = sound.end - sound.start;
    if (sound.end <= sound.start || length > 65536) {
        if (error)
            *error = QStringLiteral("DMA frame is %1 bytes").arg(length);
        return false;
    }
    QByteArray samples;
    if (!readExactMemory(this, sound.start, int(length), &samples, error))
        return false;
    return writeCapturedDma(outPath, sound.rateHz, samples, error);
}

void MainWindow::captureScreen()
{
    if (!m_host->isStopped()) {
        QMessageBox::warning(this, tr("Capture Screen"), tr("The machine is not stopped."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Capture Screen"), QString(),
                                                      tr("PiST sprite document (*.pim)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!captureScreenTo(path, &error)) {
        QMessageBox::warning(this, tr("Capture Screen"), error);
        return;
    }
    openPath(path);
}

void MainWindow::captureDma()
{
    if (!m_host->isStopped()) {
        QMessageBox::warning(this, tr("Capture DMA Sound"), tr("The machine is not stopped."));
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, tr("Capture DMA Sound"), QString(),
                                                      tr("WAV audio (*.wav)"));
    if (path.isEmpty())
        return;
    QString error;
    if (!captureDmaTo(path, &error)) {
        QMessageBox::warning(this, tr("Capture DMA Sound"), error);
        return;
    }
    statusBar()->showMessage(tr("WAV written"), 4000);
}

bool MainWindow::ripAvr(const QString &path, quint32 offset, const QString &outPath, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (!file.seek(offset)) {
        if (error)
            *error = QStringLiteral("offset is past the end");
        return false;
    }
    const QByteArray header = file.read(128);
    AvrInfo info;
    if (!parseAvrHeader(header, &info, error))
        return false;
    const QByteArray samples = file.read(int(info.length));
    if (quint32(samples.size()) != info.length) {
        if (error)
            *error = QStringLiteral("short AVR");
        return false;
    }
    QByteArray fileBytes = header;
    fileBytes.append(samples);
    const QByteArray wav = wavFromAvr(fileBytes, error);
    if (wav.isEmpty())
        return false;
    return files::write(outPath, wav, error);
}

QJsonArray MainWindow::scanFileMagic(const QString &path, QString *error)
{
    if (QFileInfo(path).size() > qint64(2 * 1024 * 1024)) {
        if (error)
            *error = QStringLiteral("file is over 2MiB");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    QJsonArray hits;
    for (const MagicHit &hit : scanMagic(file.readAll())) {
        QJsonObject object;
        object.insert(QStringLiteral("offset"), hit.offset);
        object.insert(QStringLiteral("kind"), hit.kind);
        hits.append(object);
    }
    return hits;
}

QJsonObject MainWindow::analyzeProgram(const QString &path, QString *error)
{
    if (QFileInfo(path).size() > qint64(2 * 1024 * 1024)) {
        if (error)
            *error = QStringLiteral("file is over 2MiB");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    const ProgramListing listing = listProgram(file.readAll());
    if (!listing.error.isEmpty()) {
        if (error)
            *error = listing.error;
        return {};
    }
    QJsonObject result;
    result.insert(QStringLiteral("summary"), listing.summary);
    QJsonArray rows;
    for (const ListingRow &row : listing.rows) {
        QJsonObject object;
        object.insert(QStringLiteral("address"),
                      QStringLiteral("0x") + hex::hex32(row.address, hex::Case::Lower));
        object.insert(QStringLiteral("label"), row.label);
        object.insert(QStringLiteral("bytes"), row.bytes);
        object.insert(QStringLiteral("text"), row.text);
        object.insert(QStringLiteral("note"), row.note);
        rows.append(object);
    }
    result.insert(QStringLiteral("rows"), rows);
    return result;
}

QJsonArray MainWindow::scanDiskMagic(const QString &imagePath, QString *error)
{
    QString listError;
    const QVector<floppy::Entry> entries = floppy::listImage(imagePath, &listError);
    if (!listError.isEmpty()) {
        if (error)
            *error = listError;
        return {};
    }
    QByteArray raw;
    QString rawError;
    if (!floppy::loadRaw(imagePath, &raw, &rawError)) {
        if (error)
            *error = rawError;
        return {};
    }

    QJsonArray result;
    int hitCount = 0;
    bool capped = false;
    constexpr int kCap = 64;
    for (const floppy::Entry &entry : entries) {
        if (entry.isDirectory)
            continue;
        if (hitCount >= kCap) {
            capped = true;
            break;
        }
        if (entry.size > quint32(2 * 1024 * 1024)) {
            QJsonObject row;
            row.insert(QStringLiteral("path"), entry.path);
            row.insert(QStringLiteral("error"), QStringLiteral("over 2MiB"));
            result.append(row);
            continue;
        }
        QByteArray data;
        QString readError;
        if (!floppy::readFileRaw(raw, entry.path, &data, &readError)) {
            QJsonObject row;
            row.insert(QStringLiteral("path"), entry.path);
            row.insert(QStringLiteral("error"), readError);
            result.append(row);
            continue;
        }
        const QVector<MagicHit> found = scanMagic(data);
        if (found.isEmpty())
            continue;
        QJsonArray hits;
        for (const MagicHit &hit : found) {
            if (hitCount >= kCap) {
                capped = true;
                break;
            }
            QJsonObject object;
            object.insert(QStringLiteral("offset"), hit.offset);
            object.insert(QStringLiteral("kind"), hit.kind);
            hits.append(object);
            ++hitCount;
        }
        if (found.size() == kCap)
            capped = true;
        QJsonObject row;
        row.insert(QStringLiteral("path"), entry.path);
        row.insert(QStringLiteral("hits"), hits);
        result.append(row);
        if (capped)
            break;
    }
    if (capped) {
        QJsonObject truncated;
        truncated.insert(QStringLiteral("truncated"), true);
        result.append(truncated);
    }
    return result;
}

bool MainWindow::openDiskHit(const QString &imagePath, const QString &entryPath, int offset,
                             const QString &kind, QString *error)
{
    QByteArray raw;
    QString loadError;
    if (!floppy::loadRaw(imagePath, &raw, &loadError)) {
        if (error)
            *error = loadError;
        return false;
    }
    QByteArray data;
    QString readError;
    if (!floppy::readFileRaw(raw, entryPath, &data, &readError)) {
        if (error)
            *error = readError;
        return false;
    }
    if (data.size() > 2 * 1024 * 1024) {
        if (error)
            *error = tr("The file is larger than the 2 MiB limit.");
        return false;
    }
    if (kind == QLatin1String("iff")) {
        ImportedSheet sheet;
        QString importError;
        if (importStImage(data.mid(offset), StImageFormat::Iff, PaletteKind::Ste, &sheet,
                          &importError)) {
            const QString path =
                QDir::temp().filePath(QStringLiteral("pist-iff-%1.iff").arg(++m_iffSerial));
            if (!files::write(path, data.mid(offset), error))
                return false;
            openPath(path);
            return true;
        }
    }
    const QString key = QFileInfo(imagePath).absoluteFilePath() + QLatin1Char('\n') + entryPath;
    const QString name = QFileInfo(entryPath).fileName();
    if (BinaryView *open = binaryForKey(key)) {
        m_tabs->setCurrentWidget(open);
        open->showHit(offset, kind);
        return true;
    }
    BinaryView *view = addBinaryTab(data, name, QString(), key);
    view->showHit(offset, kind);
    return true;
}

void MainWindow::scanDiskImage(const QString &imagePath)
{
    QString error;
    const QJsonArray rows = scanDiskMagic(imagePath, &error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, tr("Scan for resources"), error);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Scan for resources"));
    dialog.setObjectName(QStringLiteral("diskScanDialog"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *hits = new QListWidget(&dialog);
    hits->setObjectName(QStringLiteral("diskScanHits"));
    auto *status = new QLabel(&dialog);
    status->setObjectName(QStringLiteral("diskScanStatus"));
    bool truncated = false;
    for (const QJsonValue &value : rows) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("truncated")).toBool()) {
            truncated = true;
            continue;
        }
        if (!object.contains(QStringLiteral("hits")))
            continue;
        const QString path = object.value(QStringLiteral("path")).toString();
        for (const QJsonValue &hitValue : object.value(QStringLiteral("hits")).toArray()) {
            const QJsonObject hit = hitValue.toObject();
            const int hitOffset = hit.value(QStringLiteral("offset")).toInt();
            const QString kind = hit.value(QStringLiteral("kind")).toString();
            auto *item = new QListWidgetItem(
                tr("%1  %2  %3").arg(path).arg(hitOffset, 8, 16, QLatin1Char('0')).arg(kind), hits);
            item->setData(Qt::UserRole, path);
            item->setData(Qt::UserRole + 1, hitOffset);
            item->setData(Qt::UserRole + 2, kind);
        }
    }
    if (truncated)
        status->setText(tr("Stopped after 64 hits"));
    else if (hits->count() == 0)
        status->setText(tr("No AVR, IFF, Ice, or Atomik"));
    layout->addWidget(hits);
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    connect(hits, &QListWidget::itemDoubleClicked, &dialog,
            [this, &dialog, imagePath](QListWidgetItem *item) {
                QString hitError;
                if (!openDiskHit(imagePath, item->data(Qt::UserRole).toString(),
                                 item->data(Qt::UserRole + 1).toInt(),
                                 item->data(Qt::UserRole + 2).toString(), &hitError))
                    QMessageBox::warning(&dialog, tr("Scan for resources"), hitError);
                dialog.accept();
            });
    dialog.exec();
}

QString MainWindow::identifyPacker(const QString &path, quint32 offset, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    if (!file.seek(offset))
        return {};
    return packerMagic(file.read(4));
}

QJsonObject MainWindow::sndhInfo(const QString &path, quint32 offset, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    if (!file.seek(offset)) {
        if (error)
            *error = QStringLiteral("offset is past the end");
        return {};
    }
    const SndhTags tags = parseSndhTags(file.read(4096));
    QJsonObject object;
    object.insert(QStringLiteral("found"), tags.found);
    object.insert(QStringLiteral("truncated"), tags.truncated);
    object.insert(QStringLiteral("title"), tags.title);
    object.insert(QStringLiteral("composer"), tags.composer);
    object.insert(QStringLiteral("ripper"), tags.ripper);
    object.insert(QStringLiteral("converter"), tags.converter);
    object.insert(QStringLiteral("year"), tags.year);
    return object;
}

bool MainWindow::saveScreenshot(const QString &path)
{
    // A media session's emulator is windowless, so there is no window for the
    // capture below to see — the panel's pixels exist only as the last frame it
    // streamed, which is exactly what a screenshot should be.
    if (m_mediaDisplay && !m_lastMediaFrame.isNull())
        return m_lastMediaFrame.save(path);

    // Raise first, so the capture is this window and not whatever is on top of
    // it, and pump once so the raise has landed before the frame buffer is
    // read.
    raise();
    activateWindow();
    QGuiApplication::processEvents();
    const QImage image = screen()->grabWindow(winId()).toImage();
    return !image.isNull() && image.save(path);
}

QMetaObject::Connection MainWindow::onBuildCompleted(QObject *context,
                                                     std::function<void(bool)> handler)
{
    return connect(this, &MainWindow::buildCompleted, context, std::move(handler));
}

QMetaObject::Connection MainWindow::onSessionRunningChanged(QObject *context,
                                                            std::function<void(bool)> handler)
{
    return connect(this, &MainWindow::sessionRunningChanged, context, std::move(handler));
}

QMetaObject::Connection MainWindow::onProfileResultsReady(QObject *context,
                                                          std::function<void(bool)> handler)
{
    return connect(this, &MainWindow::profileResultsReady, context, std::move(handler));
}

QMetaObject::Connection MainWindow::onDebugCommandFinished(
    QObject *context, std::function<void(const QString &, const QString &)> handler)
{
    return connect(this, &MainWindow::debugCommandFinished, context, std::move(handler));
}

QMetaObject::Connection MainWindow::onDebugReadFinished(
    QObject *context, std::function<void(quint32, const QString &)> handler)
{
    return connect(this, &MainWindow::debugReadFinished, context, std::move(handler));
}

QMetaObject::Connection MainWindow::onDebugReadMemoryFinished(
    QObject *context, std::function<void(quint32, const QList<MemoryRow> &)> handler)
{
    return connect(this, &MainWindow::debugReadMemoryFinished, context, std::move(handler));
}

} // namespace pist
