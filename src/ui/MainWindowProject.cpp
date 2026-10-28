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

/// Whether `file` is inside the repository rooted at `root`, with canonical
/// spellings on both sides so `..` or a symlink cannot spell the way out.
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
    return f.startsWith(r + QDir::separator(), cs) || f == r;
}


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

/// Whether a project's hook may run. The policy lives in `settings` (so the
/// decision has a test); this asks it and, when the answer is "not yet", puts the
/// question to the person who opened the project.
bool MainWindow::hookMayRunFor(const QString &projectPath, bool quiet)
{
    // Use the executable-relative half of the welcome view's demo discovery,
    // covering multi-config trees such as Windows' build/Release/ without
    // treating the caller's working directory as trusted provenance. Writable
    // installed copies carry explicit trust records from demoProjects().
    if (settings::hookProjectIsTrusted(m_settings, projectPath,
                                       WelcomeView::demoSourceDir()))
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
    m_stopHookRefused = false;
    // A refusal suppresses the hook for this load only. The enabled setting
    // stays untouched so a later unrelated save cannot rewrite the project.
    if (!hookMayRunFor(projectPath)) {
        m_stopHookRefused = true;
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
    m_stopHookRefused = false;

    // Opening a source is the ordinary way into a cloned project: the settings
    // beside it are loaded here, not through the Open project dialog, so the
    // hook question has to be asked here too.
    if (!hookMayRunFor(projectPath, quiet)) {
        m_stopHookRefused = true;
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
    // This is an explicit configuration decision, not a persisted echo of the
    // earlier refusal. The new trust check still runs before the next launch.
    m_stopHookRefused = false;

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


} // namespace pist
