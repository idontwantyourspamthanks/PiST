// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "ui/DisassemblyView.h"
#include "ui/RegistersView.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
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
    connect(m_host, &EmulatorHost::stoppedChanged, this, [this](bool stopped) {
        m_actStep->setEnabled(stopped);
        m_actStepOver->setEnabled(stopped);
        m_actResume->setEnabled(stopped);
        m_statusEmulator->setText(stopped ? tr("Stopped in debugger") : tr("Running"));
    });

    createActions();
    createDocks();
    createToolBar();
    createStatusBar();

    m_caps = probeHatari(findHatari());
    m_statusToolchain->setText(
        QStringLiteral("vasm: %1").arg(QFileInfo(m_build->assemblerPath()).fileName()));
    m_statusEmulator->setText(m_caps.summary());

    setWindowTitle(tr("pist — Atari ST assembly IDE"));
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

    m_actResume = new QAction(tr("&Continue"), this);
    m_actResume->setShortcut(QKeySequence(Qt::Key_F9));
    m_actResume->setEnabled(false);
    connect(m_actResume, &QAction::triggered, this, &MainWindow::resume);
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
    bar->addSeparator();
    bar->addAction(m_actBuild);
    bar->addAction(m_actRun);
    bar->addAction(m_actStop);
    bar->addSeparator();
    bar->addAction(m_actResume);
    bar->addAction(m_actStep);
    bar->addAction(m_actStepOver);
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
    if (m_editor->loadFile(path))
        statusBar()->showMessage(tr("Opened %1").arg(path), 4000);
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
    m_build->build();
}

void MainWindow::onBuildFinished(bool success, const QList<Diagnostic> &diagnostics)
{
    QString error;
    if (success && m_lineMap.parseListing(m_build->listingFile(), &error)) {
        statusBar()->showMessage(
            tr("Build succeeded — %1 source lines mapped").arg(m_lineMap.sourceFiles().size()),
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
}

void MainWindow::run()
{
    if (m_editor->filePath().isEmpty()) {
        QMessageBox::information(this, tr("Run"), tr("Open an assembly source file first."));
        return;
    }

    // Always rebuild before running: this keeps the listing in step with the
    // binary, which is what the line map depends on.
    build();
    if (m_build->isRunning())
        return;

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
    const TosRom rom = selectPreferredRom(roms);
    if (rom.path.isEmpty()) {
        const QStringList searched = paths::tosSearchPaths();
        QMessageBox::critical(
            this, tr("Run"),
            tr("No TOS ROM image found.\n\n"
               "Original TOS images cannot be bundled with pist, so one has to be supplied "
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

void MainWindow::onStateUpdated(const MachineState &state)
{
    m_registers->setState(state);
    m_disassembly->setState(state);

    if (state.hasBases()) {
        m_bases.text = state.textBase;
        m_bases.data = state.dataBase;
        m_bases.bss = state.bssBase;
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

    // Only follow the PC into the file that is actually open.
    if (!m_editor->filePath().isEmpty()
        && address.file != QFileInfo(m_editor->filePath()).fileName()) {
        return;
    }

    m_editor->setCurrentExecutionLine(address.line);
    m_editor->gotoLine(address.line);
}

} // namespace pist
