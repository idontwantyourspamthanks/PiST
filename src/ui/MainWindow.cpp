// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "debug/Breakpoint.h"
#include "ui/BreakpointPanel.h"
#include "ui/DisassemblyView.h"
#include "ui/MemoryView.h"
#include "ui/SettingsDialog.h"
#include "ui/RegistersView.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QInputDialog>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>

namespace pist {

namespace {

/// Locate `vasmm68k_mot` without hard-coding a path.
QString findAssembler()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (!onPath.isEmpty())
        return onPath;
    return QStringLiteral("vasmm68k_mot");
}

QString findHatari()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("hatari"));
    if (!onPath.isEmpty())
        return onPath;
    return QStringLiteral("hatari");
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_editor = new CodeEditor(this);
    setCentralWidget(m_editor);

    m_build = new BuildService(this);
    m_build->setAssemblerPath(findAssembler());
    connect(m_build, &BuildService::finished, this, &MainWindow::onBuildFinished);
    connect(m_build, &BuildService::outputLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });

    m_host = new EmulatorHost(this);
    connect(m_host, &EmulatorHost::stateUpdated, this, &MainWindow::onStateUpdated);
    connect(m_host, &EmulatorHost::logLine, this, [this](const QString &line) {
        m_log->appendPlainText(line);
    });
    connect(m_host, &EmulatorHost::errorOccurred, this, [this](const QString &message) {
        m_log->appendPlainText(QStringLiteral("[error] ") + message);
    });
    connect(m_host, &EmulatorHost::memoryDumpReady, this,
            [this](quint32, const QString &response) {
                if (m_memory)
                    m_memory->applyDump(response);
            });

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

    m_caps = probeHatari(findHatari());
    m_statusToolchain->setText(
        QStringLiteral("vasm: %1").arg(QFileInfo(m_build->assemblerPath()).fileName()));
    m_statusEmulator->setText(m_caps.summary());

    setWindowTitle(tr("PiST — Atari ST assembly IDE"));
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

    m_actResume = new QAction(tr("&Continue"), this);
    m_actResume->setShortcut(QKeySequence(Qt::Key_F9));
    m_actResume->setEnabled(false);
    connect(m_actResume, &QAction::triggered, this, &MainWindow::resume);
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
    fileMenu->addAction(tr("E&xit"), this, &QWidget::close, QKeySequence::Quit);

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
}

void MainWindow::createDocks()
{
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
        return dock;
    }());

    addDockWidget(Qt::BottomDockWidgetArea, [this] {
        auto *dock = new QDockWidget(tr("Memory"), this);
        m_memory = new MemoryView(dock);
        dock->setWidget(m_memory);
        connect(m_memory, &MemoryView::dumpRequested, m_host, &EmulatorHost::requestMemoryDump);
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
    static int counter = 0;
    const QString dir = paths::sessionBaseDir()
                      + QStringLiteral("/%1-%2")
                            .arg(QCoreApplication::applicationPid())
                            .arg(++counter);
    QDir().mkpath(dir);
    return dir;
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
    if (!m_editor->loadFile(path)) {
        QMessageBox::warning(this, tr("Open"),
                             tr("Could not open %1").arg(path));
        return;
    }

    statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
    loadProjectForSource(path);
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
    // file's location — so derive it and load it.
    QString source = settings::lastSourcePath();
    const QString implied = path.left(path.size() - QLatin1String(settings::kProjectSuffix).size())
                          + QStringLiteral(".s");
    if (QFileInfo::exists(implied))
        source = implied;
    else if (!source.isEmpty() && !QFileInfo::exists(source))
        source.clear();

    if (!source.isEmpty()) {
        loaded.sourceFile = source;
        m_editor->loadFile(source);
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
        if (settings::save(m_settings, path, &error))
            settings::rememberLastProject(path, m_editor->filePath());
        else
            m_log->appendPlainText(QStringLiteral("[project] ") + error);
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
        m_log->appendPlainText(tr("[project] %1").arg(error));
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
    } else {
        m_editor->saveFile(m_editor->filePath());
    }
}

void MainWindow::build()
{
    if (m_editor->filePath().isEmpty()) {
        QMessageBox::information(this, tr("Build"),
                                 tr("Open an assembly source file first."));
        return;
    }

    m_editor->saveFile(m_editor->filePath());
    m_problems->clear();
    m_log->appendPlainText(tr("--- build ---"));

    const QFileInfo info(m_editor->filePath());
    const QString outPath = info.absolutePath() + QDir::separator()
                          + info.completeBaseName() + QStringLiteral(".prg");
    const QString listPath = info.absolutePath() + QDir::separator()
                           + info.completeBaseName() + QStringLiteral(".lst");

    m_build->setSourceFile(info.absoluteFilePath());
    m_build->setOutputFile(outPath);
    m_build->setListingFile(listPath);

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
    QString error;
    if (success && m_lineMap.parseListing(m_build->listingFile(), &error)) {
        // This counts source *files* contributing to the listing, not lines. The
        // two differ by orders of magnitude for a real project, so naming it
        // correctly matters (a multi-file build reports 2, not the 2000+ lines).
        const int files = m_lineMap.sourceFiles().size();
        statusBar()->showMessage(
            files == 1 ? tr("Build succeeded — 1 source file mapped")
                       : tr("Build succeeded — %1 source files mapped").arg(files),
            5000);
    } else if (!error.isEmpty()) {
        m_log->appendPlainText(QStringLiteral("[line map] ") + error);
    }

    QList<int> errorLines;
    for (const Diagnostic &d : diagnostics) {
        auto *item = new QTreeWidgetItem(m_problems);
        item->setText(0, d.file.isEmpty() ? tr("(build)") : d.file);
        item->setText(1, d.line > 0 ? QString::number(d.line) : QString());
        item->setText(2, d.message);
        if (d.severity == Diagnostic::Error)
            item->setForeground(2, QColor(0xc0, 0x20, 0x20));

        // Only diagnostics belonging to the open file can be marked in its gutter.
        if (d.hasLocation() && d.line > 0
            && d.file == QFileInfo(m_editor->filePath()).fileName()) {
            errorLines.append(d.line);
        }
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
    const QString prg = info.absolutePath() + QDir::separator()
                      + info.completeBaseName() + QStringLiteral(".prg");
    if (!QFileInfo::exists(prg)) {
        QMessageBox::warning(this, tr("Run"),
                             tr("The build did not produce %1.").arg(prg));
        return;
    }

    const QString sessionDir = makeSessionDir();

    SessionConfig config;
    config.hatariPath = findHatari();
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

    // The control socket is compiled into Hatari only under
    // HAVE_UNIX_DOMAIN_SOCKETS. Passing the option to a build without it makes
    // Hatari exit with "Unrecognized option", so it must be gated rather than
    // passed unconditionally (docs/PLAN.md §5 rule 12). Nothing in the IDE
    // depends on it yet: all debugger commands travel over stdin.
    if (m_caps.hasControlSocket)
        config.controlSocketPath = sessionDir + QStringLiteral("/ctl.sock");
    else
        config.controlSocketPath.clear();

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
}

void MainWindow::step()
{
    m_host->step();
    // Registers and disassembly must be re-read after the step completes.
    m_host->refresh();
}

void MainWindow::stepOver()
{
    m_host->stepOver();
    m_host->refresh();
}

void MainWindow::resume()
{
    m_host->resume();
}

void MainWindow::onDebuggerStopped()
{
    if (m_sessionArmed)
        return;
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

    // Arming is chained after those commands complete, because it needs the
    // section bases they report.
    QTimer::singleShot(0, this, [this] {
        if (m_bases.isValid())
            armBreakpoints();
        else
            m_log->appendPlainText(
                tr("[breakpoints] no program base page yet; breakpoints will arm on the next stop"));
    });
}

void MainWindow::armBreakpoints()
{
    const ArmPlan plan = planBreakpoints(m_breakpoints, m_lineMap, m_bases);

    // Clear before arming. A rebuilt program can occupy different addresses, so
    // leaving old breakpoints in place would silently break on whatever code now
    // lives at those addresses.
    m_host->clearBreakpoints();

    for (const QString &command : plan.commands)
        m_host->armBreakpoint(command);

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
    }
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
    auto it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                           [&](const Breakpoint &bp) {
                               return bp.line == line && bp.file == file;
                           });
    if (it == m_breakpoints.end()) {
        // Nothing to edit yet, so create one first rather than silently doing
        // nothing on a right-click at an empty line.
        toggleBreakpointAtLine(line);
        it = std::find_if(m_breakpoints.begin(), m_breakpoints.end(),
                          [&](const Breakpoint &bp) {
                              return bp.line == line && bp.file == file;
                          });
        if (it == m_breakpoints.end())
            return;
    }

    bool accepted = false;
    const QString condition = QInputDialog::getText(
        this, tr("Breakpoint condition"),
        tr("Extra condition for %1:%2 (ANDed with the program counter).\n\n"
           "Hatari has no memory watchpoints, so this is how you watch a value,\n"
           "for example:  d0 = $1234   or   (buf) = $ff")
            .arg(file)
            .arg(line),
        QLineEdit::Normal, it->condition, &accepted);
    if (!accepted)
        return;

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
    m_registers->setState(state);
    m_disassembly->setState(state);

    if (state.hasBases()) {
        const bool firstBases = !m_bases.isValid();
        m_bases.text = state.textBase;
        m_bases.data = state.dataBase;
        m_bases.bss = state.bssBase;

        // Point the memory pane at the program's own data on the first stop.
        // Its previous address is meaningless across sessions.
        if (firstBases && m_memory)
            m_memory->goToAddress(state.dataBase ? state.dataBase : state.textBase);
    }

    locationFromPc(state.pc);
}

void MainWindow::locationFromPc(quint32 pc)
{
    if (!m_bases.isValid() || m_lineMap.isEmpty()) {
        m_editor->clearCurrentExecutionLine();
        return;
    }

    LineMap::Address address;
    if (!m_lineMap.lineFor(pc, m_bases, &address)) {
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
