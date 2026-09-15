// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/DebugBackend.h"
#include "ui/Appearance.h"
#include "build/FloppyImage.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "image/ImageDocument.h"
#include "image/StFormats.h"
#include "toolchain/Toolchain.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "ui/BreakpointPanel.h"
#include "ui/DisassemblyView.h"
#include "ui/EmulatorDisplayWidget.h"
#include "ui/EmbedX11.h"
#include "ui/FileBrowser.h"
#include "ui/ImageEditor.h"
#include "ui/NewImageDialog.h"
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
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
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

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
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
    setCentralWidget(m_tabs);
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
    // initial visibility matches it.
    m_embeddedDisplay = QSettings().value(QStringLiteral("display/embedded"), false).toBool();

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
    resize(1280, 860);
}

CodeEditor *MainWindow::addEditorTab(const QString &path)
{
    // A pristine tab (no path, no content, unmodified) is reused rather than
    // left behind as an empty first tab, matching how editors treat an
    // untouched "untitled" buffer.
    if (!path.isEmpty() && m_tabs->count() == 1) {
        auto *only = qobject_cast<CodeEditor *>(m_tabs->widget(0));
        if (isPristineEditor(only)) {
            if (!only->loadFile(path)) {
                QMessageBox::warning(this, tr("Open"), tr("Could not open %1").arg(path));
                return nullptr;
            }
            updateTabTitle(only);
            return only;
        }
    }

    auto *editor = new CodeEditor(this);
    wireEditor(editor);
    if (!path.isEmpty() && !editor->loadFile(path)) {
        QMessageBox::warning(this, tr("Open"), tr("Could not open %1").arg(path));
        editor->deleteLater();
        return nullptr;
    }
    const int index = m_tabs->addTab(editor, editor->displayName());
    m_tabs->setCurrentIndex(index);
    updateTabTitle(editor);
    return editor;
}

ImageEditor *MainWindow::addImageTab(const QString &path)
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
    if (!path.isEmpty()) {
        const bool ok = isPimPath(path) ? editor->loadFile(path) : editor->importFile(path, false);
        if (!ok) {
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

    connect(editor, &CodeEditor::gutterClicked, this, [this](int line, Qt::MouseButton button) {
        if (button == Qt::LeftButton)
            toggleBreakpointAtLine(line);
        else if (button == Qt::RightButton)
            editBreakpointCondition(line);
    });
    connect(editor, &CodeEditor::gutterContextMenuRequested, this,
            [this, editor](int line, const QPoint &pos) {
                QMenu menu;
                const bool hasBreakpoint = std::any_of(
                    m_breakpoints.cbegin(), m_breakpoints.cend(),
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

    if (m_actExportImage)
        m_actExportImage->setEnabled(m_image != nullptr);

    const QString path = m_editor ? m_editor->filePath()
                                  : (m_image ? m_image->filePath() : QString());
    if (!path.isEmpty() && m_fileBrowser)
        m_fileBrowser->showFor(path);
    if (m_editor)
        refreshBreakpointMarkers();
    updateModifiedState();
}

void MainWindow::onTabCloseRequested(int index)
{
    QWidget *widget = m_tabs->widget(index);
    if (auto *editor = qobject_cast<CodeEditor *>(widget)) {
        if (!maybeSaveEditor(editor))
            return;
    } else if (auto *image = qobject_cast<ImageEditor *>(widget)) {
        if (!maybeSaveImage(image))
            return;
    }

    m_tabs->removeTab(index);
    if (widget)
        widget->deleteLater();

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
    const ToolInfo assembler = toolchain::findAssembler();
    m_build->setAssemblerPath(assembler.found() ? assembler.path
                                                : QStringLiteral("vasmm68k_mot"));
    m_caps = probeHatari(toolchain::findEmulator().path);
    updateEmbedActionState();
    m_statusToolchain->setText(
        QStringLiteral("vasm: %1").arg(QFileInfo(m_build->assemblerPath()).fileName()));
    m_statusEmulator->setText(m_caps.summary());
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
    m_actOpen = new QAction(tr("&Open…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openFile);

    m_actNewImage = new QAction(tr("New &Image…"), this);
    connect(m_actNewImage, &QAction::triggered, this, &MainWindow::newImage);

    m_actImportImage = new QAction(tr("&Import Image…"), this);
    connect(m_actImportImage, &QAction::triggered, this, &MainWindow::importImage);

    m_actExportImage = new QAction(tr("&Export Image…"), this);
    m_actExportImage->setEnabled(false);
    connect(m_actExportImage, &QAction::triggered, this, &MainWindow::exportImage);

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

    m_actPause = new QAction(tr("&Pause"), this);
    m_actPause->setShortcut(QKeySequence(Qt::Key_F6));
    m_actPause->setEnabled(false);
    connect(m_actPause, &QAction::triggered, this, &MainWindow::pauseSession);

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
            return;
        }
        if (m_consoleInput)
            m_consoleInput->setEnabled(running);
        m_statusEmulator->setText(m_caps.valid ? m_caps.summary() : tr("Not running"));
        if (m_actPause)
            m_actPause->setEnabled(false);
        if (m_sessionArmed) {
            m_sessionArmed = false;
            if (m_actStep) m_actStep->setEnabled(false);
            if (m_actStepOver) m_actStepOver->setEnabled(false);
            if (m_actResume) m_actResume->setEnabled(false);
            m_log->appendPlainText(tr("[session] emulator is no longer running"));
        }
    });
    connect(m_host, &IDebugBackend::memoryDumpReady, this,
            [this](quint32, const QString &response, int tag) {
                // Route the dump to the pane that asked for it, by tag.
                if (auto *view = m_memoryPanes.value(tag, nullptr))
                    view->applyDump(response);
            });
    // `info <subject>` responses feed the hardware view. Routed by command text:
    // `info basepage` is part of the normal refresh and must not land here.
    connect(m_host, &IDebugBackend::commandFinished, this,
            [this](const QString &command, const QString &response) {
                if (m_hardware && command == QLatin1String("info ") + m_hardware->subject())
                    m_hardware->setInfo(response);
                else if (m_pcHistory && command.startsWith(QLatin1String("history ")))
                    m_pcHistory->setHistory(response);
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
        m_actStep->setEnabled(stopped);
        m_actStepOver->setEnabled(stopped);
        m_actResume->setEnabled(stopped);
        m_statusEmulator->setText(stopped ? tr("Stopped in debugger") : tr("Running"));
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
        if (stopped)
            onDebuggerStopped();
    });

    connect(m_host, &IDebugBackend::stackDumpReady, this,
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
}


void MainWindow::createMenus()
{
    auto *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(m_actNewImage);
    fileMenu->addAction(m_actOpen);
    fileMenu->addAction(m_actSave);
    fileMenu->addSeparator();
    fileMenu->addAction(m_actImportImage);
    fileMenu->addAction(m_actExportImage);
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
    m_viewMenu->addAction(m_actEmbedDisplay);

    auto *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(tr("Set up tools and ROMs…"), this, &MainWindow::showToolSetup);

    auto *runMenu = menuBar()->addMenu(tr("&Run"));
    runMenu->addAction(m_actBuild);
    runMenu->addAction(m_actRun);
    runMenu->addAction(m_actStop);
    runMenu->addAction(m_actPause);
    runMenu->addSeparator();
    runMenu->addAction(m_actResume);
    runMenu->addAction(m_actStep);
    runMenu->addAction(m_actStepOver);
    runMenu->addSeparator();
    runMenu->addAction(m_actClearBreakpoints);
    runMenu->addAction(m_actAddWatchpoint);
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
    // persisted and restored across runs.
    setDockNestingEnabled(true);
    qApp->installEventFilter(this);
    setObjectName(QStringLiteral("mainWindow"));

    QList<QDockWidget *> debugTabs;

    // --- left: project navigation --------------------------------------------
    m_fileBrowser = new FileBrowser(this);
    addDockWidget(Qt::LeftDockWidgetArea,
                  makeDock(tr("Project files"), QStringLiteral("projectFilesDock"), m_fileBrowser));
    connect(m_fileBrowser, &FileBrowser::fileActivated, this, &MainWindow::openPath);
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

    // --- right, top: the emulator display, which wants to be prominent --------
    // It lives in a dock shown only when the embedded-display option is on; in
    // separate-window mode it is hidden and the widget is unused.
    m_display = new EmulatorDisplayWidget(this);
    m_displayDock = makeDock(tr("Emulator"), QStringLiteral("emulatorDisplayDock"), m_display);
    m_displayDock->setVisible(m_embeddedDisplay);
    addDockWidget(Qt::RightDockWidgetArea, m_displayDock);

    // --- right, below the display: the debug views, tabbed together -----------
    m_registers = new RegistersView(this);
    m_disassembly = new DisassemblyView(this);
    m_stack = new StackView(this);
    m_hardware = new HardwareView(this);
    m_breakpointPanel = new BreakpointPanel(this);

    m_pcHistory = new PcHistoryView(this);
    debugTabs << makeDock(tr("Registers"), QStringLiteral("registersDock"), m_registers)
              << makeDock(tr("Disassembly"), QStringLiteral("disassemblyDock"), m_disassembly)
              << makeDock(tr("Stack"), QStringLiteral("stackDock"), m_stack)
              << makeDock(tr("Hardware"), QStringLiteral("hardwareDock"), m_hardware)
              << makeDock(tr("PC history"), QStringLiteral("pcHistoryDock"), m_pcHistory)
              << makeDock(tr("Breakpoints"), QStringLiteral("breakpointsDock"), m_breakpointPanel);

    addDockWidget(Qt::RightDockWidgetArea, debugTabs.first());
    for (int i = 1; i < debugTabs.size(); ++i)
        tabifyDockWidget(debugTabs.first(), debugTabs.at(i));

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
    connect(m_hardware, &HardwareView::subjectChanged, this,
            [this](const QString &subject) {
                if (m_host->isRunning())
                    m_host->command(QStringLiteral("info ") + subject);
            });

    // A register edit becomes a debugger write: `r <reg>=<value>` (the '=' is
    // mandatory in Hatari, and it prints nothing on success). Only sent when
    // stopped, which is when editing is enabled anyway.
    connect(m_registers, &RegistersView::registerEdited, this,
            [this](const QString &regName, quint32 value) {
                setRegister(regName, value);
            });

    // --- bottom: problems, console and memory, tabbed -------------------------
    // Problems and the console are ordinary docks rather than tabs in a
    // QTabWidget inside one dock, so the bottom area is a normal tab group: you
    // can drag panels into it and out of it like any other, and the Move-to
    // menu works on their tabs.
    m_problems = new QTreeWidget(this);
    m_problems->setHeaderLabels({tr("File"), tr("Line"), tr("Message")});
    m_problems->header()->setStretchLastSection(true);
    connect(m_problems, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        const int line = item->text(1).toInt();
        if (line > 0 && m_editor)
            m_editor->gotoLine(line);
    });

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    appearance::markMono(m_log);

    m_problemsDock = makeDock(tr("Problems"), QStringLiteral("problemsDock"), m_problems);
    addDockWidget(Qt::BottomDockWidgetArea, m_problemsDock);
    // A command entry under the log turns the console into a live debugger
    // console: commands go through the backend's normal queue and the response
    // is appended when it arrives (matched by command text in the
    // commandFinished handler in wireBackend).
    m_consoleInput = new QLineEdit(this);
    m_consoleInput->setObjectName(QStringLiteral("consoleInput"));
    appearance::markMono(m_consoleInput);
    m_consoleInput->setPlaceholderText(
        tr("Debugger command (e.g. r, d, m $12596 20)"));
    m_consoleInput->setEnabled(false);
    connect(m_consoleInput, &QLineEdit::returnPressed, this, [this] {
        const QString cmd = m_consoleInput->text().trimmed();
        if (cmd.isEmpty())
            return;
        m_consoleInput->clear();
        sendConsoleCommand(cmd);
    });

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

    // --- default arrangement captured, then the user's arrangement restored ---
    // The View menu gets one show/hide action per dock, plus a way back to the
    // default layout. Built here rather than in createMenus because the docks do
    // not exist yet when the menus are made.
    if (m_viewMenu) {
        m_viewMenu->addSeparator();
        const QList<QDockWidget *> allDocks = {
            m_problemsDock, consoleDock, m_memoryDock, m_displayDock,
            debugTabs.first(), debugTabs.at(1), debugTabs.at(2), debugTabs.at(3), debugTabs.at(4),
            qobject_cast<QDockWidget *>(m_fileBrowser->parentWidget())
        };
        for (QDockWidget *dock : allDocks) {
            if (dock)
                m_viewMenu->addAction(dock->toggleViewAction());
        }
        m_viewMenu->addSeparator();
        m_viewMenu->addAction(tr("Reset layout"), this, &MainWindow::resetToDefaultLayout);
    }

    // The factory arrangement, so Reset layout has something to return to.
    m_defaultLayoutState = saveState();

    // Restore the user's own arrangement, if any; the default above is what a
    // first run gets.
    const QByteArray saved =
        QSettings().value(QStringLiteral("layout/state")).toByteArray();
    if (!saved.isEmpty())
        restoreState(saved);

    // The embedded-display toggle owns the Emulator dock's visibility, so it is
    // applied after any restored layout, which would otherwise override it.
    m_displayDock->setVisible(m_embeddedDisplay);
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
    if (m_host->isStopped()) {
        if (initialAddress)
            view->goToAddress(initialAddress);
        else
            view->refresh();
    }
}

void MainWindow::resetToDefaultLayout()
{
    if (m_defaultLayoutState.isEmpty())
        return;
    restoreState(m_defaultLayoutState);
    // Keep the toggle authoritative over the Emulator dock's visibility.
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
    bar->addSeparator();
    bar->addAction(m_actClearBreakpoints);
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
    m_actClearBreakpoints->setIcon(appearance::icon(Icon::ClearBreakpoints));
}

void MainWindow::applyAppearance()
{
    appearance::applyTheme();
    setWindowIcon(appearance::windowIcon());
    applyIcons();
    appearance::applyMonoFonts(this);
    for (CodeEditor *editor : openEditors())
        editor->applyFontPreferences();
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

void MainWindow::openPath(const QString &path)
{
    if (path.isEmpty())
        return;

    // Already open documents are raised, not reloaded: the user's undo
    // history and cursor position in the existing tab are kept.
    if (CodeEditor *open = editorForPath(path)) {
        m_tabs->setCurrentWidget(open);
    } else if (ImageEditor *open = imageForPath(path)) {
        m_tabs->setCurrentWidget(open);
    } else if (isPimPath(path) || isImportableImagePath(path)) {
        if (!addImageTab(path))
            return;
    } else if (!addEditorTab(path)) {
        return;
    }

    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    if (!isPimPath(path) && !isImportableImagePath(path))
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
    settings::rememberLastProject(path, source);
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

    settings::rememberLastProject(path, source);
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
    settings::rememberLastProject(path, source);
}

void MainWindow::syncFileBrowserDisks()
{
    if (m_fileBrowser)
        m_fileBrowser->setFloppyImages(m_settings.floppyImages);
}

void MainWindow::editSettings()
{
    SettingsDialog dialog(m_settings, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    m_settings = dialog.settings();

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
    syncFileBrowserDisks();
    m_log->appendPlainText(tr("[project] loaded %1").arg(projectPath));
    statusBar()->showMessage(
        tr("Project settings: %1, %2, %3 MiB")
            .arg(machineDisplayName(m_settings.machine), m_settings.monitor)
            .arg(m_settings.memSizeMiB),
        6000);
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
            }
        } else if (!m_image->saveFile(m_image->filePath())) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2")
                                      .arg(m_image->filePath(), m_image->lastError()));
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
        m_editor->saveFile(path);
    } else if (!m_editor->saveFile(m_editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(m_editor->filePath(), m_editor->lastError()));
    }
    updateTabTitle(m_editor);
    updateModifiedState();
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

void MainWindow::exportImage()
{
    if (!m_image)
        return;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export image"), QString(),
        tr("Degas Elite (*.pi1);;NeoChrome (*.neo);;IFF/ILBM (*.iff);;PNG (*.png);;"
           "STOS sprite bank (*.mbk);;Assembler include (*.s);;Bitplane binary (*.bin)"));
    if (path.isEmpty())
        return;
    if (!m_image->exportFile(path)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Could not export %1: %2").arg(path, m_image->lastError()));
        return;
    }
    statusBar()->showMessage(tr("Exported %1").arg(path), 4000);
}

void MainWindow::build()
{
    const QString source = buildSourcePath();
    if (source.isEmpty()) {
        QMessageBox::information(this, tr("Build"),
                                 tr("Open an assembly source file first."));
        return;
    }

    // A failed save must stop the build: otherwise the assembler runs on the
    // previous on-disk contents and reports a result for code the user is not
    // looking at.
    for (CodeEditor *editor : openEditors()) {
        if (editor->filePath().isEmpty() || !editor->isModifiedSinceLoad())
            continue;
        if (!editor->saveFile(editor->filePath())) {
            QMessageBox::critical(this, tr("Build"),
                                  tr("Could not save %1: %2")
                                      .arg(editor->filePath(), editor->lastError()));
            return;
        }
    }

    m_problems->clear();
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

        auto *item = new QTreeWidgetItem(m_problems);
        item->setText(0, file.isEmpty() ? tr("(build)") : file);
        item->setText(1, line > 0 ? QString::number(line) : QString());
        item->setText(2, d.message);
        if (d.severity == Diagnostic::Error)
            item->setForeground(2, appearance::colors().error);

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
    if (buildSourcePath().isEmpty()) {
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
    const QFileInfo info(buildSourcePath());
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

    // Probe the binary actually being launched (not the default-path one the
    // status bar probed at startup): the transport selection and the option
    // gating below must match this emulator.
    const HatariCapabilities launchedCaps = probeHatari(emulator.path);

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
    // realized now, because winId() has to be a live X11 window before Hatari
    // starts or there is nothing to reparent into.
    if (m_embeddedDisplay && canEmbedDisplay(launchedCaps) && m_display) {
        m_displayDock->setVisible(true);
        m_display->setVisible(true);
        config.parentWindowId = QString::number(m_display->winId());
    } else {
        config.parentWindowId.clear();
    }

    if (!launchedCaps.hasDebugExcept)
        config.debugExceptions.clear();

    QString error;

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
    if (m_settings.debugBackend == QLatin1String("hrdb")) {
        if (!launchedCaps.hasHrdb) {
            QMessageBox::critical(this, tr("Run"),
                tr("The debug transport is set to HRDB, but %1 is a stock Hatari, which "
                   "has no HRDB listener. Set the transport to Native/Auto, or use the "
                   "hrdb-main fork.").arg(emulator.path));
            return;
        }
        wanted = BackendKind::Hrdb;
    } else if (m_settings.debugBackend == QLatin1String("native")) {
        if (launchedCaps.hasHrdb) {
            QMessageBox::critical(this, tr("Run"),
                tr("The debug transport is set to Native, but %1 is the hrdb-main fork, "
                   "whose stdin debugger is never read once its listener binds. Set the "
                   "transport to HRDB/Auto.").arg(emulator.path));
            return;
        }
        wanted = BackendKind::Native;
    } else {
        wanted = launchedCaps.hasHrdb ? BackendKind::Hrdb : BackendKind::Native;
    }
    if (m_host->kind() != wanted) {
        m_host->stop();
        delete m_host;
        m_host = createBackend(wanted, this);
        wireBackend();
    }

    config.bootstrapScriptPath =
        EmulatorHost::writeBootstrapScript(sessionDir, launchedCaps, &error);
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
    //   known too old  -> AUTO-folder floppy fallback
    //   known good     -> the GEMDOS-HD path
    //   unknown        -> warn and ask; if the user proceeds, the same floppy
    //                     fallback, which works on every TOS version
    bool floppyBoot = false;
    if (rom.knownTooOldForAutostart()) {
        // GEMDOS HD does not exist below TOS 1.04 (Hatari refuses it), but
        // every TOS executes AUTO/*.PRG from the boot floppy — the fallback
        // instead of the old refusal (docs/PLAN.md §5 rule 3).
        floppyBoot = true;
        m_log->appendPlainText(
            tr("[run] TOS %1 has no GEMDOS-HD autostart; booting from an "
               "AUTO-folder floppy instead.").arg(rom.versionText()));
    }

    if (!floppyBoot && !rom.supportsAutostart()) {
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
            tr("This ROM cannot be confirmed to support autostarting a program from a "
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

    if (floppyBoot) {
        // Build the session's AUTO-folder floppy (AUTO/PROG.PRG plus an empty
        // EMUDESK.INF for the debug-except deferral), and route the session
        // around the GEMDOS HD: --disk-a, no positional, no -d, and --debug to
        // arm the exception mask the INF path would have armed.
        config.bootFloppyPath = sessionDir + QStringLiteral("/auto.st");
        QString floppyError;
        if (!floppy::writeAutoFolderImage(config.bootFloppyPath, prg, &floppyError)) {
            QMessageBox::critical(this, tr("Run"),
                                  tr("Could not build the AUTO-folder floppy:\n%1")
                                      .arg(floppyError));
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
                m_log->appendPlainText(
                    tr("[run] TOS %1 autostarts from drive A:, so '%2' is in B:.")
                        .arg(rom.versionText(), QFileInfo(userA).fileName()));
            } else {
                m_log->appendPlainText(
                    tr("[run] TOS %1 needs drive A: to autostart; '%2' was not mounted.")
                        .arg(rom.versionText(), QFileInfo(userA).fileName()));
            }
            config.floppyImages[0].clear();
        }
    }
    config.tosPath = rom.path;

    // A new session relocates the program, so previously resolved addresses are
    // meaningless and arming must happen again after the next entry stop.
    m_sessionArmed = false;
    m_breakpointsArmedThisSession = false;
    m_bases = LineMap::SectionBases();

    m_host->setCapabilities(launchedCaps);
    if (!m_host->start(config, &error)) {
        QMessageBox::critical(this, tr("Run"), error);
        return;
    }

    m_log->appendPlainText(tr("Session started in %1").arg(sessionDir));
    const QString diskA = !config.floppyImages.value(0).isEmpty()
        ? config.floppyImages.at(0)
        : config.bootFloppyPath;
    if (!diskA.isEmpty())
        m_log->appendPlainText(tr("[run] Floppy A: %1").arg(diskA));
    if (!config.floppyImages.value(1).isEmpty())
        m_log->appendPlainText(tr("[run] Floppy B: %1").arg(config.floppyImages.at(1)));
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

    // The panel arrangement persists across runs, so a layout the user has
    // arranged to taste is there next time (docs/FUTURE.md §7).
    QSettings().setValue(QStringLiteral("layout/state"), saveState());

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


void MainWindow::pauseSession()
{
    m_host->pause();
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
            auto *item = new QTreeWidgetItem(m_problems);
            item->setText(0, tr("(build)"));
            item->setText(2, tr("Could not read the link map, so only the first module "
                                "can be mapped to source: %1").arg(error));
            item->setForeground(2, appearance::colors().error);
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

void MainWindow::debugCommand(const QString &command)
{
    // Route through the host; the response comes back on debugCommandFinished,
    // which is just the host's commandFinished re-emitted for the socket.
    m_pendingDebugCommand = command;
    m_host->command(command);
}

void MainWindow::sendConsoleCommand(const QString &command)
{
    if (!m_host->isRunning()) {
        m_log->appendPlainText(tr("[console] no emulator session is running"));
        return;
    }
    m_log->appendPlainText(QStringLiteral("> ") + command);
    // The response streams via logLine from both backends (HRDB emits the ack
    // for console-originated commands; native streams stderr). While the
    // machine is running this defers to the next stop (native) or answers
    // immediately for control commands (HRDB).
    m_host->consoleCommand(command);
}

bool MainWindow::setMemoryByte(quint32 address, quint32 value, MemoryView *pane)
{
    if (!m_host->isStopped()) {
        m_log->appendPlainText(tr("[edit] memory can only be changed while stopped"));
        return false;
    }
    // `w b <addr> <value>` writes one byte; silent on success, so refresh the
    // pane that asked. Without a pane (the remote-control setmem path) the
    // first pane is the sensible default.
    m_host->command(QStringLiteral("w b $%1 $%2")
                        .arg(address, 0, 16)
                        .arg(value, 0, 16));
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
    // The '=' is mandatory in Hatari's register-set syntax, and a successful set
    // prints nothing — so refresh to show the new value.
    m_host->command(QStringLiteral("r %1=$%2").arg(regName).arg(value, 0, 16));
    m_host->refresh();
    return true;
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
    // Every open editor gets the markers for its own file.
    for (CodeEditor *editor : openEditors()) {
        const QString file = QFileInfo(editor->filePath()).fileName();
        QList<int> lines;
        for (const Breakpoint &bp : m_breakpoints)
            if (bp.file == file)
                lines.append(bp.line);
        editor->setBreakpointLines(lines);
    }

    if (m_breakpointPanel)
        m_breakpointPanel->setBreakpoints(m_breakpoints);
}

void MainWindow::toggleBreakpointAtLine(int line)
{
    if (!m_editor || m_editor->filePath().isEmpty() || line <= 0)
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
    if (!m_editor || m_editor->filePath().isEmpty()
        || !LineMap::sameSource(file, m_editor->filePath()))
        return;
    m_editor->gotoLine(line);
}

void MainWindow::editBreakpointCondition(int line)
{
    if (!m_editor || m_editor->filePath().isEmpty() || line <= 0)
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

    // And the execution path that led here.
    if (m_pcHistory)
        m_host->command(QStringLiteral("history 16"));

    locationFromPc(state.pc);
}

bool MainWindow::canEmbedDisplay(const HatariCapabilities &caps) const
{
    // Both ends must be X11 clients of the same display, and the size report
    // that sizes the container travels on the control socket.
    return QGuiApplication::platformName() == QLatin1String("xcb")
        && caps.hasControlSocket;
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
        if (m_editor)
            m_editor->clearCurrentExecutionLine();
        return;
    }

    LineMap::Address address;
    if (!m_programMap.lineFor(pc, &address)) {
        if (m_editor)
            m_editor->clearCurrentExecutionLine();
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
    if (!target)
        return;

    m_tabs->setCurrentWidget(target);
    target->setCurrentExecutionLine(address.line);
    target->gotoLine(address.line);
}

} // namespace pist
