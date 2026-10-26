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
#include "ui/MainWindowPrivate.h"

namespace pist {

namespace {

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

} // namespace

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


} // namespace pist
