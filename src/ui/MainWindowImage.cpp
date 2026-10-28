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

ImageEditor *MainWindow::addImageTab(const QString &path, bool quiet)
{
    // As with a text editor, creation always has a file. (The image editor may
    // become path-less *later* — an imported sheet has no file of its own yet —
    // which is its own deliberate workflow, not a creation path.)
    Q_ASSERT(!path.isEmpty());

    auto *editor = new ImageEditor(this);
    wireImage(editor);
    wireImageReExport(editor);
    const bool ok = isPimPath(path) ? editor->loadFile(path) : editor->importFile(path, false);
    if (!ok) {
        if (!quiet)
            QMessageBox::warning(this, tr("Open"),
                                 tr("Could not open %1: %2").arg(path, editor->lastError()));
        editor->deleteLater();
        return nullptr;
    }
    const int index = m_tabs->addTab(editor, editor->displayName());
    m_tabs->setCurrentIndex(index);
    updateTabTitle(editor);
    syncWatchedDocuments();
    return editor;
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
    // export leaves it. The block map comes from the editor that emitted:
    // an export can finish after the user switched tabs, and reading the
    // recipe back through m_image would then describe a different document
    // (or dereference null with no image tab open at all).
    connect(editor, &ImageEditor::bitplaneReExported, this,
            [this, editor](const QString &path, const QString &scroller, const QString &error) {
                if (!m_log)
                    return;
                if (!error.isEmpty()) {
                    m_log->appendPlainText(tr("[export] re-export failed: %1").arg(error));
                    return;
                }
                m_log->appendPlainText(tr("--- bitplane data (re-export): %1 ---")
                                           .arg(QFileInfo(path).fileName()));
                const ImageDocument &doc = editor->document();
                const int phase = editor->lastBitplaneExportPhase();
                const QVector<BitplaneBlock> blocks = bitplaneLayout(
                    doc.phases().at(phase).cellW,
                    doc.phases().at(phase).cellH,
                    doc.phases().at(phase).frames.size(),
                    editor->lastBitplaneExportOptions());
                for (const BitplaneBlock &block : blocks)
                    m_log->appendPlainText(QStringLiteral("%1 equ $%2")
                                               .arg(block.name, -20)
                                               .arg(block.offset, 4, 16, QLatin1Char('0')));
                if (!scroller.isEmpty())
                    m_log->appendPlainText(tr("%1 — press F7 to assemble it and watch this "
                                              "sprite scroll").arg(QFileInfo(scroller).fileName()));
            });
}


QList<ImageEditor *> MainWindow::openImages() const
{
    QList<ImageEditor *> editors;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *editor = qobject_cast<ImageEditor *>(m_tabs->widget(i)))
            editors.append(editor);
    return editors;
}


ImageEditor *MainWindow::imageForPath(const QString &path) const
{
    const QFileInfo wanted(path);
    for (ImageEditor *editor : openImages())
        if (QFileInfo(editor->filePath()) == wanted)
            return editor;
    return nullptr;
}


bool MainWindow::saveImage(ImageEditor *image, const QString &path)
{
    if (!image || path.isEmpty())
        return false;
    if (!image->saveFile(path))
        return false;
    if (m_watcher)
        m_watcher->rearm(path);
    syncWatchedDocuments();
    writeBackFloppyDoc(path);
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
        if (!saveImage(editor, path)) {
            QMessageBox::critical(this, tr("Save"),
                                  tr("Could not write %1: %2").arg(path, editor->lastError()));
            return false;
        }
    } else if (!saveImage(editor, editor->filePath())) {
        QMessageBox::critical(this, tr("Save"),
                              tr("Could not write %1: %2")
                                  .arg(editor->filePath(), editor->lastError()));
        return false;
    }

    updateTabTitle(editor);
    updateModifiedState();
    return true;
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


} // namespace pist
