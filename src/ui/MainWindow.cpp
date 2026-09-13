// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "toolchain/Toolchain.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "ui/BreakpointPanel.h"
#include "ui/DisassemblyView.h"
#include "ui/EmulatorDisplayWidget.h"
#include "ui/EmbedX11.h"
#include "ui/FileBrowser.h"
#include "ui/MemoryView.h"
#include "ui/HardwareView.h"
#include "ui/SettingsDialog.h"
#include "ui/StackView.h"
#include "ui/RegistersView.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QProcess>
#include <QStandardPaths>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QInputDialog>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>

namespace pist {

namespace {

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_editor = new CodeEditor(this);
    setCentralWidget(m_editor);

    m_build = new BuildService(this);
    // Resolve the assembler through the toolchain locator, which searches beside
    // the app and in a per-user tools directory before PATH. Previously this fell
    // back to a bare name, so a missing vasm surfaced as a late process-start
    // failure rather than an early, actionable message.
    const ToolInfo assembler = toolchain::findAssembler();
    m_build->setAssemblerPath(assembler.found() ? assembler.path
                                                : QStringLiteral("vasmm68k_mot"));
    connect(m_build, &BuildService::finished, this, &MainWindow::onBuildFinished);
    connect(m_build, &BuildService::outputLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });

    m_host = new EmulatorHost(this);
    connect(m_host, &EmulatorHost::stateUpdated, this, &MainWindow::onStateUpdated);
    connect(m_host, &EmulatorHost::logLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });
    // Emulator errors are reported in the log *and* the status bar. Log-only was
    // the previous behaviour, and the log is a bottom-dock tab that is not always
    // visible, so a failed launch looked like a normal window.
    connect(m_host, &EmulatorHost::errorOccurred, this, [this](const QString &message) {
        m_log->appendPlainText(QStringLiteral("[error] ") + message);
        statusBar()->showMessage(message, 15000);
    });

    // Without this the status bar kept saying "Running" after Hatari had died,
    // because only stoppedChanged was connected. A dead emulator looked live and
    // step/continue stayed enabled.
    connect(m_host, &EmulatorHost::runningChanged, this, [this](bool running) {
        emit sessionRunningChanged(running);
        if (running)
            return;
        m_statusEmulator->setText(m_caps.valid ? m_caps.summary() : tr("Not running"));
        if (m_sessionArmed) {
            m_sessionArmed = false;
            if (m_actStep) m_actStep->setEnabled(false);
            if (m_actStepOver) m_actStepOver->setEnabled(false);
            if (m_actResume) m_actResume->setEnabled(false);
            m_log->appendPlainText(tr("[session] emulator is no longer running"));
        }
    });
    connect(m_host, &EmulatorHost::memoryDumpReady, this,
            [this](quint32, const QString &response) {
                if (m_memory)
                    m_memory->applyDump(response);
            });
    // `info <subject>` responses feed the hardware view. Routed by command text:
    // `info basepage` is part of the normal refresh and must not land here.
    connect(m_host, &EmulatorHost::commandFinished, this,
            [this](const QString &command, const QString &response) {
                if (m_hardware && command == QLatin1String("info ") + m_hardware->subject())
                    m_hardware->setInfo(response);
            });
    connect(m_host, &EmulatorHost::embeddedSizeChanged, this,
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

    // Restore the display preference before any dock is created, so the dock's
    // initial visibility matches it.
    m_embeddedDisplay = QSettings().value(QStringLiteral("display/embedded"), false).toBool();

    // The title carries the modified marker, so the user can always tell whether
    // work is unsaved without looking for a toolbar state.
    connect(m_editor, &CodeEditor::modificationChanged, this,
            [this](bool) { updateModifiedState(); });

    connect(m_editor, &CodeEditor::gutterClicked, this, [this](int line, Qt::MouseButton button) {
        if (button == Qt::LeftButton)
            toggleBreakpointAtLine(line);
        else if (button == Qt::RightButton)
            editBreakpointCondition(line);
    });
    connect(m_editor, &CodeEditor::gutterContextMenuRequested, this,
            [this](int line, const QPoint &pos) {
                QMenu menu;
                const bool hasBreakpoint = std::any_of(
                    m_breakpoints.cbegin(), m_breakpoints.cend(),
                    [this, line](const Breakpoint &bp) {
                        return bp.line == line
                            && bp.file == QFileInfo(m_editor->filePath()).fileName();
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

    connect(m_host, &EmulatorHost::stoppedChanged, this, [this](bool stopped) {
        m_actStep->setEnabled(stopped);
        m_actStepOver->setEnabled(stopped);
        m_actResume->setEnabled(stopped);
        m_statusEmulator->setText(stopped ? tr("Stopped in debugger") : tr("Running"));
        if (stopped)
            onDebuggerStopped();
    });

    createActions();
    createMenus();
    createDocks();
    createToolBar();
    createStatusBar();

    // Sessions left by a crash or a kill, once they are old enough that no live
    // instance could still own them.
    paths::pruneStaleSessions();

    // Reopen where the user left off when nothing was passed on the command line.
    QTimer::singleShot(0, this, [this] {
        if (m_editor->filePath().isEmpty())
            openRecentSource();
    });

    m_caps = probeHatari(toolchain::findEmulator().path);
    updateEmbedActionState();
    m_statusToolchain->setText(
        QStringLiteral("vasm: %1").arg(QFileInfo(m_build->assemblerPath()).fileName()));
    m_statusEmulator->setText(m_caps.summary());

    updateModifiedState();
    resize(1280, 860);
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
    m_actOpen = new QAction(tr("&Open…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openFile);

    m_actSave = new QAction(tr("&Save"), this);
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
    connect(m_actBuild, &QAction::triggered, this, &MainWindow::build);

    m_actRun = new QAction(tr("&Run"), this);
    m_actRun->setShortcut(QKeySequence(Qt::Key_F5));
    connect(m_actRun, &QAction::triggered, this, &MainWindow::run);

    m_actStop = new QAction(tr("&Stop"), this);
    m_actStop->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F5));
    connect(m_actStop, &QAction::triggered, this, &MainWindow::stopSession);

    m_actStep = new QAction(tr("&Step"), this);
    m_actStep->setShortcut(QKeySequence(Qt::Key_F10));
    m_actStep->setEnabled(false);
    connect(m_actStep, &QAction::triggered, this, &MainWindow::step);

    m_actStepOver = new QAction(tr("Step &Over"), this);
    m_actStepOver->setShortcut(QKeySequence(Qt::Key_F11));
    m_actStepOver->setEnabled(false);
    connect(m_actStepOver, &QAction::triggered, this, &MainWindow::stepOver);

    m_actClearBreakpoints = new QAction(tr("Clear &Breakpoints"), this);
    m_actClearBreakpoints->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F9));
    connect(m_actClearBreakpoints, &QAction::triggered, this, &MainWindow::clearAllBreakpoints);

    m_actAddWatchpoint = new QAction(tr("Add &watchpoint…"), this);
    connect(m_actAddWatchpoint, &QAction::triggered, this, &MainWindow::addWatchpoint);

    m_actResume = new QAction(tr("&Continue"), this);
    m_actResume->setShortcut(QKeySequence(Qt::Key_F9));
    m_actResume->setEnabled(false);
    connect(m_actResume, &QAction::triggered, this, &MainWindow::resume);

    // Checked state mirrors the persisted preference; the enabled state is set
    // later, once the emulator's capabilities are known.
    m_actEmbedDisplay = new QAction(tr("&Embed emulator display"), this);
    m_actEmbedDisplay->setCheckable(true);
    m_actEmbedDisplay->setChecked(m_embeddedDisplay);
    connect(m_actEmbedDisplay, &QAction::toggled, this, &MainWindow::setDisplayEmbedded);
}

void MainWindow::createMenus()
{
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_actOpen);
    fileMenu->addAction(m_actSave);
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

    auto *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_actEmbedDisplay);

    auto *runMenu = menuBar()->addMenu(tr("&Run"));
    runMenu->addAction(m_actBuild);
    runMenu->addAction(m_actRun);
    runMenu->addAction(m_actStop);
    runMenu->addSeparator();
    runMenu->addAction(m_actResume);
    runMenu->addAction(m_actStep);
    runMenu->addAction(m_actStepOver);
    runMenu->addSeparator();
    runMenu->addAction(m_actClearBreakpoints);
    runMenu->addAction(m_actAddWatchpoint);
}

void MainWindow::createDocks()
{
    addDockWidget(Qt::LeftDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Project files"), this);
        m_fileBrowser = new FileBrowser(dock);
        dock->setWidget(m_fileBrowser);
        // Opening from the browser goes through the same path as the menu, so the
        // unsaved-changes prompt and project discovery behave identically.
        connect(m_fileBrowser, &FileBrowser::fileActivated, this, &MainWindow::openPath);
        return dock;
    }());

    addDockWidget(Qt::RightDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Registers"), this);
        m_registers = new RegistersView(dock);
        dock->setWidget(m_registers);
        return dock;
    }());

    addDockWidget(Qt::RightDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Disassembly"), this);
        m_disassembly = new DisassemblyView(dock);
        dock->setWidget(m_disassembly);
        return dock;
    }());

    addDockWidget(Qt::RightDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Breakpoints"), this);
        m_breakpointPanel = new BreakpointPanel(dock);
        dock->setWidget(m_breakpointPanel);
        connect(m_breakpointPanel, &BreakpointPanel::removeRequested,
                this, &MainWindow::removeBreakpoint);
        connect(m_breakpointPanel, &BreakpointPanel::breakpointActivated,
                this, &MainWindow::goToBreakpoint);
        connect(m_breakpointPanel, &BreakpointPanel::clearRequested,
                this, &MainWindow::clearAllBreakpoints);
        connect(m_breakpointPanel, &BreakpointPanel::watchpointRemoveRequested,
                this, &MainWindow::removeWatchpoint);
        connect(m_breakpointPanel, &BreakpointPanel::watchpointActivated,
                this, [this](quint32 address) {
                    if (m_memory)
                        m_memory->goToAddress(address);
                });
        return dock;
    }());

    // The embedded display lives in a dock that is shown only when the option
    // is on; in separate-window mode it is hidden and the widget is unused.
    addDockWidget(Qt::RightDockWidgetArea, [this] {
        m_displayDock = new QDockWidget(tr("Emulator"), this);
        m_displayDock->setObjectName(QStringLiteral("emulatorDisplayDock"));
        m_display = new EmulatorDisplayWidget(m_displayDock);
        m_displayDock->setWidget(m_display);
        m_displayDock->setVisible(m_embeddedDisplay);
        return m_displayDock;
    }());

    addDockWidget(Qt::BottomDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Memory"), this);
        m_memory = new MemoryView(dock);
        dock->setWidget(m_memory);
        connect(m_memory, &MemoryView::dumpRequested, m_host, &EmulatorHost::requestMemoryDump);
        return dock;
    }());

    // The call stack: dumped at the stack pointer each stop, with likely return
    // addresses marked. Routed over its own channel so it never clobbers the
    // memory view's dumps.
    addDockWidget(Qt::RightDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Stack"), this);
        m_stack = new StackView(dock);
        dock->setWidget(m_stack);
        connect(m_host, &EmulatorHost::stackDumpReady, this,
                [this](quint32 sp, const QString &response) {
                    if (!m_stack)
                        return;
                    // The annotation needs the *text* extent, not the data base.
                    // With no data section (or one that does not follow text) the
                    // data base gives an empty range and every return address
                    // silently goes unmarked, so take the extent from the
                    // listings and fall back to the old bound when the map has
                    // none (docs/code-review-glm-001.md, P3).
                    const quint32 textEnd = m_programMap.textEnd();
                    m_stack->setStackDump(sp, response, m_lastState.textBase,
                                          textEnd ? textEnd : m_lastState.dataBase);
                });
        return dock;
    }());

    // Hardware state (shifter, MFP, sound, ...) from Hatari's `info` commands,
    // refreshed on each stop. Read-only; the subject is selectable.
    addDockWidget(Qt::RightDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Hardware"), this);
        m_hardware = new HardwareView(dock);
        dock->setWidget(m_hardware);
        connect(m_hardware, &HardwareView::subjectChanged, this,
                [this](const QString &subject) {
                    if (m_host->isRunning())
                        m_host->command(QStringLiteral("info ") + subject);
                });
        return dock;
    }());

    m_bottomTabs = new QTabWidget(this);

    m_problems = new QTreeWidget(m_bottomTabs);
    m_problems->setHeaderLabels({tr("File"), tr("Line"), tr("Message")});
    m_problems->header()->setStretchLastSection(true);
    connect(m_problems, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        const int line = item->text(1).toInt();
        if (line > 0)
            m_editor->gotoLine(line);
    });
    m_bottomTabs->addTab(m_problems, tr("Problems"));

    m_log = new QPlainTextEdit(m_bottomTabs);
    m_log->setReadOnly(true);
    QFont mono = m_log->font();
    mono.setFamily(QStringLiteral("monospace"));
    m_log->setFont(mono);
    m_bottomTabs->addTab(m_log, tr("Build & debug console"));

    auto *dock = new QDockWidget(tr("Output"), this);
    dock->setWidget(m_bottomTabs);
    addDockWidget(Qt::BottomDockWidgetArea, dock);
}

void MainWindow::createToolBar()
{
    auto *bar = addToolBar(tr("Main"));
    bar->setObjectName(QStringLiteral("mainToolBar"));
    bar->addAction(m_actOpen);
    bar->addAction(m_actSave);
    bar->addAction(m_actSettings);
    bar->addSeparator();
    bar->addAction(m_actBuild);
    bar->addAction(m_actRun);
    bar->addAction(m_actStop);
    bar->addSeparator();
    bar->addAction(m_actResume);
    bar->addAction(m_actStep);
    bar->addAction(m_actStepOver);
    bar->addSeparator();
    bar->addAction(m_actClearBreakpoints);
}

void MainWindow::createStatusBar()
{
    m_statusToolchain = new QLabel(this);
    m_statusEmulator = new QLabel(this);
    statusBar()->addPermanentWidget(m_statusToolchain);
    statusBar()->addPermanentWidget(m_statusEmulator);
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
    const QString name = m_editor->displayName();
    const bool modified = m_editor->isModifiedSinceLoad();

    // The "[*]" placeholder is replaced by Qt when windowModified is set, which
    // is the platform-correct way to show an unsaved document.
    setWindowTitle(tr("%1[*] — PiST").arg(name));
    setWindowModified(modified);

    if (m_actSave)
        m_actSave->setEnabled(modified);
}

bool MainWindow::maybeSave()
{
    if (!m_editor->isModifiedSinceLoad())
        return true;

    const auto answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has unsaved changes.\n\nSave before continuing?")
            .arg(m_editor->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;

    // Save: to the existing path when there is one, otherwise ask for one.
    if (m_editor->filePath().isEmpty()) {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save assembly source"), QString(),
            tr("Assembly sources (*.s *.S *.asm)"));
        if (path.isEmpty())
            return false;
        if (!m_editor->saveFile(path)) {
            QMessageBox::critical(this, tr("Save"), tr("Could not write %1").arg(path));
            return false;
        }
    } else if (!m_editor->saveFile(m_editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1").arg(m_editor->filePath()));
        return false;
    }

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
        this, tr("Open assembly source"), QString(),
        tr("Assembly sources (*.s *.S *.asm *.x68);;All files (*)"));
    if (path.isEmpty())
        return;
    openPath(path);
}

void MainWindow::openPath(const QString &path)
{
    if (path.isEmpty())
        return;

    // Replacing the current document would discard unsaved work.
    if (!maybeSave())
        return;

    if (!m_editor->loadFile(path)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Could not open %1").arg(path));
        return;
    }

    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    loadProjectForSource(path);
    updateModifiedState();

    if (m_fileBrowser)
        m_fileBrowser->showFor(path);
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
        // Adopting the project's build settings while the *previous* document
        // stays open would build the old file with the new configuration.
        if (!m_editor->loadFile(source)) {
            QMessageBox::critical(this, tr("Open project"),
                                  tr("The project refers to %1, which could not be opened.")
                                      .arg(source));
            return;
        }
        loaded.sourceFile = source;
    }

    m_settings = loaded;
    settings::rememberLastProject(path, source);
    statusBar()->showMessage(tr("Opened project %1").arg(QFileInfo(path).fileName()), 5000);
    m_log->appendPlainText(tr("[project] loaded %1").arg(path));
}

void MainWindow::saveProject()
{
    if (m_editor->filePath().isEmpty()) {
        QMessageBox::information(this, tr("Save project"),
                                 tr("Open an assembly source file first."));
        return;
    }

    m_settings.sourceFile = m_editor->filePath();
    const QString path = settings::projectFileFor(m_editor->filePath());

    QString error;
    if (!settings::save(m_settings, path, &error)) {
        QMessageBox::critical(this, tr("Save project"), error);
        return;
    }

    settings::rememberLastProject(path, m_editor->filePath());
    statusBar()->showMessage(tr("Saved %1").arg(QFileInfo(path).fileName()), 5000);
    m_log->appendPlainText(tr("[project] saved %1").arg(path));
}

void MainWindow::editSettings()
{
    SettingsDialog dialog(m_settings, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_settings = dialog.settings();

    // Persist immediately when the project is already known, so a settings change
    // is not lost if the session is closed without an explicit save.
    if (!m_editor->filePath().isEmpty()) {
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
        settings::rememberLastProject(path, m_editor->filePath());
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

    settings::rememberLastProject(projectPath, sourcePath);
    m_log->appendPlainText(tr("[project] loaded %1").arg(projectPath));
    statusBar()->showMessage(
        tr("Project settings: %1, %2, %3 MiB")
            .arg(machineDisplayName(m_settings.machine), m_settings.monitor)
            .arg(m_settings.memSizeMiB),
        6000);
}

void MainWindow::saveFile()
{
    if (m_editor->filePath().isEmpty()) {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Save assembly source"), QString(),
            tr("Assembly sources (*.s *.S *.asm)"));
        if (path.isEmpty())
            return;
        m_editor->saveFile(path);
    } else if (!m_editor->saveFile(m_editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(m_editor->filePath(), m_editor->lastError()));
    }
    updateModifiedState();
}

void MainWindow::build()
{
    if (m_editor->filePath().isEmpty()) {
        QMessageBox::information(this, tr("Build"),
                                 tr("Open an assembly source file first."));
        return;
    }

    // A failed save must stop the build: otherwise the assembler runs on the
    // previous on-disk contents and reports a result for code the user is not
    // looking at.
    if (!m_editor->saveFile(m_editor->filePath())) {
        QMessageBox::critical(this, tr("Build"),
                              tr("Could not save %1: %2")
                                  .arg(m_editor->filePath(), m_editor->lastError()));
        return;
    }

    m_problems->clear();
    m_log->appendPlainText(tr("--- build ---"));

    const QFileInfo info(m_editor->filePath());
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
            QMessageBox::critical(this, tr("Linker not found"),
                                  toolchain::linkerInstallHint());
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

    m_build->build();
}

void MainWindow::onBuildFinished(bool success, const QList<Diagnostic> &diagnostics)
{
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

    QList<int> errorLines;
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

        auto *item = new QTreeWidgetItem(m_problems);
        item->setText(0, file.isEmpty() ? tr("(build)") : file);
        item->setText(1, line > 0 ? QString::number(line) : QString());
        item->setText(2, d.message);
        if (d.severity == Diagnostic::Error)
            item->setForeground(2, QColor(0xc0, 0x20, 0x20));

        // Only diagnostics belonging to the open file can be marked in its gutter.
        if (line > 0 && LineMap::sameSource(file, m_editor->filePath()))
            errorLines.append(line);
    }
    m_editor->setErrorLines(errorLines);

    if (!success)
        m_bottomTabs->setCurrentWidget(m_problems);

    m_log->appendPlainText(success ? tr("Build succeeded.")
                                   : tr("Build failed with %1 diagnostic(s).")
                                         .arg(diagnostics.size()));

    // Run was requested: continue now that the build has actually finished.
    if (m_launchAfterBuild) {
        m_launchAfterBuild = false;
        if (success)
            launchEmulator();
        else
            m_log->appendPlainText(tr("[run] build failed, so nothing was launched"));
    }
}

void MainWindow::run()
{
    if (m_editor->filePath().isEmpty()) {
        QMessageBox::information(this, tr("Run"), tr("Open an assembly source file first."));
        return;
    }

    // Always rebuild before running: this keeps the listing in step with the
    // binary, which is what the line map depends on.
    //
    // The launch cannot happen here. build() starts the assembler asynchronously
    // and returns immediately, so anything after it would run while the build was
    // still in flight — and a "is the build running?" guard here is always true,
    // which silently turned the whole launch path into dead code. The launch is
    // chained onto build completion instead, via onBuildFinished.
    m_launchAfterBuild = true;
    build();
}

void MainWindow::launchEmulator()
{
    const QFileInfo info(m_editor->filePath());
    const QString prg = settings::outputPathsFor(info.absoluteFilePath()).program;
    if (!QFileInfo::exists(prg)) {
        QMessageBox::warning(this, tr("Run"),
                             tr("The build did not produce %1.").arg(prg));
        return;
    }

    const ToolInfo emulator = toolchain::findEmulator(m_settings.hatariPath);
    if (!emulator.found()) {
        QMessageBox::critical(this, tr("Emulator not found"),
                              toolchain::emulatorInstallHint());
        return;
    }

    const QString sessionDir = makeSessionDir();

    SessionConfig config;
    config.hatariPath = emulator.path;
    config.programPath = prg;
    config.sessionDir = sessionDir;
    config.gemdosDir = info.absolutePath();

    config.machine = machineCliName(m_settings.machine);
    config.monitor = m_settings.monitor;
    config.memSizeMiB = m_settings.memSizeMiB;
    config.extraArgs = m_settings.extraEmulatorArgs;
    if (!m_settings.hardDiskImage.isEmpty()) {
        // ACSI is the safe default: it exists on every ST-family machine, unlike
        // IDE which is STe-and-later only.
        config.acsiImage = m_settings.hardDiskImage;
        config.acsiId = 0;
    }

    config.floppyImages = m_settings.floppyImages;

    // The control socket is compiled into Hatari only under
    // HAVE_UNIX_DOMAIN_SOCKETS. Passing the option to a build without it makes
    // Hatari exit with "Unrecognized option", so it must be gated rather than
    // passed unconditionally (docs/PLAN.md §5 rule 12). Nothing in the IDE
    // depends on it yet: all debugger commands travel over stdin.
    if (m_caps.hasControlSocket)
        config.controlSocketPath = sessionDir + QStringLiteral("/ctl.sock");
    else
        config.controlSocketPath.clear();

    // Embedded display: name the container's X11 window so Hatari reparents its
    // SDL window into it. Gated on the platform and the control socket, so a
    // Wayland-native session or a socket-less build falls back to a separate
    // window even when the option is on. The dock is shown and the widget
    // realized now, because winId() has to be a live X11 window before Hatari
    // starts or there is nothing to reparent into.
    if (m_embeddedDisplay && canEmbedDisplay() && m_display) {
        m_displayDock->setVisible(true);
        m_display->setVisible(true);
        config.parentWindowId = QString::number(m_display->winId());
    } else {
        config.parentWindowId.clear();
    }

    if (!m_caps.hasDebugExcept)
        config.debugExceptions.clear();

    QString error;
    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(sessionDir, m_caps, &error);
    if (config.bootstrapScriptPath.isEmpty()) {
        QMessageBox::critical(this, tr("Run"), error);
        return;
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
    if (!m_settings.tosPath.isEmpty()) {
        for (const TosRom &candidate : roms) {
            if (candidate.path == m_settings.tosPath) {
                rom = candidate;
                break;
            }
        }
        if (rom.path.isEmpty()) {
            QMessageBox::warning(
                this, tr("Run"),
                tr("The configured TOS ROM is missing:\n\n%1\n\nFalling back to the best "
                   "ROM for the %2.")
                    .arg(m_settings.tosPath, machineDisplayName(m_settings.machine)));
        }
    }
    if (rom.path.isEmpty())
        rom = selectPreferredRom(roms, m_settings.machine);
    if (rom.path.isEmpty()) {
        const QStringList searched = paths::tosSearchPaths();
        QMessageBox::critical(
            this, tr("Run"),
            tr("No TOS ROM image found.\n\n"
               "Original TOS images cannot be bundled with PiST, so one has to be supplied "
               "separately. Place a ROM image in one of these directories, or point "
               "$PIST_TOS_DIR at the directory containing it:\n\n%1")
                .arg(searched.isEmpty() ? tr("(no searchable directory found)")
                                        : searched.join(QLatin1Char('\n'))));
        return;
    }
    // Three distinct cases, because collapsing them produces either a false
    // error or the silent hang this check exists to prevent:
    //
    //   known too old  -> refuse, with the version we read
    //   known good     -> proceed
    //   unknown        -> warn and ask; a pre-1.04 ROM here boots the emulator
    //                     but never starts the program, so the IDE would wait
    //                     forever for a breakpoint that never comes
    if (rom.knownTooOldForAutostart()) {
        QMessageBox::critical(
            this, tr("Run"),
            tr("This ROM reports TOS %1, which cannot autostart a program from a GEMDOS "
               "hard disk: Hatari requires TOS 1.04 or later.\n\nChoose a newer ROM.")
                .arg(rom.versionText()));
        return;
    }

    if (!rom.supportsAutostart()) {
        QString detail;
        if (rom.versionKnown) {
            // A version was read from the filename only. Hatari never consults
            // filenames, so this is not evidence about what it will do.
            detail = tr("Its filename suggests TOS %1, but the version field in the image "
                        "header could not be read, so this cannot be confirmed.")
                         .arg(rom.versionText());
        } else {
            detail = tr("Its TOS version could not be determined.");
        }

        const auto answer = QMessageBox::warning(
            this, tr("Run"),
            tr("Autostarting a program requires TOS 1.04 or later.\n\n%1\n\n"
               "If this image is older than 1.04 the program will not start and debugging "
               "will not attach.\n\nTry to run anyway?")
                .arg(detail),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    config.tosPath = rom.path;

    // A new session relocates the program, so previously resolved addresses are
    // meaningless and arming must happen again after the next entry stop.
    m_sessionArmed = false;
    m_breakpointsArmedThisSession = false;
    m_bases = LineMap::SectionBases();

    m_host->setCapabilities(m_caps);
    if (!m_host->start(config, &error)) {
        QMessageBox::critical(this, tr("Run"), error);
        return;
    }

    m_log->appendPlainText(tr("Session started in %1").arg(sessionDir));
}

void MainWindow::stopSession()
{
    m_host->stop();

    if (!m_currentSessionDir.isEmpty()) {
        paths::removeSessionDir(m_currentSessionDir);
        m_currentSessionDir.clear();
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (!maybeSave()) {
        event->ignore();
        return;
    }

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
    // The stop this produces refreshes the state via onDebuggerStopped.
    m_host->step();
}

void MainWindow::stepOver()
{
    m_host->stepOver();
}

void MainWindow::resume()
{
    m_host->resume();
}

void MainWindow::onDebuggerStopped()
{
    // Every stop refreshes the views and the editor's execution line, so the
    // display always shows where the machine actually stopped — on a step, on a
    // breakpoint, or on an exception. The entry stop additionally runs the
    // two-phase attach below, once per session.
    if (m_sessionArmed) {
        m_host->refresh();
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
    m_host->command(QStringLiteral("symbols prg"));
    m_host->command(QStringLiteral("info basepage"));
    m_host->dumpRegisters();
    m_host->command(QStringLiteral("d"));
    // Arming happens in onStateUpdated, when the bases those commands report
    // actually arrive — not here, where they have not been read yet.
}

void MainWindow::rebuildProgramMap()
{
    m_programMap.clear();

    const QStringList listings = m_build->listingFiles();
    const QStringList objects = m_build->objectFiles();
    QFileInfo sourceInfo(m_editor->filePath());

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
            auto *item = new QTreeWidgetItem(m_problems);
            item->setText(0, tr("(build)"));
            item->setText(2, tr("Could not read the link map, so only the first module "
                                "can be mapped to source: %1").arg(error));
            item->setForeground(2, QColor(0xc0, 0x20, 0x20));
        }
    }

    const QStringList unplaced = m_programMap.unplacedModules();
    if (!unplaced.isEmpty()) {
        m_log->appendPlainText(
            tr("[link map] no placement for: %1").arg(unplaced.join(QLatin1String(", "))));
    }
}

void MainWindow::armBreakpoints()
{
    m_breakpointsArmedThisSession = true;
    const ArmPlan plan = planBreakpoints(m_breakpoints, m_programMap);

    // Clear before arming. A rebuilt program can occupy different addresses, so
    // leaving old breakpoints in place would silently break on whatever code now
    // lives at those addresses.
    m_host->clearBreakpoints();

    for (const QString &command : plan.commands)
        m_host->armBreakpoint(command);

    // Watchpoints arm alongside the source breakpoints. They are address-based,
    // so unlike source lines they are valid before the program even starts.
    for (const Watchpoint &wp : m_watchpoints)
        m_host->armBreakpoint(wp.command());

    for (const QString &label : plan.unresolved) {
        m_log->appendPlainText(
            tr("[breakpoints] %1 has no address (it emits no code or data)").arg(label));
    }

    if (!plan.commands.isEmpty()) {
        m_log->appendPlainText(
            tr("[breakpoints] armed %1 of %2")
                .arg(plan.commands.size())
                .arg(m_breakpoints.size()));
    }

    // Show the resolved addresses and flag any breakpoint that could not be
    // placed, so "why did my breakpoint not fire" is answerable at a glance.
    if (m_breakpointPanel) {
        m_breakpointPanel->setResolvable(true);
        m_breakpointPanel->setBreakpoints(mergeResolved(plan));
        m_breakpointPanel->setWatchpoints(m_watchpoints);
    }
}

void MainWindow::addWatchpoint()
{
    bool accepted = false;
    const QString text = QInputDialog::getText(
        this, tr("Add watchpoint"),
        tr("Address to watch (hex), with optional .b/.w/.l width:"),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;

    QString error;
    if (!addWatchpointAddress(text, &error))
        QMessageBox::warning(this, tr("Add watchpoint"), error);
}

bool MainWindow::addWatchpointAddress(const QString &text, QString *error)
{
    // A watchpoint breaks when the value at an address changes. Hatari has no
    // data watchpoints, so it is armed as a self-inequality breakpoint, which is
    // the debugger's change-tracking (see debug/Watchpoint.h). Width defaults to
    // word; a trailing b/w/l overrides it.
    static const QRegularExpression re(
        QStringLiteral("^\\s*(?:\\$|0x)?([0-9a-fA-F]+)(?:\\.(b|w|l))?\\s*$"));
    const auto match = re.match(text);
    if (!match.hasMatch()) {
        if (error)
            *error = tr("Could not read an address from '%1'.").arg(text);
        return false;
    }

    Watchpoint wp;
    wp.address = match.captured(1).toUInt(nullptr, 16);
    if (!match.captured(2).isEmpty())
        wp.width = match.captured(2).at(0).toLatin1();

    if (wp.address == 0) {
        if (error)
            *error = tr("Address 0 is not a useful thing to watch.");
        return false;
    }

    m_watchpoints.append(wp);
    m_log->appendPlainText(tr("[watchpoint] %1").arg(wp.label()));
    // Re-arm so it takes effect now if a session is already stopped, or on the
    // next run otherwise. Without a session there is nothing to arm, and
    // `EmulatorHost::command` would log "No emulator session is running" once per
    // command, so just show it and let the next Run arm it.
    if (m_host->isRunning()) {
        armBreakpoints();
    } else if (m_breakpointPanel) {
        m_breakpointPanel->setWatchpoints(m_watchpoints);
    }
    return true;
}

void MainWindow::removeWatchpoint(int index)
{
    if (index < 0 || index >= m_watchpoints.size())
        return;
    m_watchpoints.removeAt(index);
    armBreakpoints();
}

QList<Breakpoint> MainWindow::mergeResolved(const ArmPlan &plan) const
{
    QList<Breakpoint> merged = m_breakpoints;

    // Reset first: a breakpoint that was resolved on a previous run must not keep
    // showing a stale address from before the program was relocated.
    for (Breakpoint &bp : merged) {
        bp.resolved = false;
        bp.address = 0;
    }

    for (const Breakpoint &armed : plan.armed) {
        for (Breakpoint &bp : merged) {
            if (bp.line == armed.line && bp.file == armed.file) {
                bp.address = armed.address;
                bp.resolved = true;
                break;
            }
        }
    }

    return merged;
}

void MainWindow::refreshBreakpointMarkers()
{
    const QString file = QFileInfo(m_editor->filePath()).fileName();
    QList<int> lines;
    for (const Breakpoint &bp : m_breakpoints) {
        if (bp.file == file)
            lines.append(bp.line);
    }
    m_editor->setBreakpointLines(lines);

    if (m_breakpointPanel)
        m_breakpointPanel->setBreakpoints(m_breakpoints);
}

void MainWindow::toggleBreakpointAtLine(int line)
{
    if (m_editor->filePath().isEmpty() || line <= 0)
        return;

    const QString file = QFileInfo(m_editor->filePath()).fileName();

    // Breakpoints are keyed by base name, so two modules linked from different
    // directories under the same file name (`util.s`) collide here and in the
    // gutter. Re-keying on the full path is a larger change (the panel, the
    // listings and the linker map all identify modules the same way); until then
    // this is the documented limitation.
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [&](const Breakpoint &bp) {
                               return bp.line == line && bp.file == file;
                           });
    if (it != m_breakpoints.end())
        m_breakpoints.erase(it);
    else
        m_breakpoints.append(Breakpoint{file, line, QString(), true, 0, false});

    refreshBreakpointMarkers();

    // Re-arm immediately if a session is already running and stopped, so the new
    // breakpoint takes effect without restarting.
    if (m_sessionArmed && m_host->isStopped() && m_bases.isValid())
        armBreakpoints();
}

void MainWindow::removeBreakpoint(const QString &file, int line)
{
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [&](const Breakpoint &bp) {
                               return bp.line == line && bp.file == file;
                           });
    if (it == m_breakpoints.end())
        return;

    m_breakpoints.erase(it);
    refreshBreakpointMarkers();

    if (m_sessionArmed && m_host->isStopped() && m_bases.isValid())
        armBreakpoints();
}

void MainWindow::goToBreakpoint(const QString &file, int line)
{
    // Only navigate within the file that is open; switching documents is not
    // supported yet, so silently doing nothing is better than jumping to the
    // wrong line in the wrong file.
    if (m_editor->filePath().isEmpty() || !LineMap::sameSource(file, m_editor->filePath()))
        return;
    m_editor->gotoLine(line);
}

void MainWindow::editBreakpointCondition(int line)
{
    if (m_editor->filePath().isEmpty() || line <= 0)
        return;

    const QString file = QFileInfo(m_editor->filePath()).fileName();
    auto findBreakpoint = [&] {
        return std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                            [&](const Breakpoint &bp) {
                                return bp.line == line && bp.file == file;
                            });
    };

    auto it = findBreakpoint();
    const bool existed = it != m_breakpoints.end();

    bool accepted = false;
    const QString condition = QInputDialog::getText(
        this, tr("Breakpoint condition"),
        tr("Extra condition for %1:%2 (ANDed with the program counter).\n\n"
           "Hatari has no memory watchpoints, so this is how you watch a value,\n"
           "for example:  d0 = $1234   or   (buf) = $ff")
            .arg(file)
            .arg(line),
        QLineEdit::Normal, existed ? it->condition : QString(), &accepted);
    if (!accepted)
        return;

    if (!existed) {
        // Nothing to edit on an empty line: create the breakpoint only now the
        // dialog was accepted, so cancelling does not leave a condition-less one
        // behind.
        m_breakpoints.append(Breakpoint{file, line, QString(), true, 0, false});
        it = findBreakpoint();
        refreshBreakpointMarkers();
    }

    it->condition = condition.trimmed();
    if (m_sessionArmed && m_host->isStopped() && m_bases.isValid())
        armBreakpoints();
}

void MainWindow::clearAllBreakpoints()
{
    m_breakpoints.clear();
    refreshBreakpointMarkers();
    if (m_host->isRunning())
        m_host->clearBreakpoints();
    m_log->appendPlainText(tr("[breakpoints] all cleared"));
}

void MainWindow::onStateUpdated(const MachineState &state)
{
    m_lastState = state;
    m_registers->setState(state);
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

        // Arm breakpoints when the bases they resolve against actually arrive —
        // here, where they are set — not on a zero-delay timer that always fires
        // before the basepage response does (P1 root cause A).
        if (m_sessionArmed && !m_breakpointsArmedThisSession)
            armBreakpoints();

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
        m_host->command(QStringLiteral("info ") + m_hardware->subject());

    locationFromPc(state.pc);
}

bool MainWindow::canEmbedDisplay() const
{
    // Both ends must be X11 clients of the same display, and the size report
    // that sizes the container travels on the control socket.
    return QGuiApplication::platformName() == QLatin1String("xcb")
        && m_caps.hasControlSocket;
}

void MainWindow::setDisplayEmbedded(bool on)
{
    m_embeddedDisplay = on;
    QSettings().setValue(QStringLiteral("display/embedded"), on);
    if (m_displayDock)
        m_displayDock->setVisible(on);
    // A running session keeps the display mode it was launched with; the change
    // takes effect on the next Run.
}

void MainWindow::updateEmbedActionState()
{
    if (!m_actEmbedDisplay)
        return;
    const bool can = canEmbedDisplay();
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
    if (!m_lastState.regs.valid)
        return tr("no session state\n");

    const Registers &r = m_lastState.regs;
    QStringList lines;
    lines << QStringLiteral("pc  %1").arg(m_lastState.pc, 8, 16, QLatin1Char('0'));
    for (int i = 0; i < 8; ++i)
        lines << QStringLiteral("d%1  %2").arg(i).arg(r.d[i], 8, 16, QLatin1Char('0'));
    for (int i = 0; i < 8; ++i)
        lines << QStringLiteral("a%1  %2").arg(i).arg(r.a[i], 8, 16, QLatin1Char('0'));
    lines << QStringLiteral("sr  %1").arg(r.sr, 4, 16, QLatin1Char('0'));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

void MainWindow::locationFromPc(quint32 pc)
{
    if (!m_bases.isValid() || m_programMap.isEmpty()) {
        m_editor->clearCurrentExecutionLine();
        return;
    }

    LineMap::Address address;
    if (!m_programMap.lineFor(pc, &address)) {
        m_editor->clearCurrentExecutionLine();
        return;
    }

    // Only follow the PC into the file that is actually open. The comparison
    // must be path-tolerant: the listing records whatever path was passed to
    // vasm (usually absolute), while the editor knows the file it was opened
    // with (usually just a name).
    if (!m_editor->filePath().isEmpty()
        && !LineMap::sameSource(address.file, m_editor->filePath())) {
        return;
    }

    m_editor->setCurrentExecutionLine(address.line);
    m_editor->gotoLine(address.line);
}

} // namespace pist
