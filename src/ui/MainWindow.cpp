// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
#include "emu/HexFormat.h"
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
bool suffixIsEditableText(const QString &suffix)
{
    static const QSet<QString> text = {
        QStringLiteral("txt"), QStringLiteral("doc"), QStringLiteral("me"),
        QStringLiteral("1st"), QStringLiteral("nfo"), QStringLiteral("inf"),
        QStringLiteral("h"),   QStringLiteral("c"),   QStringLiteral("i"),
        QStringLiteral("mac"),
    };
    return text.contains(suffix);
}

bool suffixIsStillImage(const QString &suffix)
{
    static const QSet<QString> images = {
        QStringLiteral("pi1"), QStringLiteral("neo"), QStringLiteral("iff"),
        QStringLiteral("ilbm"), QStringLiteral("png"), QStringLiteral("pim"),
    };
    return images.contains(suffix);
}

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

/// Whether a dock belongs to the debug set: the docks the first-run Editing
/// arrangement keeps off screen and the Debugging preset turns on. One list,
/// because both users must agree — a debug dock added to one and not the other
/// would be hidden by Reset layout and never be shown by Debugging.
bool isDebugDockName(const QString &name)
{
    return name == QLatin1String("registersDock")
        || name == QLatin1String("disassemblyDock")
        || name == QLatin1String("stackDock")
        || name == QLatin1String("hardwareDock")
        || name == QLatin1String("pcHistoryDock")
        || name == QLatin1String("breakpointsDock")
        || name == QLatin1String("instructionRefDock")
        || name == QLatin1String("symbolsDock")
        || name == QLatin1String("profilerDock")
        || name.startsWith(QLatin1String("memoryDock"));
}

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

bool suffixIsKnownBinary(const QString &suffix)
{
    static const QSet<QString> binary = {
        // Executables and object code.
        QStringLiteral("prg"), QStringLiteral("tos"), QStringLiteral("ttp"),
        QStringLiteral("bin"), QStringLiteral("sys"), QStringLiteral("o"),
        // Disk and still-image formats the IDE recognises.
        QStringLiteral("st"),  QStringLiteral("msa"), QStringLiteral("img"),
        QStringLiteral("dim"), QStringLiteral("ipf"), QStringLiteral("pi1"),
        QStringLiteral("pi2"), QStringLiteral("pi3"), QStringLiteral("neo"),
        QStringLiteral("iff"), QStringLiteral("ilbm"), QStringLiteral("png"),
        QStringLiteral("mbk"), QStringLiteral("pim"),
        // Sources that must be edited from the hard drive.
        QStringLiteral("s"),   QStringLiteral("asm"),
        QStringLiteral("pistproject"), QStringLiteral("lst"),
    };
    return binary.contains(suffix);
}

/// The QSettings keys this window persists, one accessor per key beside the
/// place that reads it (MIN-53). Every read and every write goes through these,
/// so a key is spelled exactly once in the file — a typo in the literal pair
/// used to lose a preference with nothing to point at. The window-state keys
/// keep their historical names: they are what an existing installation's
/// persisted layout lives under.
const QString &gitBlameKey()
{
    static const QString key = QStringLiteral("git/blame");
    return key;
}

/// The one-shot "you can drag phases onto the sheet" hint.
const QString &offeredSpriteKey()
{
    static const QString key = QStringLiteral("layout/offeredSprite");
    return key;
}

const QString &offeredDebuggingKey()
{
    static const QString key = QStringLiteral("layout/offeredDebugging");
    return key;
}

const QString &layoutStateKey()
{
    static const QString key = QStringLiteral("layout/state");
    return key;
}

const QString &layoutGeometryKey()
{
    static const QString key = QStringLiteral("layout/geometry");
    return key;
}

const QString &layoutWidthKey()
{
    static const QString key = QStringLiteral("layout/width");
    return key;
}

const QString &layoutHeightKey()
{
    static const QString key = QStringLiteral("layout/height");
    return key;
}

/// The layout stashed for the duration of a preset (see applyLayoutPreset).
const QString &layoutPreviousKey()
{
    static const QString key = QStringLiteral("layout/previous");
    return key;
}

} // namespace

namespace {

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

} // namespace

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

CodeEditor *MainWindow::addEditorTab(const QString &path, bool quiet)
{
    // A document is a file on disk, so there is no path-less form of this to
    // create: the assertion documents the invariant the removal of the untitled
    // buffer rests on (docs/untitled-plan.md §8). A caller with no path has
    // nothing to show the user.
    Q_ASSERT(!path.isEmpty());

    auto *editor = new CodeEditor(appearance::editorTheme(), this);
    wireEditor(editor);
    // Completion: the vocabulary and the last build's symbols. A tab opened after
    // a build gets them at once; a build pushes a fresh list into every editor.
    editor->setCompletions(m_completionItems);
    if (!editor->loadFile(path)) {
        if (!quiet)
            QMessageBox::warning(this, tr("Open"), tr("Could not open %1").arg(path));
        editor->deleteLater();
        return nullptr;
    }
    const int index = m_tabs->addTab(editor, editor->displayName());
    m_tabs->setCurrentIndex(index);
    updateTabTitle(editor);
    syncWatchedDocuments();
    return editor;
}

ImageEditor *MainWindow::addImageTab(const QString &path, bool quiet)
{
    // As with a text editor, creation always has a file. (The image editor may
    // become path-less *later* — an imported sheet has no file of its own yet —
    // which is its own deliberate workflow, not a creation path.)
    Q_ASSERT(!path.isEmpty());

    auto *editor = new ImageEditor(this);
    wireImage(editor);
    wireImageReExport(editor);
    const bool ok = isPimPath(path) ? editor->loadFile(path) : editor->importFile(path, false);
    if (!ok) {
        if (!quiet)
            QMessageBox::warning(this, tr("Open"),
                                 tr("Could not open %1: %2").arg(path, editor->lastError()));
        editor->deleteLater();
        return nullptr;
    }
    const int index = m_tabs->addTab(editor, editor->displayName());
    m_tabs->setCurrentIndex(index);
    updateTabTitle(editor);
    syncWatchedDocuments();
    return editor;
}

void MainWindow::wireEditor(CodeEditor *editor)
{
    // The title carries the modified marker, so the user can always tell
    // whether work is unsaved without looking for a toolbar state.
    connect(editor, &CodeEditor::modificationChanged, this, [this, editor](bool) {
        updateTabTitle(editor);
        if (editor == m_editor)
            updateModifiedState();
    });

    connect(editor, &CodeEditor::visibleRangeSettled, this, [this, editor] {
        if (editor == m_editor)
            refreshBlame();
    });

    connect(editor, &CodeEditor::gutterClicked, this, [this](int line, Qt::MouseButton button) {
        if (button == Qt::LeftButton)
            toggleBreakpointAtLine(line);
        else if (button == Qt::RightButton)
            editBreakpointCondition(line);
    });

    // The gutter's PC wedge — the marker's own click target. A click there is
    // the user handing the caret to the program counter, so it engages instead
    // of toggling a breakpoint (cursor-plan §3.3, §5).
    connect(editor, &CodeEditor::executionMarkerClicked, this,
            [this] { followExecution(); });

    // The instruction reference follows the cursor, but only when its dock is
    // on show — otherwise an idle panel would churn on every keystroke. OS
    // calls win over the word: cursor on a `trap #1` line (or a push feeding
    // one) shows the call being made, not the TRAP or MOVE instruction.
    connect(editor, &CodeEditor::cursorPositionChanged, this, [this, editor] {
        // A caret move that is not the follow path's own is the user taking the
        // caret back: typing, a click, an arrow key, the Go-to bar, a jump to a
        // diagnostic. The claim ends there and the caret is theirs again
        // (cursor-plan §3.3). The guard is what keeps the PC's own move — which
        // arrives through this same signal — from ending following at every
        // step.
        if (m_pcFollow && !m_pcFollowInProgress)
            setPcFollow(false);
        if (editor == m_editor)
            updateCaretChip();
        if (editor == m_editor)
            followCursorReference(editor);
    });
    connect(editor, &CodeEditor::gutterContextMenuRequested, this,
            [this, editor](int line, const QPoint &pos) {
                QMenu menu;
                const QList<Breakpoint> &breakpoints = m_bpModel->breakpoints();
                const bool hasBreakpoint = std::any_of(
                    breakpoints.cbegin(), breakpoints.cend(),
                    [editor, line](const Breakpoint &bp) {
                        return bp.line == line
                            && bp.file == QFileInfo(editor->filePath()).fileName();
                    });
                QAction *toggle = menu.addAction(hasBreakpoint ? tr("Remove breakpoint")
                                                               : tr("Add breakpoint"));
                QAction *condition = menu.addAction(tr("Edit condition…"));
                QAction *script = menu.addAction(tr("Stop-hook script…"));
                QAction *chosen = menu.exec(pos);
                if (chosen == toggle)
                    toggleBreakpointAtLine(line);
                else if (chosen == condition)
                    editBreakpointCondition(line);
                else if (chosen == script)
                    editBreakpointScript(line);
            });
}

void MainWindow::wireImage(ImageEditor *editor)
{
    connect(editor, &ImageEditor::modificationChanged, this, [this, editor](bool) {
        updateTabTitle(editor);
        if (editor == m_image)
            updateModifiedState();
    });
}

void MainWindow::wireImageReExport(ImageEditor *editor)
{
    // A re-export repeats the block map in the console, same as the explicit
    // export leaves it. The block map comes from the editor that emitted:
    // an export can finish after the user switched tabs, and reading the
    // recipe back through m_image would then describe a different document
    // (or dereference null with no image tab open at all).
    connect(editor, &ImageEditor::bitplaneReExported, this,
            [this, editor](const QString &path, const QString &scroller, const QString &error) {
                if (!m_log)
                    return;
                if (!error.isEmpty()) {
                    m_log->appendPlainText(tr("[export] re-export failed: %1").arg(error));
                    return;
                }
                m_log->appendPlainText(tr("--- bitplane data (re-export): %1 ---")
                                           .arg(QFileInfo(path).fileName()));
                const ImageDocument &doc = editor->document();
                const int phase = editor->lastBitplaneExportPhase();
                const QVector<BitplaneBlock> blocks = bitplaneLayout(
                    doc.phases().at(phase).cellW,
                    doc.phases().at(phase).cellH,
                    doc.phases().at(phase).frames.size(),
                    editor->lastBitplaneExportOptions());
                for (const BitplaneBlock &block : blocks)
                    m_log->appendPlainText(QStringLiteral("%1 equ $%2")
                                               .arg(block.name, -20)
                                               .arg(block.offset, 4, 16, QLatin1Char('0')));
                if (!scroller.isEmpty())
                    m_log->appendPlainText(tr("%1 — press F7 to assemble it and watch this "
                                              "sprite scroll").arg(QFileInfo(scroller).fileName()));
            });
}

QList<CodeEditor *> MainWindow::openEditors() const
{
    QList<CodeEditor *> editors;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *editor = qobject_cast<CodeEditor *>(m_tabs->widget(i)))
            editors.append(editor);
    return editors;
}

QList<ImageEditor *> MainWindow::openImages() const
{
    QList<ImageEditor *> editors;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *editor = qobject_cast<ImageEditor *>(m_tabs->widget(i)))
            editors.append(editor);
    return editors;
}

CodeEditor *MainWindow::editorForPath(const QString &path) const
{
    const QFileInfo wanted(path);
    for (CodeEditor *editor : openEditors())
        if (QFileInfo(editor->filePath()) == wanted)
            return editor;
    return nullptr;
}

ImageEditor *MainWindow::imageForPath(const QString &path) const
{
    const QFileInfo wanted(path);
    for (ImageEditor *editor : openImages())
        if (QFileInfo(editor->filePath()) == wanted)
            return editor;
    return nullptr;
}

void MainWindow::updateTabTitle(QWidget *widget)
{
    const int index = m_tabs->indexOf(widget);
    if (index < 0)
        return;
    QString name;
    bool modified = false;
    if (auto *editor = qobject_cast<CodeEditor *>(widget)) {
        name = editor->displayName();
        modified = editor->isModifiedSinceLoad();
    } else if (auto *image = qobject_cast<ImageEditor *>(widget)) {
        name = image->displayName();
        modified = image->isModifiedSinceLoad();
    }
    m_tabs->setTabText(index, name + (modified ? QStringLiteral(" *") : QString()));
}

void MainWindow::onTabChanged(int index)
{
    QWidget *widget = m_tabs->widget(index);
    m_editor = qobject_cast<CodeEditor *>(widget);
    m_image = qobject_cast<ImageEditor *>(widget);

    if (m_actExportImage) {
        m_actExportImage->setEnabled(m_image != nullptr);
        m_actExportImageSafe->setEnabled(m_image != nullptr);
        m_actExportSpriteSheet->setEnabled(m_image != nullptr
                                           && m_image->currentSheetIndex() >= 0);
        m_actExportBitplanes->setEnabled(m_image != nullptr);
        if (m_actReExportBitplanes)
            m_actReExportBitplanes->setEnabled(m_image != nullptr
                                               && m_image->canReExportBitplane());
    }
    // Find and replace act on the text editor, so they come and go with it: an
    // image tab has nothing to search.
        if (m_actFind) {
        m_actFind->setEnabled(m_editor != nullptr);
        m_actFindNext->setEnabled(m_editor != nullptr);
        m_actFindPrevious->setEnabled(m_editor != nullptr);
        m_actReplace->setEnabled(m_editor != nullptr);
        if (m_actGotoLine)
            m_actGotoLine->setEnabled(m_editor != nullptr);
    }

    const QString path = m_editor ? m_editor->filePath()
                                  : (m_image ? m_image->filePath() : QString());
    if (!path.isEmpty() && m_fileBrowser)
        m_fileBrowser->showFor(path);
    if (m_editor)
        m_bpModel->refreshMarkers();
    updateModifiedState();
    updateCaretChip();
    // Deferred so a following "Opened …" status line does not eat the offer.
    // A modal would hang the offscreen image tests.
    if (m_image && !QSettings().value(offeredSpriteKey()).toBool()) {
        QTimer::singleShot(0, this, [this] {
            if (!m_image
                || QSettings().value(offeredSpriteKey()).toBool())
                return;
            QSettings().setValue(offeredSpriteKey(), true);
            if (statusBar())
                statusBar()->showMessage(
                    tr("View → Layout → Sprite gives the canvas the window."), 8000);
        });
    }
    if (m_instrStrip)
        m_instrStrip->setVisible(m_editor != nullptr);
    if (m_editor)
        followCursorReference(m_editor);
    syncGitDirectory();
    syncFindInFilesScope();
    applyGitBlame();
    // Zero tabs is reachable — by the × button, by Close File, by Closing All —
    // and it means the welcome view, not a stranded empty editor.
    updateCentralPage();
}

void MainWindow::onTabCloseRequested(int index)
{
    closeTab(index);
}

bool MainWindow::closeTab(int index, bool promptToSave)
{
    QWidget *widget = m_tabs->widget(index);
    if (!widget)
        return false;

    QString closedPath;
    if (auto *editor = qobject_cast<CodeEditor *>(widget)) {
        closedPath = editor->filePath();
        if (promptToSave && !maybeSaveEditor(editor))
            return false;
    } else if (auto *image = qobject_cast<ImageEditor *>(widget)) {
        closedPath = image->filePath();
        if (promptToSave && !maybeSaveImage(image))
            return false;
    }

    m_tabs->removeTab(index);
    widget->deleteLater();

    // The extracted-document mapping is a per-tab registry — keyed by the path
    // the closed tab held, which is the path that tab's saves wrote back
    // through (writeBackFloppyDoc). It goes with the tab: kept forever, the map
    // grew one entry per extracted file ever opened, and a save of a path
    // nothing on screen describes any more (reopened through Open rather than
    // the disk pane) still wrote itself into that image.
    if (!closedPath.isEmpty())
        m_floppyDocs.remove(QFileInfo(closedPath).absoluteFilePath());

    // The document's file stops being watched with the tab: nothing on screen
    // is bound to it any more (docs/untitled-plan.md §7.1).
    syncWatchedDocuments();
    // The last document closing lands on the welcome view
    // (docs/untitled-plan.md §5); a tab switch normally does this, but removing
    // the tab that was *not* current does not emit one.
    updateCentralPage();
    return true;
}

void MainWindow::closeCurrentFile()
{
    closeTab(m_tabs->currentIndex());
}

void MainWindow::closeAllFiles()
{
    // One at a time, through the same door as the ×: each document gets its
    // save offer, and a Cancel stops the sweep where the user stopped it.
    while (m_tabs->count() > 0 && closeTab(m_tabs->currentIndex())) {
    }
}

void MainWindow::newFile()
{
    newFileIn(suggestedSourceDirectory());
}

void MainWindow::newFileIn(const QString &directory)
{
    NewFileDialog dialog(this, directory);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const NewFileSpec spec = dialog.spec();
    QString error;
    if (!spec.create(&error)) {
        QMessageBox::warning(this, tr("New File"),
                             tr("Could not create %1: %2").arg(spec.targetPath(), error));
        return;
    }

    // The file is on disk before it is on screen, and it is what the next launch
    // reopens — a new document is a document like any other
    // (docs/untitled-plan.md §4). openPath() reveals it in the sidebar and
    // raises it rather than duplicating it if it was somehow already open.
    settings::rememberLastSource(spec.targetPath());
    openPath(spec.targetPath());
}

QString MainWindow::suggestedSourceDirectory() const
{
    // The project's own directory heads the chain: a new source is going to be
    // built beside the ones already there (docs/untitled-plan.md §4). The
    // sidebar's chosen root is the project — its default before a root is
    // chosen is the launch directory, which is not one (hasProjectDirectory).
    if (m_fileBrowser && m_fileBrowser->hasProjectDirectory())
        return m_fileBrowser->directory();
    return suggestedImageDirectory();
}

void MainWindow::updateCentralPage()
{
    if (!m_center)
        return;
    // Zero documents means the welcome view, and it is rebuilt as it is shown:
    // it is a doorway, so it must not show a recents list or a counter the
    // session has moved on from.
    const bool welcome = m_tabs->count() == 0;
    if (welcome)
        refreshWelcome();
    m_center->setCurrentIndex(welcome ? 1 : 0);
}

void MainWindow::refreshWelcome()
{
    if (!m_welcome)
        return;

    // The demo projects are the first run's doorway; the view holds them back
    // once the user has recents of their own (docs/untitled-plan.md §3).
    m_welcome->setDemoProjects(WelcomeView::demoProjects());
    m_welcome->setRecentSources(settings::recentSources());

    // Readiness, with the project's own tool overrides honoured: the line has to
    // say what a build here would actually use.
    const ToolInfo assembler = toolchain::findAssembler(m_settings.assemblerPath);
    const ToolInfo emulator = toolchain::findEmulator(m_settings.hatariPath);
    const QList<TosRom> roms = findTosRoms();
    QStringList readiness;
    readiness << (assembler.found()
                      ? tr("Assembler: %1").arg(QFileInfo(assembler.path).fileName())
                      : tr("Assembler: not found"));
    readiness << (emulator.found()
                      ? tr("Emulator: %1").arg(QFileInfo(emulator.path).fileName())
                      : tr("Emulator: not found"));
    const bool romNamed = !m_settings.tosPath.isEmpty();
    if (romNamed)
        readiness << tr("TOS ROM: %1").arg(QFileInfo(m_settings.tosPath).fileName());
    else if (roms.isEmpty())
        readiness << tr("TOS ROM: not found");
    else
        readiness << tr("TOS ROM: %1").arg(roms.first().versionText());
    const bool missing = !assembler.found() || !emulator.found()
                         || (!romNamed && roms.isEmpty());
    m_welcome->setReadiness(missing, readiness);

    WelcomeView::Stats stats;
    stats.sessions = settings::sessionCount();
    stats.builds = settings::buildCount();
    stats.runs = settings::runCount();
    stats.opened = settings::openedFileCount();
    stats.firstLaunch = settings::firstLaunchDate();
    const settings::LastSession last = settings::lastSession();
    stats.lastSessionKnown = last.known;
    stats.lastSessionBuilds = last.builds;
    stats.lastBuildOk = last.lastBuildOk;
    // "The file I was on" is this session's answer while there is one; the
    // flushed summary is only for an installation that has not opened anything
    // since it last quit.
    stats.lastFile = settings::lastSourcePath().isEmpty() ? last.lastFile
                                                          : settings::lastSourcePath();
    m_welcome->setStats(stats);
}

void MainWindow::syncWatchedDocuments()
{
    if (!m_watcher)
        return;
    QStringList paths;
    for (CodeEditor *editor : openEditors())
        paths.append(editor->filePath());
    for (ImageEditor *image : openImages())
        paths.append(image->filePath());
    m_watcher->setPaths(paths);
}

bool MainWindow::saveEditor(CodeEditor *editor, const QString &path)
{
    if (!editor || path.isEmpty())
        return false;
    if (!editor->saveFile(path))
        return false;
    // Our own write replaced the file by staging and renaming it; put the watch
    // back rather than trusting that it survived (docs/untitled-plan.md §7.1).
    if (m_watcher)
        m_watcher->rearm(path);
    // A save-as (the delete rescue) moves the document to a new path, so the
    // watched set is rebuilt rather than patched.
    syncWatchedDocuments();
    writeBackFloppyDoc(path);
    return true;
}

bool MainWindow::saveImage(ImageEditor *image, const QString &path)
{
    if (!image || path.isEmpty())
        return false;
    if (!image->saveFile(path))
        return false;
    if (m_watcher)
        m_watcher->rearm(path);
    syncWatchedDocuments();
    writeBackFloppyDoc(path);
    return true;
}

void MainWindow::documentVanished(const QString &path, bool deliberate)
{
    // The file is gone, so it is not watched again whichever way this arrived —
    // and for the sidebar's own delete the gesture has already said what it
    // wants (docs/untitled-plan.md §7.3).
    if (m_watcher)
        m_watcher->forget(path);

    QList<QWidget *> documents;
    for (CodeEditor *editor : openEditors()) {
        if (QFileInfo(editor->filePath()) == QFileInfo(path))
            documents.append(editor);
    }
    for (ImageEditor *image : openImages()) {
        if (QFileInfo(image->filePath()) == QFileInfo(path))
            documents.append(image);
    }

    for (QWidget *document : documents) {
        bool modified = false;
        if (auto *editor = qobject_cast<CodeEditor *>(document))
            modified = editor->isModifiedSinceLoad();
        else if (auto *image = qobject_cast<ImageEditor *>(document))
            modified = image->isModifiedSinceLoad();

        // The sidebar's delete is a gesture with a confirmation of its own, so
        // an unmodified tab goes with the file exactly as it always has. The
        // watcher's report is a background event, and unsaved work is worth
        // asking about either way.
        if (deliberate && !modified) {
            closeTab(m_tabs->indexOf(document));
            continue;
        }
        rescueDeletedDocument(document, path);
    }
}

void MainWindow::rescueDeletedDocument(QWidget *document, const QString &path)
{
    auto *editor = qobject_cast<CodeEditor *>(document);
    auto *image = qobject_cast<ImageEditor *>(document);
    if (!editor && !image)
        return;

    const bool modified = editor ? editor->isModifiedSinceLoad() : image->isModifiedSinceLoad();

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Deleted on disk"));
    box.setText(tr("%1 was deleted from disk.").arg(QFileInfo(path).fileName()));
    box.setInformativeText(
        modified
            ? tr("The tab still holds unsaved changes. Save them to a copy, keep editing "
                 "and save over the old name later, or close the tab and lose them.")
            : tr("Close the tab, save a copy somewhere else, or keep editing: a later "
                 "save writes the file again."));
    QPushButton *close = box.addButton(tr("Close tab"), QMessageBox::DestructiveRole);
    QPushButton *copy = box.addButton(tr("Save a copy…"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == close) {
        // The prompt above is the question and Close tab is the answer: closing
        // through the unsaved-changes offer would ask again, and its "Save"
        // would recreate the file the user just deleted.
        closeTab(m_tabs->indexOf(document), /*promptToSave=*/false);
        return;
    }
    if (box.clickedButton() != copy)
        return; // Cancel: the editor keeps the name it had, and recreates it on save.

    // Deliberately Save-As shaped, and the only gesture left that is: the buffer
    // stays bound to a real file, which is what makes it a document.
    const QString target = QFileDialog::getSaveFileName(
        this, tr("Save a copy"), path,
        editor ? tr("Assembly sources (*.s *.S *.asm *.x68)") : tr("PiST images (*.pim)"));
    if (target.isEmpty())
        return;
    const bool written = editor ? saveEditor(editor, target) : saveImage(image, target);
    if (!written) {
        QMessageBox::critical(this, tr("Save a copy"), tr("Could not write %1").arg(target));
        return;
    }
    updateTabTitle(document);
    updateModifiedState();
}

void MainWindow::followRenamedFile(const QString &oldPath, const QString &newPath)
{
    // The document itself: title, save target, the tab's idea of its file.
    for (CodeEditor *editor : openEditors()) {
        if (QFileInfo(editor->filePath()) != QFileInfo(oldPath))
            continue;
        editor->setFilePath(newPath);
        updateTabTitle(editor);
    }
    for (ImageEditor *image : openImages()) {
        if (QFileInfo(image->filePath()) != QFileInfo(oldPath))
            continue;
        image->setFilePath(newPath);
        updateTabTitle(image);
    }

    // The breakpoint store, keyed by base name: renaming a file would otherwise
    // strand its breakpoints under a name that no longer exists
    // (docs/untitled-plan.md §7.2).
    m_bpModel->followRenamedFile(QFileInfo(oldPath).fileName(), QFileInfo(newPath).fileName());

    // The project follows its source. The plan expects a `sourceFile` key in the
    // .pistproject; there is none — the project file is *named* after the source
    // (settings::projectFileFor), so the file itself is what has to move, or the
    // next load finds no project and the settings silently reset. That holds for
    // any project whose source is renamed, not only the one this window has
    // loaded, so it does not depend on which document is current.
    const bool wasProjectSource = QFileInfo(m_settings.sourceFile) == QFileInfo(oldPath);
    if (wasProjectSource)
        m_settings.sourceFile = newPath;

    const QString oldProject = settings::projectFileFor(oldPath);
    const QString newProject = settings::projectFileFor(newPath);
    if (QFileInfo::exists(oldProject) && QFileInfo(oldProject) != QFileInfo(newProject)
        && !QFile::rename(oldProject, newProject)) {
        statusBar()->showMessage(
            tr("%1 could not be renamed with its source; save the project to place it beside "
               "the new name.")
                .arg(QFileInfo(oldProject).fileName()),
            8000);
    }

    // The reopen pointer and the recents list both name the file, and both are
    // read back into this session (the startup reopen, the welcome view).
    settings::renameSource(oldPath, newPath);

    // Blame is read per path, so the lane has to be asked again.
    refreshBlame();
    updateModifiedState();
    syncWatchedDocuments();
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

void MainWindow::createActions()
{
    m_actOpen = new QAction(tr("&Open…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openFile);

    m_actNewFile = new QAction(tr("&New File…"), this);
    m_actNewFile->setObjectName(QStringLiteral("newFileAction"));
    m_actNewFile->setShortcut(QKeySequence::New);
    connect(m_actNewFile, &QAction::triggered, this, &MainWindow::newFile);

    m_actNewImage = new QAction(tr("New &Image…"), this);
    connect(m_actNewImage, &QAction::triggered, this, &MainWindow::newImage);

    m_actImportImage = new QAction(tr("&Import Image…"), this);
    connect(m_actImportImage, &QAction::triggered, this, &MainWindow::importImage);

    m_actExportImage = new QAction(tr("&Export Image…"), this);
    m_actExportImage->setEnabled(false);
    connect(m_actExportImage, &QAction::triggered, this, &MainWindow::exportImage);

    m_actExportImageSafe = new QAction(tr("Export &Sprite-Safe Image…"), this);
    m_actExportImageSafe->setEnabled(false);
    m_actExportImageSafe->setToolTip(
        tr("Export with colour 0 reserved for the background, so the image "
           "re-imports losslessly with colour 0 as transparent"));
    connect(m_actExportImageSafe, &QAction::triggered, this,
            &MainWindow::exportImageSpriteSafe);

    m_actExportSpriteSheet = new QAction(tr("Export Sprite &Sheet…"), this);
    m_actExportSpriteSheet->setEnabled(false);
    m_actExportSpriteSheet->setToolTip(
        tr("Compose the current phase's sheet from its placed phases"));
    connect(m_actExportSpriteSheet, &QAction::triggered, this,
            &MainWindow::exportSpriteSheet);

    m_actFind = new QAction(tr("&Find…"), this);
    m_actFind->setObjectName(QStringLiteral("findAction"));
    m_actGotoLine = new QAction(tr("&Go to Line…"), this);
    m_actGotoLine->setObjectName(QStringLiteral("gotoLineAction"));
    m_actGotoLine->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(m_actGotoLine, &QAction::triggered, this, [this] {
        if (m_editor)
            m_editor->showGotoBar();
    });

    // The symbol palette: the same candidates completion offers, presented for
    // navigation rather than for typing.
    m_actSymbolPalette = new QAction(tr("Go to &Symbol…"), this);
    m_actSymbolPalette->setObjectName(QStringLiteral("symbolPaletteAction"));
    m_actSymbolPalette->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
    connect(m_actSymbolPalette, &QAction::triggered, this, &MainWindow::showSymbolPalette);

    m_actFind->setShortcut(QKeySequence::Find);
    m_actFind->setEnabled(false);

    m_actNextDiagnostic = new QAction(tr("Next Diagnostic"), this);
    m_actNextDiagnostic->setShortcut(QKeySequence(Qt::Key_F4));
    connect(m_actNextDiagnostic, &QAction::triggered, this, &MainWindow::nextDiagnostic);

    // No shortcut here: the editor's own action owns Ctrl+Shift+E (two actions
    // with one shortcut is an ambiguity warning, not a feature).
    m_actReExportBitplanes = new QAction(tr("Re-export Bitplane Data"), this);
    m_actReExportBitplanes->setEnabled(false);
    connect(m_actReExportBitplanes, &QAction::triggered, this, [this] {
        if (m_image)
            m_image->reExportBitplaneData();
    });

    m_actPrevDiagnostic = new QAction(tr("Previous Diagnostic"), this);
    m_actPrevDiagnostic->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F4));

    m_actProfileStart = new QAction(tr("Profile &Start"), this);
    m_actProfileStart->setToolTip(tr("Start collecting CPU profile counts from here "
                                   "(works while stopped at a breakpoint)"));
    connect(m_actProfileStart, &QAction::triggered, this, &MainWindow::profileStart);

    m_actProfileStop = new QAction(tr("Profile &Stop and Show"), this);
    m_actProfileStop->setToolTip(tr("Save the profile, then show hot lines and gutter heat "
                                    "(works while stopped)"));
    connect(m_actProfileStop, &QAction::triggered, this, &MainWindow::profileStop);

    m_actProfileToCursor = new QAction(tr("Profile to &cursor line"), this);
    m_actProfileToCursor->setToolTip(
        tr("Arm a one-shot at the cursor line, collect profile counts to it, "
           "and show the results when the run stops there"));
    connect(m_actProfileToCursor, &QAction::triggered, this, &MainWindow::profileToCursor);
    // No session at startup: everything is disabled until stoppedChanged or a
    // state change says otherwise (QActions default to enabled).
    m_profiling->syncActions();
    connect(m_actPrevDiagnostic, &QAction::triggered, this, &MainWindow::previousDiagnostic);
    connect(m_actFind, &QAction::triggered, this, &MainWindow::showFindBar);

    m_actFindNext = new QAction(tr("Find &Next"), this);
    m_actFindNext->setObjectName(QStringLiteral("findNextAction"));
    m_actFindNext->setShortcut(QKeySequence(Qt::Key_F3));
    m_actFindNext->setEnabled(false);
    connect(m_actFindNext, &QAction::triggered, this, &MainWindow::findNextInEditor);

    m_actFindPrevious = new QAction(tr("Find &Previous"), this);
    m_actFindPrevious->setObjectName(QStringLiteral("findPreviousAction"));
    m_actFindPrevious->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    m_actFindPrevious->setEnabled(false);
    connect(m_actFindPrevious, &QAction::triggered, this, &MainWindow::findPreviousInEditor);

    m_actReplace = new QAction(tr("Find and &Replace…"), this);
    m_actReplace->setObjectName(QStringLiteral("replaceAction"));
    m_actReplace->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    m_actReplace->setEnabled(false);
    connect(m_actReplace, &QAction::triggered, this, &MainWindow::showReplaceBar);

    // Project-wide search: always available, because its scope is a directory
    // rather than the focused document — and the dock says so when there is not
    // one yet.
    m_actFindInFiles = new QAction(tr("Find in &Files…"), this);
    m_actFindInFiles->setObjectName(QStringLiteral("findInFilesAction"));
    m_actFindInFiles->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    connect(m_actFindInFiles, &QAction::triggered, this, &MainWindow::showFindInFiles);

    m_actExportBitplanes = new QAction(tr("Export Bitplane &Data…"), this);
    m_actExportBitplanes->setEnabled(false);
    m_actExportBitplanes->setToolTip(
        tr("Write a .dat of raw ST bitplanes to incbin: palette, sprite, masked "
           "sprite and pre-shifted copies"));
    connect(m_actExportBitplanes, &QAction::triggered, this,
            &MainWindow::exportBitplaneData);

    m_actSave = new QAction(tr("&Save"), this);
    m_actSave->setObjectName(QStringLiteral("saveAction"));
    m_actSave->setShortcut(QKeySequence::Save);
    connect(m_actSave, &QAction::triggered, this, &MainWindow::saveFile);

    // The deliberate route to the zero-document state. Until the welcome view
    // existed there was nothing to close *to*, and the tab's × button was the
    // only way out of a document at all (docs/untitled-plan.md §5).
    m_actCloseFile = new QAction(tr("&Close File"), this);
    m_actCloseFile->setObjectName(QStringLiteral("closeFileAction"));
    m_actCloseFile->setShortcut(QKeySequence::Close);
    connect(m_actCloseFile, &QAction::triggered, this, &MainWindow::closeCurrentFile);

    m_actCloseAll = new QAction(tr("Close All"), this);
    m_actCloseAll->setObjectName(QStringLiteral("closeAllAction"));
    connect(m_actCloseAll, &QAction::triggered, this, &MainWindow::closeAllFiles);

    m_actOpenProject = new QAction(tr("Open &Project…"), this);
    connect(m_actOpenProject, &QAction::triggered, this, &MainWindow::openProject);

    // The demo is the tour: it is a program that uses the features it shows off,
    // so the button opens a source and its project settings come with it. Disabled
    // when there is no demo to open (a packaged tree that does not carry one).
    m_actOpenDemo = new QAction(tr("Open &Demo Project"), this);
    m_actOpenDemo->setObjectName(QStringLiteral("demoAction"));
    m_actOpenDemo->setStatusTip(tr("Open the demo project: a tour of the IDE written in assembly"));
    m_actOpenDemo->setEnabled(!WelcomeView::demoProjects().isEmpty());
    connect(m_actOpenDemo, &QAction::triggered, this, &MainWindow::openDemoProject);

    m_actSaveProject = new QAction(tr("&Save Project"), this);
    connect(m_actSaveProject, &QAction::triggered, this, &MainWindow::saveProject);

    m_actSettings = new QAction(tr("Project &Settings…"), this);
    m_actSettings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(m_actSettings, &QAction::triggered, this, &MainWindow::editSettings);

    m_actBuild = new QAction(tr("&Build"), this);
    m_actBuild->setShortcut(QKeySequence(Qt::Key_F7));
    connect(m_actBuild, &QAction::triggered, this, &MainWindow::buildInteractive);

    m_actBuildRelease = new QAction(tr("Build &Release"), this);
    m_actBuildRelease->setToolTip(
        tr("Build with the release assembler arguments (no debug symbols)"));
    connect(m_actBuildRelease, &QAction::triggered, this,
            &MainWindow::buildReleaseInteractive);

    m_actRun = new QAction(tr("&Run"), this);
    m_actRun->setObjectName(QStringLiteral("runAction"));
    m_actRun->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_actRun, &QAction::triggered, this, &MainWindow::runInteractive);

    m_actStop = new QAction(tr("&Stop"), this);
    m_actStop->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    connect(m_actStop, &QAction::triggered, this, &MainWindow::stopSession);

    m_actPause = new QAction(tr("&Pause"), this);
    m_actPause->setShortcut(QKeySequence(Qt::Key_F6));
    m_actPause->setEnabled(false);
    connect(m_actPause, &QAction::triggered, this, &MainWindow::pauseSession);

    m_actReleaseInput = new QAction(tr("Release &Input"), this);
    m_actReleaseInput->setToolTip(
        tr("Give the keyboard and mouse back from the emulated machine "
           "(the media display's input grab; F12 does the same)"));
    m_actReleaseInput->setEnabled(false);
    connect(m_actReleaseInput, &QAction::triggered, this, [this] {
        if (m_display)
            m_display->setInputCaptured(false);
    });

    m_actStep = new QAction(tr("&Step"), this);
    m_actStep->setObjectName(QStringLiteral("stepAction"));
    m_actStep->setShortcut(QKeySequence(Qt::Key_F10));
    m_actStep->setEnabled(false);
    connect(m_actStep, &QAction::triggered, this, &MainWindow::step);

    m_actStepOver = new QAction(tr("Step &Over"), this);
    m_actStepOver->setObjectName(QStringLiteral("stepOverAction"));
    m_actStepOver->setShortcut(QKeySequence(Qt::Key_F11));
    m_actStepOver->setEnabled(false);
    connect(m_actStepOver, &QAction::triggered, this, &MainWindow::stepOver);

    m_actStepOut = new QAction(tr("Step O&ut"), this);
    m_actStepOut->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F11));
    m_actStepOut->setEnabled(false);
    connect(m_actStepOut, &QAction::triggered, this, &MainWindow::stepOut);

    m_actRunToCursor = new QAction(tr("Run to &Cursor"), this);
    m_actRunToCursor->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F10));
    m_actRunToCursor->setEnabled(false);
    connect(m_actRunToCursor, &QAction::triggered, this, &MainWindow::runToCursor);

    // Checkable, because the caret's ownership is a state the user has to be
    // able to see — the tick *is* m_pcFollow. Engaging goes to the PC now and
    // brings the caret along on every later stop; unticking is a release, the
    // same one an editing keystroke performs (cursor-plan §3.3).
    m_actFollow = new QAction(tr("Follow &Execution"), this);
    m_actFollow->setObjectName(QStringLiteral("followExecutionAction"));
    m_actFollow->setCheckable(true);
    m_actFollow->setEnabled(false);
    m_actFollow->setToolTip(tr("Move the caret with the program counter while the machine "
                              "runs and stops. Move the caret yourself and it is yours "
                              "again"));
    connect(m_actFollow, &QAction::toggled, this, [this](bool on) {
        if (on)
            followExecution();
        // The un-tick *is* a release, the same one an editing keystroke performs;
        // and an engage that found nothing to follow to (no stopped session) has
        // to undo its own tick. Either way the menu ends up showing where the
        // caret actually is rather than where the click hoped it would be.
        if (!on || !m_pcFollow)
            setPcFollow(false);
    });

    m_actToggleBreakpoint = new QAction(tr("Toggle &Breakpoint"), this);
    m_actToggleBreakpoint->setObjectName(QStringLiteral("toggleBreakpointAction"));
    m_actToggleBreakpoint->setShortcut(QKeySequence(Qt::Key_F8));
    connect(m_actToggleBreakpoint, &QAction::triggered, this,
            &MainWindow::toggleBreakpointAtCaret);

    m_actClearBreakpoints = new QAction(tr("Clear &Breakpoints"), this);
    m_actClearBreakpoints->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F9));
    connect(m_actClearBreakpoints, &QAction::triggered, this, &MainWindow::clearAllBreakpoints);

    m_actAddWatchpoint = new QAction(tr("Add &watchpoint…"), this);
    connect(m_actAddWatchpoint, &QAction::triggered, this, &MainWindow::addWatchpoint);

    m_actStopHooks = new QAction(tr("Stop &hooks…"), this);
    m_actStopHooks->setObjectName(QStringLiteral("stopHooksAction"));
    m_actStopHooks->setToolTip(
        tr("Run a script at every debugger stop: read and change memory and registers, log, "
           "and decide to continue."));
    connect(m_actStopHooks, &QAction::triggered, this, &MainWindow::editStopHooks);

    m_actResume = new QAction(tr("&Continue"), this);
    m_actResume->setObjectName(QStringLiteral("continueAction"));
    m_actResume->setShortcut(QKeySequence(Qt::Key_F9));
    m_actResume->setEnabled(false);
    connect(m_actResume, &QAction::triggered, this, &MainWindow::resume);

    // Off unless the user has asked for it: the lane is a view choice, so it
    // lives in application settings rather than the project file.
    m_actGitBlame = new QAction(tr("Git &blame"), this);
    m_actGitBlame->setObjectName(QStringLiteral("gitBlameAction"));
    m_actGitBlame->setCheckable(true);
    m_actGitBlame->setChecked(QSettings().value(gitBlameKey(), false).toBool());
    connect(m_actGitBlame, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(gitBlameKey(), on);
        applyGitBlame();
    });
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
        // The embedded panel renders no frames while stopped, so tell it to show
        // its paused hint rather than look frozen.
        if (m_display)
            m_display->setPaused(stopped);
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

void MainWindow::createMenus()
{
    // GEM's first menu was Desk; here it is the Fuji, with About PiST as its
    // only item (Desktop Info).
    auto *deskMenu = menuBar()->addMenu(appearance::atariLogoIcon(), QString());
    deskMenu->setObjectName(QStringLiteral("deskMenu"));
    deskMenu->setToolTip(tr("Atari"));
    deskMenu->menuAction()->setIcon(appearance::atariLogoIcon());
    auto *about = deskMenu->addAction(tr("About PiST"));
    about->setObjectName(QStringLiteral("aboutPistAction"));
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);

    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->setObjectName(QStringLiteral("fileMenu"));
    fileMenu->addAction(m_actNewFile);
    fileMenu->addAction(m_actNewImage);
    fileMenu->addAction(m_actOpen);
    fileMenu->addAction(m_actSave);
    fileMenu->addAction(m_actCloseFile);
    fileMenu->addAction(m_actCloseAll);
    // Rebuilt on every opening: the list is persisted, and entries whose file
    // has gone away are pruned as they are shown.
    auto *recentMenu = new QMenu(tr("Open &Recent"), this);
    recentMenu->setObjectName(QStringLiteral("openRecentMenu"));
    connect(recentMenu, &QMenu::aboutToShow, this, [this, recentMenu] {
        recentMenu->clear();
        QStringList recent = settings::recentSources();
        QStringList existing;
        for (const QString &path : recent)
            if (QFileInfo::exists(path))
                existing << path;
        for (const QString &path : existing)
            connect(recentMenu->addAction(QFileInfo(path).fileName() + QStringLiteral("    ")
                                          + path),
                    &QAction::triggered, this, [this, path] { openPath(path); });
        if (existing.isEmpty())
            recentMenu->addAction(tr("No Recent Files"))->setEnabled(false);
    });
    fileMenu->addMenu(recentMenu);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actImportImage);
    auto *exportMenu = fileMenu->addMenu(tr("E&xport"));
    exportMenu->setObjectName(QStringLiteral("exportMenu"));
    exportMenu->addAction(m_actExportImage);
    exportMenu->addAction(m_actExportImageSafe);
    exportMenu->addAction(m_actExportSpriteSheet);
    exportMenu->addAction(m_actExportBitplanes);
    exportMenu->addAction(m_actReExportBitplanes);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actOpenProject);
    fileMenu->addAction(m_actOpenDemo);
    fileMenu->addAction(m_actSaveProject);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actSettings);
    fileMenu->addSeparator();
    // Explicit QAction rather than the deprecated combined overload.
    auto *quit = new QAction(tr("E&xit"), this);
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(quit);

    m_viewMenu = menuBar()->addMenu(tr("&View"));
    m_viewMenu->setObjectName(QStringLiteral("viewMenu"));
    m_viewMenu->addAction(m_actGitBlame);

    auto *searchMenu = menuBar()->addMenu(tr("&Search"));
    searchMenu->addAction(m_actFind);
    searchMenu->addAction(m_actGotoLine);
    searchMenu->addAction(m_actSymbolPalette);
    searchMenu->addAction(m_actFindNext);
    searchMenu->addAction(m_actFindPrevious);
    searchMenu->addSeparator();
    searchMenu->addAction(m_actReplace);
    searchMenu->addSeparator();
    searchMenu->addAction(m_actFindInFiles);

    auto *buildMenu = menuBar()->addMenu(tr("&Build"));
    buildMenu->setObjectName(QStringLiteral("buildMenu"));
    buildMenu->addAction(m_actBuild);
    buildMenu->addAction(m_actBuildRelease);
    buildMenu->addSeparator();
    buildMenu->addAction(m_actNextDiagnostic);
    buildMenu->addAction(m_actPrevDiagnostic);

    auto *runMenu = menuBar()->addMenu(tr("&Run"));
    runMenu->setObjectName(QStringLiteral("runMenu"));
    runMenu->addAction(m_actRun);
    runMenu->addAction(m_actStop);
    runMenu->addAction(m_actPause);
    runMenu->addAction(m_actReleaseInput);
    runMenu->addSeparator();
    runMenu->addAction(m_actResume);
    runMenu->addAction(m_actStep);
    runMenu->addAction(m_actStepOver);
    runMenu->addAction(m_actStepOut);
    runMenu->addAction(m_actRunToCursor);
    // Next to Run to Cursor: both are about where the caret is relative to the
    // executing program, one of them for a single stop and one for the session.
    runMenu->addAction(m_actFollow);
    runMenu->addAction(m_actProfileStart);
    runMenu->addAction(m_actProfileStop);
    runMenu->addAction(m_actProfileToCursor);
    runMenu->addSeparator();
    runMenu->addAction(m_actToggleBreakpoint);
    runMenu->addAction(m_actClearBreakpoints);
    runMenu->addAction(m_actAddWatchpoint);
    runMenu->addAction(m_actStopHooks);
}

void MainWindow::showAbout()
{
    AboutDialog dialog(this);
    dialog.exec();
}

void MainWindow::showToolSetup()
{
    SetupDialog dialog(this);
    dialog.exec();
    // A fetch the dialog just completed lands somewhere discovery looks but
    // this window has not looked since construction.
    refreshToolchain();
}

QString MainWindow::assemblerPath() const
{
    return m_build->assemblerPath();
}

void MainWindow::showSetupIfNeeded()
{
    if (SetupDialog::shouldPromptAtStartup())
        showToolSetup();
}

void MainWindow::addDockToViewMenu(QDockWidget *dock)
{
    if (!m_viewMenu || !dock)
        return;
    QAction *toggle = dock->toggleViewAction();
    if (m_viewMenu->actions().contains(toggle))
        return;
    // No section yet: createDocks lists every dock that already exists once
    // the separator is in place. Inserting now would land the action above
    // that separator, next to Embed.
    QAction *before = nullptr;
    for (QAction *action : m_viewMenu->actions()) {
        if (action->objectName() == QLatin1String("viewDockSectionEnd")) {
            before = action;
            break;
        }
    }
    if (!before)
        return;
    m_viewMenu->insertAction(before, toggle);
}

QDockWidget *MainWindow::makeDock(const QString &title, const QString &objectName, QWidget *widget)
{
    auto *dock = new QDockWidget(title, this);
    // A stable objectName is what saveState/restoreState keys the arrangement on.
    dock->setObjectName(objectName);
    // Photoshop-style: the user can drag a dock between areas, tear it off into
    // a floating window, or close it (and re-show it from the View menu).
    dock->setFeatures(QDockWidget::DockWidgetMovable
                      | QDockWidget::DockWidgetFloatable
                      | QDockWidget::DockWidgetClosable);
    dock->setWidget(widget);

    // Advertise the drag surface: an open-hand cursor over the title bar. The
    // title bar is painted by the dock itself — there is no title-bar child
    // widget — so the dock's own cursor is what shows over it. Cursors inherit
    // from parent to child, so the content is given an explicit arrow to stop
    // the hand leaking into the panel's body (text views keep their own I-beam,
    // which they set on their viewport, overriding this).
    dock->setCursor(Qt::OpenHandCursor);
    widget->setCursor(Qt::ArrowCursor);

    return dock;
}

void MainWindow::showDockMoveMenu(QDockWidget *dock, const QPoint &globalPos)
{
    // Non-modal (popup) rather than exec(), so a test can drive it without a
    // nested event loop, and so it composes with the rest of the UI.
    auto *menu = new QMenu(dock);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setObjectName(QStringLiteral("dockMoveMenu"));
    menu->addAction(tr("Move to left"), this,
                    [this, dock] { addDockWidget(Qt::LeftDockWidgetArea, dock); });
    menu->addAction(tr("Move to right"), this,
                    [this, dock] { addDockWidget(Qt::RightDockWidgetArea, dock); });
    menu->addAction(tr("Move to bottom"), this,
                    [this, dock] { addDockWidget(Qt::BottomDockWidgetArea, dock); });
    menu->addSeparator();
    menu->addAction(tr("Float"), this, [dock] { dock->setFloating(true); });
    if (dock->objectName().startsWith(QLatin1String("memoryDock"))) {
        menu->addSeparator();
        menu->addAction(tr("Open another memory pane"), this, [this] { addMemoryPane(); });
    }
    menu->popup(globalPos);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // Rearranging panels has two gestures over the same targets — the dock tab
    // bars and the painted title bars:
    //   right-click -> offer a "Move to ..." menu, so moving a panel is a
    //                  discoverable choice rather than a hidden drag, and
    //   left-press  -> the possible start of a native drag.
    const QEvent::Type type = event->type();

    // A dock tab group's tab bar gets an open-hand cursor to advertise that a
    // tab can be dragged to rearrange it or tear the panel out. Only the dock
    // tab bars (QMainWindowTabBar) — not a content QTabWidget's tab bar.
    if (type == QEvent::Show || type == QEvent::Enter) {
        if (auto *tabBar = qobject_cast<QTabBar *>(watched);
            tabBar
            && QLatin1String(tabBar->metaObject()->className())
                   == QLatin1String("QMainWindowTabBar")) {
            tabBar->setCursor(Qt::OpenHandCursor);
            // The panel name is a dock title whether it is painted as a title
            // bar or as one of these tabs. Document tabs stay at the regular
            // weight; these are the names of the panels.
            QFont font = tabBar->font();
            if (!font.bold()) {
                font.setBold(true);
                tabBar->setFont(font);
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    // Column headers are left-aligned everywhere: a stretched column's title
    // should hug its content, not float centred in the middle of a wide column
    // (which is what put "Instruction" far from the instructions).
    if (type == QEvent::Polish) {
        if (auto *header = qobject_cast<QHeaderView *>(watched))
            header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        return QMainWindow::eventFilter(watched, event);
    }
    // The hover card: a short reference for what the pointer is over, resolved the
    // same way the dock resolves the caret. Resolving nothing shows nothing and
    // does *not* swallow the event, so a widget's own tooltip still gets its turn.
    if (type == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        auto *hovered = qobject_cast<QWidget *>(watched);
        CodeEditor *editor = qobject_cast<CodeEditor *>(hovered);
        if (!editor && hovered)
            editor = qobject_cast<CodeEditor *>(hovered->parentWidget());
        if (editor) {
            const QPoint pos = editor->viewport()->mapFromGlobal(help->globalPos());
            const QString card = referenceCardAt(editor, pos);
            if (!card.isEmpty()) {
                QToolTip::showText(help->globalPos(), card, editor);
                return true;
            }
        }
        return QMainWindow::eventFilter(watched, event);
    }

    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return QMainWindow::eventFilter(watched, event);

    auto *me = static_cast<QMouseEvent *>(event);

    // The session chip's click: it is a QLabel, so the gesture that hands the
    // caret to the program counter arrives as a filtered event rather than as a
    // signal (cursor-plan §3.3). Qt hands the release to the widget the press
    // started on even when the pointer has left it, which is why the position
    // is checked: a press on the chip that is dragged away and released
    // elsewhere must not engage.
    if (watched == m_statusSession && type == QEvent::MouseButtonRelease
        && me->button() == Qt::LeftButton
        && m_statusSession->rect().contains(me->position().toPoint())) {
        followExecution();
        return true;
    }

    if (type == QEvent::MouseButtonRelease)
        return QMainWindow::eventFilter(watched, event);

    auto *w = qobject_cast<QWidget *>(watched);

    // Ctrl+click in an editor: an include directive opens its target (resolved
    // against the current file's directory and the project's include paths);
    // any other word jumps to its label definition in the same document.
    if (type == QEvent::MouseButtonPress && me->button() == Qt::LeftButton
        && me->modifiers().testFlag(Qt::ControlModifier)) {
        if (auto *editor = qobject_cast<CodeEditor *>(w ? w->parentWidget() : nullptr)) {
            const QTextCursor cursor = editor->cursorForPosition(me->position().toPoint());
            const QString lineText = cursor.block().text();
            const QString include = includeTargetAt(lineText);
            if (!include.isEmpty()) {
                const QString target = resolveInclude(
                    include, QFileInfo(editor->filePath()).absolutePath(),
                    m_settings.includePaths);
                if (!target.isEmpty())
                    openPath(target);
                else
                    statusBar()->showMessage(tr("Include not found: %1").arg(include), 5000);
                return true;
            }
            const QString word = wordAtCursor(lineText, cursor.positionInBlock());
            if (!word.isEmpty()) {
                const int line = labelLine(editor->toPlainText(), word);
                if (line > 0) {
                    editor->gotoLine(line);
                    return true;
                }
            }
        }
    }
    QDockWidget *dock = w ? dockAtPress(w, me->globalPosition().toPoint()) : nullptr;
    if (!dock)
        return QMainWindow::eventFilter(watched, event);

    if (me->button() == Qt::RightButton) {
        showDockMoveMenu(dock, me->globalPosition().toPoint());
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

QDockWidget *MainWindow::dockAtPress(QWidget *pressed, const QPoint &globalPos)
{
    // A tab of a tabbed dock group. The tab bar belongs to the dock area, not
    // to the dock, so the dock is found by matching the tab's text to a dock's
    // title. A tab bar owning no dock's title is a content tab widget (the
    // Output pane's Problems/console tabs), not a dock group, and is ignored.
    if (auto *tabBar = qobject_cast<QTabBar *>(pressed)) {
        const int index = tabBar->tabAt(tabBar->mapFromGlobal(globalPos));
        if (index < 0)
            return nullptr;
        const QString title = tabBar->tabText(index);
        for (QDockWidget *dock : findChildren<QDockWidget *>())
            if (dock->windowTitle() == title)
                return dock;
        return nullptr;
    }

    // A painted title bar: the press is inside a dock but outside its content
    // widget. (The default title bar is drawn by the dock itself, so there is
    // no title-bar child widget to walk up to.)
    for (QWidget *p = pressed; p; p = p->parentWidget()) {
        if (auto *dock = qobject_cast<QDockWidget *>(p)) {
            QWidget *content = dock->widget();
            const bool onContent = content
                && (pressed == content || content->isAncestorOf(pressed));
            return onContent ? nullptr : dock;
        }
    }
    return nullptr;
}


void MainWindow::createDocks()
{
    // Photoshop-style panels: any dock can be nested beside another in an area,
    // and any dock can be dragged onto another to tab them together. The default
    // arrangement below groups the debug views, and the whole arrangement is
    // persisted and restored across runs. Three passes, because the order
    // *within* each is free while the order between them is not: every panel has
    // to exist before anything is wired to it, and the View menu is built from
    // the docks that exist.
    buildPanels();
    wirePanels();
    buildViewMenu();
}

void MainWindow::buildPanels()
{
    // Photoshop-style panels: any dock can be nested beside another in an area,
    // and any dock can be dragged onto another to tab them together. The default
    // arrangement below groups the debug views, and the whole arrangement is
    // persisted and restored across runs.
    setDockNestingEnabled(true);
    qApp->installEventFilter(this);
    setObjectName(QStringLiteral("mainWindow"));

    QList<QDockWidget *> debugTabs;

    // --- left: project navigation --------------------------------------------
    // One group, in this order: Project files, Find in Files, Git. Project
    // files stays on top. Find in Files is present on a first run (Search ▸
    // Find in Files raises it); it is not a debug dock, so the factory hide
    // loop leaves the group here. Sprite hides the whole group.
    m_fileBrowser = new FileBrowser(this);
    auto *projectDock =
        makeDock(tr("Project files"), QStringLiteral("projectFilesDock"), m_fileBrowser);
    addDockWidget(Qt::LeftDockWidgetArea, projectDock);

    m_findInFiles = new FindInFilesView(this);
    m_findInFiles->navigate = [this](const QString &path, int line, int column, int length) {
        if (!navigateToSourceLine(path, line))
            return;
        // The caret is on the line now, and marking the match itself is the
        // rest of what "take me to that hit" means.
        if (m_editor)
            m_editor->selectSpan(line, column, length);
    };
    m_findInFilesDock =
        makeDock(tr("Find in Files"), QStringLiteral("findInFilesDock"), m_findInFiles);
    addDockWidget(Qt::LeftDockWidgetArea, m_findInFilesDock);
    tabifyDockWidget(projectDock, m_findInFilesDock);

    m_gitPanel = new GitPanel(this);
    m_gitDock = makeDock(tr("Git"), QStringLiteral("gitDock"), m_gitPanel);
    addDockWidget(Qt::LeftDockWidgetArea, m_gitDock);
    tabifyDockWidget(projectDock, m_gitDock);
    projectDock->raise();

    // --- right, top: the emulator display, which wants to be prominent --------
    // Hidden until a media session engages it (docs/PLAN.md §12): with no
    // frames to paint it would be an unexplained black rectangle.
    m_display = new EmulatorDisplayWidget(this);
    m_displayDock = makeDock(tr("Emulator"), QStringLiteral("emulatorDisplayDock"), m_display);
    m_displayDock->setVisible(false);
    // The input grab (docs/PLAN.md §12.4): the panel translates, the backend
    // forwards. Keys arrive as ST scancodes already (ui/StKeyboard); mouse
    // deltas arrive scaled to guest pixels.
    if (m_display) {
        connect(m_display, &EmulatorDisplayWidget::keyIntent, this,
                [this](int scancode, bool down) {
                    m_host->mediaKey(quint8(scancode), down);
                });
        connect(m_display, &EmulatorDisplayWidget::mouseIntent, this,
                [this](qint16 dx, qint16 dy, quint8 buttons) {
                    m_host->mediaMouse(dx, dy, buttons);
                });
        // A focus change releases the grab: the user Alt-Tabbing away must
        // not leave their keyboard held by a window they can't see.
        connect(qApp, &QApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state) {
                    if (state != Qt::ApplicationActive && m_display)
                        m_display->setInputCaptured(false);
                });
        connect(m_display, &EmulatorDisplayWidget::inputCaptureChanged, this,
                [this](bool captured) {
                    if (m_actReleaseInput)
                        m_actReleaseInput->setEnabled(captured);
                });
    }

    m_profiler = new ProfilerView(this);
    m_profiler->setActions(m_actProfileStart, m_actProfileStop, m_actProfileToCursor);
    addDockWidget(Qt::RightDockWidgetArea, m_displayDock);

    // --- right, below the display: the debug views, tabbed together -----------
    m_registers = new RegistersView(this);
    m_disassembly = new DisassemblyView(this);
    m_stack = new StackView(this);
    m_hardware = new HardwareView(this);
    m_breakpointPanel = new BreakpointPanel(this);

    m_pcHistory = new PcHistoryView(this);
    m_instrRef = new InstructionRefView(this);
    m_symbolsView = new SymbolsView(this);
    debugTabs << makeDock(tr("Registers"), QStringLiteral("registersDock"), m_registers)
              << makeDock(tr("Disassembly"), QStringLiteral("disassemblyDock"), m_disassembly)
              << makeDock(tr("Stack"), QStringLiteral("stackDock"), m_stack)
              << makeDock(tr("Hardware"), QStringLiteral("hardwareDock"), m_hardware)
              << makeDock(tr("PC history"), QStringLiteral("pcHistoryDock"), m_pcHistory)
              << makeDock(tr("Breakpoints"), QStringLiteral("breakpointsDock"), m_breakpointPanel)
              << makeDock(tr("Instructions"), QStringLiteral("instructionRefDock"), m_instrRef)
              << makeDock(tr("Symbols"), QStringLiteral("symbolsDock"), m_symbolsView)
              << (m_profilerDock = makeDock(tr("Profiler"), QStringLiteral("profilerDock"),
                                            m_profiler));

    addDockWidget(Qt::RightDockWidgetArea, debugTabs.first());
    for (int i = 1; i < debugTabs.size(); ++i)
        tabifyDockWidget(debugTabs.first(), debugTabs.at(i));
    // tabifyDockWidget leaves the last dock on top, which made a first run
    // open on Profiler. Registers is the tab the debug group should show.
    debugTabs.first()->raise();
    // Vertical split: the emulator display goes on top of the debug tab group,
    // where a media session shows it. It starts hidden; the split decides where
    // it lands when one engages it.
    splitDockWidget(m_displayDock, debugTabs.first(), Qt::Vertical);
    m_displayDock->raise();
    debugTabs.first()->raise();

    // --- bottom: problems, console and memory, tabbed -------------------------
    // Problems and the console are ordinary docks rather than tabs in a
    // QTabWidget inside one dock, so the bottom area is a normal tab group: you
    // can drag panels into it and out of it like any other, and the Move-to
    // menu works on their tabs.
    m_problems = new QTreeWidget(this);
    m_problems->setHeaderLabels({tr("File"), tr("Line"), tr("Message")});
    m_problems->header()->setStretchLastSection(true);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    appearance::markMono(m_log);

    m_problemsDock = makeDock(tr("Problems"), QStringLiteral("problemsDock"), m_problems);
    addDockWidget(Qt::BottomDockWidgetArea, m_problemsDock);

    // A command entry under the log turns the console into a live debugger
    // console: commands go through the backend's normal queue and the response
    // is appended when it arrives (matched by command text in the
    // commandFinished handler in wireBackend).
    m_consoleInput = new ConsoleInput(this);
    m_consoleInput->setObjectName(QStringLiteral("consoleInput"));
    appearance::markMono(m_consoleInput);
    m_consoleInput->setPlaceholderText(
        tr("Debugger command (e.g. r, d, m $12596 20)"));
    m_consoleInput->setToolTip(
        tr("r registers, d disassemble, m address length, b breakpoint."));
    m_consoleInput->setEnabled(false);
    // The command verbs are always completable; symbol names join the
    // candidate list when a build's listings are parsed (rebuildProgramMap).
    m_consoleVerbs = {QStringLiteral("r"), QStringLiteral("d"), QStringLiteral("m"),
                      QStringLiteral("w"), QStringLiteral("l"), QStringLiteral("s"),
                      QStringLiteral("n"), QStringLiteral("c"), QStringLiteral("b"),
                      QStringLiteral("info"), QStringLiteral("symbols"),
                      QStringLiteral("profile"), QStringLiteral("setopt"),
                      QStringLiteral("help"), QStringLiteral("quit")};
    m_consoleInput->setCompletions(m_consoleVerbs);

    auto *consoleWidget = new QWidget(this);
    auto *consoleLayout = new QVBoxLayout(consoleWidget);
    consoleLayout->setContentsMargins(0, 0, 0, 0);
    consoleLayout->setSpacing(0);
    consoleLayout->addWidget(m_log);
    consoleLayout->addWidget(m_consoleInput);

    auto *consoleDock =
        makeDock(tr("Build & debug console"), QStringLiteral("consoleDock"), consoleWidget);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock);
    tabifyDockWidget(m_problemsDock, consoleDock);

    // The first memory pane (tag 0) tabs onto this group; more can be added
    // from its "+" button.
    addMemoryPane();
}

void MainWindow::wirePanels()
{
    connect(m_fileBrowser, &FileBrowser::fileActivated, this, &MainWindow::openPath);
    connect(m_fileBrowser, &FileBrowser::floppyEntryActivated, this,
            [this](int drive, const QString &entryPath) { openFloppyEntry(drive, entryPath); });
    connect(m_fileBrowser, &FileBrowser::newImageRequested, this, &MainWindow::newImageIn);
    connect(m_fileBrowser, &FileBrowser::floppyImageChanged, this,
            [this](int drive, const QString &path) {
                while (m_settings.floppyImages.size() <= drive)
                    m_settings.floppyImages.append(QString());
                if (drive >= 0 && drive < m_settings.floppyImages.size())
                    m_settings.floppyImages[drive] = path;
                persistSettings();
                if (m_host)
                    m_host->setFloppyImage(drive, path);
            });
    connect(m_fileBrowser, &FileBrowser::newFileRequested, this, &MainWindow::newFileIn);
    connect(m_fileBrowser, &FileBrowser::pathRenamed, this, &MainWindow::followRenamedFile);
    connect(m_fileBrowser, &FileBrowser::pathDeleted, this,
            [this](const QString &path) { documentVanished(path, /*deliberate=*/true); });

    connect(m_gitDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible && m_gitPanel)
            m_gitPanel->refresh();
    });
    connect(m_gitPanel, &GitPanel::repositoryChanged, this, [this](bool, const QString &) {
        applyGitBlame();
    });
    connect(m_gitPanel, &GitPanel::blameReady, this,
            [this](const QString &file, const GitBlameMap &lines) {
                for (CodeEditor *editor : openEditors()) {
                    if (QFileInfo(editor->filePath()) == QFileInfo(file))
                        editor->setBlame(lines);
                }
            });

    // The reference dock's "insert binding" gesture drops the call's
    // canonical binding into the current source, above the cursor's line, as
    // one undo step. The placeholder parameter names are the user's to edit.
    connect(m_instrRef, &InstructionRefView::osCallInsertRequested, this,
            [this](int trap, int opcode) {
                if (!m_editor)
                    return; // the current tab is not a source editor
                const OsCallInfo *info = osCallRef(trap, opcode);
                if (!info)
                    return;
                QTextCursor cursor = m_editor->textCursor();
                cursor.beginEditBlock();
                cursor.movePosition(QTextCursor::StartOfBlock);
                cursor.insertText(osCallBinding(*info));
                cursor.endEditBlock();
                m_editor->setFocus();
            });
    connect(m_profiler, &ProfilerView::lineActivated, this, [this](int line) {
        if (m_editor)
            m_editor->gotoLine(line);
    });

    connect(m_breakpointPanel, &BreakpointPanel::removeRequested,
            this, &MainWindow::removeBreakpoint);
    connect(m_breakpointPanel, &BreakpointPanel::breakpointActivated,
            this, &MainWindow::goToBreakpoint);
    connect(m_symbolsView, &SymbolsView::symbolActivated,
            this, &MainWindow::goToBreakpoint);
    connect(m_breakpointPanel, &BreakpointPanel::clearRequested,
            this, &MainWindow::clearAllDebugTargets);
    connect(m_breakpointPanel, &BreakpointPanel::watchpointRemoveRequested,
            this, &MainWindow::removeWatchpoint);
    connect(m_breakpointPanel, &BreakpointPanel::watchpointActivated,
            this, [this](quint32 address) {
                if (m_memory) {
                    // The pane is tabbed with Problems/console: navigation to
                    // an invisible tab would look like the click did nothing.
                    if (m_memoryDock) {
                        m_memoryDock->show();
                        m_memoryDock->raise();
                    }
                    m_memory->goToAddress(address);
                }
            });
    // Following a pointer or a slot's address on the stack goes to the first
    // memory pane, same as a watchpoint activation.
    connect(m_stack, &StackView::addressActivated, this, [this](quint32 address) {
        if (m_memory) {
            if (m_memoryDock) {
                m_memoryDock->show();
                m_memoryDock->raise();
            }
            m_memory->goToAddress(address);
        }
    });
    // A disassembly row opens its source line when the program map knows it,
    // and does nothing when the address is unmapped. PC history does the same,
    // and falls through to the memory pane the way the stack does.
    auto openMappedSource = [this](quint32 address) -> bool {
        if (!m_bases.isValid() || m_programMap.isEmpty())
            return false;
        LineMap::Address loc;
        if (!m_programMap.lineFor(address, &loc))
            return false;
        navigateToSourceLine(loc.file, loc.line);
        return true;
    };
    auto openMemory = [this](quint32 address) {
        if (!m_memory)
            return;
        if (m_memoryDock) {
            m_memoryDock->show();
            m_memoryDock->raise();
        }
        m_memory->goToAddress(address);
    };
    connect(m_disassembly, &DisassemblyView::addressActivated, this,
            [openMappedSource](quint32 address) { openMappedSource(address); });
    connect(m_pcHistory, &PcHistoryView::addressActivated, this,
            [openMappedSource, openMemory](quint32 address) {
                if (!openMappedSource(address))
                    openMemory(address);
            });
    connect(m_hardware, &HardwareView::subjectChanged, this,
            [this](const QString &subject) {
                if (m_host->isRunning())
                    m_host->infoSubject(subject);
            });

    // A register edit becomes a debugger write: `r <reg>=<value>` (the '=' is
    // mandatory in Hatari, and it prints nothing on success). Only sent when
    // stopped, which is when editing is enabled anyway.
    connect(m_registers, &RegistersView::registerEdited, this,
            [this](const QString &regName, quint32 value) {
                setRegister(regName, value);
            });

    connect(m_problems, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (const RemoteStateAdapter::ProblemRow *entry = problemEntryFor(item))
            navigateToSourceLine(entry->file, entry->line);
    });

    connect(m_consoleInput, &QLineEdit::returnPressed, this, [this] {
        const QString cmd = m_consoleInput->text().trimmed();
        if (cmd.isEmpty())
            return;
        m_consoleInput->clear();
        sendConsoleCommand(cmd);
    });
}

void MainWindow::buildViewMenu()
{
    // --- default arrangement captured, then the user's arrangement restored ---
    // The View menu gets one show/hide action per dock, plus a way back to the
    // default layout. Built here rather than in createMenus because the docks do
    // not exist yet when the menus are made. The list is every dock the window
    // owns, not a hand-written subset: a dock forgotten by that subset can be
    // closed and never reopened (Reset layout is the only other way back, and
    // it discards the rest of the arrangement).
    if (m_viewMenu) {
        m_viewMenu->addSeparator();
        // Toggles are inserted before this separator, so Reset layout stays last
        // and a dock created later (another memory pane) joins the same group.
        auto *sectionEnd = m_viewMenu->addSeparator();
        sectionEnd->setObjectName(QStringLiteral("viewDockSectionEnd"));
        auto *layoutMenu = m_viewMenu->addMenu(tr("Layout"));
        layoutMenu->setObjectName(QStringLiteral("layoutMenu"));
        const struct {
            const char *id;
            const char *title;
        } presets[] = {
            {"Editing", QT_TR_NOOP("Editing")},
            {"Debugging", QT_TR_NOOP("Debugging")},
            {"Sprite", QT_TR_NOOP("Sprite")},
        };
        for (const auto &preset : presets) {
            auto *action = layoutMenu->addAction(tr(preset.title));
            action->setObjectName(QStringLiteral("layout") + QLatin1String(preset.id));
            const QString id = QString::fromLatin1(preset.id);
            connect(action, &QAction::triggered, this, [this, id] { applyLayoutPreset(id); });
        }
        m_restoreLayoutAction = layoutMenu->addAction(tr("Restore my layout"), this,
                                                      &MainWindow::restorePreviousLayout);
        m_restoreLayoutAction->setObjectName(QStringLiteral("restoreMyLayoutAction"));
        m_restoreLayoutAction->setEnabled(false);

        auto *reset = m_viewMenu->addAction(tr("Reset layout"), this,
                                            &MainWindow::resetToDefaultLayout);
        reset->setObjectName(QStringLiteral("resetLayoutAction"));

        // Preferred order is only the reading order. Anything not named here
        // is still listed, after these, so a new dock cannot miss the menu
        // by being left out of the list.
        const QStringList preferred = {
            QStringLiteral("projectFilesDock"),
            QStringLiteral("findInFilesDock"),
            QStringLiteral("gitDock"),
            QStringLiteral("emulatorDisplayDock"),
            QStringLiteral("registersDock"),
            QStringLiteral("disassemblyDock"),
            QStringLiteral("stackDock"),
            QStringLiteral("hardwareDock"),
            QStringLiteral("pcHistoryDock"),
            QStringLiteral("breakpointsDock"),
            QStringLiteral("instructionRefDock"),
            QStringLiteral("symbolsDock"),
            QStringLiteral("profilerDock"),
            QStringLiteral("problemsDock"),
            QStringLiteral("consoleDock"),
            QStringLiteral("memoryDock"),
        };
        const QList<QDockWidget *> found = findChildren<QDockWidget *>();
        for (const QString &name : preferred) {
            for (QDockWidget *dock : found) {
                if (dock->objectName() == name)
                    addDockToViewMenu(dock);
            }
        }
        QList<QDockWidget *> rest;
        for (QDockWidget *dock : found) {
            if (!preferred.contains(dock->objectName()))
                rest.append(dock);
        }
        std::sort(rest.begin(), rest.end(), [](const QDockWidget *a, const QDockWidget *b) {
            return a->objectName() < b->objectName();
        });
        for (QDockWidget *dock : rest)
            addDockToViewMenu(dock);
    }

    // A first run is the Editing arrangement: the source fills the window, and
    // the debug docks stay on the View menu until a session needs them. Reset
    // layout captures this state. The Emulator dock is not one of them — the
    // embed checkbox still owns it.
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        if (isDebugDockName(dock->objectName()))
            dock->hide();
    }
    if (m_problemsDock)
        m_problemsDock->raise();
}

void MainWindow::addMemoryPane(quint32 initialAddress)
{
    const int tag = m_nextMemoryTag++;
    auto *view = new MemoryView(this);
    m_memoryPanes.insert(tag, view);

    // The first pane keeps the plain "Memory" title and the m_memory pointer
    // (the auto-refresh and first-stop navigation use it), and its dock is the
    // base the others tab with. Later panes are numbered and tab onto it.
    const bool first = (tag == 0);
    auto *dock = makeDock(first ? tr("Memory") : tr("Memory %1").arg(tag + 1),
                          first ? QStringLiteral("memoryDock")
                                : QStringLiteral("memoryDock%1").arg(tag),
                          view);
    if (first) {
        m_memory = view;
        m_memoryDock = dock;
        addDockWidget(Qt::BottomDockWidgetArea, dock);
        // Tab the base memory dock with Problems/console so the bottom stays one group.
        tabifyDockWidget(m_problemsDock, dock);
    }

    // Route this pane's requests with its tag, so its dumps come back to it and
    // not to a sibling pane watching a different region.
    connect(view, &MemoryView::dumpRequested, this,
            [this, tag](quint32 address, int length) {
                m_host->requestMemoryDump(address, length, tag);
            });
    connect(view, &MemoryView::memoryEdited, this,
            [this, view](quint32 address, quint32 value) { setMemoryByte(address, value, view); });
    connect(view, &MemoryView::addPaneRequested, this, [this] { addMemoryPane(); });

    // Later panes tab onto the base memory dock.
    if (!first)
        tabifyDockWidget(m_memoryDock, dock);

    view->setEditingEnabled(m_host->isStopped());
    // A pane opened from the "+" is a dock the user can close. It has to be
    // on the View menu or that close is permanent until Reset layout.
    addDockToViewMenu(dock);
    if (m_host->isStopped()) {
        if (initialAddress)
            view->goToAddress(initialAddress);
        else
            view->refresh();
    }
}

void MainWindow::applyFactoryDockSizes()
{
    // 1280-wide window: left ~268 (Project files shares it with Find in Files,
    // which needs a query and a path), right ~280, so the editor keeps about
    // half once the splitter handles are counted. The bottom group is a strip,
    // not a second editor.
    auto *files = findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    auto *registers = findChild<QDockWidget *>(QStringLiteral("registersDock"));
    if (files && registers)
        resizeDocks({files, registers}, {268, 280}, Qt::Horizontal);
    // The bottom group keeps its size hint, which is already a short strip.
    // Pinning it taller is recorded by saveState, and restoring that pin
    // crashes, so Reset layout could not survive the force.
}

void MainWindow::finalizeLayout()
{
    const QByteArray savedLayout =
        QSettings().value(layoutStateKey()).toByteArray();
    const QByteArray savedGeometry =
        QSettings().value(layoutGeometryKey()).toByteArray();

    // Geometry is restored last. restoreState changes the window size, and
    // the saved geometry is the size the user actually had.
    if (savedGeometry.isEmpty())
        resize(1280, 860);

    // Dock sizes set before the window is on screen do not stick, so a first
    // run applies them from showEvent and recaptures the factory state there.
    // A saved arrangement is restored now and left alone.
    m_defaultLayoutState = saveState();
    m_applyFactorySizes = savedLayout.isEmpty();
    if (!savedLayout.isEmpty())
        restoreState(savedLayout);
    if (!savedGeometry.isEmpty())
        restoreGeometry(savedGeometry);
    const int savedWidth = QSettings().value(layoutWidthKey()).toInt();
    const int savedHeight = QSettings().value(layoutHeightKey()).toInt();
    if (savedWidth >= 640 && savedHeight >= 400)
        m_restoredSize = QSize(savedWidth, savedHeight);

}

void MainWindow::resetToDefaultLayout()
{
    if (m_defaultLayoutState.isEmpty())
        return;
    restoreState(m_defaultLayoutState);
}

void MainWindow::applyLayoutPreset(const QString &preset)
{
    const bool editing = preset == QLatin1String("Editing");
    const bool debugging = preset == QLatin1String("Debugging");
    const bool sprite = preset == QLatin1String("Sprite");
    if (!editing && !debugging && !sprite)
        return;

    QSettings().setValue(layoutPreviousKey(), saveState());
    if (m_restoreLayoutAction)
        m_restoreLayoutAction->setEnabled(true);

    auto dockNamed = [this](const QString &name) -> QDockWidget * {
        return findChild<QDockWidget *>(name);
    };

    // Project files, Find in Files and Git share one tab. Sprite puts the
    // group away; Editing and Debugging bring it back with Project files on
    // top, so neither of the other two becomes the left pane just because it
    // was shown last.
    QDockWidget *project = dockNamed(QStringLiteral("projectFilesDock"));
    QDockWidget *find = dockNamed(QStringLiteral("findInFilesDock"));
    QDockWidget *git = dockNamed(QStringLiteral("gitDock"));
    QList<QDockWidget *> projectGroup;
    if (find)
        projectGroup << find;
    if (git)
        projectGroup << git;
    if (project)
        projectGroup << project;
    if (sprite) {
        // Background tabs first. Hiding the current tab last is what drops
        // the group's tab bar.
        for (QDockWidget *dock : projectGroup)
            if (!dock->isVisible())
                dock->hide();
        for (QDockWidget *dock : projectGroup)
            if (dock->isVisible())
                dock->hide();
    } else if (project) {
        project->show();
        if (find) {
            find->show();
            tabifyDockWidget(project, find);
        }
        if (git) {
            git->show();
            tabifyDockWidget(project, git);
        }
        project->raise();
    }
    if (QDockWidget *problems = dockNamed(QStringLiteral("problemsDock")))
        problems->setVisible(!sprite);

    QDockWidget *console = dockNamed(QStringLiteral("consoleDock"));
    if (console)
        console->setVisible(!sprite);

    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        if (!isDebugDockName(dock->objectName()))
            continue;
        dock->setVisible(debugging);
        if (debugging && console)
            tabifyDockWidget(console, dock);
    }
    // Debugging is the preset that wants the picture: the display dock joins
    // the right area and shows — a media session paints it, and before one it
    // says what it is.
    if (debugging && m_displayDock) {
        addDockWidget(Qt::RightDockWidgetArea, m_displayDock);
        m_displayDock->setVisible(true);
    }
    if (debugging) {
        if (QDockWidget *regs = dockNamed(QStringLiteral("registersDock")))
            regs->raise();
    }

    if (m_displayDock && !debugging)
        m_displayDock->setVisible(false);
}

void MainWindow::restorePreviousLayout()
{
    const QByteArray previous = QSettings().value(layoutPreviousKey()).toByteArray();
    if (previous.isEmpty())
        return;
    restoreState(previous);
    QSettings().remove(layoutPreviousKey());
    if (m_restoreLayoutAction)
        m_restoreLayoutAction->setEnabled(false);
}

void MainWindow::createToolBar()
{
    auto *bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setIconSize(QSize(22, 22));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setFloatable(false);
    bar->addAction(m_actOpen);
    bar->addAction(m_actOpenDemo);
    bar->addAction(m_actSave);
    bar->addAction(m_actSettings);
    bar->addSeparator();
    bar->addAction(m_actBuild);
    // The Build button is a split button: clicking it builds with the debug
    // arguments (F7), and its arrow offers the Release build too — one icon, no
    // second near-identical one to tell apart.
    if (auto *buildButton = qobject_cast<QToolButton *>(bar->widgetForAction(m_actBuild))) {
        auto *buildChoices = new QMenu(buildButton);
        buildChoices->addAction(m_actBuild);
        buildChoices->addAction(m_actBuildRelease);
        buildButton->setMenu(buildChoices);
        buildButton->setPopupMode(QToolButton::MenuButtonPopup);
    }
    bar->addAction(m_actRun);
    bar->addAction(m_actStop);
    bar->addAction(m_actPause);
    bar->addSeparator();
    bar->addAction(m_actResume);
    bar->addAction(m_actStep);
    bar->addAction(m_actStepOver);
    bar->addAction(m_actStepOut);
    bar->addSeparator();
    bar->addAction(m_actClearBreakpoints);

    refreshToolbarStatusTips();
}

void MainWindow::refreshToolbarStatusTips()
{
    auto *bar = findChild<QToolBar *>(QStringLiteral("mainToolBar"));
    if (!bar)
        return;
    // Icon-only buttons do not show their shortcut. The status bar does,
    // while the pointer is on the button.
    for (QAction *action : bar->actions()) {
        QString text = action->text();
        text.remove(QLatin1Char('&'));
        if (text.isEmpty())
            continue;
        const QString key = action->shortcut().toString(QKeySequence::NativeText);
        if (!key.isEmpty())
            text += QStringLiteral(" (") + key + QLatin1Char(')');
        action->setStatusTip(text);
    }
}

void MainWindow::applyShortcutScheme()
{
    const bool common = appearance::shortcutScheme() == QLatin1String("common");
    m_actBuild->setShortcut(QKeySequence(Qt::Key_F7));
    m_actStepOut->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F11));
    m_actRunToCursor->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F10));
    m_actClearBreakpoints->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F9));
    if (common) {
        m_actStep->setShortcut(QKeySequence(Qt::Key_F11));
        m_actStepOver->setShortcut(QKeySequence(Qt::Key_F10));
        m_actToggleBreakpoint->setShortcut(QKeySequence(Qt::Key_F9));
    } else {
        m_actStep->setShortcut(QKeySequence(Qt::Key_F10));
        m_actStepOver->setShortcut(QKeySequence(Qt::Key_F11));
        m_actToggleBreakpoint->setShortcut(QKeySequence(Qt::Key_F8));
        m_actRun->setShortcut(QKeySequence(Qt::Key_F5));
        m_actResume->setShortcut(QKeySequence(Qt::Key_F9));
    }
    updateRunContinueShortcut();
    refreshToolbarStatusTips();
    if (m_breakpointPanel && m_actToggleBreakpoint) {
        const QString key =
            m_actToggleBreakpoint->shortcut().toString(QKeySequence::NativeText);
        m_breakpointPanel->setEmptyHint(
            key.isEmpty()
                ? tr("Click the gutter to set a breakpoint.")
                : tr("Click the gutter to set a breakpoint. %1 toggles the current line.")
                      .arg(key));
    }
}

void MainWindow::updateRunContinueShortcut()
{
    // Common gives F5 to whichever of Run and Continue should answer. They
    // must not both hold it: Qt drops an ambiguous shortcut.
    if (appearance::shortcutScheme() != QLatin1String("common"))
        return;
    if (!m_actRun || !m_actResume)
        return;
    const bool stopped = m_host && m_host->isStopped();
    m_actRun->setShortcut(stopped ? QKeySequence() : QKeySequence(Qt::Key_F5));
    m_actResume->setShortcut(stopped ? QKeySequence(Qt::Key_F5) : QKeySequence());
}

void MainWindow::toggleBreakpointAtCaret()
{
    if (!m_editor)
        return;
    toggleBreakpointAtLine(m_editor->textCursor().blockNumber() + 1);
}

void MainWindow::applyIcons()
{
    using appearance::Icon;
    m_actOpen->setIcon(appearance::icon(Icon::Open));
    // The Atari logo, the same mark the Desk menu uses: the demo is the project's
    // own sample, and a second folder glyph beside Open… would read as the same
    // button twice.
    m_actOpenDemo->setIcon(appearance::atariLogoIcon());
    m_actSave->setIcon(appearance::icon(Icon::Save));
    m_actSettings->setIcon(appearance::icon(Icon::Settings));
    m_actBuild->setIcon(appearance::icon(Icon::Build));
    m_actRun->setIcon(appearance::icon(Icon::Run));
    m_actStop->setIcon(appearance::icon(Icon::Stop));
    m_actPause->setIcon(appearance::icon(Icon::Pause));
    m_actResume->setIcon(appearance::icon(Icon::Continue));
    m_actStep->setIcon(appearance::icon(Icon::Step));
    m_actStepOver->setIcon(appearance::icon(Icon::StepOver));
    m_actStepOut->setIcon(appearance::icon(Icon::StepOut));
    m_actClearBreakpoints->setIcon(appearance::icon(Icon::ClearBreakpoints));
    m_actProfileStart->setIcon(appearance::icon(Icon::ProfileStart));
    m_actProfileStop->setIcon(appearance::icon(Icon::ProfileStop));
    m_actProfileToCursor->setIcon(appearance::icon(Icon::ProfileToCursor));
    if (auto *desk = findChild<QMenu *>(QStringLiteral("deskMenu")))
        desk->setIcon(appearance::atariLogoIcon());
}

void MainWindow::applyAppearance()
{
    applyShortcutScheme();
    appearance::applyTheme();
    applyAudioOutputDevice();
    setWindowIcon(appearance::windowIcon());
    applyIcons();
    appearance::applyMonoFonts(this);
    if (m_instrStrip) {
        m_instrStrip->setStyleSheet(QStringLiteral(
            "QPushButton#instructionStrip {"
            " text-align: left; padding: 1px 8px; border: none;"
            " border-top: 1px solid palette(mid); }"));
        m_instrStrip->setFixedHeight(m_instrStrip->fontMetrics().height() + 6);
    }
    for (CodeEditor *editor : openEditors())
        editor->setTheme(appearance::editorTheme());
    for (ImageEditor *image : openImages())
        image->applyAppearance();
    if (m_registers)
        m_registers->applyAppearance();
    if (m_disassembly)
        m_disassembly->applyAppearance();
    if (m_stack)
        m_stack->applyAppearance();
    if (m_breakpointPanel)
        m_breakpointPanel->applyAppearance();
    for (MemoryView *view : m_memoryPanes)
        view->applyAppearance();
    if (m_instrRef)
        m_instrRef->applyAppearance();
    if (m_symbolsView)
        m_symbolsView->applyAppearance();
    if (m_profiler)
        m_profiler->applyAppearance();
    if (m_welcome)
        m_welcome->applyAppearance();
}

void MainWindow::applyAudioOutputDevice()
{
    // The audio output device is an application-wide preference (the media
    // channel's sound, phase 3). Only a stored id that resolves to a real
    // device changes anything: an empty or vanished id must leave the
    // system default alone — a default-constructed QAudioDevice is a *null*
    // device in Qt6, and handing one to the sink opens no stream at all.
    if (!m_audio)
        return;
    const QByteArray wanted =
        QSettings().value(QLatin1String(kAudioOutputDeviceKey)).toByteArray();
    if (wanted.isEmpty())
        return;
    const QList<QAudioDevice> devices = QMediaDevices::audioOutputs();
    for (const QAudioDevice &d : devices)
        if (d.id() == wanted) {
            m_audio->setOutputDevice(d);
            return;
        }
    // A stored device that no longer exists: fall back to the real default.
    m_audio->setOutputDevice(QMediaDevices::defaultAudioOutput());
}

void MainWindow::createStatusBar()
{
    // Permanent widgets are laid out from the right, so the first one added
    // is the rightmost. The session is the one a glance should land on.
    m_statusSession = new QLabel(tr("Not running"), this);
    m_statusSession->setObjectName(QStringLiteral("statusSession"));
    // One of the three engage affordances (cursor-plan §3.3). A QLabel has no
    // clicked signal, so the click is handled in eventFilter; the hand cursor
    // and the hint updateSessionChip appends to the tooltip are what make it
    // discoverable.
    m_statusSession->setCursor(Qt::PointingHandCursor);
    m_statusBuild = new QLabel(this);
    m_statusBuild->setObjectName(QStringLiteral("statusBuild"));
    m_statusToolchain = new QLabel(this);
    m_statusToolchain->setObjectName(QStringLiteral("statusToolchain"));
    m_statusCaret = new QLabel(this);
    m_statusCaret->setObjectName(QStringLiteral("statusCaret"));
    statusBar()->addPermanentWidget(m_statusSession);
    statusBar()->addPermanentWidget(m_statusBuild);
    statusBar()->addPermanentWidget(m_statusToolchain);
    statusBar()->addPermanentWidget(m_statusCaret);
}

void MainWindow::updateSessionChip()
{
    if (!m_statusSession)
        return;
    // The capability probe used to be the label. It reads as a fault
    // ("control socket: NO") and says nothing about the session, so it stays
    // available as the tooltip of the chip that does.
    m_statusSession->setToolTip(m_caps.summary());
    if (!m_host || !m_host->isRunning()) {
        m_statusSession->setText(tr("Not running"));
        return;
    }
    if (!m_host->isStopped()) {
        m_statusSession->setText(tr("Running"));
        return;
    }
    // Stopped is the only state in which the chip's click does anything — it
    // hands the caret to the program counter (cursor-plan §3.3) — so the hint
    // lives here, and a chip that would refuse the click does not advertise it.
    m_statusSession->setToolTip(m_caps.summary() + QStringLiteral("\n\n")
                                + tr("Click to follow the program counter with the caret"));
    if (m_stoppedLine > 0 && !m_stoppedFile.isEmpty())
        m_statusSession->setText(tr("Stopped — %1:%2").arg(m_stoppedFile).arg(m_stoppedLine));
    else
        m_statusSession->setText(tr("Stopped"));
}

void MainWindow::updateCaretChip()
{
    if (!m_statusCaret)
        return;
    if (!m_editor) {
        m_statusCaret->clear();
        return;
    }
    const QTextCursor cursor = m_editor->textCursor();
    const int line = cursor.blockNumber() + 1;
    const int column = cursor.positionInBlock() + 1;
    const QString name = m_editor->displayName();
    const int execution = m_editor->currentExecutionLine();
    const bool stopped = m_host && m_host->isRunning() && m_host->isStopped();
    if (stopped && execution > 0 && execution != line)
        m_statusCaret->setText(tr("%1: caret %2 · PC %3").arg(name).arg(line).arg(execution));
    else
        m_statusCaret->setText(QStringLiteral("%1:%2:%3").arg(name).arg(line).arg(column));
}

void MainWindow::updateRegisterStrip()
{
    if (!m_registerStrip)
        return;
    const bool show = m_host && m_host->isRunning() && m_host->isStopped()
                      && m_lastState.regs.valid;
    m_registerStrip->setVisible(show);
    if (!show)
        return;
    const Registers &r = m_lastState.regs;
    auto word = [](quint32 value) {
        return hex::hex32(value);
    };
    QString data;
    QString addr;
    for (int i = 0; i < 8; ++i) {
        data += QStringLiteral("D%1 %2 ").arg(i).arg(word(r.d[i]));
        addr += QStringLiteral("A%1 %2 ").arg(i).arg(word(r.a[i]));
    }
    auto mark = [](bool set) { return set ? QLatin1Char('1') : QLatin1Char('-'); };
    const QString flags = QStringLiteral("X%1 N%2 Z%3 V%4 C%5")
                              .arg(mark(r.flagX))
                              .arg(mark(r.flagN))
                              .arg(mark(r.flagZ))
                              .arg(mark(r.flagV))
                              .arg(mark(r.flagC));
    const QString text = data.trimmed() + QLatin1Char('\n')
                       + addr.trimmed()
                       + QStringLiteral("   PC %1  %2").arg(word(m_lastState.pc), flags);
    m_registerStrip->setText(text);
    m_registerStrip->setToolTip(text);
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

void MainWindow::updateModifiedState()
{
    QString name = QStringLiteral("PiST");
    bool modified = false;
    if (m_editor) {
        name = m_editor->displayName();
        modified = m_editor->isModifiedSinceLoad();
    } else if (m_image) {
        name = m_image->displayName();
        modified = m_image->isModifiedSinceLoad();
    }

    // The "[*]" placeholder is replaced by Qt when windowModified is set, which
    // is the platform-correct way to show an unsaved document.
    setWindowTitle(tr("%1[*] — PiST").arg(name));
    setWindowModified(modified);

    if (m_actSave)
        m_actSave->setEnabled(modified || m_image != nullptr || m_editor != nullptr);
    if (m_actCloseFile)
        m_actCloseFile->setEnabled(m_tabs->count() > 0);
    if (m_actCloseAll)
        m_actCloseAll->setEnabled(m_tabs->count() > 0);
}

bool MainWindow::maybeSave()
{
    for (int i = 0; i < m_tabs->count(); ++i) {
        QWidget *widget = m_tabs->widget(i);
        m_tabs->setCurrentWidget(widget);
        if (auto *editor = qobject_cast<CodeEditor *>(widget)) {
            if (!maybeSaveEditor(editor))
                return false;
        } else if (auto *image = qobject_cast<ImageEditor *>(widget)) {
            if (!maybeSaveImage(image))
                return false;
        }
    }
    return true;
}

bool MainWindow::maybeSaveEditor(CodeEditor *editor)
{
    if (!editor || !editor->isModifiedSinceLoad())
        return true;

    const auto answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has unsaved changes.\n\nSave before continuing?")
            .arg(editor->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;

    // Save means the document's own path: a source document always has one, and
    // there is no longer any way to be holding a buffer without one
    // (docs/untitled-plan.md §8).
    Q_ASSERT(!editor->filePath().isEmpty());
    if (!saveEditor(editor, editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(editor->filePath(), editor->lastError()));
        return false;
    }

    updateTabTitle(editor);
    updateModifiedState();
    return true;
}

bool MainWindow::maybeSaveImage(ImageEditor *editor)
{
    if (!editor || !editor->isModifiedSinceLoad())
        return true;

    const auto answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has unsaved changes.\n\nSave before continuing?")
            .arg(editor->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;

    if (editor->filePath().isEmpty()) {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save image"), QString(), tr("PiST images (*.pim)"));
        if (path.isEmpty())
            return false;
        if (!saveImage(editor, path)) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2").arg(path, editor->lastError()));
            return false;
        }
    } else if (!saveImage(editor, editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(editor->filePath(), editor->lastError()));
        return false;
    }

    updateTabTitle(editor);
    updateModifiedState();
    return true;
}

void MainWindow::openRecentSource()
{
    // Offered when there is nothing open, so a restart lands somewhere useful
    // rather than on an empty editor.
    const QString last = settings::lastSourcePath();
    if (!last.isEmpty() && QFileInfo::exists(last))
        openPath(last);
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open"), QString(),
        tr("PiST files (*.s *.S *.asm *.x68 *.pim *.pi1 *.PI1 *.neo *.NEO *.iff *.png);;"
           "Assembly sources (*.s *.S *.asm *.x68);;"
           "PiST images (*.pim);;"
           "Atari ST images (*.pi1 *.PI1 *.neo *.NEO *.iff);;"
           "All files (*)"));
    if (path.isEmpty())
        return;
    openPath(path);
}

bool MainWindow::openPath(const QString &path)
{
    return openPathImpl(path, /*quiet=*/false);
}

bool MainWindow::openPathQuiet(const QString &path)
{
    return openPathImpl(path, /*quiet=*/true);
}

bool MainWindow::openPathImpl(const QString &path, bool quiet)
{
    if (path.isEmpty())
        return false;

    // Already open documents are raised, not reloaded: the user's undo
    // history and cursor position in the existing tab are kept.
    bool created = false;
    if (CodeEditor *open = editorForPath(path)) {
        m_tabs->setCurrentWidget(open);
    } else if (ImageEditor *open = imageForPath(path)) {
        m_tabs->setCurrentWidget(open);
    } else if (isPimPath(path) || isImportableImagePath(path)) {
        if (!addImageTab(path, quiet))
            return false;
        created = true;
    } else {
        if (!addEditorTab(path, quiet))
            return false;
        created = true;
    }

    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    // The lifetime counter counts documents the user opened, not the ones an
    // extraction or a project open created behind the scenes
    // (docs/untitled-plan.md §6).
    if (created)
        settings::noteFileOpened();
    if (!isPimPath(path) && !isImportableImagePath(path))
        loadProjectForSource(path);
    updateModifiedState();

    if (m_fileBrowser)
        m_fileBrowser->showFor(path);
    // The opened file is what decides the project directory, and raising the
    // document that is already current emits no tab change at all, so the
    // per-tab pass in onTabChanged cannot be relied on here. Both calls are
    // idempotent (MIN-81).
    syncGitDirectory();
    syncFindInFilesScope();
    applyGitBlame();
    return true;
}

QString MainWindow::extractedFloppyPath(const QString &imagePath, const QString &entryPath) const
{
    // Deterministic, so reopening the same entry raises its existing tab. The
    // image is identified by a content hash of its path, so two disks called
    // disk.st from different folders never share an extracted file.
    const QString imageKey = QString::fromLatin1(
        QCryptographicHash::hash(imagePath.toUtf8(), QCryptographicHash::Sha1)
            .toHex()
            .left(8));
    const QString name = QFileInfo(imagePath).completeBaseName();
    // Outside the session directory: pruneStaleSessions() deletes those on age
    // alone, and this file backs an open editor tab that writes back to the
    // image (paths::documentExtractDir()).
    const QString dir = QDir(paths::documentExtractDir())
                            .absoluteFilePath(QStringLiteral("%1-%2").arg(name, imageKey));
    return QDir(dir).absoluteFilePath(QString(entryPath).replace(QLatin1Char('/'),
                                                                 QLatin1Char('_')));
}

QWidget *MainWindow::openFloppyEntry(int drive, const QString &entryPath)
{
    const QString imagePath = m_fileBrowser ? m_fileBrowser->floppyImages().value(drive)
                                            : QString();
    if (imagePath.isEmpty() || entryPath.isEmpty())
        return nullptr;

    // Already open documents are raised, not reloaded: the extracted file is
    // deterministic per image entry, so the existing tab is found by path.
    const QString extracted = extractedFloppyPath(imagePath, entryPath);
    if (CodeEditor *open = editorForPath(extracted)) {
        m_tabs->setCurrentWidget(open);
        return open;
    }

    QString error;
    QByteArray raw;
    if (!floppy::loadRaw(imagePath, &raw, &error)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Could not read %1:\n%2")
                                 .arg(QFileInfo(imagePath).fileName(), error));
        return nullptr;
    }
    QByteArray data;
    if (!floppy::readFileRaw(raw, entryPath, &data, &error)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Could not read %1 from %2:\n%3")
                                 .arg(entryPath, QFileInfo(imagePath).fileName(), error));
        return nullptr;
    }

    const QString suffix = QFileInfo(entryPath).suffix().toLower();
    // A still image opens as an image tab over its extracted file: the
    // 320×200 sheet maps 1:1 onto the file inside the image, so saving
    // writes the same format straight back.
    const bool stillImage = suffixIsStillImage(suffix);
    if (QStringLiteral("s") == suffix || QStringLiteral("asm") == suffix) {
        QMessageBox::information(
            this, tr("Open"),
            tr("Assembly sources are built from the hard drive, so copy %1 out of the "
               "disk first (Copy on the disk, Paste on the hard drive).").arg(entryPath));
        return nullptr;
    }
    if (!stillImage) {
        const bool editable = suffixIsEditableText(suffix)
            || (!suffixIsKnownBinary(suffix) && !data.contains('\0'));
        if (!editable) {
            QMessageBox::information(this, tr("Open"),
                                     tr("%1 is not a text document.").arg(entryPath));
            return nullptr;
        }
    }

    if (!paths::ensureDirectory(QFileInfo(extracted).absolutePath(), &error)) {
        QMessageBox::warning(this, tr("Open"), error);
        return nullptr;
    }
    // Through the shared rule, and finished — bytes on disk, temporary renamed
    // away — before the editor tab below opens the file. An extraction replaces
    // whatever a previous one left at that path, so a write that cannot reach
    // the disk must leave that file alone instead of truncating it.
    if (!files::write(extracted, data, &error)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Could not write %1").arg(extracted));
        return nullptr;
    }

    QWidget *tab = stillImage ? static_cast<QWidget *>(addImageTab(extracted))
                              : addEditorTab(extracted);
    if (!tab)
        return nullptr;
    FloppyDoc doc;
    doc.imagePath = QFileInfo(imagePath).absoluteFilePath();
    doc.entryPath = entryPath;
    m_floppyDocs.insert(QFileInfo(extracted).absoluteFilePath(), doc);
    const int index = m_tabs->indexOf(tab);
    m_tabs->setTabToolTip(index, tr("%1 — edited inside %2")
                                    .arg(entryPath, QFileInfo(imagePath).fileName()));
    return tab;
}

void MainWindow::writeBackFloppyDoc(const QString &path)
{
    const auto it = m_floppyDocs.constFind(QFileInfo(path).absoluteFilePath());
    if (it == m_floppyDocs.constEnd())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = file.readAll();

    // Replace, not add: the entry exists on the disk, and updateImage would
    // otherwise give the saved copy a numbered name beside the original.
    floppy::Item item;
    item.destPath = it->entryPath;
    item.data = data;
    QString error;
    // Saving rewrites the whole image, so a disk that is not already PiST's
    // canonical layout is asked about first — the same policy the file
    // browser's copy and move paths use, and the reason it lives there.
    if (m_fileBrowser && !m_fileBrowser->confirmFloppyRewrite(it->imagePath))
        return;
    if (!floppy::updateImage(it->imagePath, {item}, {it->entryPath}, &error)) {
        QMessageBox::warning(this, tr("Save"),
                             tr("The text was saved to the session copy, but could not be "
                                "written back into %1:\n%2")
                                 .arg(QFileInfo(it->imagePath).fileName(), error));
        return;
    }
    // Refresh the panes in place from the browser's own mounted images: the
    // settings mirror is only updated by the Change/Eject flows.
    if (m_fileBrowser)
        m_fileBrowser->refreshFloppyImages();
    statusBar()->showMessage(tr("Saved %1 into %2")
                                 .arg(it->entryPath, QFileInfo(it->imagePath).fileName()),
                             4000);
}

void MainWindow::openDemoIfIdle()
{
    // Nothing is open: no file was given on the command line and the last document
    // is gone (or there never was one). The demo is what a first run should land
    // on — and only a first run does: the next launch reopens whatever was last
    // opened. Never insisted on, and inert when no demo is reachable.
    if (m_tabs->count() > 0 || !m_actOpenDemo || !m_actOpenDemo->isEnabled())
        return;
    openDemoProject();
}

void MainWindow::openDemoProject()
{
    const QList<WelcomeView::Entry> demos = WelcomeView::demoProjects();
    if (demos.isEmpty()) {
        // The action is disabled when the demos are absent, so reaching here means
        // the tree moved under a running window: say so rather than open nothing.
        statusBar()->showMessage(
            tr("No demo project was found — the demos travel with the source tree."), 8000);
        return;
    }

    // The entries are project files; what a demo *is* is the source beside them.
    // `hello` is the tour, so it wins over whatever order the list comes in.
    QString project = demos.first().path;
    for (const WelcomeView::Entry &demo : demos) {
        if (QFileInfo(demo.path).completeBaseName() == QLatin1String("hello")) {
            project = demo.path;
            break;
        }
    }

    const QString source = sourceForProject(project);
    if (source.isEmpty()) {
        statusBar()->showMessage(tr("%1 has no source beside it — the demo may have moved.")
                                     .arg(QFileInfo(project).fileName()), 8000);
        return;
    }
    openPath(source);
}

void MainWindow::openProject()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open project"), QString(),
        tr("PiST projects (*%1);;All files (*)")
            .arg(QLatin1String(settings::kProjectSuffix)));
    if (path.isEmpty())
        return;

    ProjectSettings loaded;
    QString error;
    if (!settings::load(&loaded, path, &error)) {
        QMessageBox::critical(this, tr("Open project"), error);
        return;
    }

    // The source path is not stored in the file — it is implied by the project
    // file's location — so derive it and load it. Picking a file through the
    // "All files" filter gives a name without the suffix, and truncating that
    // blindly would produce an arbitrary path.
    QString source = settings::lastSourcePath();
    QString implied;
    if (path.endsWith(QLatin1String(settings::kProjectSuffix), Qt::CaseInsensitive)) {
        implied = path.left(path.size() - QLatin1String(settings::kProjectSuffix).size())
                + QStringLiteral(".s");
    }
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
    settings::rememberLastSource(source);
    syncFileBrowserDisks();
    statusBar()->showMessage(tr("Opened project %1").arg(QFileInfo(path).fileName()), 5000);
    m_log->appendPlainText(tr("[project] loaded %1").arg(path));
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

void MainWindow::loadProjectForSource(const QString &sourcePath)
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

void MainWindow::saveFile()
{
    // Save is unconditionally "write to the path this document already has"
    // (docs/untitled-plan.md §8). The branch that used to fall into a Save-As
    // dialog for a path-less buffer is gone with the buffer — it was also where
    // a failed write left a document quietly path-less while the user believed
    // the save had happened.
    if (m_image) {
        if (m_image->filePath().isEmpty()) {
            // The one document that can still have no path: an image imported
            // before it had a file of its own. That asymmetry is deliberate
            // (docs/untitled-plan.md §9).
            const QString path = QFileDialog::getSaveFileName(
                this, tr("Save image"), QString(), tr("PiST images (*.pim)"));
            if (path.isEmpty())
                return;
            if (!saveImage(m_image, path)) {
                QMessageBox::critical(this, tr("Save"),
                                      tr("Could not write %1: %2")
                                          .arg(path, m_image->lastError()));
                return;
            }
            statusBar()->showMessage(tr("Saved %1").arg(path), 4000);
        } else if (!saveImage(m_image, m_image->filePath())) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2")
                                      .arg(m_image->filePath(), m_image->lastError()));
        } else {
            statusBar()->showMessage(tr("Saved %1").arg(m_image->filePath()), 4000);
        }
        updateTabTitle(m_image);
        updateModifiedState();
        return;
    }

    if (!m_editor)
        return;
    Q_ASSERT(!m_editor->filePath().isEmpty());
    if (!saveEditor(m_editor, m_editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(m_editor->filePath(), m_editor->lastError()));
    }
    updateTabTitle(m_editor);
    updateModifiedState();
    // The status lists the file on disk, so a save is what moves a row from
    // Changes to clean. Blame reads the buffer, which this save just matched.
    if (m_gitPanel)
        m_gitPanel->refresh();
    refreshBlame();
}

QString MainWindow::buildSourcePath() const
{
    // What you see is what you build: the focused editor's file is the source,
    // full stop. The fallbacks below serve the two states in which there is no
    // focused *editor* — an image tab is current, or nothing is open — and they
    // name real files, where the phantom buffer could only be skipped silently
    // (docs/untitled-plan.md §2).
    if (m_editor)
        return m_editor->filePath();
    if (!m_settings.sourceFile.isEmpty() && QFileInfo::exists(m_settings.sourceFile))
        return m_settings.sourceFile;
    const QList<CodeEditor *> editors = openEditors();
    if (!editors.isEmpty())
        return editors.first()->filePath();
    return {};
}

void MainWindow::newImage()
{
    newImageIn(suggestedImageDirectory());
}

QString MainWindow::suggestedImageDirectory() const
{
    auto dirOf = [](const QString &path) -> QString {
        return path.isEmpty() ? QString() : QFileInfo(path).absolutePath();
    };
    QString dir;
    if (m_image)
        dir = dirOf(m_image->filePath());
    if (dir.isEmpty() && m_editor)
        dir = dirOf(m_editor->filePath());
    if (dir.isEmpty() && m_fileBrowser)
        dir = m_fileBrowser->directory();
    if (dir.isEmpty())
        dir = dirOf(settings::lastSourcePath());
    if (dir.isEmpty())
        dir = QDir::homePath();
    return dir;
}

void MainWindow::newImageIn(const QString &directory)
{
    NewImageDialog dialog(this, directory);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString path = dialog.filePath();
    if (path.isEmpty())
        return;

    ImageDocument doc = ImageDocument::create(dialog.imageWidth(), dialog.imageHeight(),
                                              dialog.paletteKind());
    QString error;
    if (!doc.save(path, &error)) {
        QMessageBox::warning(this, tr("New Image"),
                             tr("Could not create %1: %2").arg(path, error));
        return;
    }

    if (ImageEditor *open = imageForPath(path)) {
        if (!open->loadFile(path)) {
            QMessageBox::warning(this, tr("New Image"),
                                 tr("Could not open %1: %2").arg(path, open->lastError()));
            return;
        }
        m_tabs->setCurrentWidget(open);
        updateTabTitle(open);
        updateModifiedState();
        return;
    }
    if (!addImageTab(path))
        return;
    if (m_fileBrowser)
        m_fileBrowser->showFor(path);
}

void MainWindow::importImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Import image"), QString(),
        tr("Atari ST images (*.pi1 *.PI1 *.neo *.NEO *.iff *.png);;All files (*)"));
    if (path.isEmpty())
        return;

    if (m_image) {
        QMessageBox box(this);
        box.setWindowTitle(tr("Import image"));
        box.setText(tr("Add as a new frame, or replace this image?"));
        QPushButton *add = box.addButton(tr("Add frame"), QMessageBox::AcceptRole);
        QPushButton *replace = box.addButton(tr("Replace"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.exec();
        if (box.clickedButton() != add && box.clickedButton() != replace)
            return;
        if (!m_image->importFile(path, box.clickedButton() == add)) {
            QMessageBox::warning(this, tr("Import"),
                                 tr("Could not import %1: %2").arg(path, m_image->lastError()));
            return;
        }
        updateTabTitle(m_image);
        updateModifiedState();
        return;
    }

    openPath(path);
}

void MainWindow::showFindBar()
{
    if (m_editor)
        m_editor->showFindBar(false);
}

void MainWindow::showReplaceBar()
{
    if (m_editor)
        m_editor->showFindBar(true);
}

void MainWindow::findNextInEditor()
{
    if (m_editor)
        m_editor->findNext();
}

void MainWindow::findPreviousInEditor()
{
    if (m_editor)
        m_editor->findPrevious();
}

void MainWindow::exportImage()
{
    exportImageWith(false);
}

void MainWindow::exportImageSpriteSafe()
{
    exportImageWith(true);
}

void MainWindow::exportImageWith(bool spriteSafe)
{
    if (!m_image)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export image"), QString(),
        tr("Degas Elite (*.pi1);;NeoChrome (*.neo);;IFF/ILBM (*.iff);;PNG (*.png);;"
           "STOS sprite bank (*.mbk);;Assembler include (*.s);;Bitplane binary (*.bin)"));
    if (path.isEmpty())
        return;
    if (!m_image->exportFile(path, spriteSafe)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Could not export %1: %2").arg(path, m_image->lastError()));
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(path), 4000);
}

void MainWindow::exportSpriteSheet()
{
    if (!m_image)
        return;
    const int sheet = m_image->currentSheetIndex();
    if (sheet < 0) {
        QMessageBox::information(this, tr("Export sprite sheet"),
                                 tr("This image has no sprite sheets. Import one first."));
        return;
    }
    const QString suggested = m_image->document().sheets().at(sheet).path;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export sprite sheet"), suggested,
        tr("Degas Elite (*.pi1);;NeoChrome (*.neo);;IFF/ILBM (*.iff);;PNG (*.png);;"
           "Assembler include (*.s);;Bitplane binary (*.bin)"));
    if (path.isEmpty())
        return;
    exportSpriteSheetTo(path);
}

bool MainWindow::exportSpriteSheetTo(const QString &path)
{
    if (!m_image)
        return false;
    const int sheet = m_image->currentSheetIndex();
    if (sheet < 0) {
        QMessageBox::information(this, tr("Export sprite sheet"),
                                 tr("This image has no sprite sheets. Import one first."));
        return false;
    }
    if (!m_image->exportSheetFile(path, sheet, false)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Could not export %1: %2").arg(path, m_image->lastError()));
        return false;
    }
    // When the sheet came off a mounted floppy, the recomposition belongs
    // back in the disk image.
    writeBackFloppyDoc(path);
    statusBar()->showMessage(tr("Exported %1").arg(path), 4000);
    return true;
}

void MainWindow::exportBitplaneData()
{
    if (!m_image)
        return;
    const ImageDocument &doc = m_image->document();
    QVector<BitplaneExportPhase> phases;
    for (const ImagePhase &phase : doc.phases()) {
        phases.append(BitplaneExportPhase{phase.name, phase.cellW, phase.cellH,
                                          int(phase.frames.size())});
    }
    BitplaneExportDialog dialog(phases, doc.currentPhase(), doc.paletteKind(), doc.active(),
                                stColourIndex(doc.background(), doc.active()), this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export bitplane data"), QString(), tr("Bitplane data (*.dat)"));
    if (path.isEmpty())
        return;

    const BitplaneDataOptions options = dialog.options();
    const int phase = dialog.phase();

    // The scroller goes beside the .dat, taking the .dat's name, so it is not a
    // file the user picked in the dialog — and a `.s` that is already there is
    // somebody's source. Ask before it goes.
    const QFileInfo info(path);
    QString scroller;
    const bool wantsScroller = dialog.writesScrollDemo();
    if (wantsScroller) {
        scroller = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                   + QStringLiteral(".s");
        if (QFileInfo::exists(scroller)
            && QMessageBox::question(this, tr("Export"),
                                     tr("%1 already exists. Overwrite it with the scroller?")
                                         .arg(QFileInfo(scroller).fileName()),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                   != QMessageBox::Yes) {
            scroller.clear(); // the .dat still goes out; that file is left alone
        }
    }

    if (!m_image->exportBitplaneFile(path, phase, options)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Could not export %1: %2").arg(path, m_image->lastError()));
        return;
    }
    if (!scroller.isEmpty()
        && !m_image->exportScrollDemoFile(scroller, phase, options, info.fileName())) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Wrote %1, but could not write the scroller %2: %3")
                                 .arg(info.fileName(), scroller, m_image->lastError()));
        return;
    }

    // Remember the pair for Ctrl+Shift+E (re-export), scroller included; an
    // empty scroller records "no scroller", so a declined overwrite is not
    // re-asked on every re-export.
    m_image->setBitplaneExportScroller(scroller);
    if (m_actReExportBitplanes)
        m_actReExportBitplanes->setEnabled(m_image->canReExportBitplane());

    // The blob carries no offsets of its own, and the source needs them for its
    // `equ`s — so leave the map where it can still be read after the dialog
    // closes. `bitplaneLayout()` is what the encoder walked, so these are the
    // real offsets.
    const QVector<BitplaneBlock> blocks =
        bitplaneLayout(doc.phases().at(phase).cellW, doc.phases().at(phase).cellH,
                       doc.phases().at(phase).frames.size(), options);
    if (m_log) {
        m_log->appendPlainText(tr("--- bitplane data: %1 ---").arg(info.fileName()));
        for (const BitplaneBlock &block : blocks)
            m_log->appendPlainText(QStringLiteral("%1 equ $%2")
                                       .arg(block.name, -20)
                                       .arg(block.offset, 4, 16, QLatin1Char('0')));
        m_log->appendPlainText(tr("total %1 bytes").arg(blocks.last().offset + blocks.last().bytes));
        if (!scroller.isEmpty())
            m_log->appendPlainText(tr("%1 — press F7 to assemble it and watch this sprite scroll")
                                       .arg(QFileInfo(scroller).fileName()));
        else if (wantsScroller)
            m_log->appendPlainText(tr("no scroller written — %1 was left as it is")
                                       .arg(info.completeBaseName() + QStringLiteral(".s")));
    }
    statusBar()->showMessage(scroller.isEmpty()
                                 ? tr("Exported %1").arg(info.fileName())
                                 : tr("Exported %1 and %2").arg(info.fileName(), scroller),
                             4000);
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
