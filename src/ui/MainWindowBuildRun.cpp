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
#include "build/LinkMap.h"
#include "build/ProgramDiff.h"
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
#include "image/AssetBuild.h"
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
#include "ui/ProgramDiffDialog.h"
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
    // could not be built at all. The include path gains the build directory
    // below, after sprite exports, and only for this build.
    m_build->setDefines(m_settings.defines);
    m_build->setCpu(m_settings.cpu);
    m_build->setAssemblerArgs(release ? m_settings.releaseAssemblerArgs
                                      : m_settings.debugAssemblerArgs);

    // Keep the program this build is about to replace. The build deletes the
    // output first, so the copy has to happen here, and a failed copy must not
    // stop the build — the snapshot is only for the comparison dialog.
    QString mapPath;
    if (linking)
        mapPath = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                  + QStringLiteral(".map");
    QString snapError;
    if (!snapshotPreviousBuild(outPath, mapPath, &snapError))
        m_log->appendPlainText(snapError);

    const QString buildDir = info.absolutePath() + QStringLiteral("/build");
    QSet<QString> outputs;
    for (const AssetExport &asset : m_settings.assets) {
        if (outputs.contains(asset.output)) {
            refuseBuild(tr("Build"),
                        tr("Two sprite assets both write %1.").arg(asset.output), true);
            return;
        }
        outputs.insert(asset.output);
        AssetBuildRequest request;
        request.phaseName = asset.phase;
        request.outputFileName = asset.output;
        request.options.palette = asset.palette;
        request.options.sprite = asset.sprite;
        request.options.masked = asset.masked;
        request.options.shifted = asset.shifted;
        request.options.shiftedMasked = asset.shiftedMasked;
        request.options.preShifts = asset.preShifts;
        request.options.transparent = asset.transparent;
        const AssetBuildResult exported = buildDeclaredAsset(asset.source, request, buildDir);
        if (!exported.ok) {
            refuseBuild(tr("Build"), exported.error, true);
            return;
        }
        m_log->appendPlainText(exported.unchanged
                                   ? tr("%1 unchanged").arg(QFileInfo(asset.source).fileName())
                                   : tr("Exported %1").arg(exported.dataPath));
    }
    QStringList includes = m_settings.includePaths;
    if (!m_settings.assets.isEmpty())
        includes.append(buildDir);
    m_build->setIncludePaths(includes);

    if (m_statusBuild)
        m_statusBuild->setText(release ? tr("Building (release)…") : tr("Building…"));
    m_build->build();
}


MainWindow::BuildDiffMiss MainWindow::gatherBuildDiff(ProgramDiff *diff, ProgramDiffHints *hints,
                                                       QString *detail) const
{
    const QString source = buildSourcePath();
    if (source.isEmpty())
        return BuildDiffMiss::NoSource;

    const QFileInfo info(source);
    const QString program = settings::outputPathsFor(info.absoluteFilePath()).program;
    const QString previous = previousBuildProgram(program);
    if (!QFileInfo::exists(previous))
        return BuildDiffMiss::NoPrevious;
    if (!QFileInfo::exists(program))
        return BuildDiffMiss::NoCurrent;

    const auto readAll = [](const QString &path, QByteArray *bytes, QString *error) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = file.errorString();
            return false;
        }
        *bytes = file.readAll();
        return file.error() == QFile::NoError;
    };
    QByteArray oldBytes;
    QByteArray newBytes;
    QString error;
    if (!readAll(previous, &oldBytes, &error) || !readAll(program, &newBytes, &error)) {
        if (detail)
            *detail = error;
        return BuildDiffMiss::Unreadable;
    }

    const QString mapPath = info.absolutePath() + QLatin1Char('/') + info.completeBaseName()
                            + QStringLiteral(".map");
    LinkMap previousMap;
    LinkMap currentMap;
    QString mapError;
    const bool havePreviousMap =
        QFileInfo::exists(previousBuildLinkMap(mapPath))
        && previousMap.parse(previousBuildLinkMap(mapPath), &mapError);
    const bool haveCurrentMap = QFileInfo::exists(mapPath) && currentMap.parse(mapPath, &mapError);

    QList<SectionAnchor> anchors;
    for (const SymbolEntry &sym : m_symbols) {
        if (sym.equate || sym.line <= 0 || sym.file.isEmpty() || sym.name.isEmpty())
            continue;
        const int line = m_programMap.nextAddressLine(sym.file, sym.line);
        if (line <= 0)
            continue;
        SectionAnchor anchor;
        if (!m_programMap.sectionOffsetFor(sym.file, line, &anchor.section, &anchor.offset))
            continue;
        if (anchor.section != QLatin1String("text") && anchor.section != QLatin1String("data"))
            continue;
        anchor.name = sym.name;
        anchors.append(anchor);
    }

    ProgramDiff compared =
        diffTosPrograms(oldBytes, newBytes, anchors, havePreviousMap ? &previousMap : nullptr,
                        haveCurrentMap ? &currentMap : nullptr);
    if (!compared.ok) {
        if (detail)
            *detail = compared.error;
        return BuildDiffMiss::BadImage;
    }

    ProgramDiffHints live;
    if (m_host->isStopped() && m_bases.isValid()) {
        live.haveBases = true;
        live.textBase = m_lastState.textBase;
        live.dataBase = m_lastState.dataBase;
        live.bssBase = m_lastState.bssBase;
        live.havePc = m_lastState.regs.valid;
        live.pc = m_lastState.pc;
    }
    if (m_stack) {
        quint32 ret = 0;
        if (m_stack->innermostLong(&ret)) {
            live.haveReturn = true;
            live.returnAddress = ret;
        }
    }
    if (diff)
        *diff = compared;
    if (hints)
        *hints = live;
    return BuildDiffMiss::Ok;
}


QJsonObject MainWindow::buildDiffJson() const
{
    ProgramDiff diff;
    ProgramDiffHints hints;
    QString detail;
    const BuildDiffMiss miss = gatherBuildDiff(&diff, &hints, &detail);
    if (miss == BuildDiffMiss::Ok)
        return programDiffToJson(diff, hints);

    QString error;
    switch (miss) {
    case BuildDiffMiss::NoSource:
        error = QStringLiteral("open an assembly source file first");
        break;
    case BuildDiffMiss::NoPrevious:
        error = QStringLiteral("no previous build");
        break;
    case BuildDiffMiss::NoCurrent:
        error = QStringLiteral("the current program is missing");
        break;
    case BuildDiffMiss::Unreadable:
    case BuildDiffMiss::BadImage:
        error = detail;
        break;
    case BuildDiffMiss::Ok:
        break;
    }
    QJsonObject object;
    object.insert(QStringLiteral("ok"), false);
    object.insert(QStringLiteral("written"), false);
    object.insert(QStringLiteral("error"), error);
    return object;
}


void MainWindow::compareWithLastBuild()
{
    ProgramDiff diff;
    ProgramDiffHints hints;
    QString detail;
    switch (gatherBuildDiff(&diff, &hints, &detail)) {
    case BuildDiffMiss::NoSource:
        QMessageBox::information(this, tr("Compare with last build"),
                                 tr("Open an assembly source file first."));
        return;
    case BuildDiffMiss::NoPrevious:
        QMessageBox::information(
            this, tr("Compare with last build"),
            tr("There is no previous build to compare. Build the program again "
               "and PiST will keep a copy of this one."));
        return;
    case BuildDiffMiss::NoCurrent:
        QMessageBox::information(this, tr("Compare with last build"),
                                 tr("The current program is missing. The last build did not produce one."));
        return;
    case BuildDiffMiss::Unreadable:
    case BuildDiffMiss::BadImage:
        QMessageBox::warning(this, tr("Compare with last build"), detail);
        return;
    case BuildDiffMiss::Ok:
        break;
    }

    ProgramDiffDialog dialog(diff, hints, this);
    dialog.exec();
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
        if (QFileInfo::exists(projectPath)) {
            const bool allowed = hookMayRunFor(projectPath, quiet);
            m_stopHookRefused = !allowed;
            if (!allowed)
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
