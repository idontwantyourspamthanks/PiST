// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"
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
#include "ui/MainWindowPrivate.h"

namespace pist {

namespace {

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

} // namespace

void MainWindow::createActions()
{
    m_actOpen = new QAction(tr("&Open…"), this);
    m_actOpen->setShortcut(QKeySequence::Open);
    connect(m_actOpen, &QAction::triggered, this, &MainWindow::openFile);

    m_actOpenBinary = new QAction(tr("Open &Binary…"), this);
    m_actOpenBinary->setObjectName(QStringLiteral("openBinaryAction"));
    connect(m_actOpenBinary, &QAction::triggered, this, &MainWindow::openBinary);

    m_actOpenAnalysis = new QAction(tr("Open for &Analysis…"), this);
    m_actOpenAnalysis->setObjectName(QStringLiteral("openAnalysisAction"));
    connect(m_actOpenAnalysis, &QAction::triggered, this, &MainWindow::openAnalysis);

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

    m_actCompareBuild = new QAction(tr("Compare With Last &Build"), this);
    m_actCompareBuild->setObjectName(QStringLiteral("compareWithLastBuild"));
    m_actCompareBuild->setToolTip(
        tr("Show what the last rebuild changed. Nothing is written to the machine."));
    connect(m_actCompareBuild, &QAction::triggered, this, &MainWindow::compareWithLastBuild);

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

    m_actCaptureScreen = new QAction(tr("Capture &Screen…"), this);
    m_actCaptureScreen->setObjectName(QStringLiteral("captureScreenAction"));
    m_actCaptureScreen->setEnabled(false);
    connect(m_actCaptureScreen, &QAction::triggered, this, &MainWindow::captureScreen);

    m_actCaptureDma = new QAction(tr("Capture &DMA Sound…"), this);
    m_actCaptureDma->setObjectName(QStringLiteral("captureDmaAction"));
    m_actCaptureDma->setEnabled(false);
    connect(m_actCaptureDma, &QAction::triggered, this, &MainWindow::captureDma);

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
    fileMenu->addAction(m_actOpenBinary);
    fileMenu->addAction(m_actOpenAnalysis);
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
    buildMenu->addAction(m_actCompareBuild);
    buildMenu->addSeparator();
    buildMenu->addAction(m_actNextDiagnostic);
    buildMenu->addAction(m_actPrevDiagnostic);

    auto *runMenu = menuBar()->addMenu(tr("&Run"));
    runMenu->setObjectName(QStringLiteral("runMenu"));
    runMenu->addAction(m_actRun);
    runMenu->addAction(m_actStop);
    runMenu->addAction(m_actCaptureScreen);
    runMenu->addAction(m_actCaptureDma);
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
        // The entry-stop card is a Continue button: the panel is where the
        // user is looking when the loaded screen stops moving.
        connect(m_display, &EmulatorDisplayWidget::resumeRequested, this,
                &MainWindow::resume);
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
    connect(m_fileBrowser, &FileBrowser::openBinaryRequested, this, &MainWindow::openBinaryAt);
    connect(m_fileBrowser, &FileBrowser::openAnalysisRequested, this, &MainWindow::openAnalysisAt);
    connect(m_fileBrowser, &FileBrowser::diskScanRequested, this, &MainWindow::scanDiskImage);
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
    connect(view, &MemoryView::saveBytesRequested, this,
            [this](quint32 address) { saveMemoryBytes(address); });
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
        // Hiding every tab of the group drops its tab bar. The whole set is hidden
        // here, and the current tab goes last because `projectGroup` ends with the
        // dock that was raised — hiding the current tab first lets the group pick
        // another one, and the bar survives.
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
        buildButton->setObjectName(QStringLiteral("buildToolButton"));
        buildButton->setMinimumWidth(46);
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
    // A stopped session's pause card names the Continue shortcut, and the
    // scheme can change while it is up.
    if (m_display && m_host && m_host->isStopped())
        m_display->setContinueHint(m_actResume->shortcut().toString(QKeySequence::NativeText));
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


} // namespace pist
