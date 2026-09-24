// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
#include "emu/HexFormat.h"
#include "support/FileWrite.h"

#include "ui/BreakpointWatchpointModel.h"
#include "ui/DebugSessionController.h"
#include "ui/ProfilerController.h"
#include "ui/RemoteStateAdapter.h"
#include "ui/SessionLauncher.h"
#include "ui/UiText.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/DebugBackend.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"
#include "ui/InstructionRefView.h"
#include "build/FloppyImage.h"
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
#include "ui/EmbedX11.h"
#include "ui/FileBrowser.h"
#include "ui/GitPanel.h"
#include "ui/ImageEditor.h"
#include "ui/NewImageDialog.h"
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
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

bool isPristineEditor(CodeEditor *editor)
{
    return editor && editor->filePath().isEmpty() && !editor->isModifiedSinceLoad()
        && editor->document()->isEmpty();
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
const QString &embeddedDisplayKey()
{
    static const QString key = QStringLiteral("display/embedded");
    return key;
}

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
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
    sessionHost.bases = [this]() -> const LineMap::SectionBases & { return m_bases; };
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
    // The embedded display container, realized and shown only once the session
    // is known to be able to embed one: winId() has to be a live native window
    // before Hatari starts, or there is nothing to reparent into.
    launcherHost.embedDisplayWindowId = [this](const HatariCapabilities &caps) -> QString {
        if (!(m_embeddedDisplay && canEmbedDisplay(caps) && m_display))
            return QString();
        m_displayDock->setVisible(true);
        m_display->setVisible(true);
#ifdef Q_OS_WIN
        // Windows gets no window id in the environment. Hatari's reparenting is
        // compiled in only under X11 upstream, while the PARENT_WIN_ID check
        // that creates its SDL window *hidden* is not guarded at all — so
        // naming our window there would leave the user with no emulator on
        // screen if the adoption then failed. The container adopts Hatari's own
        // window once the process is running instead (runningChanged, below),
        // where every failure path ends at the detached window that works today.
        return QString();
#else
        return QString::number(m_display->winId());
#endif
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
    auto *center = new QWidget(this);
    auto *column = new QVBoxLayout(center);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    column->addWidget(m_tabs, 1);
    m_instrStrip = new QPushButton(center);
    m_instrStrip->setObjectName(QStringLiteral("instructionStrip"));
    m_instrStrip->setFlat(true);
    m_instrStrip->setFocusPolicy(Qt::NoFocus);
    m_instrStrip->setCursor(Qt::PointingHandCursor);
    m_instrStrip->setToolTip(tr("Open the instruction reference"));
    appearance::markMono(m_instrStrip);
    connect(m_instrStrip, &QPushButton::clicked, this, &MainWindow::raiseInstructionRef);
    column->addWidget(m_instrStrip);
    m_registerStrip = new QLabel(center);
    m_registerStrip->setObjectName(QStringLiteral("registerStrip"));
    m_registerStrip->setVisible(false);
    appearance::markMono(m_registerStrip);
    column->addWidget(m_registerStrip);
    setCentralWidget(center);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::onTabCloseRequested);

    // Every session starts with one pristine editor tab; opening a file reuses
    // it while it is still untouched.
    addEditorTab(QString());

    m_build = new BuildService(this);
    connect(m_build, &BuildService::finished, this, &MainWindow::onBuildFinished);
    connect(m_build, &BuildService::outputLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });

    m_host = createBackend(BackendKind::Native, this);
    wireBackend();

    // Restore the display preference before any dock is created, so the dock's
    // initial visibility matches it. With no stored choice the default is
    // embedded — a docked display is the point of the IDE — and the probe below
    // turns that default back off where the platform cannot embed, without
    // storing anything, so a first run on macOS or Wayland-without-XWayland
    // does not open with a panel that can never fill.
    m_embeddedDisplayChosen = QSettings().contains(embeddedDisplayKey());
    m_embeddedDisplay = QSettings().value(embeddedDisplayKey(), true).toBool();

    createActions();
    createMenus();
    createDocks();
    createToolBar();
    createStatusBar();
    applyAppearance();

    // Sessions left by a crash or a kill, once they are old enough that no live
    // instance could still own them.
    paths::pruneStaleSessions();

    // Reopen where the user left off when nothing was passed on the command line.
    QTimer::singleShot(0, this, [this] {
        if (m_editor && m_editor->filePath().isEmpty() && !m_image)
            openRecentSource();
    });

    refreshToolchain();

    updateModifiedState();
    // The actions exist only now, and the pristine tab was added before them —
    // so run the per-tab pass once, or Find would stay greyed out until the
    // first tab switch.
    onTabChanged(m_tabs->currentIndex());
    finalizeLayout();
}

CodeEditor *MainWindow::addEditorTab(const QString &path, bool quiet)
{
    // A pristine tab (no path, no content, unmodified) is reused rather than
    // left behind as an empty first tab, matching how editors treat an
    // untouched "untitled" buffer.
    if (!path.isEmpty() && m_tabs->count() == 1) {
        auto *only = qobject_cast<CodeEditor *>(m_tabs->widget(0));
        if (isPristineEditor(only)) {
            if (!only->loadFile(path)) {
                if (!quiet)
                    QMessageBox::warning(this, tr("Open"), tr("Could not open %1").arg(path));
                return nullptr;
            }
            updateTabTitle(only);
            return only;
        }
    }

    auto *editor = new CodeEditor(appearance::editorTheme(), this);
    wireEditor(editor);
    if (!path.isEmpty() && !editor->loadFile(path)) {
        if (!quiet)
            QMessageBox::warning(this, tr("Open"), tr("Could not open %1").arg(path));
        editor->deleteLater();
        return nullptr;
    }
    const int index = m_tabs->addTab(editor, editor->displayName());
    m_tabs->setCurrentIndex(index);
    updateTabTitle(editor);
    return editor;
}

ImageEditor *MainWindow::addImageTab(const QString &path, bool quiet)
{
    // Opening an image into a single untouched assembly tab replaces it so the
    // session does not keep a stranded untitled .s.
    if (m_tabs->count() == 1) {
        auto *only = qobject_cast<CodeEditor *>(m_tabs->widget(0));
        if (isPristineEditor(only)) {
            m_tabs->removeTab(0);
            only->deleteLater();
        }
    }

    auto *editor = new ImageEditor(this);
    wireImage(editor);
    wireImageReExport(editor);
    if (!path.isEmpty()) {
        const bool ok = isPimPath(path) ? editor->loadFile(path) : editor->importFile(path, false);
        if (!ok) {
            if (!quiet)
                QMessageBox::warning(this, tr("Open"),
                                     tr("Could not open %1: %2").arg(path, editor->lastError()));
            editor->deleteLater();
            return nullptr;
        }
    }
    const int index = m_tabs->addTab(editor, editor->displayName());
    m_tabs->setCurrentIndex(index);
    updateTabTitle(editor);
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

    // The instruction reference follows the cursor, but only when its dock is
    // on show — otherwise an idle panel would churn on every keystroke. OS
    // calls win over the word: cursor on a `trap #1` line (or a push feeding
    // one) shows the call being made, not the TRAP or MOVE instruction.
    connect(editor, &CodeEditor::cursorPositionChanged, this, [this, editor] {
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
                QAction *chosen = menu.exec(pos);
                if (chosen == toggle)
                    toggleBreakpointAtLine(line);
                else if (chosen == condition)
                    editBreakpointCondition(line);
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
    applyGitBlame();
}

void MainWindow::onTabCloseRequested(int index)
{
    QWidget *widget = m_tabs->widget(index);
    QString closedPath;
    if (auto *editor = qobject_cast<CodeEditor *>(widget)) {
        closedPath = editor->filePath();
        if (!maybeSaveEditor(editor))
            return;
    } else if (auto *image = qobject_cast<ImageEditor *>(widget)) {
        closedPath = image->filePath();
        if (!maybeSaveImage(image))
            return;
    }

    m_tabs->removeTab(index);
    if (widget)
        widget->deleteLater();

    // The extracted-document mapping is a per-tab registry — keyed by the path
    // the closed tab held, which is the path that tab's saves wrote back
    // through (writeBackFloppyDoc). It goes with the tab: kept forever, the map
    // grew one entry per extracted file ever opened, and a save of a path
    // nothing on screen describes any more (reopened through Open rather than
    // the disk pane) still wrote itself into that image.
    if (!closedPath.isEmpty())
        m_floppyDocs.remove(QFileInfo(closedPath).absoluteFilePath());

    // There is always at least one tab: closing the last document leaves a
    // pristine editor rather than an empty central widget.
    if (m_tabs->count() == 0)
        addEditorTab(QString());
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
    // the wrong answer for the status bar and the embed action.
    m_caps = probeHatari(toolchain::findEmulator(m_settings.hatariPath).path);
    // A first run on a platform that cannot embed keeps the detached window:
    // the default is not a choice, so it yields to capability, while a stored
    // choice — including "off" — always wins.
    if (!m_embeddedDisplayChosen && !canEmbedDisplay(m_caps)) {
        m_embeddedDisplay = false;
        if (m_displayDock)
            m_displayDock->setVisible(false);
        // createActions checked the box from the pre-probe default; uncheck it
        // without emitting, or the toggled slot would persist a choice the user
        // never made.
        if (m_actEmbedDisplay) {
            const QSignalBlocker blocker(m_actEmbedDisplay);
            m_actEmbedDisplay->setChecked(false);
        }
    }
    updateEmbedActionState();
    m_statusToolchain->setText(
        QStringLiteral("vasm: %1").arg(QFileInfo(m_build->assemblerPath()).fileName()));
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
}

void MainWindow::createActions()
{
    m_actOpen = new QAction(tr("&Open…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openFile);

    m_actNewFile = new QAction(tr("&New File"), this);
    m_actNewFile->setObjectName(QStringLiteral("newFileAction"));
    m_actNewFile->setShortcut(QKeySequence::New);
    connect(m_actNewFile, &QAction::triggered, this, [this] { addEditorTab(QString()); });

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

    m_actOpenProject = new QAction(tr("Open &Project…"), this);
    connect(m_actOpenProject, &QAction::triggered, this, &MainWindow::openProject);

    m_actSaveProject = new QAction(tr("&Save Project"), this);
    connect(m_actSaveProject, &QAction::triggered, this, &MainWindow::saveProject);

    m_actSettings = new QAction(tr("Project &Settings…"), this);
    m_actSettings->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(m_actSettings, &QAction::triggered, this, &MainWindow::editSettings);

    m_actBuild = new QAction(tr("&Build"), this);
    m_actBuild->setShortcut(QKeySequence(Qt::Key_F7));
    connect(m_actBuild, &QAction::triggered, this, &MainWindow::buildInteractive);

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

    m_actResume = new QAction(tr("&Continue"), this);
    m_actResume->setObjectName(QStringLiteral("continueAction"));
    m_actResume->setShortcut(QKeySequence(Qt::Key_F9));
    m_actResume->setEnabled(false);
    connect(m_actResume, &QAction::triggered, this, &MainWindow::resume);

    // Checked state mirrors the persisted preference; the enabled state is set
    // later, once the emulator's capabilities are known.
    m_actEmbedDisplay = new QAction(tr("&Embed emulator display"), this);
    m_actEmbedDisplay->setCheckable(true);
    m_actEmbedDisplay->setChecked(m_embeddedDisplay);
    connect(m_actEmbedDisplay, &QAction::toggled, this, &MainWindow::setDisplayEmbedded);

    // Off unless the user has asked for it: the lane is a view choice, so it
    // lives in application settings rather than the project file. (The other
    // view choice, the embedded display, defaults on where the platform can.)
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
            if (m_actPause)
                m_actPause->setEnabled(!m_host->isStopped());
            if (m_consoleInput)
                m_consoleInput->setEnabled(true);
            // Windows: the emulator has just made its own window, and the
            // container adopts it now that there is a process to look for. A
            // no-op elsewhere, where Hatari reparents itself into the container
            // and reports its video size over the control socket.
            if (m_display && m_embeddedDisplay)
                m_display->attachEmulatorProcess(m_host->emulatorProcessId());
            return;
        }
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
            if (m_actResume) m_actResume->setEnabled(false);
            m_log->appendPlainText(tr("[session] emulator is no longer running"));
        }
        // One owner for everything an ended session leaves behind — the same
        // reset the next launch performs, so stale arming state, bases, the
        // cached machine state and a pending remote command cannot survive
        // into whatever happens next.
        m_session->resetSessionState();
        updateRegisterStrip();
        // The embedded display belonged to the session that just ended: forget
        // its window, so the panel paints its empty state rather than a stale
        // last frame that reads as the emulator's current one.
        if (m_display)
            m_display->clearEmbedded();
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
    connect(m_host, &IDebugBackend::embeddedSizeChanged, this,
            [this](int width, int height) {
                if (!m_display)
                    return;
                // The size report arrives right after the reparent, and Hatari
                // never maps the SDL window it created hidden, so show it now —
                // otherwise the display stays black. The reported size is the
                // video's native resolution, which the fit uses to keep aspect.
                m_display->setVideoSize(width, height);
                m_display->showEmbedded();
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
        m_actResume->setEnabled(stopped);
        updateRunContinueShortcut();
        updateSessionChip();
        updateRegisterStrip();
        // The embedded panel renders no frames while stopped, so tell it to show
        // its paused hint rather than look frozen.
        if (m_display)
            m_display->setPaused(stopped);
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
    m_viewMenu->addAction(m_actEmbedDisplay);
    m_viewMenu->addAction(m_actGitBlame);

    auto *searchMenu = menuBar()->addMenu(tr("&Search"));
    searchMenu->addAction(m_actFind);
    searchMenu->addAction(m_actGotoLine);
    searchMenu->addAction(m_actFindNext);
    searchMenu->addAction(m_actFindPrevious);
    searchMenu->addSeparator();
    searchMenu->addAction(m_actReplace);

    auto *buildMenu = menuBar()->addMenu(tr("&Build"));
    buildMenu->setObjectName(QStringLiteral("buildMenu"));
    buildMenu->addAction(m_actBuild);
    buildMenu->addSeparator();
    buildMenu->addAction(m_actNextDiagnostic);
    buildMenu->addAction(m_actPrevDiagnostic);

    auto *runMenu = menuBar()->addMenu(tr("&Run"));
    runMenu->setObjectName(QStringLiteral("runMenu"));
    runMenu->addAction(m_actRun);
    runMenu->addAction(m_actStop);
    runMenu->addAction(m_actPause);
    runMenu->addSeparator();
    runMenu->addAction(m_actResume);
    runMenu->addAction(m_actStep);
    runMenu->addAction(m_actStepOver);
    runMenu->addAction(m_actStepOut);
    runMenu->addAction(m_actRunToCursor);
    runMenu->addAction(m_actProfileStart);
    runMenu->addAction(m_actProfileStop);
    runMenu->addAction(m_actProfileToCursor);
    runMenu->addSeparator();
    runMenu->addAction(m_actToggleBreakpoint);
    runMenu->addAction(m_actClearBreakpoints);
    runMenu->addAction(m_actAddWatchpoint);
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
    //   left-press  -> the possible start of a native drag; make the embedded
    //                  video input-transparent until the release, so the drag
    //                  keeps tracking when the cursor crosses the video.
    const QEvent::Type type = event->type();

    // A dock tab group's tab bar gets an open-hand cursor to advertise that a
    // tab can be dragged to rearrange it or tear the panel out. Only the dock
    // tab bars (QMainWindowTabBar) — not a content QTabWidget's tab bar.
    if (type == QEvent::Show || type == QEvent::Enter) {
        if (auto *tabBar = qobject_cast<QTabBar *>(watched);
            tabBar
            && QLatin1String(tabBar->metaObject()->className())
                   == QLatin1String("QMainWindowTabBar"))
            tabBar->setCursor(Qt::OpenHandCursor);
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
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseButtonRelease)
        return QMainWindow::eventFilter(watched, event);

    auto *me = static_cast<QMouseEvent *>(event);

    // Any release ends a possible drag, restoring the video's input region.
    if (type == QEvent::MouseButtonRelease) {
        setDragVideoPassthrough(false);
        return QMainWindow::eventFilter(watched, event);
    }

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
    if (me->button() == Qt::LeftButton)
        setDragVideoPassthrough(true);
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

void MainWindow::setDragVideoPassthrough(bool on)
{
    if (on == m_dragVideoPassthrough)
        return;
    m_dragVideoPassthrough = on;
    // Only meaningful while the emulator is embedded and on screen; without an
    // embedded child the helper simply finds no window to reshape.
    if (m_embeddedDisplay && m_display)
        setEmbeddedChildrenInputTransparent(m_display->winId(), on);
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
    m_fileBrowser = new FileBrowser(this);
    addDockWidget(Qt::LeftDockWidgetArea,
                  makeDock(tr("Project files"), QStringLiteral("projectFilesDock"), m_fileBrowser));

    // Tabbed with Project files, and not raised: tabify would put Git on top,
    // and a first run is the project-files picture. Not a debug dock, so the
    // factory hide loop leaves it here. Sprite hides it with the project pane.
    m_gitPanel = new GitPanel(this);
    auto *projectDock = findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    m_gitDock = makeDock(tr("Git"), QStringLiteral("gitDock"), m_gitPanel);
    addDockWidget(Qt::LeftDockWidgetArea, m_gitDock);
    if (projectDock) {
        tabifyDockWidget(projectDock, m_gitDock);
        projectDock->raise();
    }

    // --- right, top: the emulator display, which wants to be prominent --------
    // It lives in a dock shown only when the embedded-display option is on; in
    // separate-window mode it is hidden and the widget is unused.
    m_display = new EmulatorDisplayWidget(this);
    m_displayDock = makeDock(tr("Emulator"), QStringLiteral("emulatorDisplayDock"), m_display);
    m_displayDock->setVisible(m_embeddedDisplay);
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
    if (m_embeddedDisplay) {
        // Vertical split: the first dock goes on top, the second underneath.
        splitDockWidget(m_displayDock, debugTabs.first(), Qt::Vertical);
        m_displayDock->raise();
        debugTabs.first()->raise();
    }

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
    // The Windows adoption cannot be exercised on any machine this is built on,
    // so its outcomes go to the console: one line says which branch ran when a
    // user reports a docked display that is not there.
    connect(m_display, &EmulatorDisplayWidget::embedEvent, this, [this](const QString &text) {
        if (m_log)
            m_log->appendPlainText(QStringLiteral("[embed] ") + text);
    });
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
    connect(m_fileBrowser, &FileBrowser::pathRenamed, this,
            [this](const QString &oldPath, const QString &newPath) {
                // An open document follows its file; breakpoints are file:line
                // and the file's *content* is unchanged, so they stay valid.
                for (CodeEditor *editor : openEditors()) {
                    if (QFileInfo(editor->filePath()) == QFileInfo(oldPath)) {
                        editor->setFilePath(newPath);
                        updateTabTitle(editor);
                        if (QFileInfo(m_settings.sourceFile) == QFileInfo(oldPath))
                            m_settings.sourceFile = newPath;
                    }
                }
                for (ImageEditor *image : openImages()) {
                    if (QFileInfo(image->filePath()) == QFileInfo(oldPath)) {
                        image->setFilePath(newPath);
                        updateTabTitle(image);
                    }
                }
                updateModifiedState();
            });
    connect(m_fileBrowser, &FileBrowser::pathDeleted, this,
            [this](const QString &path) {
                for (CodeEditor *editor : openEditors()) {
                    if (QFileInfo(editor->filePath()) != QFileInfo(path))
                        continue;
                    if (editor->isModifiedSinceLoad()) {
                        statusBar()->showMessage(
                            tr("%1 was deleted on disk; your modified copy is still open.")
                                .arg(QFileInfo(path).fileName()),
                            8000);
                    } else {
                        onTabCloseRequested(m_tabs->indexOf(editor));
                    }
                }
                for (ImageEditor *image : openImages()) {
                    if (QFileInfo(image->filePath()) != QFileInfo(path))
                        continue;
                    if (image->isModifiedSinceLoad()) {
                        statusBar()->showMessage(
                            tr("%1 was deleted on disk; your modified copy is still open.")
                                .arg(QFileInfo(path).fileName()),
                            8000);
                    } else {
                        onTabCloseRequested(m_tabs->indexOf(image));
                    }
                }
            });

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
    // 1280-wide window: left ~200, right ~280, so the editor keeps about 60%
    // once the splitter handles are counted. The bottom group is a strip, not
    // a second editor.
    auto *files = findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    auto *registers = findChild<QDockWidget *>(QStringLiteral("registersDock"));
    if (files && registers)
        resizeDocks({files, registers}, {200, 280}, Qt::Horizontal);
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

    // The embedded-display toggle owns the Emulator dock's visibility, so it is
    // applied after any restored layout, which would otherwise override it.
    if (m_displayDock)
        m_displayDock->setVisible(m_embeddedDisplay);
}

void MainWindow::resetToDefaultLayout()
{
    if (m_defaultLayoutState.isEmpty())
        return;
    restoreState(m_defaultLayoutState);
    // Keep the toggle authoritative over the Emulator dock's visibility.
    m_displayDock->setVisible(m_embeddedDisplay);
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

    // Debugging is the preset that wants the picture. It may check the box
    // when embedding is actually possible; it does not uncheck it, and every
    // preset finishes by letting the box own the Emulator dock.
    if (debugging && canEmbedDisplay(m_caps) && m_actEmbedDisplay && !m_actEmbedDisplay->isChecked())
        m_actEmbedDisplay->setChecked(true);

    auto dockNamed = [this](const QString &name) -> QDockWidget * {
        return findChild<QDockWidget *>(name);
    };

    // Git stays in the project-files tab. Sprite puts both away; Editing and
    // Debugging bring them back with Project files on top, so the Git tab
    // does not become the left pane just because it was shown last.
    QDockWidget *project = dockNamed(QStringLiteral("projectFilesDock"));
    QDockWidget *git = dockNamed(QStringLiteral("gitDock"));
    if (sprite) {
        // Git first. It is the background tab; hiding the current tab last is
        // what drops the group's tab bar, the same way Project files alone did.
        if (git)
            git->hide();
        if (project)
            project->hide();
    } else if (project) {
        project->show();
        if (git) {
            git->show();
            tabifyDockWidget(project, git);
            project->raise();
        }
    }
    if (QDockWidget *problems = dockNamed(QStringLiteral("problemsDock")))
        problems->setVisible(!sprite);

    QDockWidget *console = dockNamed(QStringLiteral("consoleDock"));
    if (console)
        console->setVisible(!sprite);

    QDockWidget *disassembly = dockNamed(QStringLiteral("disassemblyDock"));
    const bool disassemblyOnRight = debugging && !m_embeddedDisplay;
    for (QDockWidget *dock : findChildren<QDockWidget *>()) {
        if (!isDebugDockName(dock->objectName()))
            continue;
        const bool onRight = disassemblyOnRight && dock == disassembly;
        dock->setVisible(debugging);
        if (!debugging)
            continue;
        if (onRight) {
            addDockWidget(Qt::RightDockWidgetArea, dock);
            dock->raise();
        } else if (console) {
            tabifyDockWidget(console, dock);
        }
    }
    if (debugging && m_embeddedDisplay && m_displayDock)
        addDockWidget(Qt::RightDockWidgetArea, m_displayDock);
    if (debugging) {
        if (QDockWidget *regs = dockNamed(QStringLiteral("registersDock")))
            regs->raise();
    }

    if (m_displayDock)
        m_displayDock->setVisible(m_embeddedDisplay);
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
    if (m_displayDock)
        m_displayDock->setVisible(m_embeddedDisplay);
}

void MainWindow::createToolBar()
{
    auto *bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->setIconSize(QSize(20, 20));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setFloatable(false);
    bar->addAction(m_actOpen);
    bar->addAction(m_actSave);
    bar->addAction(m_actSettings);
    bar->addSeparator();
    bar->addAction(m_actBuild);
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
}

void MainWindow::createStatusBar()
{
    // Permanent widgets are laid out from the right, so the first one added
    // is the rightmost. The session is the one a glance should land on.
    m_statusSession = new QLabel(tr("Not running"), this);
    m_statusSession->setObjectName(QStringLiteral("statusSession"));
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

void MainWindow::followCursorReference(CodeEditor *editor)
{
    if (!editor || editor != m_editor)
        return;

    const QTextCursor cursor = editor->textCursor();
    const int blockNumber = cursor.blockNumber();

    // The scanner only needs a small window around the cursor: OS calls
    // are a local idiom (OsCallScan bounds its scans to a few lines).
    const QTextDocument *doc = editor->document();
    const int base = qMax(0, blockNumber - 10);
    const int last = qMin(doc->blockCount() - 1, blockNumber + 10);
    QStringList window;
    window.reserve(last - base + 1);
    for (int i = base; i <= last; ++i)
        window.append(doc->findBlockByNumber(i).text());
    const OsCallMatch match = osCallAt(window, blockNumber - base);

    auto showStrip = [this](const QString &text) {
        if (!m_instrStrip)
            return;
        m_instrStrip->setText(text);
        m_instrStrip->setToolTip(text.isEmpty() ? tr("Open the instruction reference") : text);
    };

    if (match.call) {
        if (m_instrRef)
            m_instrRef->showOsCall(match.call->trap, match.call->opcode, match.args);
        QString stack;
        if (!match.args.isEmpty())
            stack = match.args.join(QStringLiteral(", "));
        if (match.call->stackBytes > 0) {
            if (!stack.isEmpty())
                stack += QStringLiteral(" · ");
            stack += tr("%1 bytes").arg(match.call->stackBytes);
        }
        QString line = QStringLiteral("%1 — %2").arg(match.call->name, match.call->summary);
        if (!stack.isEmpty())
            line += tr("  Stack: %1").arg(stack);
        showStrip(line);
        return;
    }
    if (match.trapContext) {
        // A trap whose function number is not statically known: the
        // generic TRAP entry still says what the instruction does.
        if (m_instrRef)
            m_instrRef->showInstruction(QStringLiteral("trap"));
        if (const InstructionInfo *info = instructionRef(QStringLiteral("trap")))
            showStrip(QStringLiteral("%1 — %2").arg(info->mnemonic, info->summary));
        else
            showStrip(QString());
        return;
    }

    const QString line = cursor.block().text();
    const int col = cursor.positionInBlock();
    int start = col;
    while (start > 0
           && (line.at(start - 1).isLetterOrNumber() || line.at(start - 1) == QLatin1Char('.')))
        --start;
    int end = col;
    while (end < line.size()
           && (line.at(end).isLetterOrNumber() || line.at(end) == QLatin1Char('.')))
        ++end;
    const QString word = line.mid(start, end - start);
    if (m_instrRef)
        m_instrRef->showInstruction(word);
    if (const InstructionInfo *info = instructionRef(word))
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

    // Save: to the existing path when there is one, otherwise ask for one.
    if (editor->filePath().isEmpty()) {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save assembly source"), QString(),
            tr("Assembly sources (*.s *.S *.asm)"));
        if (path.isEmpty())
            return false;
        if (!editor->saveFile(path)) {
            QMessageBox::critical(this, tr("Save"), tr("Could not write %1").arg(path));
            return false;
        }
    } else if (!editor->saveFile(editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1").arg(editor->filePath()));
        return false;
    }
    writeBackFloppyDoc(editor->filePath());

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
        if (!editor->saveFile(path)) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2").arg(path, editor->lastError()));
            return false;
        }
    } else if (!editor->saveFile(editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(editor->filePath(), editor->lastError()));
        return false;
    }
    writeBackFloppyDoc(editor->filePath());

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
    if (CodeEditor *open = editorForPath(path)) {
        m_tabs->setCurrentWidget(open);
    } else if (ImageEditor *open = imageForPath(path)) {
        m_tabs->setCurrentWidget(open);
    } else if (isPimPath(path) || isImportableImagePath(path)) {
        if (!addImageTab(path, quiet))
            return false;
    } else if (!addEditorTab(path, quiet)) {
        return false;
    }

    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    if (!isPimPath(path) && !isImportableImagePath(path))
        loadProjectForSource(path);
    updateModifiedState();

    if (m_fileBrowser)
        m_fileBrowser->showFor(path);
    // The opened file is what decides the project directory, and the per-tab pass
    // in onTabChanged does not always run here: a pristine first tab takes the
    // file's contents in place, so the current index never changes and no tab
    // switch is emitted — git would keep pointing at the previous project (or at
    // none) while a project is open (MIN-81). Both calls are idempotent.
    syncGitDirectory();
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
    if (m_editor && !m_editor->filePath().isEmpty()) {
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

void MainWindow::syncGitDirectory()
{
    if (!m_gitPanel)
        return;
    // Only a directory the user actually opened counts as the project. The
    // browser's model root is the process working directory until something is
    // opened — so aiming git at it made a window with no project run `git
    // status` in whichever repository happened to contain the launch directory.
    // PiST's own suite, run from a build tree inside its checkout, did exactly
    // that ~90 times per run; an abandoned status child at teardown leaves
    // `.git/index.lock` in the developer's repository (MIN-81). With nothing
    // open the panel is told so, and it says as much instead of guessing.
    QString dir;
    if (m_fileBrowser && m_fileBrowser->hasProjectDirectory())
        dir = m_fileBrowser->directory();
    else if (m_editor && !m_editor->filePath().isEmpty())
        dir = QFileInfo(m_editor->filePath()).absolutePath();
    else if (m_image && !m_image->filePath().isEmpty())
        dir = QFileInfo(m_image->filePath()).absolutePath();
    m_gitPanel->setDirectory(dir);
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
    if (m_image) {
        if (m_image->filePath().isEmpty()) {
            const QString path = QFileDialog::getSaveFileName(
                this, tr("Save image"), QString(), tr("PiST images (*.pim)"));
            if (path.isEmpty())
                return;
            if (!m_image->saveFile(path)) {
                QMessageBox::critical(this, tr("Save"),
                                      tr("Could not write %1: %2")
                                          .arg(path, m_image->lastError()));
                return;
            }
            statusBar()->showMessage(tr("Saved %1").arg(path), 4000);
        } else if (!m_image->saveFile(m_image->filePath())) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2")
                                      .arg(m_image->filePath(), m_image->lastError()));
        } else {
            writeBackFloppyDoc(m_image->filePath());
            statusBar()->showMessage(tr("Saved %1").arg(m_image->filePath()), 4000);
        }
        updateTabTitle(m_image);
        updateModifiedState();
        return;
    }

    if (!m_editor)
        return;
    if (m_editor->filePath().isEmpty()) {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save assembly source"), QString(),
            tr("Assembly sources (*.s *.S *.asm)"));
        if (path.isEmpty())
            return;
        // The result was discarded here, so a failed write said nothing at all:
        // no dialog, no status, and — because saveFile() adopts the path only
        // on success — a buffer that quietly stayed untitled while the user
        // believed the save had gone through. Report it as the path-known
        // branch and maybeSaveEditor do, and stop before the paths below that
        // describe a document on disk.
        if (!m_editor->saveFile(path)) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2")
                                      .arg(path, m_editor->lastError()));
            return;
        }
    } else if (!m_editor->saveFile(m_editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(m_editor->filePath(), m_editor->lastError()));
    } else {
        writeBackFloppyDoc(m_editor->filePath());
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
    if (m_editor && !m_editor->filePath().isEmpty())
        return m_editor->filePath();
    if (!m_settings.sourceFile.isEmpty() && QFileInfo::exists(m_settings.sourceFile))
        return m_settings.sourceFile;
    for (CodeEditor *editor : openEditors()) {
        if (!editor->filePath().isEmpty())
            return editor->filePath();
    }
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
    buildImpl(/*quiet=*/true);
}

void MainWindow::buildInteractive()
{
    buildImpl(/*quiet=*/false);
}

void MainWindow::buildImpl(bool quiet)
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
        if (editor->filePath().isEmpty() || !editor->isModifiedSinceLoad())
            continue;
        if (!editor->saveFile(editor->filePath())) {
            refuseBuild(tr("Build"),
                        tr("Could not save %1: %2")
                            .arg(editor->filePath(), editor->lastError()),
                        true);
            return;
        }
        writeBackFloppyDoc(editor->filePath());
    }

    m_problems->clear();
    m_problemEntries.clear();
    m_log->appendPlainText(tr("--- build ---"));

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
    m_build->setExtraArgs(m_settings.extraBuildArgs);

    if (m_statusBuild)
        m_statusBuild->setText(tr("Building…"));
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
    buildImpl(quiet);
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
    // step-back snapshot all depend on it).
    m_host->step();
    m_host->refresh();
}

void MainWindow::stepOver()
{
    m_host->stepOver();
    m_host->refresh();
}

void MainWindow::stepOut()
{
    m_session->stepOut();
}

void MainWindow::runToCursor()
{
    if (!m_host->isStopped() || !m_editor || m_editor->filePath().isEmpty())
        return;
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

bool MainWindow::addWatchpointAddress(const QString &text, QString *error)
{
    return m_bpModel->addWatchpointAddress(text, error);
}

void MainWindow::removeWatchpoint(int index)
{
    m_bpModel->removeWatchpoint(index);
}

bool MainWindow::toggleBreakpointAtLine(int line)
{
    if (!m_editor || m_editor->filePath().isEmpty() || line <= 0)
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
    if (m_image)
        return !m_image->filePath().isEmpty() && m_image->saveFile(m_image->filePath());
    if (!m_editor || m_editor->filePath().isEmpty())
        return false;
    return m_editor->saveFile(m_editor->filePath());
}

QJsonArray MainWindow::profilerResultsJson() const
{
    return m_remoteState->profilerResultsJson();
}

bool MainWindow::lineHasCode(int line) const
{
    if (!m_editor || m_editor->filePath().isEmpty() || line <= 0)
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
    }
    m_disassembly->setState(state);

    bool navigated = false;
    if (state.hasBases()) {
        const bool firstBases = !m_bases.isValid();
        m_bases.text = state.textBase;
        m_bases.data = state.dataBase;
        m_bases.bss = state.bssBase;

        // Hand the live bases to the program map. Without this, lineFor and
        // addressFor return false forever: no breakpoint ever resolves, and the
        // editor never follows the program counter (docs/code-review-glm-001.md
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

bool MainWindow::canEmbedDisplay(const HatariCapabilities &caps) const
{
    const QString platform = QGuiApplication::platformName();
    // Windows: PiST adopts the emulator's own window with SetParent
    // (ui/EmbedWin32.h), so all that is needed is a container to adopt it
    // into. The video size comes from that window rather than from a
    // control-socket report, so the socket is not a precondition here — a stock
    // Windows Hatari does not have one.
    if (platform == QLatin1String("windows"))
        return true;
    // X11: both ends must be clients of the same display, and the size report
    // that sizes the container travels on the control socket.
    return platform == QLatin1String("xcb") && caps.hasControlSocket;
}

void MainWindow::setDisplayEmbedded(bool on)
{
    m_embeddedDisplay = on;
    m_embeddedDisplayChosen = true;
    QSettings().setValue(embeddedDisplayKey(), on);
    if (m_displayDock)
        m_displayDock->setVisible(on);
    // A running session keeps the display mode it was launched with; the change
    // takes effect on the next Run.
}

void MainWindow::updateEmbedActionState()
{
    if (!m_actEmbedDisplay)
        return;
    const bool can = canEmbedDisplay(m_caps);
    m_actEmbedDisplay->setEnabled(can);
    m_actEmbedDisplay->setToolTip(
        can ? tr("Run the emulator's display inside the IDE rather than in a "
                 "separate window.")
            : tr("Embedded display needs X11 (this session is on '%1') and an "
                 "emulator with a control socket.")
                  .arg(QGuiApplication::platformName()));
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
    // Raise first, so the capture is this window and not whatever is on top of
    // it, and pump once so the raise has landed before the frame buffer is
    // read. XGetImage (ui/EmbedX11.h) rather than QScreen::grabWindow, which
    // returns black for a top-level window under XWayland on this setup and
    // cannot see the reparented foreign window the embedded emulator lives in.
    raise();
    activateWindow();
    QGuiApplication::processEvents();
    const QImage image = captureWindowImage(winId());
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
    if (!m_bases.isValid() || m_programMap.isEmpty()) {
        if (m_editor)
            m_editor->clearCurrentExecutionLine();
        m_stoppedFile.clear();
        m_stoppedLine = 0;
        updateSessionChip();
        updateCaretChip();
        return;
    }

    LineMap::Address address;
    if (!m_programMap.lineFor(pc, &address)) {
        if (m_editor)
            m_editor->clearCurrentExecutionLine();
        m_stoppedFile.clear();
        m_stoppedLine = 0;
        updateSessionChip();
        updateCaretChip();
        return;
    }

    // The editor follows the program counter into whatever file it lands in:
    // raise the tab showing it, opening one when it is not already open. The
    // comparison must be path-tolerant: the listing records whatever path was
    // passed to vasm (usually absolute), while the editor knows the file it
    // was opened with (usually just a name).
    CodeEditor *target = nullptr;
    for (CodeEditor *editor : openEditors())
        if (LineMap::sameSource(address.file, editor->filePath())) {
            target = editor;
            break;
        }
    if (!target && QFileInfo::exists(address.file))
        target = addEditorTab(address.file);

    m_stoppedFile = QFileInfo(address.file).fileName();
    m_stoppedLine = address.line;
    updateSessionChip();

    if (!target)
        return;

    m_tabs->setCurrentWidget(target);
    target->setCurrentExecutionLine(address.line);
    target->gotoLine(address.line);
    updateCaretChip();
}

} // namespace pist
