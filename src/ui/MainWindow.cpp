// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
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
#include "image/ImageDocument.h"
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
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
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

namespace {

/// One media-channel frame (docs/PLAN.md §12) as pixels to paint.
///
/// The fork's normal stream is 32 bpp RGB32 from the dummy driver — masks
/// r=0x00ff0000, g=0x0000ff00, b=0x000000ff — which in little-endian memory is
/// B,G,R,X, exactly QImage::Format_RGB32, so that case is a direct wrap plus
/// the one copy that makes the image own the frame's pixels. Any other mask
/// layout is expanded component by component, because the header carries the
/// masks precisely so a different driver renders instead of producing garbage.
QImage mediaFrameImage(const MediaFrame &frame)
{
    if (frame.w == 0 || frame.h == 0 || frame.bpp != 32)
        return {};
    const qint64 bytes = qint64(frame.pitch) * frame.h;
    if (frame.pitch < frame.w * 4 || frame.pixels.size() < bytes)
        return {};

    if (frame.rmask == 0x00ff0000u && frame.gmask == 0x0000ff00u
        && frame.bmask == 0x000000ffu) {
        // The const-uchar constructor *borrows* its buffer (verified: the
        // image's bits are the caller's pointer), and the frame is gone when
        // the signal handler returns — copy() is what makes the image own its
        // pixels.
        return QImage(reinterpret_cast<const uchar *>(frame.pixels.constData()),
                      int(frame.w), int(frame.h), int(frame.pitch),
                      QImage::Format_RGB32)
            .copy();
    }

    const auto component = [](quint32 pixel, quint32 mask) -> int {
        if (mask == 0)
            return 0;
        int shift = 0;
        while (((mask >> shift) & 1u) == 0)
            ++shift;
        const quint32 max = mask >> shift;
        return int(((pixel & mask) >> shift) * 255u / max);
    };

    QImage image(int(frame.w), int(frame.h), QImage::Format_RGB32);
    for (quint32 y = 0; y < frame.h; ++y) {
        const uchar *row = reinterpret_cast<const uchar *>(frame.pixels.constData())
            + qint64(y) * frame.pitch;
        QRgb *out = reinterpret_cast<QRgb *>(image.scanLine(int(y)));
        for (quint32 x = 0; x < frame.w; ++x) {
            const quint32 pixel = qFromLittleEndian<quint32>(row + qint64(x) * 4);
            out[x] = qRgb(component(pixel, frame.rmask),
                          component(pixel, frame.gmask),
                          component(pixel, frame.bmask));
        }
    }
    return image;
}

// Suffixes that name text worth opening straight off a floppy disk. Assembly
// sources are deliberately absent: building one would run vasm on the session
// copy, so the flow is to copy it to the hard drive first (the file browser
// can do that directly).


void markProblemSeverity(QTreeWidgetItem *item, bool error)
{
    const QColor ink = error ? appearance::colors().error : appearance::colors().warning;
    QPixmap square(8, 8);
    square.fill(ink);
    item->setIcon(0, QIcon(square));
    item->setForeground(2, ink);
}

/// Item data role holding a Problems row's index into m_problemEntries. Not
/// Qt::DisplayRole: what the row shows is formatted for the eye (a translated
/// placeholder for a file-less note, a line number as text), which is exactly
/// what the pane must not read its own model back through.
constexpr int kProblemEntryRole = Qt::UserRole;


bool fileInsideRepo(const QString &root, const QString &file)
{
    if (root.isEmpty() || file.isEmpty())
        return false;
    const QString r = QDir(root).absolutePath();
    const QString f = QFileInfo(file).absoluteFilePath();
    const Qt::CaseSensitivity cs =
#ifdef Q_OS_WIN
        Qt::CaseInsensitive;
#else
        Qt::CaseSensitive;
#endif
    return f.compare(r, cs) == 0 || f.startsWith(r + QDir::separator(), cs);
}




const QString &offeredDebuggingKey()
{
    static const QString key = QStringLiteral("layout/offeredDebugging");
    return key;
}








} // namespace

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
    hookHost.enabled = [this] { return m_settings.stopHookEnabled; };
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
    stateHost.hookEnabled = [this] { return m_settings.stopHookEnabled; };
    stateHost.hookPath = [this] { return m_settings.stopHookPath; };
    stateHost.hookAutoContinue = [this] { return m_settings.stopHookAutoContinue; };
    stateHost.hookLastRun = [this]() -> const StopHookRunner::LastRun & {
        return m_hooks->lastRun();
    };
    m_remoteState = new RemoteStateAdapter(stateHost, this);

    // Documents live in tabs. openPath() dispatches `.pim` / ST still-images
    // to addImageTab() and everything else to addEditorTab(). m_editor tracks
    // the current *text* editor and is null on an image tab; m_image is the
    // reverse. Build/Run use buildSourcePath() so a focused image does not
    // strand the assembler.
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

void MainWindow::wireBackend()
{
    connect(m_host, &IDebugBackend::stateUpdated, this, &MainWindow::onStateUpdated);
    connect(m_host, &IDebugBackend::logLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });
    // Emulator errors are reported in the log *and* the status bar. Log-only was
    // the previous behaviour, and the log is a bottom-dock tab that is not always
    // visible, so a failed launch looked like a normal window.
    connect(m_host, &IDebugBackend::errorOccurred, this, [this](const QString &message) {
        m_log->appendPlainText(QStringLiteral("[error] ") + message);
        statusBar()->showMessage(message, 15000);
    });

    // Without this the status bar kept saying "Running" after Hatari had died,
    // because only stoppedChanged was connected. A dead emulator looked live and
    // step/continue stayed enabled.
    connect(m_host, &IDebugBackend::runningChanged, this, [this](bool running) {
        emit sessionRunningChanged(running);
        if (running) {
            // One launched session (docs/untitled-plan.md §6). The counter sits
            // on the transport's own "the machine is up" transition, so a launch
            // that never came up is not counted.
            settings::noteRun();
            if (m_actPause)
                m_actPause->setEnabled(!m_host->isStopped());
            if (m_consoleInput)
                m_consoleInput->setEnabled(true);
        }
        // The panel's media-session state (cursor hiding, the input grab) is
        // simply "media display engaged and a session is up" — false again
        // here on the way down, which also releases any capture (§12.4).
        if (m_display)
            m_display->setMediaSession(m_mediaDisplay && running);
        // The audio buffer belongs to the session too: a dead emulator's
        // last samples must not keep playing into the next one.
        if (m_audio && !running)
            m_audio->reset();
        if (running)
            return;
        if (m_consoleInput)
            m_consoleInput->setEnabled(running);
        updateSessionChip();
        if (m_actPause)
            m_actPause->setEnabled(false);
        if (m_session->sessionArmed()) {
            if (m_actStep) m_actStep->setEnabled(false);
            if (m_actStepOver) m_actStepOver->setEnabled(false);
            if (m_actStepOut) m_actStepOut->setEnabled(false);
            if (m_actRunToCursor) m_actRunToCursor->setEnabled(false);
            if (m_actFollow) m_actFollow->setEnabled(false);
            if (m_actResume) m_actResume->setEnabled(false);
            m_log->appendPlainText(tr("[session] emulator is no longer running"));
        }
        // One owner for everything an ended session leaves behind — the same
        // reset the next launch performs, so stale arming state, bases, the
        // cached machine state and a pending remote command cannot survive
        // into whatever happens next.
        m_session->resetSessionState();
        updateRegisterStrip();
        // The media display belonged to the session that just ended: forget
        // its frame, so the panel paints its empty state rather than a stale
        // last frame that reads as the emulator's current one.
        if (m_display)
            m_display->setFrame(QImage());
        // The last media frame belonged to the session that just ended too;
        // keeping it would make a later screenshot show a stale screen, and the
        // media mode itself ends with the session.
        m_lastMediaFrame = QImage();
        m_mediaDisplay = false;
        // Both transcripts describe the session that just ended, and neither is
        // re-requested without one: left on screen they read as the hardware's
        // current state. The views' own `clear()` documents the no-session state.
        if (m_hardware)
            m_hardware->clear();
        if (m_pcHistory)
            m_pcHistory->clear();
        // A session's profile and its gutter heat belong to that session.
        if (m_profiler)
            m_profiler->clear();
        for (CodeEditor *editor : openEditors())
            editor->setLineHeat({});
    });
    connect(m_host, &IDebugBackend::memoryDumpReady, this,
            [this](quint32 address, const QList<MemoryRow> &rows, int tag) {
                // The remote `readmem` verb's dump carries a tag no pane owns
                // (kRemoteReadTag) and belongs to the waiting verb, not to a
                // view. Otherwise: route the dump to the pane that asked for it.
                // Either way the rows are already parsed (MAJ-45) — the verb
                // renders its JSON from them and no one re-reads the dump text.
                if (tag == kRemoteReadTag) {
                    emit debugReadMemoryFinished(address, rows);
                    return;
                }
                if (auto *view = m_memoryPanes.value(tag, nullptr))
                    view->applyDump(rows);
            });
    connect(m_host, &IDebugBackend::disassemblyReady, this,
            [this](quint32 address, const QString &response) {
                // Only the remote `disasm` verb reads the disassembly at a given
                // address: the snapshot's own read is readDisassembly() at the
                // PC, which reports no text.
                emit debugReadFinished(address, response);
            });
    // The hardware view's `info <subject>` reports, named by the subject that
    // was asked for — the pane is the only consumer, and only for the subject it
    // is showing. The report is parsed in the backend (MAJ-45), so the pane
    // renders a summary rather than a transcript it reads meaning out of.
    // Nothing else internal matches on a command's text.
    connect(m_host, &IDebugBackend::hardwareInfoReady, this,
            [this](const QString &subject, const HardwareSummary &summary) {
                if (m_hardware && subject == m_hardware->subject())
                    m_hardware->setInfo(summary);
            });
    connect(m_host, &IDebugBackend::historyReady, this,
            [this](const QString &response) {
                if (m_pcHistory)
                    m_pcHistory->setHistory(response);
            });
    connect(m_host, &IDebugBackend::profileSaveFinished, this,
            [this](const QString &) {
                // The save's file is what was asked for; showProfileResults
                // parses it and reports a failure in the console itself.
                if (!m_session->consumeProfileSavePending())
                    return;
                showProfileResults();
            });
    connect(m_host, &IDebugBackend::commandFinished, this,
            [this](const QString &command, const QString &response) {
                // Only the remote `cmd` verb's own text is matched here: it is
                // the user's command, echoed back so its response can be
                // delivered to the client waiting on it.
                if (!m_pendingDebugCommand.isEmpty() && command == m_pendingDebugCommand) {
                    m_pendingDebugCommand.clear();
                    emit debugCommandFinished(command, response);
                }
            });

    // The media channel's frames (docs/PLAN.md §12): convert once, keep the
    // latest for the screenshot verb, and hand the panel its pixels. The
    // conversion is here, not in emu/, because pist_emu links no QtGui.
    connect(m_host, &IDebugBackend::mediaFrameReceived, this,
            [this](const pist::MediaFrame &frame) {
                if (!m_display)
                    return;
                const QImage image = mediaFrameImage(frame);
                if (image.isNull())
                    return;
                m_lastMediaFrame = image;
                m_display->setFrame(image);
            });

    // The media channel's audio (phase 3): chunks of s16le stereo from the
    // fork, played through PiST's own sink (EmuAudio owns pacing and the
    // jitter buffer; it is created once, beside the display widget).
    if (m_audio)
        connect(m_host, &IDebugBackend::mediaAudioReceived, this,
                [this](quint32 rate, const QByteArray &samples) {
                    m_audio->writeChunk(rate, samples);
                });

    connect(m_host, &IDebugBackend::stoppedChanged, this, [this](bool stopped) {
        // Watchers (remote-control `watch`, the MCP shim) learn the running
        // edge here; the stopped edge waits for onStateUpdated, which has the
        // PC worth reporting.
        if (m_eventSink) {
            if (stopped) {
                m_session->setStopEventPending(true);
            } else {
                // A stop whose state never became valid (no register batch
                // followed) must not leave a watcher hanging: publish it
                // without the pc detail rather than drop it.
                if (m_session->consumeStopEventPending()) {
                    m_eventSink->publishEvent(QStringLiteral("stopped"));
                }
                m_eventSink->publishEvent(QStringLiteral("running"));
            }
        }
        m_actStep->setEnabled(stopped);
        m_actStepOver->setEnabled(stopped);
        m_actStepOut->setEnabled(stopped);
        m_actRunToCursor->setEnabled(stopped);
        m_actFollow->setEnabled(stopped);
        m_actResume->setEnabled(stopped);
        updateRunContinueShortcut();
        updateSessionChip();
        updateRegisterStrip();
        // The first stop of a session is the entry stop: the program has
        // loaded and the debugger is waiting before its first instruction.
        // That is the one stop a new user reads as a hang, so the panel names
        // it and offers Continue rather than only muting the video.
        const bool entryStop = stopped && !m_session->sessionArmed();
        const QString continueHint =
            m_actResume->shortcut().toString(QKeySequence::NativeText);
        // The embedded panel renders no frames while stopped, so tell it to show
        // its paused hint rather than look frozen.
        if (m_display)
            m_display->setPaused(stopped, entryStop, continueHint);
        // A stop is also when the user wants their keyboard back: a machine
        // halted in its debugger cannot consume input, so anything typed
        // would sit in the media socket until a resume replayed it (seen
        // live in the phase-2 desktop proof). §12.4's stop edge.
        if (stopped && m_display)
            m_display->setInputCaptured(false);
        // Registers are only writable while stopped, through the debugger.
        if (m_actPause)
            m_actPause->setEnabled(!stopped && m_host->isRunning());
        if (m_registers)
            m_registers->setEditingEnabled(stopped);
        if (m_memory)
            m_memory->setEditingEnabled(stopped);
        m_profiling->syncActions();
        if (stopped) {
            m_session->onDebuggerStopped();
            // A status line, not a dialog: a modal here hangs the offscreen
            // stop tests. The key makes the offer once per settings store.
            if (!QSettings().value(offeredDebuggingKey()).toBool()) {
                QSettings().setValue(offeredDebuggingKey(), true);
                statusBar()->showMessage(
                    tr("View → Layout → Debugging arranges the panels around a stop."),
                    8000);
            }
            // The panel carries the same message, but it is hidden when the
            // session uses an emulator without the media channel — and a
            // loaded program that never moves is the report that prompted
            // this. The status bar is the one surface both transports share.
            if (entryStop && m_display && !m_display->isVisible()) {
                statusBar()->showMessage(
                    continueHint.isEmpty()
                        ? tr("Program loaded and paused — choose Continue to run it.")
                        : tr("Program loaded and paused — press %1 to run it.")
                              .arg(continueHint),
                    10000);
            }
        }
    });

    connect(m_host, &IDebugBackend::stackDumpReady, this,
            [this](quint32 sp, const QList<MemoryRow> &rows) {
                // A step-out dump is consumed regardless of the stack view: any
                // stack response is at the same SP with the return address on
                // top. Those rows are the dump, parsed once in the backend
                // (MAJ-45) — the controller reads the return address out of
                // them and the view below renders the same ones, instead of
                // both parsing the transcript.
                if (!m_session->handleStackDumpForStepOut(rows))
                    return;
                if (!m_stack)
                    return;
                // The annotation needs the *text* extent, not the data base.
                // With no data section (or one that does not follow text) the
                // data base gives an empty range and every return address
                // silently goes unmarked, so take the extent from the
                // listings and fall back to the old bound when the map has
                // none (docs/code-review-glm-001.md, P3).
                const quint32 textEnd = m_programMap.textEnd();
                m_stack->setStackDump(sp, rows, m_lastState.textBase,
                                      textEnd ? textEnd : m_lastState.dataBase);
            });
}

QString MainWindow::referenceCardAt(CodeEditor *editor, const QPoint &viewportPos) const
{
    if (!editor)
        return {};

    // The *pointer*, not the caret: the dock follows the caret, and hovering
    // something else should describe that thing.
    const QTextCursor under = editor->cursorForPosition(viewportPos);
    const CursorReference reference =
        referenceAroundCursor(editor, under.blockNumber(), under.positionInBlock());

    if (reference.call) {
        QStringList lines;
        lines << QStringLiteral("%1 — %2").arg(reference.call->name, reference.call->summary);
        lines << reference.call->prototype;
        if (!reference.args.isEmpty())
            lines << tr("Calling with: %1.").arg(reference.args.join(QStringLiteral(", ")));
        return lines.join(QLatin1Char('\n'));
    }
    if (reference.trapContext) {
        if (const InstructionInfo *info = instructionRef(QStringLiteral("trap")))
            return QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary);
        return {};
    }
    if (reference.word.isEmpty())
        return {};

    // What the word *is* comes from the same candidate list completion offers: a
    // mnemonic gets the reference's summary and flags, a symbol where it is
    // defined, a directive nothing (there is no honest summary for one yet) and a
    // word nobody knows nothing at all.
    const CompletionItem *known = nullptr;
    for (const CompletionItem &candidate : m_completionItems) {
        if (candidate.text.compare(reference.word, Qt::CaseInsensitive) == 0) {
            known = &candidate;
            break;
        }
    }
    if (!known)
        return {};

    switch (known->kind) {
    case CompletionKind::Mnemonic: {
        const InstructionInfo *info = instructionRef(known->text);
        if (!info)
            return {}; // an extra (bkpt, an FPU name): the reference has no entry
        QString text = QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary);
        if (!info->flags.isEmpty())
            text += QLatin1Char('\n') + tr("Flags: %1.").arg(info->flags);
        return text;
    }
    case CompletionKind::Label:
    case CompletionKind::Equate:
        if (known->file.isEmpty() || known->line <= 0)
            return tr("%1 — the build gives it no source position.").arg(known->text);
        return tr("%1 — defined at %2:%3").arg(known->text, known->file).arg(known->line);
    case CompletionKind::Directive:
        return {};
    }
    return {};
}

CursorReference MainWindow::referenceAroundCursor(CodeEditor *editor, int blockNumber,
                                                  int column) const
{
    if (!editor || !editor->document())
        return {};
    const QTextDocument *doc = editor->document();
    if (blockNumber < 0 || blockNumber >= doc->blockCount())
        return {};

    // The scanner only needs a small window around the position: OS calls are a
    // local idiom (OsCallScan bounds its scans to a few lines).
    const int base = qMax(0, blockNumber - 10);
    const int last = qMin(doc->blockCount() - 1, blockNumber + 10);
    QStringList window;
    window.reserve(last - base + 1);
    for (int i = base; i <= last; ++i)
        window.append(doc->findBlockByNumber(i).text());
    return referenceAt(window, blockNumber - base, column);
}

void MainWindow::followCursorReference(CodeEditor *editor)
{
    if (!editor || editor != m_editor)
        return;

    const QTextCursor cursor = editor->textCursor();
    const CursorReference reference =
        referenceAroundCursor(editor, cursor.blockNumber(), cursor.positionInBlock());

    auto showStrip = [this](const QString &text) {
        if (!m_instrStrip)
            return;
        m_instrStrip->setText(text);
        m_instrStrip->setToolTip(text.isEmpty() ? tr("Open the instruction reference") : text);
    };

    if (reference.call) {
        if (m_instrRef)
            m_instrRef->showOsCall(reference.call->trap, reference.call->opcode, reference.args);
        QString stack;
        if (!reference.args.isEmpty())
            stack = reference.args.join(QStringLiteral(", "));
        if (reference.call->stackBytes > 0) {
            if (!stack.isEmpty())
                stack += QStringLiteral(" · ");
            stack += tr("%1 bytes").arg(reference.call->stackBytes);
        }
        QString line = QStringLiteral("%1 — %2").arg(reference.call->name, reference.call->summary);
        if (!stack.isEmpty())
            line += tr("  Stack: %1").arg(stack);
        showStrip(line);
        return;
    }
    if (reference.trapContext) {
        // A trap whose function number is not statically known: the generic TRAP
        // entry still says what the instruction does.
        if (m_instrRef)
            m_instrRef->showInstruction(QStringLiteral("trap"));
        if (const InstructionInfo *info = instructionRef(QStringLiteral("trap")))
            showStrip(QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary));
        else
            showStrip(QString());
        return;
    }

    if (m_instrRef)
        m_instrRef->showInstruction(reference.word);
    if (const InstructionInfo *info = instructionRef(reference.word))
        showStrip(QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary));
    else
        showStrip(QString());
}

void MainWindow::raiseInstructionRef()
{
    auto *dock = findChild<QDockWidget *>(QStringLiteral("instructionRefDock"));
    if (!dock)
        return;
    dock->show();
    dock->raise();
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

bool MainWindow::hookMayRunFor(const QString &projectPath, bool quiet)
{
    const QString shipped = QCoreApplication::applicationDirPath()
        + QStringLiteral("/../share/pist");
    // A copied demo was recorded as trusted when the copy was made (see
    // WelcomeView::demoProjects()), so the shipped-tree check here is enough.
    if (settings::hookProjectIsTrusted(m_settings, projectPath, shipped))
        return true;

    // The remote-control `open` must answer; a modal it cannot dismiss would
    // hold the reply until the client's timeout. Refusing is the safe answer.
    if (quiet)
        return false;

    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Stop hook"),
        tr("This project runs a script at every debugger stop:\n\n%1\n\n"
           "A hook can read and write the machine's memory and registers. Only allow "
           "it if you trust this project and the current contents of that script.")
            .arg(QDir::toNativeSeparators(m_settings.stopHookPath)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return false;

    // The in-memory path, not a re-read: it is the script the prompt just named.
    settings::trustHookProject(projectPath, m_settings.stopHookPath);
    return true;
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open project"), QString(),
        tr("PiST projects (*%1);;All files (*)")
            .arg(QLatin1String(settings::kProjectSuffix)));
    if (path.isEmpty())
        return;

    // A project picked from the installed demo directory is read-only. Open its
    // copy instead, or its implied source and its settings would land there.
    const QString projectPath = WelcomeView::writableDemoPath(path);

    ProjectSettings loaded;
    QString error;
    if (!settings::load(&loaded, projectPath, &error)) {
        QMessageBox::critical(this, tr("Open project"), error);
        return;
    }

    // The source path is not stored in the file — it is implied by the project
    // file's location — so derive it and load it. Picking a file through the
    // "All files" filter gives a name without the suffix, and truncating that
    // blindly would produce an arbitrary path.
    QString source = settings::lastSourcePath();
    QString implied;
    if (projectPath.endsWith(QLatin1String(settings::kProjectSuffix), Qt::CaseInsensitive)) {
        implied = projectPath.left(projectPath.size()
                                  - QLatin1String(settings::kProjectSuffix).size())
                + QStringLiteral(".s");
    }
    // A stale last source can still name the read-only tree after that file
    // has gone; translate it before deciding it no longer exists, because its
    // copy may well be there.
    source = WelcomeView::writableDemoPath(source);
    if (!implied.isEmpty() && QFileInfo::exists(implied))
        source = implied;
    else if (!source.isEmpty() && !QFileInfo::exists(source))
        source.clear();

    if (!source.isEmpty()) {
        // The project's source becomes the current document; other tabs stay
        // open, since they may belong to this project too.
        if (CodeEditor *open = editorForPath(source)) {
            m_tabs->setCurrentWidget(open);
        } else if (!addEditorTab(source)) {
            QMessageBox::critical(this, tr("Open project"),
                                  tr("The project refers to %1, which could not be opened.")
                                      .arg(source));
            return;
        }
        loaded.sourceFile = source;
    }

    m_settings = loaded;
    // A project that came with a hook asks once, and a refusal leaves the hook
    // off for this load (the setting itself stays in the file, so the decision is
    // the user's, not an edit of their project).
    if (!hookMayRunFor(projectPath)) {
        m_settings.stopHookEnabled = false;
        m_log->appendPlainText(tr("[hook] not allowed for this project; it will not run"));
    }
    settings::rememberLastSource(source);
    syncFileBrowserDisks();
    statusBar()->showMessage(
        tr("Opened project %1").arg(QFileInfo(projectPath).fileName()), 5000);
    m_log->appendPlainText(tr("[project] loaded %1").arg(projectPath));
}

void MainWindow::saveProject()
{
    const QString source = buildSourcePath();
    if (source.isEmpty()) {
        QMessageBox::information(this, tr("Save project"),
                                 tr("Open an assembly source file first."));
        return;
    }

    m_settings.sourceFile = source;
    const QString path = settings::projectFileFor(source);

    QString error;
    if (!settings::save(m_settings, path, &error)) {
        QMessageBox::critical(this, tr("Save project"), error);
        return;
    }

    settings::rememberLastSource(source);
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 5000);
    m_log->appendPlainText(tr("[project] saved %1").arg(path));
}

void MainWindow::persistSettings()
{
    const QString source = buildSourcePath();
    if (source.isEmpty())
        return;
    m_settings.sourceFile = source;
    QString error;
    const QString path = settings::projectFileFor(source);
    if (!settings::save(m_settings, path, &error)) {
        statusBar()->showMessage(tr("Could not save project settings: %1").arg(error), 8000);
        return;
    }
    settings::rememberLastSource(source);
}

void MainWindow::syncFileBrowserDisks()
{
    if (m_fileBrowser)
        m_fileBrowser->setFloppyImages(m_settings.floppyImages);
}

void MainWindow::editSettings()
{
    SettingsDialog dialog(m_settings, this);
    connect(&dialog, &SettingsDialog::toolsSetupClosed, this, &MainWindow::refreshToolchain);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_settings = dialog.settings();

    // A tool path the dialog just changed must take effect without a restart.
    refreshToolchain();

    // The dialog persisted the application-wide appearance preferences on
    // accept; bring them into effect now rather than at next start.
    applyAppearance();
    syncFileBrowserDisks();
    if (m_host) {
        m_host->setFloppyImage(0, m_settings.floppyImages.value(0));
        m_host->setFloppyImage(1, m_settings.floppyImages.value(1));
    }

    // Persist immediately when the project is already known, so a settings change
    // is not lost if the session is closed without an explicit save.
    if (m_editor) {
        m_settings.sourceFile = m_editor->filePath();
        const QString path = settings::projectFileFor(m_editor->filePath());
        QString error;
        if (!settings::save(m_settings, path, &error)) {
            // Report before the success message below, and do not swallow it: the
            // in-memory settings have already changed, so saying "updated" would
            // be a false claim that silently reverts on restart.
            QMessageBox::warning(this, tr("Project settings"),
                                 tr("The settings were applied to this session but could not "
                                    "be saved:\n\n%1")
                                     .arg(error));
            return;
        }
        settings::rememberLastSource(m_editor->filePath());
    }

    statusBar()->showMessage(
        tr("Settings updated: %1, %2, %3 MiB")
            .arg(machineDisplayName(m_settings.machine), m_settings.monitor)
            .arg(m_settings.memSizeMiB),
        5000);
}

void MainWindow::loadProjectForSource(const QString &sourcePath, bool quiet)
{
    if (sourcePath.isEmpty())
        return;

    const QString projectPath = settings::projectFileFor(sourcePath);
    if (!QFileInfo::exists(projectPath)) {
        // No project file: keep the current settings but point them at the new
        // source, so Build uses the source the user just opened.
        m_settings.sourceFile = sourcePath;
        return;
    }

    ProjectSettings loaded;
    QString error;
    if (!settings::load(&loaded, projectPath, &error)) {
        // Previously log-only, which meant a corrupt project file silently ran the
        // previous project's settings with no indication of why.
        m_log->appendPlainText(tr("[project] %1").arg(error));
        statusBar()->showMessage(tr("Project settings ignored: %1").arg(error), 15000);
        m_settings.sourceFile = sourcePath;
        return;
    }

    loaded.sourceFile = sourcePath;
    m_settings = loaded;

    // Opening a source is the ordinary way into a cloned project: the settings
    // beside it are loaded here, not through the Open project dialog, so the
    // hook question has to be asked here too.
    if (!hookMayRunFor(projectPath, quiet)) {
        m_settings.stopHookEnabled = false;
        m_log->appendPlainText(tr("[hook] not allowed for this project; it will not run"));
    }

    // A project that names tool paths must not keep the construction-time
    // discovery answer: the override wins as soon as it is known.
    if (!m_settings.assemblerPath.isEmpty() || !m_settings.linkerPath.isEmpty()
        || !m_settings.hatariPath.isEmpty()) {
        refreshToolchain();
    }

    settings::rememberLastSource(sourcePath);
    syncFileBrowserDisks();
    m_log->appendPlainText(tr("[project] loaded %1").arg(projectPath));
    statusBar()->showMessage(
        tr("Project settings: %1, %2, %3 MiB")
            .arg(machineDisplayName(m_settings.machine), m_settings.monitor)
            .arg(m_settings.memSizeMiB),
        6000);
}

QString MainWindow::projectScopeDirectory() const
{
    // Only a directory the user actually opened counts as the project. The
    // browser's model root is the process working directory until something is
    // opened, and an unsaved document has no directory at all, so an unguarded
    // answer here points project-wide work at whatever happens to contain the
    // launch directory (MIN-81) — which is why nothing is answered rather than
    // guessed when there is nothing open.
    if (m_fileBrowser && m_fileBrowser->hasProjectDirectory())
        return m_fileBrowser->directory();
    if (m_editor && !m_editor->filePath().isEmpty())
        return QFileInfo(m_editor->filePath()).absolutePath();
    if (m_image && !m_image->filePath().isEmpty())
        return QFileInfo(m_image->filePath()).absolutePath();
    return {};
}

void MainWindow::syncGitDirectory()
{
    if (!m_gitPanel)
        return;
    // With nothing open the panel is told so, and it says as much instead of
    // guessing: aiming git at the browser's unguarded root made a window with no
    // project run `git status` in whichever repository happened to contain the
    // launch directory. PiST's own suite, run from a build tree inside its
    // checkout, did exactly that ~90 times per run, and an abandoned status
    // child at teardown leaves `.git/index.lock` in the developer's repository
    // (MIN-81).
    m_gitPanel->setDirectory(projectScopeDirectory());
}

void MainWindow::syncFindInFilesScope()
{
    if (m_findInFiles)
        m_findInFiles->setScope(projectScopeDirectory());
}

void MainWindow::applyGitBlame()
{
    const bool want = m_actGitBlame && m_actGitBlame->isChecked();
    const QString root = m_gitPanel ? m_gitPanel->repositoryRoot() : QString();
    const bool inRepo = m_gitPanel && m_gitPanel->inRepository();
    for (CodeEditor *editor : openEditors()) {
        const bool show = want && inRepo && fileInsideRepo(root, editor->filePath());
        editor->setBlameShown(show);
    }
    if (want)
        refreshBlame();
}

void MainWindow::refreshBlame()
{
    if (!m_actGitBlame || !m_actGitBlame->isChecked() || !m_editor || !m_gitPanel)
        return;
    if (!m_gitPanel->inRepository() || !fileInsideRepo(m_gitPanel->repositoryRoot(), m_editor->filePath()))
        return;
    int first = 0;
    int last = 0;
    m_editor->visibleLineRange(first, last);
    if (first <= 0 || last < first)
        return;
    m_gitPanel->blame(m_editor->filePath(), first, last, m_editor->toPlainText().toUtf8());
}

void MainWindow::build()
{
    buildImpl(/*quiet=*/true, /*release=*/false);
}

void MainWindow::buildRelease()
{
    buildImpl(/*quiet=*/true, /*release=*/true);
}

void MainWindow::buildInteractive()
{
    buildImpl(/*quiet=*/false, /*release=*/false);
}

void MainWindow::buildReleaseInteractive()
{
    buildImpl(/*quiet=*/false, /*release=*/true);
}

void MainWindow::buildImpl(bool quiet, bool release)
{
    crash::note("build");
    // A remote-initiated build has nobody to dismiss a modal, and the dialog's
    // own event loop would hold its reply until the operation timeout, so its
    // refusals are reported on the console and the status bar instead (the
    // same reason openPathQuiet exists; MAJ-17). The flag is carried on the
    // window because run()'s launch happens after the build's async hop.
    m_quietDialogs = quiet;

    const QString source = buildSourcePath();
    if (source.isEmpty()) {
        refuseBuild(tr("Build"), tr("Open an assembly source file first."), false);
        return;
    }

    // A failed save must stop the build: otherwise the assembler runs on the
    // previous on-disk contents and reports a result for code the user is not
    // looking at.
    for (CodeEditor *editor : openEditors()) {
        if (!editor->isModifiedSinceLoad())
            continue;
        if (!saveEditor(editor, editor->filePath())) {
            refuseBuild(tr("Build"),
                        tr("Could not save %1: %2")
                            .arg(editor->filePath(), editor->lastError()),
                        true);
            return;
        }
    }

    m_problems->clear();
    m_problemEntries.clear();
    m_log->appendPlainText(release ? tr("--- build (release) ---") : tr("--- build ---"));

    const QFileInfo info(source);
    const settings::OutputPaths paths = settings::outputPathsFor(info.absoluteFilePath());
    const QString outPath = paths.program;
    const QString listPath = paths.listing;

    m_build->setSourceFile(info.absoluteFilePath());
    m_build->setOutputFile(outPath);
    m_build->setListingFile(listPath);
    m_build->setAdditionalSources(m_settings.additionalSources);

    // A multi-module build needs a linker and a placement map. Reported up front
    // rather than letting the link fail with a less useful message.
    const bool linking = !m_settings.additionalSources.isEmpty();
    if (linking) {
        const ToolInfo linker = toolchain::findLinker(m_settings.linkerPath);
        if (!linker.found()) {
            refuseBuild(tr("Linker not found"), toolchain::linkerInstallHint(), true);
            return;
        }
        m_build->setLinkerPath(linker.path);
        m_build->setLinkMapFile(info.absolutePath() + QDir::separator()
                                + info.completeBaseName() + QStringLiteral(".map"));
    } else {
        m_build->setLinkMapFile(QString());
    }

    // Everything else comes from the project settings. Before this existed the
    // include/define/cpu setters were never called, so a multi-file project
    // could not be built at all.
    m_build->setIncludePaths(m_settings.includePaths);
    m_build->setDefines(m_settings.defines);
    m_build->setCpu(m_settings.cpu);
    m_build->setAssemblerArgs(release ? m_settings.releaseAssemblerArgs
                                      : m_settings.debugAssemblerArgs);

    if (m_statusBuild)
        m_statusBuild->setText(release ? tr("Building (release)…") : tr("Building…"));
    m_build->build();
}

void MainWindow::refuseBuild(const QString &title, const QString &reason, bool critical)
{
    // A refused build is still a completed build as far as callers are
    // concerned: run()'s launch intent must be dropped rather than left armed
    // for the next successful build, and a remote-control `build` must be
    // answered now rather than after its timeout.
    m_session->setLaunchAfterBuild(false);
    if (m_statusBuild)
        m_statusBuild->setText(tr("Build failed"));
    m_log->appendPlainText(tr("--- build refused: %1 ---").arg(reason));
    emit buildCompleted(false);
    // A remote-initiated build is answered by buildCompleted and the console
    // above; opening a modal here would leave the reply waiting on a human who
    // is not there until the operation timeout expires.
    if (m_quietDialogs) {
        statusBar()->showMessage(firstLine(reason), 15000);
        return;
    }
    if (critical)
        QMessageBox::critical(this, title, reason);
    else
        QMessageBox::information(this, title, reason);
}

void MainWindow::refuseRun(const QString &title, const QString &reason, bool critical)
{
    // The same contract as refuseBuild, one step later in the run: the build
    // already reported success, so this is the only place a `run` that cannot
    // start is announced. buildCompleted(false) is the channel RemoteControl's
    // reply wait ends on (sessionRunningChanged(true) is the success channel),
    // and it is only sent for a quiet run: an interactive run has already
    // reported its build, and its user is looking at the dialog. The build chip
    // is left alone — the build really did succeed (MAJ-17).
    if (m_quietDialogs) {
        m_log->appendPlainText(QStringLiteral("[run] ") + reason);
        statusBar()->showMessage(firstLine(reason), 15000);
        emit buildCompleted(false);
        return;
    }
    if (critical)
        QMessageBox::critical(this, title, reason);
    else
        QMessageBox::warning(this, title, reason);
}

QTreeWidgetItem *MainWindow::addProblemRow(const RemoteStateAdapter::ProblemRow &entry)
{
    // One writer for the pane, so the row and its entry cannot drift apart: the
    // row's index into m_problemEntries is its whole link to the data
    // (problemsJson() reports that list, and the navigation paths go through it
    // rather than through these column strings).
    m_problemEntries.append(entry);

    auto *item = new QTreeWidgetItem(m_problems);
    item->setData(0, kProblemEntryRole, m_problemEntries.size() - 1);
    item->setText(0, entry.file.isEmpty() ? tr("(build)") : entry.file);
    item->setText(1, entry.line > 0 ? QString::number(entry.line) : QString());
    item->setText(2, entry.message);
    markProblemSeverity(item, entry.error);
    return item;
}

const RemoteStateAdapter::ProblemRow *MainWindow::problemEntryFor(const QTreeWidgetItem *item) const
{
    if (!item)
        return nullptr;
    const QVariant index = item->data(0, kProblemEntryRole);
    if (!index.isValid())
        return nullptr;
    const int row = index.toInt();
    if (row < 0 || row >= m_problemEntries.size())
        return nullptr;
    return &m_problemEntries.at(row);
}

void MainWindow::onBuildFinished(bool success, const QList<Diagnostic> &diagnostics)
{
    // The lifetime counters, and this session's summary for the welcome view
    // (docs/untitled-plan.md §6). A refused build never reaches here, which is
    // right: nothing was assembled.
    settings::noteBuild();
    ++m_sessionBuilds;
    m_lastBuildOk = success;

    if (m_statusBuild)
        m_statusBuild->setText(success ? tr("Build ok") : tr("Build failed"));
    emit buildCompleted(success);

    QString error;
    if (success)
        rebuildProgramMap();

    const int files = m_programMap.sourceFiles().size();
    if (success && !m_programMap.isEmpty())
        statusBar()->showMessage(files == 1
                                     ? tr("Build succeeded — 1 source file mapped")
                                     : tr("Build succeeded — %1 source files mapped")
                                           .arg(files),
                                 5000);
    else if (success)
        statusBar()->showMessage(tr("Build succeeded"), 5000);

    QHash<CodeEditor *, QList<int>> errorLinesFor;
    for (const Diagnostic &d : diagnostics) {
        // A linker diagnostic names a module and a section offset rather than a
        // line. Turning it into a line here is what makes "undefined symbol" point
        // at the offending source, rather than at an object file name.
        QString file = d.file;
        int line = d.line;
        if (d.hasObjectOffset) {
            LineMap::Address resolved;
            if (m_programMap.lineForObjectOffset(d.objectFile, d.section,
                                                 d.sectionOffset, &resolved)) {
                file = QFileInfo(resolved.file).fileName();
                line = resolved.line;
            } else {
                file = d.objectFile;
            }
        }

        // The entry keeps the real file (empty for a build-level note) and the
        // real line; how the row renders them is addProblemRow's business.
        addProblemRow({file, line > 0 ? line : 0, d.message,
                       d.severity == Diagnostic::Error});

        // Mark the gutter of whichever open editor shows this diagnostic's
        // file (with several documents open, errors are not all in one file).
        if (line > 0)
            for (CodeEditor *editor : openEditors())
                if (LineMap::sameSource(file, editor->filePath()))
                    errorLinesFor[editor].append(line);
    }
    for (CodeEditor *editor : openEditors())
        editor->setErrorLines(errorLinesFor.value(editor));

    // Surface the Problems pane on failure (raising selects its tab when it is
    // tabbed with the console/memory).
    if (!success && m_problemsDock) {
        m_problemsDock->show();
        m_problemsDock->raise();
    }

    m_log->appendPlainText(success ? tr("Build succeeded.")
                                   : tr("Build failed with %1 diagnostic(s).")
                                         .arg(diagnostics.size()));

    // Run was requested: continue now that the build has actually finished.
    if (m_session->consumeLaunchAfterBuild()) {
        if (success)
            m_launcher->launch();
        else
            m_log->appendPlainText(tr("[run] build failed, so nothing was launched"));
    }
}

void MainWindow::run()
{
    runImpl(/*quiet=*/true);
}

void MainWindow::runInteractive()
{
    runImpl(/*quiet=*/false);
}

void MainWindow::runImpl(bool quiet)
{
    crash::note("run");

    // The project file or its script may have changed since it was opened — a
    // `git pull` is the ordinary case. Trust is content-keyed, so ask again
    // before the session that would actually run the hook.
    const QString hookSource = buildSourcePath();
    if (!hookSource.isEmpty()) {
        const QString projectPath = settings::projectFileFor(hookSource);
        if (QFileInfo::exists(projectPath) && !hookMayRunFor(projectPath, quiet)) {
            m_settings.stopHookEnabled = false;
            m_log->appendPlainText(tr("[hook] not allowed for this project; it will not run"));
        }
    }
    // Always rebuild before running: this keeps the listing in step with the
    // binary, which is what the line map depends on.
    //
    // The launch cannot happen here. build() starts the assembler asynchronously
    // and returns immediately, so anything after it would run while the build was
    // still in flight — and a "is the build running?" guard here is always true,
    // which silently turned the whole launch path into dead code. The launch is
    // chained onto build completion instead, via onBuildFinished.
    //
    // No source check of its own: build()'s refusal handles it (and must, or a
    // refused run would leave the session's launch intent armed for the next
    // build).
    m_session->setLaunchAfterBuild(true);
    // Run is a debug session, so it always builds with the debug arguments —
    // a release build would leave the debugger with no symbol table.
    buildImpl(quiet, /*release=*/false);
}

void MainWindow::stopSession()
{
    m_host->stop();

    if (!m_currentSessionDir.isEmpty()) {
        paths::removeSessionDir(m_currentSessionDir);
        m_currentSessionDir.clear();
    }
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

void MainWindow::step()
{
    // A plain step is not a running->stopped transition, so stoppedChanged does
    // not fire and onDebuggerStopped is never called — the refresh has to be
    // explicit or nothing updates (registers, disassembly, the editor, and the
    // step-back snapshot all depend on it). The same fact is why the stop hook
    // needs the step flag: the state batch that follows is the only report of
    // the movement (docs/stop-hooks-plan.md §6).
    m_session->setStepEventPending(true);
    m_host->step();
    m_host->refresh();
}

void MainWindow::stepOver()
{
    m_session->setStepEventPending(true);
    m_host->stepOver();
    m_host->refresh();
}

void MainWindow::stepOut()
{
    m_session->stepOut();
}

void MainWindow::runToCursor()
{
    if (!m_host->isStopped() || !m_editor)
        return;
    // "Run to my line" is a caret gesture like any other: it reads honestly as
    // "run until *my* line" only while the caret is the user's, so following
    // ends with it rather than dragging the caret off the line being run to
    // (cursor-plan §4.4).
    setPcFollow(false);
    const int line = m_editor->textCursor().blockNumber() + 1;
    quint32 address = 0;
    if (!m_programMap.codeAddressFor(m_editor->filePath(), line, &address)) {
        m_log->appendPlainText(tr("[run to cursor] no code address for %1:%2")
                                   .arg(m_editor->filePath())
                                   .arg(line));
        return;
    }
    m_log->appendPlainText(tr("[run to cursor] %1:%2 -> 0x%3")
                               .arg(m_editor->filePath())
                               .arg(line)
                               .arg(address, 0, 16));
    m_host->breakAtAddressOnce(address);
    m_host->resume();
}

void MainWindow::profileStart()
{
    m_profiling->start();
}

bool MainWindow::profileStop()
{
    return m_profiling->stop();
}

void MainWindow::profileToCursor()
{
    m_profiling->toCursor();
}

void MainWindow::showProfileResults()
{
    m_profiling->showResults();
}

void MainWindow::syncProfileActions()
{
    m_profiling->syncActions();
}

void MainWindow::pauseSession()
{
    m_host->pause();
}

void MainWindow::resume()
{
    m_host->resume();
}

void MainWindow::rebuildProgramMap()
{
    m_programMap.clear();

    const QStringList listings = m_build->listingFiles();
    const QStringList objects = m_build->objectFiles();
    QFileInfo sourceInfo(buildSourcePath());

    QStringList sources{sourceInfo.absoluteFilePath()};
    sources += m_settings.additionalSources;

    QString error;
    for (int i = 0; i < listings.size() && i < sources.size(); ++i) {
        // A module whose listing cannot be read is reported and skipped rather
        // than abandoning the whole map: the program still built and runs, only
        // that module's source lines are unavailable.
        if (!m_programMap.addModule(sources.at(i), objects.value(i), listings.at(i), &error)) {
            m_log->appendPlainText(QStringLiteral("[line map] ") + error);
        }
    }

    if (m_build->usesLinker()) {
        LinkMap linkMap;
        if (linkMap.parse(m_build->linkMapFile(), &error)) {
            m_programMap.setLinkMap(linkMap);
        } else {
            // Without the map, modules past the first cannot be located, so
            // breakpoints in them would be armed at the wrong address. Say so
            // instead of mapping them wrongly.
            m_log->appendPlainText(QStringLiteral("[link map] ") + error);
            addProblemRow({QString(), 0,
                           tr("Could not read the link map, so only the first module "
                              "can be mapped to source: %1").arg(error),
                           true});
        }
    }

    // The symbol browser reads the same listings. Addresses stay blank until
    // a session resolves the live bases (the view is re-fed in onStateUpdated);
    // the names feed the console's completion immediately.
    if (m_symbolsView) {
        m_symbols.clear();
        for (int i = 0; i < listings.size() && i < sources.size(); ++i)
            m_symbols += symbolsFromListing(listings.at(i), sources.at(i));
        m_symbolsView->setSymbols(m_symbols, &m_programMap);
        if (m_consoleInput) {
            QStringList all = m_consoleVerbs;
            for (const SymbolEntry &s : m_symbols)
                all << s.name;
            m_consoleInput->setCompletions(all);
        }

        // One candidate list, three surfaces (docs/glm-features.md §4): the
        // editor's completion popup filters this, the palette lists its symbols,
        // and the hover uses its kinds to decide what a word means. A symbol with
        // no source position (a command-line define) is still offered by name; its
        // file and line stay empty, so nothing claims a location it does not have.
        m_completionItems = CompletionModel::vocabulary();
        for (const SymbolEntry &symbol : m_symbols) {
            CompletionItem item;
            item.text = symbol.name;
            item.kind = symbol.equate ? CompletionKind::Equate : CompletionKind::Label;
            item.file = symbol.file;
            item.line = symbol.line;
            m_completionItems.append(item);
        }
        for (CodeEditor *editor : openEditors())
            editor->setCompletions(m_completionItems);
    }
    // Only a linked build can lose a module: a single-module `-Ftos` build has no
    // linker and no map, and its one module owns the whole program, so it is
    // placed by definition and its lines resolve against the live bases
    // directly. The line used to fire for exactly that case — naming the only
    // module of a plain single-file build as unplaced, before any live base
    // existed to place it (MIN-84).
    if (m_build->usesLinker()) {
        const QStringList unplaced = m_programMap.unplacedModules();
        if (!unplaced.isEmpty()) {
            m_log->appendPlainText(
                tr("[link map] no placement for: %1").arg(unplaced.join(QLatin1String(", "))));
        }
    }
}

void MainWindow::addWatchpoint()
{
    m_bpModel->addWatchpoint();
}

namespace {

/// The suffix that marks a file as a hook script, so the naming rule can tell one
/// from the source a script would be named after.
constexpr const char *kStopHookScriptSuffix = ".hooks.lua";

/// Where the project's stop-hook script lives when PiST makes one: beside the
/// source it hooks, named after it (`hello.s` → `hello.hooks.lua`).
///
/// Named after the source rather than a bare `hooks.lua` so that a directory
/// holding two projects cannot have them fight over one file, and so a project's
/// hook is findable without anyone being told where it is — which is what lets
/// the dialog offer it.
QString stopHookScriptPathFor(const QString &sourcePath)
{
    if (sourcePath.isEmpty())
        return QString();
    // Already a hook script — which it is whenever the script is the focused
    // tab, since the "source" is whatever is in front of you. Naming a script
    // after a script would give `hello.hooks.hooks.lua`: the second ask would
    // grow the name every time instead of adopting what is there.
    if (sourcePath.endsWith(QLatin1String(kStopHookScriptSuffix), Qt::CaseInsensitive))
        return QFileInfo(sourcePath).absoluteFilePath();
    const QFileInfo info(sourcePath);
    if (info.fileName().isEmpty())
        return QString();
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
         + QLatin1String(kStopHookScriptSuffix);
}

/// The starter script: a working example with the API written where the person
/// editing it will read it. A file PiST makes should be documentation as well as
/// a starting point — the alternative is sending someone to the manual before
/// their first line.
QByteArray stopHookStarterScript()
{
    return QByteArrayLiteral(R"lua(-- Stop hook: runs at every debugger stop, with the machine held still.
--
-- Available here, and nothing else (no io, os, require or debug):
--
--   readmem(addr, len)      a string of len bytes at addr; string.byte(s, 1)
--                           reads one byte, string.byte(s, 1, 4) reads four
--   writemem(addr, value)   write one byte
--   readreg(name)           d0-d7, a0-a7, pc, sr, usp, isp
--   writereg(name, value)   d0-d7, a0-a7, pc, sr  (Hatari cannot set usp/isp)
--   sym(name)               a label's address in this run; addresses move
--                           between runs, which is why this exists
--   log(text) / print(...)  one line in the Build & debug console, as [hook]
--   continue() / resume()   let the program run on; without "continue without
--                           asking" switched on, the stop holds instead
--
-- Two values describe the stop itself:
--
--   reason                  "breakpoint", "step", or "unknown" (a pause, an
--                           exception or a watchpoint — the emulator does not
--                           say which)
--   at                      the breakpoint that fired, "file:line"; only set
--                           when reason is "breakpoint"
--
--
-- Two things worth knowing before you rely on it:
--
--   * Addresses move. GEMDOS relocates the program on every run, so a data
--     address that worked last run is wrong this run. Breakpoints dodge this by
--     being file:line, and sym() resolves a name through this run's bases; an
--     address written into the script by hand cannot dodge it.
--   * Every stop gets a fresh interpreter: nothing carries over, so state that
--     must survive a stop belongs in the guest's own memory.
--
-- The line below is a starting point. Edit it, press Continue, and watch the
-- console.

log(string.format("stop at $%06x  d0=$%08x", readreg("pc"), readreg("d0")))
)lua");
}

} // namespace

QString MainWindow::createStopHookScript()
{
    const QString source = buildSourcePath();
    if (source.isEmpty()) {
        m_log->appendPlainText(
            tr("[hook] open a source first: the script is named after it"));
        return QString();
    }

    const QString path = stopHookScriptPathFor(source);
    if (QFileInfo::exists(path)) {
        // Never overwritten. A script someone has edited is the whole point of
        // the file, and this gesture is as likely to mean "take me to it".
        m_log->appendPlainText(
            tr("[hook] using the script already beside the source: %1").arg(path));
    } else {
        QString error;
        if (!files::write(path, stopHookStarterScript(), &error)) {
            QMessageBox::critical(this, tr("Stop hooks"), error);
            return QString();
        }
        m_log->appendPlainText(tr("[hook] wrote %1 — edit it, then press Continue").arg(path));
    }

    openPath(path);
    return path;
}

void MainWindow::editStopHooks()
{
    // An empty setting offers the conventional script when one already sits
    // beside the source — the naming rule exists precisely so nobody has to be
    // told where a project's hook is.
    QString path = m_settings.stopHookPath;
    if (path.isEmpty()) {
        const QString beside = stopHookScriptPathFor(buildSourcePath());
        if (!beside.isEmpty() && QFileInfo::exists(beside))
            path = beside;
    }

    StopHooksDialog dialog(path, m_settings.stopHookEnabled, m_settings.stopHookAutoContinue,
                           m_hooks ? m_hooks->lastRunSummary() : QString(), this);
    dialog.setCreateScriptHandler([this] { return createStopHookScript(); });
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_settings.stopHookPath = dialog.scriptPath();
    m_settings.stopHookEnabled = dialog.enabled();
    m_settings.stopHookAutoContinue = dialog.autoContinue();

    // The hook lives in the project file, and a project file is named after a
    // source: with none open there is nowhere to keep it, and saying so beats a
    // setting that quietly disappears.
    if (buildSourcePath().isEmpty()) {
        m_log->appendPlainText(tr("[hook] set for this session; open a source (and save the "
                                  "project) to keep it"));
        return;
    }
    persistSettings();

    if (m_settings.stopHookEnabled)
        m_log->appendPlainText(
            tr("[hook] %1 runs at every stop%2")
                .arg(m_settings.stopHookPath,
                     m_settings.stopHookAutoContinue ? tr(", and may continue on its own")
                                                     : QString()));
    else
        m_log->appendPlainText(tr("[hook] off"));
}

void MainWindow::debugCommand(const QString &command)
{
    // Route through the host; the response comes back on debugCommandFinished,
    // which is just the host's commandFinished re-emitted for the socket. The
    // text is the client's own, so it is the one place besides the console
    // where a debugger command is passed through as written.
    m_pendingDebugCommand = command;
    m_host->command(command);
}

void MainWindow::debugReadMemory(quint32 address, int length)
{
    // A tagged dump that no memory pane owns, so it reaches the verb that asked
    // (see kRemoteReadTag) instead of being applied to a view.
    m_host->requestMemoryDump(address, length, kRemoteReadTag);
}

void MainWindow::debugReadDisassembly(quint32 address)
{
    // The read reports its text on disassemblyReady, which this relays as a
    // remote read's answer.
    m_host->readDisassemblyAt(address);
}

void MainWindow::sendConsoleCommand(const QString &command)
{
    if (!m_host->isRunning()) {
        m_log->appendPlainText(tr("[console] no emulator session is running"));
        return;
    }
    m_log->appendPlainText(QStringLiteral("> ") + command);
    // The response streams via logLine from both backends (HRDB emits the ack
    // for console-originated commands; native streams stderr). While the machine
    // is running a debugger command cannot run: the native debugger does not
    // read stdin until it is entered, and HRDB's console handler only runs from
    // its break loop — so both hold it for the next stop, and HRDB says so in
    // the log rather than deferring it in silence.
    m_host->consoleCommand(command);
}

bool MainWindow::setMemoryByte(quint32 address, quint32 value, MemoryView *pane)
{
    if (!m_host->isStopped()) {
        m_log->appendPlainText(tr("[edit] memory can only be changed while stopped"));
        return false;
    }
    // One byte is written; the debugger is silent on success, so refresh the
    // pane that asked. Without a pane (the remote-control setmem path) the
    // first pane is the sensible default.
    m_host->writeMemoryByte(address, quint8(value));
    if (pane)
        pane->refresh();
    else if (m_memory)
        m_memory->refresh();
    return true;
}

bool MainWindow::setRegister(const QString &regName, quint32 value)
{
    if (!m_host->isStopped()) {
        m_log->appendPlainText(tr("[edit] registers can only be changed while stopped"));
        return false;
    }
    // A successful register write prints nothing — so refresh to show the new
    // value.
    m_host->writeRegister(regName, value);
    m_host->refresh();
    return true;
}

bool MainWindow::addWatchpointAddress(const QString &text, Watchpoint *added, QString *error)
{
    return m_bpModel->addWatchpointAddress(text, added, error);
}

bool MainWindow::clearWatchpoints(const QString &text, int *removed, QString *error)
{
    return m_bpModel->clearWatchpoints(text, removed, error);
}

void MainWindow::removeWatchpoint(int index)
{
    m_bpModel->removeWatchpoint(index);
}

bool MainWindow::toggleBreakpointAtLine(int line)
{
    if (!m_editor || line <= 0)
        return false;

    // Breakpoints are keyed by base name, so two modules linked from different
    // directories under the same file name (`util.s`) collide here and in the
    // gutter. Re-keying on the full path is a larger change (the panel, the
    // listings and the linker map all identify modules the same way); until then
    // this is the documented limitation.
    return m_bpModel->toggle(QFileInfo(m_editor->filePath()).fileName(), line);
}

bool MainWindow::toggleBreakpoint(const QString &file, int line)
{
    return m_bpModel->toggle(file, line);
}

bool MainWindow::toggleBreakpointAtLabel(const QString &name, QString *detail)
{
    return m_bpModel->toggleAtLabel(name, detail);
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

bool MainWindow::lineHasCode(int line) const
{
    if (!m_editor || line <= 0)
        return false;
    if (!m_programMap.isResolved())
        return true; // no build map to judge by; arming will decide
    quint32 address = 0;
    return m_programMap.codeAddressFor(QFileInfo(m_editor->filePath()).fileName(),
                                       line, &address);
}

void MainWindow::removeBreakpoint(const QString &file, int line)
{
    m_bpModel->remove(file, line);
}

QString MainWindow::resolveNavigablePath(const QString &file) const
{
    if (file.isEmpty())
        return {};
    if (QFileInfo(file).isAbsolute() && QFileInfo::exists(file))
        return QFileInfo(file).absoluteFilePath();
    for (const QString &source : m_programMap.sourceFiles()) {
        if (LineMap::sameSource(file, source) && QFileInfo::exists(source))
            return QFileInfo(source).absoluteFilePath();
    }
    const QString anchor = buildSourcePath();
    if (!anchor.isEmpty()) {
        const QString beside = QFileInfo(anchor).absolutePath() + QLatin1Char('/')
                             + QFileInfo(file).fileName();
        if (QFileInfo::exists(beside))
            return beside;
    }
    if (QFileInfo::exists(file))
        return QFileInfo(file).absoluteFilePath();
    return {};
}

bool MainWindow::navigateToSourceLine(const QString &file, int line)
{
    if (line <= 0)
        return false;

    // A build-level note has no source file. Stay in the editor that is open.
    if (file.isEmpty()) {
        if (!m_editor)
            return false;
        m_tabs->setCurrentWidget(m_editor);
        m_editor->gotoLine(line);
        return true;
    }

    CodeEditor *target = nullptr;
    for (CodeEditor *editor : openEditors()) {
        if (LineMap::sameSource(file, editor->filePath())) {
            target = editor;
            break;
        }
    }
    if (!target) {
        const QString path = resolveNavigablePath(file);
        if (path.isEmpty() || !openPath(path)) {
            statusBar()->showMessage(tr("Could not open %1").arg(file), 5000);
            return false;
        }
        for (CodeEditor *editor : openEditors()) {
            if (LineMap::sameSource(path, editor->filePath())) {
                target = editor;
                break;
            }
        }
        if (!target) {
            statusBar()->showMessage(tr("Could not open %1").arg(file), 5000);
            return false;
        }
    }
    m_tabs->setCurrentWidget(target);
    target->gotoLine(line);
    target->setFocus();
    return true;
}

void MainWindow::goToBreakpoint(const QString &file, int line)
{
    navigateToSourceLine(file, line);
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

void MainWindow::showSymbolPalette()
{
    if (!m_symbolPalette) {
        m_symbolPalette = new SymbolPaletteDialog(this);
        connect(m_symbolPalette, &SymbolPaletteDialog::symbolActivated, this,
                [this](const QString &file, int line) { navigateToSourceLine(file, line); });
    }
    // The symbols belong to the build, and a build may have run since the palette
    // was last shown: hand it the current candidates every time.
    m_symbolPalette->setSymbols(m_completionItems);
    m_symbolPalette->show();
    m_symbolPalette->raise();
    m_symbolPalette->activateWindow();
    m_symbolPalette->focusFilter();
}

void MainWindow::stepDiagnostic(int direction)
{
    const int count = m_problems ? m_problems->topLevelItemCount() : 0;
    if (count == 0)
        return;

    // From the current row, wrapping; an item without a line (build-level
    // messages) cannot be navigated to, so it is skipped.
    int row = m_problems->indexOfTopLevelItem(m_problems->currentItem());
    for (int i = 0; i < count; ++i) {
        row = ((row + direction) % count + count) % count;
        QTreeWidgetItem *item = m_problems->topLevelItem(row);
        const RemoteStateAdapter::ProblemRow *entry = problemEntryFor(item);
        if (!entry || entry->line <= 0)
            continue;
        // Same contract as double-clicking the item: navigate within the open
        // document only (goToBreakpoint's rule), never switch files blindly.
        m_problems->setCurrentItem(item);
        navigateToSourceLine(entry->file, entry->line);
        return;
    }
}

void MainWindow::nextDiagnostic()
{
    stepDiagnostic(+1);
}

void MainWindow::previousDiagnostic()
{
    stepDiagnostic(-1);
}

void MainWindow::editBreakpointCondition(int line)
{
    m_bpModel->editCondition(line);
}

void MainWindow::editBreakpointScript(int line)
{
    m_bpModel->editScript(line);
}

void MainWindow::clearAllBreakpoints()
{
    m_bpModel->clear();
}

void MainWindow::clearAllDebugTargets()
{
    m_bpModel->clearAllTargets();
}

void MainWindow::onStateUpdated(const MachineState &state)
{
    m_lastState = state;
    m_registers->setState(state);
    // Publish the stop only once a register batch has landed: the entry
    // attach's standalone `info basepage` emits a state update with pc still
    // 0, and publishing that (observed as "event stopped pc=0x00000000")
    // tells a watcher the machine stopped at address 0. The refresh that
    // follows every stop carries real registers, so the event keeps its pc.
    if (m_session->stopEventPending() && state.regs.valid) {
        m_session->setStopEventPending(false);
        if (m_eventSink)
            m_eventSink->publishEvent(QStringLiteral("stopped"),
                                      QStringLiteral("pc=0x%1").arg(hex::hex32(state.pc, hex::Case::Lower)));
        // The stop hook runs from here, and nowhere earlier: this is the moment
        // the stop is real enough to report — the register batch has landed, so
        // readreg() has the machine's actual truth, and the attach's own
        // commands have completed (docs/stop-hooks-plan.md D1).
        if (m_hooks) {
            // The cause, as far as the transport can tell us: a breakpoint whose
            // armed address is this PC. A pause, an exception and a watchpoint
            // are indistinguishable from here, so they stay "unknown" rather
            // than being guessed at.
            StopTrigger how;
            if (const Breakpoint *hit = breakpointAt(m_bpModel->breakpoints(), state.pc)) {
                how.reason = StopReason::Breakpoint;
                how.at = hit->label();
                how.script = hit->scriptPath;
            }
            m_hooks->onStopPublished(state, how);
        }
    } else if (m_session->stepEventPending() && state.regs.valid) {
        // A step never reaches the branch above: `s`/`n` are not running→stopped
        // transitions, so stoppedChanged does not fire for them (see step()).
        // The flag makes "every stop" include them.
        m_session->setStepEventPending(false);
        if (m_hooks) {
            StopTrigger how;
            how.reason = StopReason::Step;
            m_hooks->onStopPublished(state, how);
        }
    }
    m_disassembly->setState(state);

    bool navigated = false;
    if (state.hasBases()) {
        const bool firstBases = !m_bases.isValid();
        m_bases.text = state.textBase;
        m_bases.data = state.dataBase;
        m_bases.bss = state.bssBase;

        // Hand the live bases to the program map. Without this, lineFor and
        // addressFor return false forever: no breakpoint ever resolves, and no
        // stop ever finds a source line to place the execution marker on — the
        // chips read "Stopped" and nothing more (docs/code-review-glm-001.md
        // P1 root cause B).
        m_programMap.setLiveBases(m_bases);

        // The symbol browser resolves addresses only now that the bases exist.
        if (m_symbolsView && !m_symbols.isEmpty())
            m_symbolsView->setSymbols(m_symbols, &m_programMap);

        // Arm breakpoints when the bases they resolve against actually arrive —
        // here, where they are set — not on a zero-delay timer that always fires
        // before the basepage response does (P1 root cause A).
        if (m_session->sessionArmed() && !m_session->breakpointsArmedThisSession())
            m_session->armBreakpoints();

        // Point the memory pane at the program's own data on the first stop.
        // Its previous address is meaningless across sessions.
        if (firstBases && m_memory) {
            m_memory->goToAddress(state.dataBase ? state.dataBase : state.textBase);
            navigated = true;
        }
    }

    // Re-read the memory pane whenever the machine state changes, not only when
    // the user navigates: a memory view that goes stale the moment you step is
    // broken. The first stop is skipped because it just navigated, and that
    // already requested the dump it needs.
    if (m_memory && !navigated)
        m_memory->refresh();

    // Re-dump the stack too: it changes on every call, return, push and pop, so
    // like the memory view it is only meaningful if it tracks the machine.
    if (m_stack && state.regs.valid)
        m_host->requestStackDump(state.regs.a[7], 96);

    // Refresh the hardware registers as well.
    if (m_hardware)
        m_host->infoSubject(m_hardware->subject());

    // And the execution path that led here.
    if (m_pcHistory)
        m_host->readHistory(16);

    locationFromPc(state.pc);
    updateRegisterStrip();
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

void MainWindow::locationFromPc(quint32 pc)
{
    LineMap::Address address;
    const bool mapped = m_bases.isValid() && !m_programMap.isEmpty()
                        && m_programMap.lineFor(pc, &address);
    if (!mapped) {
        // Unmapped: library or ROM code, or a PC of 0 before the bases are
        // live. The marker goes (it is nothing but a lie now), and the caret,
        // the scroll, the focus and the tab layout — the user's, all of them —
        // stay exactly where they were (cursor-plan §3.4).
        setExecutionMarker(nullptr, 0);
        m_stoppedFile.clear();
        m_stoppedLine = 0;
        updateSessionChip();
        updateCaretChip();
        return;
    }

    // The PC's file among the open editors. The comparison must be
    // path-tolerant: the listing records whatever path was passed to vasm
    // (usually absolute), while the editor knows the file it was opened with
    // (usually just a name).
    CodeEditor *target = nullptr;
    for (CodeEditor *editor : openEditors())
        if (LineMap::sameSource(address.file, editor->filePath())) {
            target = editor;
            break;
        }

    m_stoppedFile = QFileInfo(address.file).fileName();
    m_stoppedLine = address.line;
    updateSessionChip();

    if (!m_pcPlaced) {
        // The session's first stop is a *placement*. It arrives at the
        // program's own first line moments after the user pressed Run, which is
        // exactly where a caret-following editor used to drag them (the
        // reported injury, cursor-plan §1). The marker and the chips say where
        // the machine is; nothing else moves, and no file is opened for it.
        m_pcPlaced = true;
        setExecutionMarker(target, address.line);
        updateCaretChip();
        return;
    }

    if (!m_pcFollow) {
        // The caret is the user's. The marker follows the PC; the caret, the
        // scroll, the focus and the tab layout do not, and a file the PC has
        // walked into is not opened on its behalf (cursor-plan §3.2, §4.3).
        setExecutionMarker(target, address.line);
        updateCaretChip();
        return;
    }

    // Following: this movement is the PC's, and the caret comes along.
    // Everything below — opening and raising the tab, and the caret move itself
    // — happens under the guard, so the cursorPositionChanged those produce is
    // not mistaken for the user claiming the caret back (cursor-plan §4.2).
    // Saved and restored rather than just cleared: opening a tab can run a
    // nested event loop (a warning modal for a file that will not load), and a
    // state update delivered in there re-enters this function — the guard the
    // outer call set has to survive that.
    const bool wasFollowingInProgress = m_pcFollowInProgress;
    m_pcFollowInProgress = true;

    // Opening the PC's file is part of following: the caret has to have
    // somewhere to land. It is the one thing the released path above must not
    // do, and the guard's opening paragraph covers it too — a tab created here
    // lands its own cursor in the document.
    if (!target && QFileInfo::exists(address.file))
        target = addEditorTab(address.file);

    if (target) {
        m_tabs->setCurrentWidget(target);
        setExecutionMarker(target, address.line);
        target->followExecutionLine(address.line);
    } else {
        setExecutionMarker(nullptr, 0);
    }

    m_pcFollowInProgress = wasFollowingInProgress;
    updateCaretChip();
}

bool MainWindow::engagePcFollow()
{
    if (!m_host || !m_host->isRunning() || !m_host->isStopped())
        return false;

    // The stop this engagement applies to is a movement of the PC's, not the
    // untouched placement of a fresh session: the user is asking for the caret
    // to come along from here on, whether or not a placement has happened yet
    // (it has not when the first stop's PC did not map).
    m_pcPlaced = true;
    setPcFollow(true);
    return true;
}

void MainWindow::setPcFollow(bool following)
{
    m_pcFollow = following;
    if (!m_actFollow)
        return;
    // Reported, not clicked: letting this through would re-enter the toggled
    // handler that calls the very function this is inside.
    const QSignalBlocker blocker(m_actFollow);
    m_actFollow->setChecked(following);
}

void MainWindow::setExecutionMarker(CodeEditor *target, int line)
{
    for (CodeEditor *editor : openEditors())
        if (editor != target)
            editor->clearCurrentExecutionLine();
    if (target)
        target->setCurrentExecutionLine(line);
}

void MainWindow::followExecution()
{
    if (!engagePcFollow())
        return;
    // Go to where the machine is now: every engage affordance is something the
    // user clicked *in order to* get to the PC, so engaging is also the jump.
    // An unmapped PC — library code, or a session whose first stop has not
    // arrived — has nowhere to go, and locationFromPc says so; following is
    // armed all the same, and the caret moves at the next mapped stop.
    locationFromPc(m_lastState.pc);
}

} // namespace pist
