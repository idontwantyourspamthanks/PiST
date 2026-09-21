// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MainWindow.h"

#include "build/BuildService.h"
#include "editor/CodeEditor.h"
#include "emu/EmulatorHost.h"
#include "emu/DebugBackend.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"
#include "ui/InstructionRefView.h"
#include "build/FloppyImage.h"
#include "editor/IncludeNav.h"
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
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QTextBlock>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
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

    auto *editor = new CodeEditor(this);
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
        if (!m_instrRef || editor != m_editor || !m_instrRef->isVisible())
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
        if (match.call) {
            m_instrRef->showOsCall(match.call->trap, match.call->opcode, match.args);
            return;
        }
        if (match.trapContext) {
            // A trap whose function number is not statically known: the
            // generic TRAP entry still says what the instruction does.
            m_instrRef->showInstruction(QStringLiteral("trap"));
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
        m_instrRef->showInstruction(line.mid(start, end - start));
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

void MainWindow::wireImageReExport(ImageEditor *editor)
{
    // A re-export repeats the block map in the console, same as the explicit
    // export leaves it.
    connect(editor, &ImageEditor::bitplaneReExported, this,
            [this](const QString &path, const QString &scroller, const QString &error) {
                if (!m_log)
                    return;
                if (!error.isEmpty()) {
                    m_log->appendPlainText(tr("[export] re-export failed: %1").arg(error));
                    return;
                }
                m_log->appendPlainText(tr("--- bitplane data (re-export): %1 ---")
                                           .arg(QFileInfo(path).fileName()));
                const ImageDocument &doc = m_image->document();
                const QVector<BitplaneBlock> blocks = bitplaneLayout(
                    doc.phases().at(m_image->lastBitplaneExportPhase()).cellW,
                    doc.phases().at(m_image->lastBitplaneExportPhase()).cellH,
                    doc.phases().at(m_image->lastBitplaneExportPhase()).frames.size(),
                    m_image->lastBitplaneExportOptions());
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
    }

    const QString path = m_editor ? m_editor->filePath()
                                  : (m_image ? m_image->filePath() : QString());
    if (!path.isEmpty() && m_fileBrowser)
        m_fileBrowser->showFor(path);
    if (m_editor)
        refreshBreakpointMarkers();
    updateModifiedState();
    updateCaretChip();
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
    const ToolInfo assembler = toolchain::findAssembler(m_settings.assemblerPath);
    m_build->setAssemblerPath(assembler.found() ? assembler.path
                                                : QStringLiteral("vasmm68k_mot"));
    // Probe the emulator the session would actually run: with a project
    // override pointing elsewhere, the discovered binary's capabilities are
    // the wrong answer for the status bar and the embed action.
    m_caps = probeHatari(toolchain::findEmulator(m_settings.hatariPath).path);
    updateEmbedActionState();
    m_statusToolchain->setText(
        QStringLiteral("vasm: %1").arg(QFileInfo(m_build->assemblerPath()).fileName()));
    updateSessionChip();
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
    syncProfileActions();
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

    m_actStepOut = new QAction(tr("Step O&ut"), this);
    m_actStepOut->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F11));
    m_actStepOut->setEnabled(false);
    connect(m_actStepOut, &QAction::triggered, this, &MainWindow::stepOut);

    m_actRunToCursor = new QAction(tr("Run to &Cursor"), this);
    m_actRunToCursor->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_F10));
    m_actRunToCursor->setEnabled(false);
    connect(m_actRunToCursor, &QAction::triggered, this, &MainWindow::runToCursor);

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
        updateSessionChip();
        if (m_actPause)
            m_actPause->setEnabled(false);
        if (m_sessionArmed) {
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
        resetSessionState();
        // A session's profile and its gutter heat belong to that session.
        if (m_profiler)
            m_profiler->clear();
        for (CodeEditor *editor : openEditors())
            editor->setLineHeat({});
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
                else if (m_profileSavePending
                         && command.startsWith(QLatin1String("profile save"))) {
                    m_profileSavePending = false;
                    showProfileResults();
                }
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
                m_stopEventPending = true;
            } else {
                // A stop whose state never became valid (no register batch
                // followed) must not leave a watcher hanging: publish it
                // without the pc detail rather than drop it.
                if (m_stopEventPending) {
                    m_stopEventPending = false;
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
        updateSessionChip();
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
        syncProfileActions();
        if (stopped)
            onDebuggerStopped();
    });

    connect(m_host, &IDebugBackend::stackDumpReady, this,
            [this](quint32 sp, const QString &response) {
                // A step-out dump is consumed here regardless of the stack
                // view: any stack response is at the same SP with the return
                // address on top.
                if (m_stepOutPending) {
                    m_stepOutPending = false;
                    const QList<MemoryRow> rows = parseMemoryDump(response);
                    const quint32 returnAddress =
                        (!rows.isEmpty() && rows.first().bytes.size() >= 4)
                        ? readLongBE(QByteArray::fromRawData(
                                         reinterpret_cast<const char *>(
                                             rows.first().bytes.constData()),
                                         rows.first().bytes.size()),
                                     0)
                        : 0;
                    if (!looksLikeAddress(returnAddress)) {
                        m_log->appendPlainText(
                            tr("[step out] no plausible return address at A7 "
                               "(0x%1) — not inside a subroutine?")
                                .arg(returnAddress, 0, 16));
                        return;
                    }
                    m_log->appendPlainText(tr("[step out] to 0x%1")
                                               .arg(returnAddress, 0, 16));
                    m_host->armBreakpoint(QStringLiteral("b pc = $%1 :once")
                                              .arg(returnAddress, 0, 16));
                    m_host->resume();
                }
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
    fileMenu->addAction(m_actExportImage);
    fileMenu->addAction(m_actExportImageSafe);
    fileMenu->addAction(m_actExportSpriteSheet);
    fileMenu->addAction(m_actExportBitplanes);
    fileMenu->addAction(m_actReExportBitplanes);
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

    auto *searchMenu = menuBar()->addMenu(tr("&Search"));
    searchMenu->addAction(m_actFind);
    searchMenu->addAction(m_actFindNext);
    searchMenu->addAction(m_actFindPrevious);
    searchMenu->addSeparator();
    searchMenu->addAction(m_actReplace);
    searchMenu->addSeparator();
    searchMenu->addAction(m_actNextDiagnostic);
    searchMenu->addAction(m_actPrevDiagnostic);

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
    runMenu->addAction(m_actStepOut);
    runMenu->addAction(m_actRunToCursor);
    runMenu->addAction(m_actProfileStart);
    runMenu->addAction(m_actProfileStop);
    runMenu->addAction(m_actProfileToCursor);
    runMenu->addSeparator();
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
    m_consoleInput = new ConsoleInput(this);
    m_consoleInput->setObjectName(QStringLiteral("consoleInput"));
    appearance::markMono(m_consoleInput);
    m_consoleInput->setPlaceholderText(
        tr("Debugger command (e.g. r, d, m $12596 20)"));
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
        auto *reset = m_viewMenu->addAction(tr("Reset layout"), this,
                                            &MainWindow::resetToDefaultLayout);
        reset->setObjectName(QStringLiteral("resetLayoutAction"));

        // Preferred order is only the reading order. Anything not named here
        // is still listed, after these, so a new dock cannot miss the menu
        // by being left out of the list.
        const QStringList preferred = {
            QStringLiteral("projectFilesDock"),
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
        QSettings().value(QStringLiteral("layout/state")).toByteArray();
    const QByteArray savedGeometry =
        QSettings().value(QStringLiteral("layout/geometry")).toByteArray();

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
    const int savedWidth = QSettings().value(QStringLiteral("layout/width")).toInt();
    const int savedHeight = QSettings().value(QStringLiteral("layout/height")).toInt();
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
    m_actProfileStart->setIcon(appearance::icon(Icon::ProfileStart));
    m_actProfileStop->setIcon(appearance::icon(Icon::ProfileStop));
    m_actProfileToCursor->setIcon(appearance::icon(Icon::ProfileToCursor));
    if (auto *desk = findChild<QMenu *>(QStringLiteral("deskMenu")))
        desk->setIcon(appearance::atariLogoIcon());
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
    const QString dir = QDir(paths::sessionBaseDir())
                            .absoluteFilePath(QStringLiteral("docs/%1-%2")
                                                  .arg(name, imageKey));
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
    {
        // Scoped so the file is closed — its bytes on disk — before the
        // editor tab below reads it.
        QFile out(extracted);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || out.write(data) != data.size()) {
            QMessageBox::warning(this, tr("Open"),
                                 tr("Could not write %1").arg(extracted));
            return nullptr;
        }
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

    // A project that names tool paths must not keep the construction-time
    // discovery answer: the override wins as soon as it is known.
    if (!m_settings.assemblerPath.isEmpty() || !m_settings.linkerPath.isEmpty()
        || !m_settings.hatariPath.isEmpty()) {
        refreshToolchain();
    }

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
        m_editor->saveFile(path);
    } else if (!m_editor->saveFile(m_editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(m_editor->filePath(), m_editor->lastError()));
    } else {
        writeBackFloppyDoc(m_editor->filePath());
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

void MainWindow::resetSessionState()
{
    // GEMDOS relocates the program on every run, so resolved addresses and
    // armed breakpoints are meaningless; the cached machine state and any
    // pending remote command must not answer for a session that no longer
    // exists. Both call sites (launch, session end) are idempotent resets.
    m_sessionArmed = false;
    m_stoppedFile.clear();
    m_stoppedLine = 0;
    m_profileGuided = false;
    m_profileGuidedAddr = 0;
    m_profilingActive = false;
    m_breakpointsArmedThisSession = false;
    m_bases = LineMap::SectionBases();
    m_lastState = MachineState();
    m_pendingDebugCommand.clear();
}

void MainWindow::refuseBuild(const QString &title, const QString &reason, bool critical)
{
    // A refused build is still a completed build as far as callers are
    // concerned: run()'s launch intent must be dropped rather than left armed
    // for the next successful build, and a remote-control `build` must be
    // answered now rather than after its timeout.
    m_launchAfterBuild = false;
    if (m_statusBuild)
        m_statusBuild->setText(tr("Build failed"));
    m_log->appendPlainText(tr("--- build refused: %1 ---").arg(reason));
    emit buildCompleted(false);
    if (critical)
        QMessageBox::critical(this, title, reason);
    else
        QMessageBox::information(this, title, reason);
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

        const QString shownFile = file.isEmpty() ? tr("(build)") : file;
        const int shownLine = line > 0 ? line : 0;
        m_problemEntries.append({shownFile, shownLine, d.message,
                                 d.severity == Diagnostic::Error});

        auto *item = new QTreeWidgetItem(m_problems);
        item->setText(0, shownFile);
        item->setText(1, shownLine > 0 ? QString::number(shownLine) : QString());
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
    // refused run would leave m_launchAfterBuild armed for the next build).
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
    resetSessionState();

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
    settings.setValue(QStringLiteral("layout/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("layout/width"), width());
    settings.setValue(QStringLiteral("layout/height"), height());
    settings.setValue(QStringLiteral("layout/state"), saveState());

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
    // Hatari has no step-out primitive, so: read the return address from the
    // top of the stack (valid inside a jsr/bsr subroutine that has not
    // adjusted A7), arm a one-shot breakpoint there and resume. The read is
    // asynchronous — the stackDumpReady handler does the arming.
    if (!m_host->isStopped() || !m_lastState.regs.valid) {
        m_log->appendPlainText(tr("[step out] needs a stopped program with known registers"));
        return;
    }
    if (m_stepOutPending)
        return;
    m_stepOutPending = true;
    m_host->requestStackDump(m_lastState.regs.a[7], 16);
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
    m_host->armBreakpoint(QStringLiteral("b pc = $%1 :once").arg(address, 0, 16));
    m_host->resume();
}


void MainWindow::profileStart()
{
    // Hatari starts collecting on continue (DebugCpu_SetDebugging) and zeroes
    // the counters if any breakpoint command is issued mid-run — including
    // Pause's one-shot — so profiling is armed while stopped and the stopping
    // breakpoint must exist before this. The flow that works: set a
    // breakpoint, Profile Start here, continue, the breakpoint stops the run.
    if (!m_host->isStopped()) {
        const QString hint = tr("Needs a stopped session — run (F5), stop at a "
                                "breakpoint, then Profile Start");
        m_log->appendPlainText(QStringLiteral("[profile] ") + hint);
        m_profiler->showMessage(hint);
        return;
    }
    m_host->command(QStringLiteral("profile on"));
    m_profilingActive = true;
    syncProfileActions();
    m_log->appendPlainText(tr("[profile] on — continue to collect, then use "
                              "Profile Stop at the next breakpoint stop"));
    m_profiler->showMessage(tr("Collecting — continue, then Profile Stop at the next stop"));
}

bool MainWindow::profileStop()
{
    if (!m_host->isStopped() || m_currentSessionDir.isEmpty()) {
        const QString hint = tr("Needs a stopped session — the save command "
                                "needs the debugger");
        m_log->appendPlainText(QStringLiteral("[profile] ") + hint);
        m_profiler->showMessage(hint);
        return false;
    }
    // The UAE disassembler core (the default with PiST's isolated config)
    // writes profile disassembly to the trace file instead of the save file,
    // so the save would contain only "[...]" gap markers — switch engines for
    // the save. Without Capstone the save stays empty and the parser's error
    // says so plainly.
    m_profileSavePending = true;
    m_host->command(QStringLiteral("setopt --disasm ext"));
    m_host->command(QStringLiteral("profile save %1/profile.txt").arg(m_currentSessionDir));
    m_host->command(QStringLiteral("profile off"));
    // The commands run in order, so the save is complete before this restores
    // the session's default engine — the Disassembly pane must not silently
    // keep the external renderer for the rest of the run.
    m_host->command(QStringLiteral("setopt --disasm uae"));
    m_profilingActive = false;
    syncProfileActions();
    m_profiler->showMessage(tr("Saving — results appear when the save lands"));
    return true;
}

void MainWindow::profileToCursor()
{
    // The whole ritual in one gesture: a one-shot at the cursor line (armed
    // BEFORE `profile on`, since any arm after it would zero the counters),
    // collection on, continue — onDebuggerStopped saves and shows.
    if (!m_host->isStopped() || !m_editor || m_editor->filePath().isEmpty()) {
        const QString hint = tr("Needs a stopped session — run (F5) and stop at "
                                "a breakpoint first");
        m_log->appendPlainText(QStringLiteral("[profile] ") + hint);
        m_profiler->showMessage(hint);
        return;
    }
    const int line = m_editor->textCursor().blockNumber() + 1;
    quint32 address = 0;
    if (!m_programMap.codeAddressFor(m_editor->filePath(), line, &address)) {
        const QString hint = tr("No code on %1:%2 — put the cursor on an instruction line")
                                 .arg(m_editor->filePath())
                                 .arg(line);
        m_log->appendPlainText(QStringLiteral("[profile] ") + hint);
        m_profiler->showMessage(hint);
        return;
    }
    m_profileGuided = true;
    m_profileGuidedAddr = address;
    m_profilingActive = true;
    syncProfileActions();
    m_host->armBreakpoint(QStringLiteral("b pc = $%1 :once").arg(address, 0, 16));
    m_host->command(QStringLiteral("profile on"));
    const QString collecting = tr("Collecting to %1:%2 — results show on the stop")
                                   .arg(m_editor->filePath())
                                   .arg(line);
    m_log->appendPlainText(QStringLiteral("[profile] ") + collecting);
    m_profiler->showMessage(collecting);
    m_host->resume();
}

void MainWindow::showProfileResults()
{
    ProfileData data;
    QString error;
    if (!parseProfile(m_currentSessionDir + QStringLiteral("/profile.txt"), &data, &error)) {
        m_log->appendPlainText(QStringLiteral("[profile] ") + error);
        emit profileResultsReady(false);
        return;
    }
    m_profiler->setProfile(data, &m_programMap,
                           m_editor ? m_editor->filePath() : QString(), m_symbols);
    if (m_profilerDock) {
        m_profilerDock->show();
        m_profilerDock->raise();
    }
    if (m_editor)
        m_editor->setLineHeat(m_profiler->lineCounts());
    m_log->appendPlainText(tr("[profile] %1 instructions, %2 cycles at %3 Hz")
                               .arg(data.totalCount)
                               .arg(data.totalCycles)
                               .arg(data.clockHz));
    emit profileResultsReady(true);
}

void MainWindow::syncProfileActions()
{
    // Profiling is a mode: once collecting, Start and Profile-to-cursor make
    // no sense until Stop; with nothing collecting, Stop has nothing to save.
    // The tooltip always says WHY, so a disabled button is never a riddle.
    const bool stopped = m_host && m_host->isStopped();
    const QString needStopped = tr("Needs a stopped session — run (F5) and stop at a breakpoint");
    if (m_actProfileStart) {
        m_actProfileStart->setEnabled(stopped && !m_profilingActive);
        m_actProfileStart->setToolTip(m_profilingActive
                                          ? tr("Already collecting — Profile Stop and Show ends the run")
                                          : stopped ? tr("Start collecting CPU profile counts from here")
                                                    : needStopped);
    }
    if (m_actProfileStop) {
        m_actProfileStop->setEnabled(stopped && m_profilingActive);
        m_actProfileStop->setToolTip(!m_profilingActive
                                         ? tr("Nothing is collecting — Profile Start begins a run")
                                         : stopped ? tr("Save the profile, then show hot lines and gutter heat")
                                                   : needStopped);
    }
    if (m_actProfileToCursor) {
        m_actProfileToCursor->setEnabled(stopped && !m_profilingActive);
        m_actProfileToCursor->setToolTip(m_profilingActive
                                             ? tr("Already collecting — Profile Stop and Show ends the run")
                                             : stopped ? tr("Collect profile counts to the cursor line, then show the results")
                                                       : needStopped);
    }
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
        if (m_profileGuided) {
            m_profileGuided = false;
            // Any stop ends the guided run: the one-shot's, or a user
            // breakpoint that won the race (results are then partial, which is
            // what "composes" means). The delete covers the race case; when
            // the one-shot itself stopped the run it is already consumed and
            // the delete is a harmless note in the console.
            profileStop();
            m_host->command(QStringLiteral("db pc = $%1 :once").arg(m_profileGuidedAddr, 0, 16));
        }
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
    // Re-arm only with a live session: armBreakpoints() pushes debugger commands,
    // and with no process running each one logs "No emulator session is running."
    // (a false error on a plain edit). Without a session, just refresh the panel —
    // the next Run arms from the models. Matches addWatchpointAddress.
    if (m_host->isRunning())
        armBreakpoints();
    else if (m_breakpointPanel)
        m_breakpointPanel->setWatchpoints(m_watchpoints);
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

bool MainWindow::toggleBreakpointAtLine(int line)
{
    if (!m_editor || m_editor->filePath().isEmpty() || line <= 0)
        return false;

    // Breakpoints are keyed by base name, so two modules linked from different
    // directories under the same file name (`util.s`) collide here and in the
    // gutter. Re-keying on the full path is a larger change (the panel, the
    // listings and the linker map all identify modules the same way); until then
    // this is the documented limitation.
    return toggleBreakpoint(QFileInfo(m_editor->filePath()).fileName(), line);
}

bool MainWindow::toggleBreakpoint(const QString &file, int line)
{
    if (file.isEmpty() || line <= 0)
        return false;

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
    return true;
}

bool MainWindow::toggleBreakpointAtLabel(const QString &name, QString *detail)
{
    for (const SymbolEntry &sym : m_symbols) {
        if (sym.name != name)
            continue;
        if (sym.file.isEmpty()) {
            // A command-line define or macro-generated name has no source
            // position to break at.
            *detail = tr("error symbol '%1' has no source position").arg(name);
            return false;
        }
        // Breakpoints key by base name everywhere (gutter, arming, the
        // panel); the symbol table carries full paths.
        const QString base = QFileInfo(sym.file).fileName();
        // A label written on its own line has no code there — `count:`
        // followed by the instruction on the next line is the norm — and a
        // breakpoint on that line can never fire. Resolve to the first code
        // line at or after the definition; base-independent, so this holds
        // before a session supplies live bases too. An `equ` has no code of
        // its own and finds nothing, rather than resolving to whatever
        // unrelated instruction follows it.
        const int line = m_programMap.nextCodeLine(sym.file, sym.line);
        if (!line) {
            *detail = tr("error '%1' has no code at or after its definition "
                         "(an equate cannot be broken on)").arg(name);
            return false;
        }
        toggleBreakpoint(base, line);
        *detail = QStringLiteral("ok %1:%2").arg(base).arg(line);
        // Tell the agent the address too, when the map can — it confirms the
        // label resolved to the instruction they meant.
        quint32 address = 0;
        if (m_programMap.isResolved()
            && m_programMap.codeAddressFor(sym.file, line, &address))
            *detail += QStringLiteral(" = 0x%1").arg(address, 8, 16, QLatin1Char('0'));
        return true;
    }
    *detail = tr("error no symbol named '%1' (has the project been built?)").arg(name);
    return false;
}

QJsonArray MainWindow::symbolsJson(const QString &filter) const
{
    QJsonArray list;
    for (const SymbolEntry &sym : m_symbols) {
        if (!filter.isEmpty() && !sym.name.contains(filter, Qt::CaseInsensitive))
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("name"), sym.name);
        if (!sym.file.isEmpty()) {
            object.insert(QStringLiteral("file"), sym.file);
            object.insert(QStringLiteral("line"), sym.line);
            // SymbolsView's honesty rule, shared: an address only once the map
            // has live bases, and only from the definition's own line.
            if (m_programMap.isResolved()) {
                quint32 address = 0;
                if (m_programMap.codeAddressFor(sym.file, sym.line, &address))
                    object.insert(QStringLiteral("address"),
                                  QStringLiteral("0x%1").arg(address, 8, 16, QLatin1Char('0')));
            }
        }
        list.append(object);
    }
    return list;
}

QJsonObject MainWindow::documentJson() const
{
    QJsonObject object;
    if (!m_editor || m_editor->filePath().isEmpty())
        return object;
    object.insert(QStringLiteral("path"), m_editor->filePath());
    object.insert(QStringLiteral("text"), m_editor->toPlainText());
    return object;
}

QJsonObject MainWindow::stateJson() const
{
    QJsonObject object;
    const bool running = m_host && m_host->isRunning();
    const bool stopped = m_host && m_host->isStopped();
    object.insert(QStringLiteral("running"), running);
    object.insert(QStringLiteral("stopped"), stopped);
    if (!m_lastState.regs.valid)
        return object;

    const Registers &r = m_lastState.regs;
    const auto hex = [](quint32 value) {
        return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
    };
    object.insert(QStringLiteral("pc"), hex(m_lastState.pc));
    QJsonObject d, a;
    for (int i = 0; i < 8; ++i) {
        d.insert(QStringLiteral("d%1").arg(i), hex(r.d[i]));
        a.insert(QStringLiteral("a%1").arg(i), hex(r.a[i]));
    }
    object.insert(QStringLiteral("d"), d);
    object.insert(QStringLiteral("a"), a);
    object.insert(QStringLiteral("sr"),
                  QStringLiteral("0x%1").arg(r.sr, 4, 16, QLatin1Char('0')));
    return object;
}

QJsonArray MainWindow::problemsJson() const
{
    QJsonArray list;
    for (const ProblemEntry &entry : m_problemEntries) {
        QJsonObject problem;
        problem.insert(QStringLiteral("file"), entry.file);
        problem.insert(QStringLiteral("line"), entry.line);
        problem.insert(QStringLiteral("message"), entry.message);
        problem.insert(QStringLiteral("severity"),
                       entry.error ? QStringLiteral("error") : QStringLiteral("warning"));
        list.append(problem);
    }
    return list;
}

QJsonArray MainWindow::tabsJson() const
{
    QJsonArray list;
    for (int i = 0; i < m_tabs->count(); ++i) {
        QJsonObject tab;
        QString path;
        bool modified = false;
        if (auto *editor = qobject_cast<CodeEditor *>(m_tabs->widget(i))) {
            path = editor->filePath();
            modified = editor->isModifiedSinceLoad();
        } else if (auto *image = qobject_cast<ImageEditor *>(m_tabs->widget(i))) {
            path = image->filePath();
            modified = image->isModifiedSinceLoad();
        }
        tab.insert(QStringLiteral("path"), path);
        tab.insert(QStringLiteral("modified"), modified);
        tab.insert(QStringLiteral("current"), i == m_tabs->currentIndex());
        list.append(tab);
    }
    return list;
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
    QJsonArray list;
    if (!m_profiler)
        return list;
    const QHash<int, quint64> counts = m_profiler->lineCounts();
    QList<QPair<int, quint64>> sorted;
    sorted.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it)
        sorted.append({it.key(), it.value()});
    // Counts descend; ties break by line, the same order the profiler table
    // and the gutter heat use, so all three presentations agree.
    std::sort(sorted.begin(), sorted.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
    });
    for (const auto &[line, count] : sorted) {
        QJsonObject entry;
        entry.insert(QStringLiteral("line"), line);
        // A number, not a string: agents do arithmetic on these.
        entry.insert(QStringLiteral("count"), QJsonValue::fromVariant(count));
        list.append(entry);
    }
    return list;
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
        const int line = item->text(1).toInt();
        if (line <= 0)
            continue;
        // Same contract as double-clicking the item: navigate within the open
        // document only (goToBreakpoint's rule), never switch files blindly.
        const QString file = item->text(0);
        m_problems->setCurrentItem(item);
        if (m_editor && (file.isEmpty() || file == tr("(build)")
                         || LineMap::sameSource(file, m_editor->filePath())))
            m_editor->gotoLine(line);
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
        // Re-derive the host from the models rather than a bare `b all`: watchpoints
        // are armed as `b` conditions (Hatari has no data watchpoints), so `b all`
        // would disarm them too, leaving the panel listing watchpoints that are dead
        // on the emulator until the next arm. armBreakpoints() clears then re-arms —
        // the cleared breakpoints go, the retained watchpoints come back.
        armBreakpoints();
    m_log->appendPlainText(tr("[breakpoints] all cleared"));
}

void MainWindow::clearAllDebugTargets()
{
    // The dock's "Clear all" lists watchpoints too (its button enables when only
    // watchpoints are present), so it clears both models — unlike the
    // breakpoint-only Run-menu action. Watchpoints are armed as `b` conditions
    // (Hatari has no data watchpoints), so the host's `b all` removes them
    // alongside breakpoints; refreshBreakpointMarkers() updates the panel's
    // breakpoint rows but not its watchpoint rows, so those are set explicitly.
    m_breakpoints.clear();
    m_watchpoints.clear();
    refreshBreakpointMarkers();
    if (m_breakpointPanel)
        m_breakpointPanel->setWatchpoints(m_watchpoints);
    if (m_host->isRunning())
        m_host->clearBreakpoints();
    m_log->appendPlainText(tr("[breakpoints] all breakpoints and watchpoints cleared"));
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
    if (m_stopEventPending && state.regs.valid) {
        m_stopEventPending = false;
        if (m_eventSink)
            m_eventSink->publishEvent(QStringLiteral("stopped"),
                                      QStringLiteral("pc=0x%1").arg(state.pc, 8, 16, QLatin1Char('0')));
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
    if (!m_lastState.regs.valid) {
        // "no session state" reads like failure to a remote caller whose
        // natural first move after `run` is `state`; say why instead.
        if (m_host && m_host->isRunning() && !m_host->isStopped())
            return tr("machine is running; state is captured when the debugger stops\n");
        return tr("no session state\n");
    }

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
