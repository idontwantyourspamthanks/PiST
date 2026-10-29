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
#include "analysis/AnalysisNotes.h"
#include "analysis/ProgramTrace.h"
#include "ui/AnalysisView.h"
#include "ui/BinaryView.h"
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

bool suffixIsKnownBinary(const QString &suffix)
{
    static const QSet<QString> binary = {
        QStringLiteral("prg"), QStringLiteral("ttp"), QStringLiteral("app"),
        QStringLiteral("acc"), QStringLiteral("ovl"), QStringLiteral("img"),
        QStringLiteral("st"),  QStringLiteral("msa"), QStringLiteral("dat"),
        QStringLiteral("bin"), QStringLiteral("raw"), QStringLiteral("mod"),
        QStringLiteral("s"),   QStringLiteral("asm"),
        QStringLiteral("pistproject"), QStringLiteral("lst"),
    };
    return binary.contains(suffix);
}

/// The one-shot "you can drag phases onto the sheet" hint.
const QString &offeredSpriteKey()
{
    static const QString key = QStringLiteral("layout/offeredSprite");
    return key;
}

} // namespace

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


BinaryView *MainWindow::addBinaryTab(const QByteArray &bytes, const QString &name,
                                     const QString &path, const QString &originKey)
{
    auto *view = new BinaryView(this);
    wireBinary(view);
    view->setIdentity(path, originKey, name);
    view->setBytes(bytes, name);
    const int index = m_tabs->addTab(view, name);
    m_tabs->setCurrentIndex(index);
    updateTabTitle(view);
    return view;
}


AnalysisView *MainWindow::addAnalysisTab(const QByteArray &bytes, const QString &name,
                                         const QString &path)
{
    auto *view = new AnalysisView(this);
    view->setIdentity(path, name);
    view->setBytes(bytes);
    const int index = m_tabs->addTab(view, name);
    m_tabs->setCurrentIndex(index);
    updateTabTitle(view);
    // A tab opened after the entry stop still has a PC to mark: the state
    // update that carried it has already been and gone.
    applyAnalysisExecution();
    return view;
}


QList<AnalysisView *> MainWindow::openAnalysisViews() const
{
    QList<AnalysisView *> views;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *view = qobject_cast<AnalysisView *>(m_tabs->widget(i)))
            views.append(view);
    return views;
}


AnalysisView *MainWindow::analysisForPath(const QString &path) const
{
    if (path.isEmpty())
        return nullptr;
    const QFileInfo wanted(path);
    for (AnalysisView *view : openAnalysisViews()) {
        if (!view->filePath().isEmpty() && QFileInfo(view->filePath()) == wanted)
            return view;
    }
    return nullptr;
}


void MainWindow::wireBinary(BinaryView *view)
{
    connect(view, &BinaryView::previewReady, this, [this](const QString &path) {
        openPath(path);
    });
    connect(view, &BinaryView::iffHitReady, this, &MainWindow::openIffBytes);
}


QList<BinaryView *> MainWindow::openBinaryViews() const
{
    QList<BinaryView *> views;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *view = qobject_cast<BinaryView *>(m_tabs->widget(i)))
            views.append(view);
    return views;
}


BinaryView *MainWindow::binaryForKey(const QString &originKey) const
{
    if (originKey.isEmpty())
        return nullptr;
    for (BinaryView *view : openBinaryViews()) {
        if (view->originKey() == originKey)
            return view;
        if (!view->filePath().isEmpty() && QFileInfo(view->filePath()) == QFileInfo(originKey))
            return view;
    }
    return nullptr;
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


QList<CodeEditor *> MainWindow::openEditors() const
{
    QList<CodeEditor *> editors;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *editor = qobject_cast<CodeEditor *>(m_tabs->widget(i)))
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
    } else if (auto *binary = qobject_cast<BinaryView *>(widget)) {
        name = binary->displayName();
    } else if (auto *analysis = qobject_cast<AnalysisView *>(widget)) {
        name = analysis->displayName();
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

    QString path;
    if (m_editor)
        path = m_editor->filePath();
    else if (m_image)
        path = m_image->filePath();
    else if (auto *binary = qobject_cast<BinaryView *>(widget))
        path = binary->filePath();
    else if (auto *analysis = qobject_cast<AnalysisView *>(widget))
        path = analysis->filePath();
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
    for (BinaryView *view : openBinaryViews()) {
        if (!view->filePath().isEmpty() && QFileInfo(view->filePath()) == QFileInfo(path))
            documents.append(view);
    }
    for (AnalysisView *view : openAnalysisViews()) {
        if (!view->filePath().isEmpty() && QFileInfo(view->filePath()) == QFileInfo(path))
            documents.append(view);
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
    for (BinaryView *view : openBinaryViews()) {
        if (view->filePath().isEmpty() || QFileInfo(view->filePath()) != QFileInfo(oldPath))
            continue;
        const QString absolute = QFileInfo(newPath).absoluteFilePath();
        view->setIdentity(absolute, absolute, QFileInfo(newPath).fileName());
        updateTabTitle(view);
    }
    for (AnalysisView *view : openAnalysisViews()) {
        if (view->filePath().isEmpty() || QFileInfo(view->filePath()) != QFileInfo(oldPath))
            continue;
        const QString absolute = QFileInfo(newPath).absoluteFilePath();
        view->setIdentity(absolute, QFileInfo(newPath).fileName());
        updateTabTitle(view);
    }
    const QString oldNotes = analysisNotesPath(QFileInfo(oldPath).absoluteFilePath());
    const QString newNotes = analysisNotesPath(QFileInfo(newPath).absoluteFilePath());
    if (QFileInfo::exists(oldNotes) && QFileInfo(oldNotes) != QFileInfo(newNotes)
        && !QFile::rename(oldNotes, newNotes)) {
        statusBar()->showMessage(
            tr("%1 could not be renamed with its program.").arg(QFileInfo(oldNotes).fileName()), 8000);
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
    } else if (m_tabs) {
        if (auto *binary = qobject_cast<BinaryView *>(m_tabs->currentWidget()))
            name = binary->displayName();
        else if (auto *analysis = qobject_cast<AnalysisView *>(m_tabs->currentWidget()))
            name = analysis->displayName();
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


void MainWindow::openBinary()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Binary"), QString(),
                                                      tr("All files (*)"));
    if (path.isEmpty())
        return;
    openBinaryAt(path);
}

void MainWindow::openBinaryAt(const QString &path)
{
    if (!QFileInfo(path).isFile())
        return;
    const QString key = QFileInfo(path).absoluteFilePath();
    if (BinaryView *open = binaryForKey(key)) {
        m_tabs->setCurrentWidget(open);
        return;
    }

    constexpr int kLimit = 2 * 1024 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Open Binary"), file.errorString());
        return;
    }
    if (file.size() > kLimit) {
        QMessageBox::warning(this, tr("Open Binary"),
                             tr("The file is larger than %1 bytes.").arg(kLimit));
        return;
    }
    addBinaryTab(file.readAll(), QFileInfo(path).fileName(), key, key);
    settings::noteFileOpened();
}


void MainWindow::openAnalysis()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open for Analysis"), QString(),
        tr("TOS executables (*.prg *.ttp *.app *.acc);;All files (*)"));
    if (path.isEmpty())
        return;
    openAnalysisAt(path);
}


void MainWindow::openAnalysisAt(const QString &path)
{
    if (!QFileInfo(path).isFile())
        return;
    const QString key = QFileInfo(path).absoluteFilePath();
    if (AnalysisView *open = analysisForPath(key)) {
        m_tabs->setCurrentWidget(open);
        return;
    }

    constexpr int kLimit = 2 * 1024 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Open for Analysis"), file.errorString());
        return;
    }
    if (file.size() > kLimit) {
        QMessageBox::warning(this, tr("Open for Analysis"),
                             tr("The file is larger than %1 bytes.").arg(kLimit));
        return;
    }
    const QByteArray bytes = file.readAll();
    AnalysisNotes notes;
    QString notesError;
    if (!loadAnalysisNotes(analysisNotesPath(key), &notes, &notesError)) {
        QMessageBox::warning(this, tr("Open for Analysis"), notesError);
        return;
    }
    const ProgramListing listing = listProgram(bytes, notes);
    if (!listing.error.isEmpty()) {
        QMessageBox::warning(this, tr("Open for Analysis"), listing.error);
        return;
    }
    addAnalysisTab(bytes, QFileInfo(path).fileName(), key);
    settings::noteFileOpened();
}

void MainWindow::openIffBytes(const QByteArray &bytes)
{
    ImportedSheet sheet;
    QString error;
    if (!importStImage(bytes, StImageFormat::Iff, PaletteKind::Ste, &sheet, &error)) {
        statusBar()->showMessage(error, 4000);
        return;
    }
    const QString path = QDir::temp().filePath(QStringLiteral("pist-iff-%1.iff").arg(++m_iffSerial));
    if (!files::write(path, bytes, &error)) {
        statusBar()->showMessage(error, 4000);
        return;
    }
    openPath(path);
}


bool MainWindow::openPath(const QString &path)
{
    return openPathImpl(WelcomeView::writableDemoPath(path), /*quiet=*/false);
}


bool MainWindow::openPathQuiet(const QString &path)
{
    return openPathImpl(WelcomeView::writableDemoPath(path), /*quiet=*/true);
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
        loadProjectForSource(path, quiet);
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


void MainWindow::applyAnalysisExecution()
{
    const bool live = m_lastState.regs.valid && m_lastState.hasBases() && !m_launchedProgram.isEmpty();
    const QFileInfo launched(m_launchedProgram);
    for (AnalysisView *view : openAnalysisViews()) {
        const bool mine = live && !view->filePath().isEmpty() && QFileInfo(view->filePath()) == launched;
        if (!mine) {
            view->clearExecutionPc();
            continue;
        }
        view->setExecutionPc(m_lastState.pc, m_lastState.textBase, m_lastState.dataBase,
                             m_lastState.bssBase);
    }
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


} // namespace pist
