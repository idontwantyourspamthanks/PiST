// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// End-to-end test of the debug loop through MainWindow: build, launch, attach,
// resolve a source-line breakpoint, hit it, and confirm the editor follows the
// program counter.
//
// This covers the layer that no other suite touches. The README claims the
// editor follows the PC and that breakpoints work, so those claims are asserted
// here rather than assumed.

#include "editor/CodeEditor.h"
#include "image/ImageDocument.h"
#include "image/StFormats.h"
#include "ui/ImageEditor.h"
#include "ui/ImageCanvas.h"
#include "ui/SheetCanvas.h"
#include "ui/NewImageDialog.h"
#include "ui/BitplaneExportDialog.h"
#include "emu/EmulatorHost.h"
#include "emu/Paths.h"
#include "emu/TosRom.h"
#include "ui/FileBrowser.h"
#include "ui/MainWindow.h"
#include "build/FloppyImage.h"
#include "ui/MemoryView.h"
#include "ui/SetupDialog.h"
#include "ui/Appearance.h"
#include "toolchain/Toolchain.h"
#include "emu/HrdbBackend.h"
#include "ui/StackView.h"
#include "toolchain/ToolFetch.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMenu>
#include <QMenuBar>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QTabBar>
#include <QTableWidget>
#include <QTimer>
#include <QSpinBox>
#include <QToolButton>
#include <QProcess>
#include <QTreeView>
#include <QAbstractItemModel>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QSettings>
#include <QtTest>

#include <functional>

using namespace pist;

class TstGui : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void windowConstructs();
    void aboutMenuIsFirstAndOpensGemDialog();
    void assemblesAndMapsLines();
    void editorShowsExecutionLineAndBreakpoints();

    /// Run must eventually start an emulator session — and must do so *after* the
    /// asynchronous build finishes, not alongside it.
    void runStartsAnEmulatorSession();
    void breakpointSetBeforeRunFiresAndEditorFollows();
    void dockLayoutPersistsAcrossRestart();
    void dockTabMoveMenuMovesDockBetweenAreas();
    void dockTitleBarMoveMenuMovesDock();
    void titleBarLeftPressIsNotConsumed();
    void bottomPanelsAreMovableDocks();
    void dragSurfacesAdvertiseHandCursor();
    void columnHeadersAreLeftAligned();
    void memoryPanesAreIndependent();

    /// The user's actual HRDB path: project settings pick the fork binary and
    /// the hrdb transport, Run must swap the backend object, and the session
    /// must stop at entry — the createBackend/bootstrap/socket-gate chain.
    void hrdbProjectRunsThroughTheSwapPath();

    /// With no explicit transport setting (the default), the probe of the
    /// launched binary decides: a fork lands on HRDB with no configuration.
    void hrdbAutoSelectedFromForkProbe();

    /// A dump applied while editing is enabled (the stopped state) must not
    /// emit memoryEdited per rewritten cell — each of those is a debugger
    /// write plus a refresh, so one dump would start an unbounded
    /// write/refresh storm that destroys any in-progress edit.
    void dumpRewriteDoesNotEmitEdits();

    /// The positive half of the storm fix: with the rewrite blocked, a real
    /// user edit must still emit memoryEdited with the right address and
    /// value. (An over-broad blocker that killed editing would pass the
    /// negative test and keep the pane uneditable — the reported bug.)
    void memoryEditEmitsForRealEdit();

    /// A dump requested before the write landed must not paint the old byte
    /// over the value the user just typed.
    void memoryEditIgnoresStaleDump();

    /// The reported bug, end to end against a real emulator: a byte edited in
    /// the memory view while stopped must survive the follow-up refresh — the
    /// write/refresh storm was what made the value "return", and exactly one
    /// debugger write must result from one edit.
    void memoryEditSurvivesRefreshLive();

    /// Double-clicking a stack row whose value points into text follows the
    /// value; double-clicking a plain value follows the slot's own address.
    /// This is the only way off the stack into the memory view.
    void stackViewDoubleClickFollowsAddress();

    /// Floppy images reach the emulator command line.
    void floppyImagesReachTheCommandLine();
    void fileBrowserShowsTheProjectDirectory();
    /// Pointing the hard-drive pane at another folder moves the selection to
    /// that folder, so new files and pastes land where the user is looking.
    void fileBrowserDirectorySwitchMovesSelection();
    /// Create/rename/delete through the project files panel, including what
    /// happens to the open document when its file is renamed or deleted.
    void fileBrowserFileOperations();
    /// Disk A/B groups list a mounted image, eject it, and export the
    /// hard-drive selection to a new .st / .msa floppy.
    void fileBrowserFloppyGroups();
    /// Copy, cut and paste through the browser panes: duplicate and move on
    /// the hard drive (with pathRenamed for open documents), and across the
    /// hard-drive and floppy panes.
    void fileBrowserCopyMovePanes();
    /// A text entry activated on a disk opens in an editor tab and saves back
    /// into the image, replacing its entry; binaries are refused and
    /// reopening raises the existing tab.
    void floppyTextOpensAndSavesBack();
    /// A Degas image on a disk registers as a sprite sheet in a new untitled
    /// document: slicing a phase pulls its frames out of the sheet, and
    /// Export Sprite Sheet recomposes the sheet back into the image entry.
    void floppyImageOpensAndSavesBack();
    /// Spritesheet mode: phases place on sheets (numerically and by drag),
    /// the mode toggle shows the composed sheet, and placement persists.
    void phasePlacementAndSheetMode();
    /// A saved document's sheet pixels reload from the recorded file, so
    /// slicing works after a reopen, and selecting a phase retargets the
    /// animation preview.
    void sheetPixelsSurviveReopen();
    /// A document with unplaced phases and no sheets: New sheet creates the
    /// 320×200 target and staged phases drag straight onto it.
    void newSheetAndStagingDrag();
    /// The full import journey: staged phase dragged onto the sheet slices
    /// the art under it, and a frame added afterwards slices the next cell.
    void stagedPhaseSlicesOnPlacement();
    /// Phase cell size is editable from the panel and adding frames in sheet
    /// mode extends the strip.
    void phaseCellSizeAndStripGrowth();
    /// The slice dialog cuts an N-frame strip out of an imported sheet into a
    /// new phase's frames.
    void sliceDialogBuildsPhaseFromSheet();
    /// The select tool: a marquee drag sets the selection, dragging inside it
    /// moves the pixels, and copy/paste/nudge/delete edit the active layer
    /// through the undo stack.
    void imageSelectionCopyPaste();
    /// Importing an ST image adopts the file's palette registers as the
    /// active set, so the colours its pixels use are not overspill.
    void importAdoptsTheFilePalette();
    /// A debugger command typed into the console's entry line must be sent
    /// through the backend and its response appended to the console log.
    void consoleCommandRoundTrips();
    void editorTracksUnsavedChanges();
    void diagnoseReportsToolsAndRoms();
    /// The claim the first-run flow rests on: a tool the setup dialog just
    /// installed takes effect in the open window without a restart.
    void setupInstallTakesEffectWithoutRestart();
    /// With every discovery input stripped, the setup dialog must report all
    /// three pieces missing and offer their remedies — the deterministic form
    /// of a first run on a bare machine.
    void setupDialogShowsMissingPieces();
    /// Font-size, font-family and theme preferences take effect on the editor
    /// and the application palette.
    void appearancePreferencesApply();
    /// Documents open in tabs: pristine-tab reuse, raise-not-duplicate, the
    /// modified marker on the label, and the never-empty invariant.
    void documentTabsManageOpenFiles();
    /// Opening a `.pim` creates an ImageEditor tab that coexists with `.s`
    /// tabs; Build still finds the assembly source when the image is focused.
    void imageTabsOpenBesideAssembly();
    /// New Image… requires a file name and writes the blank `.pim` before
    /// the editor opens, rather than defaulting every sprite to sprite.pim.
    void newImageDialogResolvesFileName();
    /// The bitplane export dialog's boxes and pre-shift picker are what the
    /// encoder is handed, and its map is the file's block table.
    void bitplaneExportDialogMapsChoices();
    /// Find and replace in the editor: incremental hits, case/word options,
    /// wrapping, one-step undo for Replace All, and the bar closing cleanly.
    void editorFindAndReplace();
    /// The Search menu exists, its shortcuts land on the focused editor, and an
    /// image tab has nothing to search.
    void searchMenuFollowsTheEditor();

private:
    QString m_vasm;
    QString m_tos;
    QTemporaryDir *m_work = nullptr;
    QString m_source;
};

void TstGui::initTestCase()
{
    m_vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (m_vasm.isEmpty())
        QSKIP("needs vasmm68k_mot");

    m_work = new QTemporaryDir;
    QVERIFY(m_work->isValid());
    m_source = m_work->path() + QStringLiteral("/prog.s");
}

void TstGui::windowConstructs()
{
    // The window builds its whole dock layout, prober and action set here. A
    // failure to start Hatari or vasm must not prevent construction.
    MainWindow window;
    QVERIFY(!window.windowTitle().isEmpty());
    QVERIFY2(window.windowTitle().contains(QLatin1String("PiST")),
             qPrintable(window.windowTitle()));
}

void TstGui::aboutMenuIsFirstAndOpensGemDialog()
{
    MainWindow window;
    const QList<QAction *> menus = window.menuBar()->actions();
    QVERIFY(!menus.isEmpty());
    auto *desk = menus.first()->menu();
    QVERIFY2(desk, "the first menu must be the Atari/Desk menu");
    QCOMPARE(desk->objectName(), QStringLiteral("deskMenu"));
    QVERIFY(!desk->menuAction()->icon().isNull());
    QCOMPARE(desk->actions().size(), 1);
    QAction *about = desk->actions().first();
    QCOMPARE(about->objectName(), QStringLiteral("aboutPistAction"));
    QCOMPARE(about->text().remove(QLatin1Char('&')), QStringLiteral("About PiST"));

    QTimer::singleShot(0, &window, []() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        QCOMPARE(dialog->objectName(), QStringLiteral("aboutDialog"));
        auto *title = dialog->findChild<QLabel *>(QStringLiteral("aboutTitle"));
        auto *version = dialog->findChild<QLabel *>(QStringLiteral("aboutVersion"));
        auto *copyright = dialog->findChild<QLabel *>(QStringLiteral("aboutCopyright"));
        auto *credit = dialog->findChild<QLabel *>(QStringLiteral("aboutCredit"));
        QVERIFY(title && version && copyright && credit);
        QCOMPARE(title->text(), QStringLiteral("PIST, Program in ST"));
        QCOMPARE(version->text(), QStringLiteral(PIST_VERSION));
        QVERIFY(copyright->text().contains(QStringLiteral("2026")));
        QCOMPARE(credit->text(), QStringLiteral("Koala Software/Dad.PRG"));
        dialog->accept();
    });
    about->trigger();
}

// The line map is what turns a gutter click into an address and a PC back into a
// line. Verify against a real listing rather than a fixture.
void TstGui::assemblesAndMapsLines()
{
    QFile src(m_source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tpea\tmsg(pc)\n"
              "\tmove.w\t#9,-(sp)\n"
              "\ttrap\t#1\n"
              "\taddq.l\t#6,sp\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "msg:\tdc.b\t\"HI\",0\n"
              "\teven\n"
              "\tend\n");
    src.close();

    const QString listing = m_work->path() + QStringLiteral("/prog.lst");
    const QString prg = m_work->path() + QStringLiteral("/prog.prg");
    QProcess vasm;
    vasm.start(m_vasm, {QStringLiteral("-quiet"), QStringLiteral("-Ftos"),
                        QStringLiteral("-L"), listing, QStringLiteral("-o"), prg, m_source});
    QVERIFY(vasm.waitForFinished(20000));
    QCOMPARE(vasm.exitCode(), 0);

    LineMap map;
    QString error;
    QVERIFY2(map.parseListing(listing, &error), qPrintable(error));
    QVERIFY(!map.isEmpty());

    // `loop:` is on line 6 and must resolve once we know where the program was
    // loaded. This is the address a gutter click would produce.
    LineMap::SectionBases bases;
    bases.text = 0x12596;
    bases.data = 0x125a4;
    bases.bss = 0x125a6;

    quint32 loopAddress = 0;
    QVERIFY(map.addressFor(QStringLiteral("prog.s"), 6, bases, &loopAddress));
    QVERIFY(loopAddress > bases.text);

    // ...and the reverse direction: the PC inside that instruction resolves back
    // to line 6, which is what drives the editor highlight.
    LineMap::Address back;
    QVERIFY(map.lineFor(loopAddress, bases, &back));
    QCOMPARE(back.line, 6);
    // The listing records the path as it was passed to vasm, which here is
    // absolute. Matching must therefore be path-tolerant, and this assertion is
    // the regression guard for the highlight silently never appearing.
    QVERIFY2(LineMap::sameSource(back.file, QStringLiteral("prog.s")),
             qPrintable("recorded path does not match the open file: " + back.file));
    QVERIFY(LineMap::sameSource(back.file, m_source));
}

// The last step of the debug loop: MainWindow turns a resolved PC into a
// highlight on the editor widget, and a gutter click into a marker. Both are
// plain widget state, so they are asserted directly rather than inferred from
// the line map being correct.
void TstGui::editorShowsExecutionLineAndBreakpoints()
{
    MainWindow window;
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY2(editor, "MainWindow must own a CodeEditor");

    editor->setPlainText(QStringLiteral("\ttext\nstart:\tnop\n\tbra.s\tstart\n\tend\n"));

    // Highlight follows whatever the debug loop resolved.
    QCOMPARE(editor->currentExecutionLine(), 0);
    editor->setCurrentExecutionLine(2);
    QCOMPARE(editor->currentExecutionLine(), 2);

    editor->clearCurrentExecutionLine();
    QCOMPARE(editor->currentExecutionLine(), 0);

    // Gutter markers, which the breakpoint list drives.
    QVERIFY(editor->breakpointLines().isEmpty());
    editor->setBreakpointLines({2, 3});
    QCOMPARE(editor->breakpointLines(), QList<int>({2, 3}));
    editor->setBreakpointLines({});
    QVERIFY(editor->breakpointLines().isEmpty());
}

// The regression this guards: Run called build() (asynchronous) and then checked
// whether a build was in flight, which is always true immediately after starting
// one — so the entire emulator-launch path was unreachable dead code and Run
// appeared to "just build".
void TstGui::runStartsAnEmulatorSession()
{
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        QSKIP("needs hatari");
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    // A program that assembles cleanly and spins, so the session stops at entry.
    const QString source = m_work->path() + QStringLiteral("/run.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    // A project file with no include paths, so no modal dialog can interrupt.
    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error), qPrintable(error));

    MainWindow window;
    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY2(host, "MainWindow must own an EmulatorHost");
    QVERIFY(!host->isRunning());

    window.openPath(source);
    QCOMPARE(window.windowTitle().contains(QLatin1String("PiST")), true);

    QSignalSpy runningSpy(host, &EmulatorHost::runningChanged);

    // Invoke Run exactly as the toolbar action does.
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    // The emulator must come up without further interaction. The build runs first,
    // so allow for it.
    QTRY_VERIFY_WITH_TIMEOUT(host->isRunning(), 30000);

    // ...and it must be a real session, not just a spawned process.
    QSignalSpy stoppedSpy(host, &EmulatorHost::stoppedChanged);
    QVERIFY2(stoppedSpy.wait(30000) || host->isStopped(),
             "the emulator started but never reached the debugger");

    host->stop();
}

void TstGui::hrdbProjectRunsThroughTheSwapPath()
{
    const QString fork = qEnvironmentVariable("PIST_HRDB_HATARI");
    if (fork.isEmpty() || !QFileInfo::exists(fork)) {
        if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR"))
            QFAIL("PIST_REQUIRE_EMULATOR is set but no fork is at $PIST_HRDB_HATARI");
        QSKIP("needs the hrdb-main Hatari fork ($PIST_HRDB_HATARI)");
    }
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    const QString source = m_work->path() + QStringLiteral("/hrdb.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    settings.hatariPath = fork;
    settings.debugBackend = QStringLiteral("hrdb");
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    window.openPath(source);

    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    // The backend object is swapped at launch, so find it after.
    HrdbBackend *backend = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((backend = window.findChild<HrdbBackend *>()) != nullptr
                                 && backend->isRunning(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(backend->isStopped(), 30000);
    backend->stop();
}

void TstGui::hrdbAutoSelectedFromForkProbe()
{
    const QString fork = qEnvironmentVariable("PIST_HRDB_HATARI");
    if (fork.isEmpty() || !QFileInfo::exists(fork)) {
        if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR"))
            QFAIL("PIST_REQUIRE_EMULATOR is set but no fork is at $PIST_HRDB_HATARI");
        QSKIP("needs the hrdb-main Hatari fork ($PIST_HRDB_HATARI)");
    }
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    const QString source = m_work->path() + QStringLiteral("/auto.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    settings.hatariPath = fork;
    // No debugBackend: the default "auto" must follow the probed capability.
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    HrdbBackend *backend = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((backend = window.findChild<HrdbBackend *>()) != nullptr

                                 && backend->isRunning(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(backend->isStopped(), 30000);
    backend->stop();
}

void TstGui::dumpRewriteDoesNotEmitEdits()
{
    MemoryView view;
    QSignalSpy edits(&view, &MemoryView::memoryEdited);

    // Dumps whose first row is not the view's base are discarded as stale, so
    // the fixture is labelled for the base goToAddress aligns to.
    view.goToAddress(0x12590);
    const QString dump = QStringLiteral(
        "00012590: 70 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  p.a.t.`.r.Nu..HI\n"
        "000125a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................\n");

    // Editing disabled: baseline, no signals either way.
    view.applyDump(dump);
    QCOMPARE(edits.size(), 0);

    // Editing enabled (the stopped state, when a user edit can be in flight):
    // the rewrite must still emit nothing — the bytes were not user edits.
    view.setEditingEnabled(true);

    view.applyDump(dump);
    QCOMPARE(edits.size(), 0);
}
void TstGui::memoryEditEmitsForRealEdit()
{
    MemoryView view;
    view.resize(800, 400);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setEditingEnabled(true);

    // Navigate before dumping: applyDump discards a dump whose first row is
    // not the view's base, and goToAddress is what sets it. The base aligns
    // down to 0x12590, so the fixture is labelled for that.
    view.goToAddress(0x12596);
    const QString dump = QStringLiteral(
        "00012590: 70 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  p.a.t.`.r.Nu..HI\n"
        "000125a0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................\n");
    view.applyDump(dump);

    QSignalSpy edits(&view, &MemoryView::memoryEdited);
    auto *table = view.findChild<QTableWidget *>();
    QVERIFY(table);

    // Commit a valid edit on byte 1 (row 0). goToAddress aligns the window
    // down to 0x12590, so byte 1 is at $12591.
    auto *item = table->item(0, 1 + 1);  // address column + byte 1
    QVERIFY(item);
    table->editItem(item);
    auto *editor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    QVERIFY2(editor, "editItem must open an editor");
    editor->setText(QStringLiteral("ab"));
    QTest::keyClick(editor, Qt::Key_Tab);

    QCOMPARE(edits.size(), 1);
    QCOMPARE(edits.first().at(0).toUInt(), 0x12591u);
    QCOMPARE(edits.first().at(1).toUInt(), 0xabu);
    // Confirm the write so the next dump is not treated as stale.
    view.applyDump(QStringLiteral(
        "00012590: 70 ab 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  p.a.t.`.r.Nu..HI\n"));
    // A double-click on a cell whose 4-byte window looks like an address must
    // EDIT, not navigate — pointer-following is Alt+double-click. Use bytes
    // 00 01 25 96 (an address-like long) in row 1's first long position by
    // re-dumping a window that has them.
    QSignalSpy nav(&view, &MemoryView::dumpRequested);
    view.applyDump(QStringLiteral(
        "00012590: 00 01 25 96 70 01 61 04 74 03 60 fe 72 02 4e 75  ....p.a.t.`.r.Nu\n"));
    emit table->cellDoubleClicked(0, 1 + 1);
    QVERIFY2(qobject_cast<QLineEdit *>(QApplication::focusWidget()),
             "double-click on an address-like cell must open the editor");
    // Abandon that editor so it cannot commit on teardown.
    if (auto *ed = qobject_cast<QLineEdit *>(QApplication::focusWidget()))
        QTest::keyClick(ed, Qt::Key_Escape);
}

void TstGui::memoryEditIgnoresStaleDump()
{
    MemoryView view;
    view.resize(800, 400);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.setEditingEnabled(true);
    view.goToAddress(0x12590);
    view.applyDump(QStringLiteral(
        "00012590: 00 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  ..a.t.`.r.Nu..HI\n"));

    auto *table = view.findChild<QTableWidget *>();
    QVERIFY(table);
    auto *item = table->item(0, 1);
    QVERIFY(item);
    QCOMPARE(item->text(), QStringLiteral("00"));

    QSignalSpy edits(&view, &MemoryView::memoryEdited);
    table->editItem(item);
    auto *editor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    QVERIFY2(editor, "editItem must open an editor");
    editor->setText(QStringLiteral("70"));
    QTest::keyClick(editor, Qt::Key_Tab);
    QCOMPARE(edits.size(), 1);

    // This dump still has the old byte: it was requested before the write.
    view.applyDump(QStringLiteral(
        "00012590: 00 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  ..a.t.`.r.Nu..HI\n"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("70"));

    view.applyDump(QStringLiteral(
        "00012590: 70 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  p.a.t.`.r.Nu..HI\n"));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("70"));
}

void TstGui::memoryEditSurvivesRefreshLive()
{
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        QSKIP("needs hatari");
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    const QString source = m_work->path() + QStringLiteral("/edit.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);
    QTRY_VERIFY_WITH_TIMEOUT(host->isStopped(), 30000);

    // The memory pane auto-navigates to the program on the first stop. Edit
    // the first byte of the first row.
    auto *view = window.findChild<MemoryView *>();
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(view->currentAddress() != 0, 10000);

    auto *table = view->findChild<QTableWidget *>();
    QVERIFY(table);
    // The navigation emits dumpRequested; the dump arrives asynchronously, so
    // the item fetch must be re-evaluated inside the wait.
    QTableWidgetItem *item = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((item = table->item(0, 1)) && !item->text().isEmpty(), 10000);

    QSignalSpy edits(view, &MemoryView::memoryEdited);
    const QString original = item->text();
    const QString replacement = (original == QLatin1String("70")) ? QStringLiteral("71")
                                                                  : QStringLiteral("70");
    table->editItem(item);
    auto *editor = qobject_cast<QLineEdit *>(QApplication::focusWidget());
    QVERIFY2(editor, "editItem must open an editor");
    editor->setText(replacement);
    QTest::keyClick(editor, Qt::Key_Tab);

    QCOMPARE(edits.size(), 1);

    // The cell already shows the typed byte; that is not a round-trip. Blank
    // it so the wait below is for a dump that actually contains the write.
    {
        const QSignalBlocker blocker(table);
        if (auto *cell = table->item(0, 1))
            cell->setText(QString());
    }

    // The user's bug: the value "returned" after the write's follow-up
    // refresh. Wait for the refresh to land and require the new value to
    // still be displayed.
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 1)->text(), replacement.toUpper(), 15000);
    QTest::qWait(500);  // let any storm fire
    QCOMPARE(table->item(0, 1)->text(), replacement.toUpper());
    QCOMPARE(edits.size(), 1);

    host->stop();
}

void TstGui::stackViewDoubleClickFollowsAddress()
{
    StackView view;
    view.resize(480, 400);
    view.show();

    // Two longs at SP 0x1000: 0x00012596 (inside the text range, so a likely
    // return address) and 0x43 (plain data — odd, so not address-like).
    view.setStackDump(0x1000,
                      QStringLiteral("00001000: 00 01 25 96 00 00 00 43 00 00 00 00 00 00 00 00\n"),
                      0x12500, 0x12600);

    QSignalSpy spy(&view, &StackView::addressActivated);
    auto *table = view.findChild<QTableWidget *>();
    QVERIFY(table);

    // Drive the table's cellDoubleClicked directly: synthetic double-clicks
    // (QTest::mouseDClick and bare sendEvent alike) do not reach the view's
    // handler offscreen, and the gesture→signal delivery is Qt's contract —
    // ours is the row→address mapping, which is what this pins.
    emit table->cellDoubleClicked(0, 1);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).toUInt(), 0x12596u);

    spy.clear();
    emit table->cellDoubleClicked(1, 1);
    QCOMPARE(spy.size(), 1);
    QCOMPARE(spy.first().at(0).toUInt(), 0x1004u);
}

// The core debug-loop regression: a breakpoint set before Run must fire, and the
// editor must follow the program counter to it. This is the one test that would
// have caught the bug where the arming chain and the live-base wiring were both
// missing — every component passed its own test while the loop as a whole did
// nothing (docs/code-review-glm-001.md P1).
void TstGui::breakpointSetBeforeRunFiresAndEditorFollows()
{
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        QSKIP("needs hatari");
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    // A program that spins, with the loop body on line 3, so a breakpoint there
    // fires the moment it resumes past the entry stop.
    const QString source = m_work->path() + QStringLiteral("/bp.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                     // line 1
              "start:\tmoveq\t#0,d0\n"       // line 2
              "loop:\taddq.w\t#1,d0\n"        // line 3  <- breakpoint
              "\tbra.s\tloop\n"               // line 4
              "\teven\n"                       // line 5
              "\tend\n");                      // line 6
    src.close();

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY2(host, "MainWindow must own an EmulatorHost");
    QVERIFY2(editor, "MainWindow must own a CodeEditor");

    window.openPath(source);

    // Set the breakpoint BEFORE Run, exactly as a gutter click does.
    QVERIFY(QMetaObject::invokeMethod(&window, "toggleBreakpointAtLine",
                                      Qt::DirectConnection, Q_ARG(int, 3)));

    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    // The editor reaching the entry line means bases arrived and armBreakpoints
    // has been queued. Resume then flushes those `b` commands before `c` —
    // a fixed wait after the first stop was racing the attach.
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 3, 30000);

    host->stop();
}

// The panel arrangement is Photoshop-style: docks are movable/floatable/
// closable, and the whole arrangement persists across runs. Prove the round-trip
// without relying on a mouse: change a dock's visibility in one window, save the
// state, and confirm a fresh window restores exactly that.

// The dock "Move to" menu is a non-modal popup; find it among the top-levels.
static QMenu *findDockMoveMenu()
{
    for (QWidget *w : QApplication::topLevelWidgets())
        if (auto *m = qobject_cast<QMenu *>(w))
            if (m->objectName() == QLatin1String("dockMoveMenu"))
                return m;
    return nullptr;
}

static QAction *menuAction(QMenu *menu, const char *text)
{
    for (QAction *a : menu->actions())
        if (a->text() == QLatin1String(text))
            return a;
    return nullptr;
}

// The tab bar of a tabbed dock group showing the given tab (a QMainWindowTabBar,
// not a content QTabWidget's tab bar), located by the dock's title.
static QTabBar *findDockTabBar(QWidget *root, const QString &tabText, int *index)
{
    for (QTabBar *tb : root->findChildren<QTabBar *>())
        for (int i = 0; i < tb->count(); ++i)
            if (tb->tabText(i) == tabText) {
                *index = i;
                return tb;
            }
    return nullptr;
}

// Right-clicking a dock's *tab* in a tabbed group offers the "Move to" menu;
// choosing an entry re-docks that panel. This is the case the first version
// missed: a tabbed dock has no title bar, and its tab belongs to the dock area,
// not the dock. The synthesized press goes through QApplication::notify, which
// is what the app-level event filter listens on.
void TstGui::dockTabMoveMenuMovesDockBetweenAreas()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("memoryDock"));
    QVERIFY(dock);
    // The memory dock starts tabbed in the bottom area, so moving it left is an
    // observable change.
    QVERIFY(window.dockWidgetArea(dock) != Qt::LeftDockWidgetArea);

    int index = -1;
    QTabBar *tabBar = findDockTabBar(&window, dock->windowTitle(), &index);
    QVERIFY2(tabBar, "the memory dock is tabbed, so its tab must exist");
    QVERIFY(index >= 0);

    // Press-only on the tab (no release) so the popup that appears doesn't have
    // an item under the cursor get triggered by a click's release half.
    const QPoint pos = tabBar->tabRect(index).center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), tabBar->mapToGlobal(pos),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(tabBar, &press);
    QCoreApplication::processEvents();

    QMenu *menu = findDockMoveMenu();
    QVERIFY2(menu, "right-clicking a dock tab must offer the move menu");
    QVERIFY(menu->isVisible());
    QAction *toLeft = menuAction(menu, "Move to left");
    QVERIFY(toLeft);
    toLeft->trigger();  // fires the move; the popup closes itself
    QCoreApplication::processEvents();
    QCOMPARE(window.dockWidgetArea(dock), Qt::LeftDockWidgetArea);
}

// Right-clicking a non-tabbed dock's painted title bar offers the same menu.
void TstGui::dockTitleBarMoveMenuMovesDock()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    // The project-files dock sits alone in the left area, so it has a painted
    // title bar rather than a tab.
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    QVERIFY(dock);
    QVERIFY(window.tabifiedDockWidgets(dock).isEmpty());
    QVERIFY(window.dockWidgetArea(dock) != Qt::BottomDockWidgetArea);

    // The title bar is the strip above the content; press near the top-centre.
    const QPoint pos(dock->width() / 2, 5);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), dock->mapToGlobal(pos),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(dock, &press);
    QCoreApplication::processEvents();

    QMenu *menu = findDockMoveMenu();
    QVERIFY2(menu, "right-clicking a dock title bar must offer the move menu");
    QVERIFY(menu->isVisible());
    QAction *toBottom = menuAction(menu, "Move to bottom");
    QVERIFY(toBottom);
    toBottom->trigger();
    QCoreApplication::processEvents();
    QCOMPARE(window.dockWidgetArea(dock), Qt::BottomDockWidgetArea);
}

// A left-press on a title bar must pass through the app-level event filter to
// the dock, or native dragging could never start. The filter now also hooks the
// left-press (to arm the video pass-through), so this pins that it does not
// *consume* the press. (A full synthetic drag is not testable offscreen: the
// transient floating window a cross-area drag creates crashes the offscreen
// platform's backing-store teardown, so drag itself is verified on a real
// display instead.)
void TstGui::titleBarLeftPressIsNotConsumed()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    QVERIFY(dock);

    // A spy recording whether the dock itself receives the press. App-level
    // filters run before the target's own filters, so if the app filter
    // consumed the press this would never fire.
    struct Spy : QObject {
        int presses = 0;
        bool eventFilter(QObject *, QEvent *e) override {
            if (e->type() == QEvent::MouseButtonPress)
                ++presses;
            return false;
        }
    } spy;
    dock->installEventFilter(&spy);

    const QPoint pos(dock->width() / 2, 5);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), dock->mapToGlobal(pos),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dock, &press);
    QCoreApplication::processEvents();

    dock->removeEventFilter(&spy);
    QCOMPARE(spy.presses, 1);
}

// Problems and the build/debug console are ordinary docks tabbed with Memory in
// the bottom area — not tabs locked inside a QTabWidget — so they can be moved
// like every other panel, and other panels can be dragged into the bottom group.
void TstGui::bottomPanelsAreMovableDocks()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *problems = window.findChild<QDockWidget *>(QStringLiteral("problemsDock"));
    auto *console = window.findChild<QDockWidget *>(QStringLiteral("consoleDock"));
    auto *memory = window.findChild<QDockWidget *>(QStringLiteral("memoryDock"));
    QVERIFY2(problems && console && memory,
             "Problems, Console and Memory must each be a real dock");

    // All three start tabbed together in the bottom area.
    QCOMPARE(window.dockWidgetArea(console), Qt::BottomDockWidgetArea);
    QVERIFY(window.tabifiedDockWidgets(console).contains(problems));
    QVERIFY(window.tabifiedDockWidgets(console).contains(memory));

    // Right-clicking the console's tab offers the move menu, and moving it to
    // the right takes it out of the bottom group.
    int index = -1;
    QTabBar *tabBar = findDockTabBar(&window, console->windowTitle(), &index);
    QVERIFY2(tabBar, "the console is tabbed, so its tab must exist");
    const QPoint pos = tabBar->tabRect(index).center();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(pos), tabBar->mapToGlobal(pos),
                      Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(tabBar, &press);
    QCoreApplication::processEvents();

    QMenu *menu = findDockMoveMenu();
    QVERIFY2(menu, "right-clicking the console tab must offer the move menu");
    QAction *toRight = menuAction(menu, "Move to right");
    QVERIFY(toRight);
    toRight->trigger();
    QCoreApplication::processEvents();
    QCOMPARE(window.dockWidgetArea(console), Qt::RightDockWidgetArea);
    QVERIFY(!window.tabifiedDockWidgets(console).contains(problems));
}


// Drag surfaces advertise themselves with a cursor: an open hand over a dock's
// title bar and over a dock tab. The dock's *content* keeps the arrow (the hand
// must not inherit into the panel body).
void TstGui::dragSurfacesAdvertiseHandCursor()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    // Title bar: the dock carries the open-hand cursor (its title bar is painted
    // by the dock, so the dock's cursor is what shows over it).
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("memoryDock"));
    QVERIFY(dock);
    QCOMPARE(dock->cursor().shape(), Qt::OpenHandCursor);
    // The panel body stays an arrow, so the hand doesn't leak into it.
    QVERIFY(dock->widget());
    QCOMPARE(dock->widget()->cursor().shape(), Qt::ArrowCursor);

    // Tab: the dock tab bar carries the open hand too.
    int index = -1;
    QTabBar *tabBar = findDockTabBar(&window, dock->windowTitle(), &index);
    QVERIFY2(tabBar, "the memory dock is tabbed, so its tab bar must exist");
    QCOMPARE(tabBar->cursor().shape(), Qt::OpenHandCursor);
}


// A stretched column's header must be left-aligned so its title sits over the
// content rather than centred in a wide column. Enforced globally by the app
// event filter polishing every QHeaderView; check it landed on the table views.
void TstGui::columnHeadersAreLeftAligned()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    int checked = 0;
    for (QTableWidget *table : window.findChildren<QTableWidget *>()) {
        QHeaderView *h = table->horizontalHeader();
        if (!h->isVisibleTo(&window))
            continue;  // e.g. MemoryView hides its header
        QCOMPARE(h->defaultAlignment(), Qt::AlignLeft | Qt::AlignVCenter);
        ++checked;
    }
    QVERIFY2(checked >= 3, "expected several visible table headers to check");
}

void TstGui::consoleCommandRoundTrips()
{
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        QSKIP("needs hatari");
    {
        const QList<TosRom> roms = findTosRoms();
        const TosRom rom = selectPreferredRom(roms, Machine::St);
        if (rom.path.isEmpty() || !rom.supportsAutostart())
            QSKIP("needs an autostart-capable TOS ROM for an ST");
    }

    const QString source = m_work->path() + QStringLiteral("/console.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"
              "start:\tmoveq\t#1,d0\n"
              "loop:\tbra.s\tloop\n"
              "\teven\n"
              "\tend\n");
    src.close();

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);
    QTRY_VERIFY_WITH_TIMEOUT(host->isStopped(), 30000);

    // Type the command into the actual entry widget: a dead input line (never
    // enabled, or a broken returnPressed wiring) fails here rather than
    // passing on a direct call to the slot.
    auto *input = window.findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    QVERIFY(input);
    QTRY_VERIFY_WITH_TIMEOUT(input->isEnabled(), 5000);
    input->setText(QStringLiteral("r"));
    QTest::keyClick(input, Qt::Key_Return);

    // The register dump for the typed `r` must land in the console log, after
    // its echo.
    QTRY_VERIFY_WITH_TIMEOUT(window.debugConsoleText().contains(QLatin1String("> r"))
                                 && window.debugConsoleText().contains(QLatin1String("D0")),
                             10000);
    host->stop();
}

void TstGui::dockLayoutPersistsAcrossRestart()
{
    QByteArray state;
    {
        MainWindow window;
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("registersDock"));
        QVERIFY2(dock, "the Registers dock must have a stable objectName for saveState");
        dock->setVisible(false);
        state = window.saveState();
        QVERIFY(!state.isEmpty());
    }
    {
        MainWindow window;
        QVERIFY(window.restoreState(state));
        auto *dock = window.findChild<QDockWidget *>(QStringLiteral("registersDock"));
        QVERIFY(dock);
        // The hidden Registers dock must come back hidden: the user's arrangement
        // survived the save/restore cycle that closeEvent and the constructor use.
        QVERIFY2(!dock->isVisibleTo(&window), "hidden dock did not persist as hidden");
    }
}

// Multiple memory panes: each is its own tabbed dock with its own routing tag,
// so two regions can be watched at once without dumps bleeding between panes.
void TstGui::memoryPanesAreIndependent()
{
    MainWindow window;

    // The first pane exists from construction, in the base "memoryDock".
    QVERIFY(window.findChild<QDockWidget *>(QStringLiteral("memoryDock")));

    // Adding another pane creates a second, distinctly-named dock.
    QMetaObject::invokeMethod(&window, "addMemoryPane", Qt::DirectConnection);
    auto *second = window.findChild<QDockWidget *>(QStringLiteral("memoryDock1"));
    QVERIFY2(second, "the second memory pane must exist as memoryDock1");

    // And it holds a real MemoryView, not an empty dock.
    QVERIFY(second->widget() != nullptr);
    QVERIFY(qobject_cast<MemoryView *>(second->widget()));
}

// Floppy drives are the last emulator feature that had no UI. Verify the
// configured images actually reach `--disk-a`/`--disk-b`, since a dialog field
// that never reaches the command line is the failure mode this project has hit
// before (BuildService's include paths, and the run path itself).
void TstGui::floppyImagesReachTheCommandLine()
{
    SessionConfig config;
    config.hatariPath = QStringLiteral("hatari");
    config.programPath = QStringLiteral("/tmp/prog.prg");
    config.floppyImages = {QStringLiteral("/disks/boot.st"),
                           QStringLiteral("/disks/data.st")};

    const QStringList argv = config.toArgv();
    const QString joined = argv.join(QLatin1Char(' '));

    QVERIFY2(joined.contains(QLatin1String("--disk-a /disks/boot.st")), qPrintable(joined));
    QVERIFY2(joined.contains(QLatin1String("--disk-b /disks/data.st")), qPrintable(joined));

    // A user might also supply only a disk for A:, which must not emit a bare
    // --disk-b with nothing after it.
    SessionConfig onlyA;
    onlyA.hatariPath = QStringLiteral("hatari");
    onlyA.programPath = QStringLiteral("/tmp/prog.prg");
    onlyA.floppyImages = {QStringLiteral("/disks/boot.st"), QString()};
    const QString joinedA = onlyA.toArgv().join(QLatin1Char(' '));
    QVERIFY(joinedA.contains(QLatin1String("--disk-a")));
    QVERIFY2(!joinedA.contains(QLatin1String("--disk-b")), qPrintable(joinedA));

    // Last --disk-a wins. A user image on A: must survive the AUTO-folder path
    // that TOS < 1.04 uses to autostart — otherwise the sidebar lists a
    // magazine while Hatari boots auto.st.
    SessionConfig clash;
    clash.hatariPath = QStringLiteral("hatari");
    clash.programPath = QStringLiteral("/tmp/prog.prg");
    clash.floppyImages = {QStringLiteral("/disks/magazine.st"), QString()};
    clash.bootFloppyPath = QStringLiteral("/tmp/auto.st");
    const QStringList clashArgv = clash.toArgv();
    QVERIFY2(clashArgv.contains(QStringLiteral("/disks/magazine.st")),
             qPrintable(clashArgv.join(QLatin1Char(' '))));
    QVERIFY2(!clashArgv.contains(QStringLiteral("/tmp/auto.st")),
             qPrintable(clashArgv.join(QLatin1Char(' '))));

    SessionConfig spaced;
    spaced.hatariPath = QStringLiteral("hatari");
    spaced.programPath = QStringLiteral("/tmp/prog.prg");
    spaced.floppyImages = {
        QStringLiteral("/home/ryan/Code/AtariST/ST Format Magazine Issue 08 (1990-03)(Future Publishing).st")
    };
    QVERIFY(spaced.toArgv().contains(spaced.floppyImages.at(0)));
}

void TstGui::fileBrowserShowsTheProjectDirectory()
{
    // A project directory with a couple of files, so the browser has something to
    // root itself in.
    const QString dir = m_work->path() + QStringLiteral("/browse");
    QVERIFY(QDir().mkpath(dir));
    const QString src = dir + QStringLiteral("/prog.s");
    QFile f(src);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("\tnop\n");
    f.close();

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY2(browser, "MainWindow must own a FileBrowser");

    // Opening a file must point the browser at that file's directory, so the
    // browser and the editor never disagree about which project is open.
    window.openPath(src);

    auto *view = browser->findChild<QTreeView *>(QStringLiteral("hardDriveView"));
    QVERIFY(view);
    QVERIFY2(view->model(), "the browser must have a model");
    const QString rootPath = view->model()->data(view->rootIndex(), Qt::UserRole + 1).toString();
    QCOMPARE(rootPath, dir);

    // The file being edited is the selected entry.
    const QString selected = view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString();
    QCOMPARE(selected, src);
}


void TstGui::fileBrowserDirectorySwitchMovesSelection()
{
    // Two project directories, the first with the file the IDE opens.
    const QString dirA = m_work->path() + QStringLiteral("/switchA");
    const QString dirB = m_work->path() + QStringLiteral("/switchB");
    QVERIFY(QDir().mkpath(dirA));
    QVERIFY(QDir().mkpath(dirB));
    const QString src = dirA + QStringLiteral("/prog.s");
    QFile f(src);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("\tnop\n");
    f.close();

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY2(browser, "MainWindow must own a FileBrowser");
    window.openPath(src);

    auto *view = browser->findChild<QTreeView *>(QStringLiteral("hardDriveView"));
    QVERIFY(view);
    QCOMPARE(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(), src);

    // Typing another folder into the path field moves the selection to that
    // folder: the context menu (New File…, Paste) acts on the current index,
    // so with the old selection left in place a new file was created in dirA
    // while the user was looking at dirB.
    browser->showDirectory(dirB);
    QCOMPARE(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(), dirB);
    QCOMPARE(browser->selectedHardDrivePaths(), QStringList{dirB});
}


void TstGui::fileBrowserFileOperations()
{
    const QString dir = m_work->path() + QStringLiteral("/ops");
    QVERIFY(QDir().mkpath(dir));
    const QString src = dir + QStringLiteral("/prog.s");
    QFile f(src);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("\tnop\n");
    f.close();

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(browser && editor);
    window.openPath(src);

    // Create: lands on disk, refuses duplicates and names with separators.
    const QString created = browser->createFile(dir, QStringLiteral("new.s"));
    QCOMPARE(created, dir + QStringLiteral("/new.s"));
    QVERIFY(QFileInfo::exists(created));
    QVERIFY(browser->createFile(dir, QStringLiteral("new.s")).isEmpty());
    QVERIFY(browser->createFile(dir, QStringLiteral("a/b.s")).isEmpty());
    const QString folder = browser->createFolder(dir, QStringLiteral("sub"));
    QVERIFY(QFileInfo(folder).isDir());

    // Rename: the file moves on disk and the open document follows it.
    const QString renamed = dir + QStringLiteral("/renamed.s");
    QVERIFY(browser->renamePath(src, QStringLiteral("renamed.s")));
    QVERIFY(!QFileInfo::exists(src));
    QVERIFY(QFileInfo::exists(renamed));
    QCOMPARE(editor->filePath(), renamed);
    // Renaming something else must not touch the document.
    QVERIFY(browser->renamePath(created, QStringLiteral("other.s")));
    QCOMPARE(editor->filePath(), renamed);

    // Delete: files and folders go away; an unmodified open document's tab
    // closes rather than pointing at a ghost.
    QVERIFY(browser->deletePath(dir + QStringLiteral("/other.s")));
    QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/other.s")));
    QVERIFY(!editor->isModifiedSinceLoad());
    QVERIFY(browser->deletePath(renamed));
    QVERIFY(!QFileInfo::exists(renamed));
    // The document's tab closed rather than pointing at a ghost (the widget
    // itself lingers until the event loop deletes it).
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    for (int i = 0; i < tabs->count(); ++i) {
        auto *open = qobject_cast<CodeEditor *>(tabs->widget(i));
        QVERIFY(open);
        QVERIFY(open->filePath() != renamed);
    }
    QVERIFY(browser->deletePath(folder));
    QVERIFY(!QFileInfo(folder).exists());
}

void TstGui::fileBrowserFloppyGroups()
{
    const QString dir = m_work->path() + QStringLiteral("/disks");
    QVERIFY(QDir().mkpath(dir));
    const QString src = dir + QStringLiteral("/prog.s");
    {
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\tnop\n");
    }
    const QString payload = dir + QStringLiteral("/hello.txt");
    {
        QFile f(payload);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hi");
    }

    const QString prgPath = dir + QStringLiteral("/boot.prg");
    {
        QFile f(prgPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(32, 'A'));
    }
    const QString image = dir + QStringLiteral("/boot.st");
    QString error;
    QVERIFY2(floppy::writeAutoFolderImage(image, prgPath, &error), qPrintable(error));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    window.openPath(src);

    QVERIFY(browser->findChild<QTreeView *>(QStringLiteral("hardDriveView")));
    auto *diskA = browser->findChild<QTreeView *>(QStringLiteral("diskAView"));
    auto *diskB = browser->findChild<QTreeView *>(QStringLiteral("diskBView"));
    auto *exportBtn = browser->findChild<QPushButton *>(QStringLiteral("hardDriveExport"));
    auto *changeA = browser->findChild<QPushButton *>(QStringLiteral("diskAChange"));
    auto *ejectA = browser->findChild<QPushButton *>(QStringLiteral("diskAEject"));
    QVERIFY(diskA && diskB && exportBtn && changeA && ejectA);

    QSignalSpy spy(browser, &FileBrowser::floppyImageChanged);
    browser->setFloppyImages({image, QString()});
    QCOMPARE(browser->floppyImages().at(0), image);
    QVERIFY(ejectA->isEnabled());
    auto *nameA = browser->findChild<QLabel *>(QStringLiteral("diskAName"));
    QVERIFY(nameA);
    QCOMPARE(nameA->text(), QStringLiteral("boot.st"));

    std::function<bool(QAbstractItemModel *, const QModelIndex &, const QString &)> findInTree;
    findInTree = [&](QAbstractItemModel *model, const QModelIndex &parent,
                     const QString &text) -> bool {
        const int rows = model->rowCount(parent);
        for (int r = 0; r < rows; ++r) {
            const QModelIndex idx = model->index(r, 0, parent);
            if (idx.data().toString().compare(text, Qt::CaseInsensitive) == 0)
                return true;
            if (findInTree(model, idx, text))
                return true;
        }
        return false;
    };
    QVERIFY2(findInTree(diskA->model(), QModelIndex(), QStringLiteral("AUTO")),
             "Disk A must list the AUTO folder from the mounted image");
    QVERIFY(findInTree(diskA->model(), QModelIndex(), QStringLiteral("PROG.PRG")));

    ejectA->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), 0);
    QVERIFY(spy.at(0).at(1).toString().isEmpty());
    QVERIFY(browser->floppyImages().at(0).isEmpty());
    QVERIFY(!ejectA->isEnabled());

    auto *hd = browser->findChild<QTreeView *>(QStringLiteral("hardDriveView"));
    QVERIFY(hd);
    auto *fs = qobject_cast<QFileSystemModel *>(hd->model());
    QVERIFY(fs);
    const QModelIndex found = fs->index(payload);
    QVERIFY2(found.isValid(), "hello.txt must be visible on the hard drive");
    hd->selectionModel()->select(found, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    const QString exported = dir + QStringLiteral("/out.st");
    QVERIFY2(browser->exportHardDriveSelection(exported, &error), qPrintable(error));
    const QVector<floppy::Entry> listed = floppy::listImage(exported, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    bool haveHello = false;
    for (const floppy::Entry &e : listed) {
        if (e.path.compare(QStringLiteral("HELLO.TXT"), Qt::CaseInsensitive) == 0)
            haveHello = true;
    }
    QVERIFY2(haveHello, "exported image must contain the selected hard-drive file");

    const QString exportedMsa = dir + QStringLiteral("/out.msa");
    QVERIFY2(browser->exportHardDriveSelection(exportedMsa, &error), qPrintable(error));
    QVERIFY(QFileInfo(exportedMsa).size() > 10);
}

void TstGui::fileBrowserCopyMovePanes()
{
    const QString dir = m_work->path() + QStringLiteral("/copymove");
    QVERIFY(QDir().mkpath(dir));
    QVERIFY(QDir().mkpath(dir + QStringLiteral("/sub")));
    const QString alpha = dir + QStringLiteral("/alpha.txt");
    {
        QFile f(alpha);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("alpha");
    }
    const QString beta = dir + QStringLiteral("/beta.txt");
    {
        QFile f(beta);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("beta");
    }

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    window.openPath(alpha);

    auto contentOf = [](const QString &path) {
        QFile f(path);
        f.open(QIODevice::ReadOnly);
        return f.readAll();
    };

    // Copy within the hard drive: the original stays, a duplicate lands in sub.
    browser->copyHardDrivePaths({alpha}, false);
    QVERIFY(browser->pasteIntoDirectory(dir + QStringLiteral("/sub")));
    QCOMPARE(contentOf(dir + QStringLiteral("/sub/alpha.txt")), QByteArray("alpha"));
    QCOMPARE(contentOf(alpha), QByteArray("alpha"));

    // Cut within the hard drive: the file moves and open documents follow.
    QSignalSpy renamed(browser, &FileBrowser::pathRenamed);
    browser->copyHardDrivePaths({beta}, true);
    QVERIFY(browser->pasteIntoDirectory(dir + QStringLiteral("/sub")));
    QVERIFY(!QFileInfo::exists(beta));
    QCOMPARE(contentOf(dir + QStringLiteral("/sub/beta.txt")), QByteArray("beta"));
    QCOMPARE(renamed.count(), 1);
    QCOMPARE(renamed.at(0).at(0).toString(), beta);
    QCOMPARE(renamed.at(0).at(1).toString(), dir + QStringLiteral("/sub/beta.txt"));

    // A cut is one-shot: the clipboard is spent by the paste above.
    QVERIFY(!browser->pasteIntoDirectory(dir + QStringLiteral("/sub")));

    // Two disks: A holds a file, B is empty.
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("ONE.TXT");
    file.data = QByteArrayLiteral("one");
    items.append(file);
    const QString imageA = dir + QStringLiteral("/a.st");
    const QString imageB = dir + QStringLiteral("/b.st");
    QString error;
    QVERIFY2(floppy::writeImage(imageA, items, &error), qPrintable(error));
    QVERIFY2(floppy::writeImage(imageB, {}, &error), qPrintable(error));
    browser->setFloppyImages({imageA, imageB});

    // Copy from disk A to disk B.
    browser->copyFloppyEntries(0, {QStringLiteral("ONE.TXT")}, false);
    QVERIFY(browser->pasteIntoFloppy(1, QString()));
    const QVector<floppy::Entry> onB = floppy::listImage(imageB, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(onB.size(), 1);
    QCOMPARE(onB.at(0).path, QStringLiteral("ONE.TXT"));
    // The copy on A is untouched, and pasting again keeps adding.
    QVERIFY(browser->pasteIntoFloppy(1, QString()));
    const QVector<floppy::Entry> onBAgain = floppy::listImage(imageB, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(onBAgain.size(), 2);

    // Cut off disk A onto the hard drive: extracted here, removed there.
    browser->copyFloppyEntries(0, {QStringLiteral("ONE.TXT")}, true);
    QVERIFY(browser->pasteIntoDirectory(dir));
    QCOMPARE(contentOf(dir + QStringLiteral("/ONE.TXT")), QByteArray("one"));
    QStringList leftOnA;
    for (const floppy::Entry &e : floppy::listImage(imageA, &error))
        leftOnA.append(e.path);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!leftOnA.contains(QStringLiteral("ONE.TXT")));
}

void TstGui::floppyTextOpensAndSavesBack()
{
    const QString dir = m_work->path() + QStringLiteral("/floppytext");
    QVERIFY(QDir().mkpath(dir));

    QVector<floppy::Item> items;
    floppy::Item docsDir;
    docsDir.destPath = QStringLiteral("DOCS");
    docsDir.isDirectory = true;
    items.append(docsDir);
    floppy::Item doc;
    doc.destPath = QStringLiteral("DOC.TXT");
    doc.data = QByteArrayLiteral("hello");
    items.append(doc);
    floppy::Item nested;
    nested.destPath = QStringLiteral("DOCS/NOTE.TXT");
    nested.data = QByteArrayLiteral("note");
    items.append(nested);
    floppy::Item readme;
    readme.destPath = QStringLiteral("README");
    readme.data = QByteArrayLiteral("readme text");
    items.append(readme);
    floppy::Item prg;
    prg.destPath = QStringLiteral("PROG.PRG");
    prg.data = QByteArray(64, '\0');
    items.append(prg);

    const QString image = dir + QStringLiteral("/text.st");
    QString error;
    QVERIFY2(floppy::writeImage(image, items, &error), qPrintable(error));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    browser->setFloppyImages({image, QString()});

    // Opening extracts to the session and lands in a text tab.
    auto *editor = qobject_cast<CodeEditor *>(
        window.openFloppyEntry(0, QStringLiteral("DOC.TXT")));
    QVERIFY2(editor, "a text entry must open in an editor tab");
    QCOMPARE(editor->toPlainText(), QStringLiteral("hello"));
    const int editorsAfterOpen = window.findChildren<CodeEditor *>().size();

    // Reopening raises the same tab instead of extracting a second copy.
    QCOMPARE(window.openFloppyEntry(0, QStringLiteral("DOC.TXT")), editor);
    QCOMPARE(window.findChildren<CodeEditor *>().size(), editorsAfterOpen);

    // A suffix-less text file is sniffed open; a binary is refused. The
    // refusal note is modal, so the test dismisses it from a single-shot
    // timer that fires inside the dialog's own event loop.
    QVERIFY(window.openFloppyEntry(0, QStringLiteral("README")));
    QTimer::singleShot(0, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    QVERIFY(!window.openFloppyEntry(0, QStringLiteral("PROG.PRG")));

    // Editing and saving writes back into the image, replacing its entry.
    editor->setPlainText(QStringLiteral("changed\n"));
    // Raise the DOC.TXT tab (the README tab opened above is current) and save.
    QCOMPARE(window.openFloppyEntry(0, QStringLiteral("DOC.TXT")), editor);
    auto *saveAction = window.findChild<QAction *>(QStringLiteral("saveAction"));
    QVERIFY(saveAction);
    saveAction->trigger();
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    QByteArray saved;
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("DOC.TXT"), &saved, &error),
             qPrintable(error));
    QCOMPARE(saved, QByteArray("changed\n"));
    QVERIFY(!floppy::readFileRaw(raw, QStringLiteral("DOC1.TXT"), &saved, &error));

    // An entry inside a folder round-trips through its subdirectory.
    auto *note = qobject_cast<CodeEditor *>(
        window.openFloppyEntry(0, QStringLiteral("DOCS/NOTE.TXT")));
    QVERIFY(note);
    note->setPlainText(QStringLiteral("edited"));
    saveAction->trigger();
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("DOCS/NOTE.TXT"), &saved, &error),
             qPrintable(error));
    QCOMPARE(saved, QByteArray("edited"));
}

// The editor's modified state drives the window title and the save prompt, and
// until now isModifiedSinceLoad() was never called from anywhere.
void TstGui::editorTracksUnsavedChanges()
{
    MainWindow window;
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);

    editor->setPlainText(QStringLiteral("\tnop\n"));
    editor->document()->setModified(false);
    QVERIFY(!editor->isModifiedSinceLoad());

    editor->insertPlainText(QStringLiteral("\trts\n"));
    QVERIFY2(editor->isModifiedSinceLoad(),
             "editing must mark the document modified, or the save prompt never fires");

    editor->document()->setModified(false);
    QVERIFY(!editor->isModifiedSinceLoad());

    // An id in the title with the modified marker is how Qt shows unsaved state.
    QVERIFY2(window.windowTitle().contains(QLatin1String("[*]")),
             qPrintable("title must carry the modified placeholder: " + window.windowTitle()));
}

// `--diagnose` is what release packaging uses to prove an archive resolves the
// assembler it carries, so the option has to be dependable. Run the built binary
// and check that it reports something for each section.
void TstGui::diagnoseReportsToolsAndRoms()
{
    // The test executable is not pist, so locate the application binary beside
    // it. On macOS the application is a bundle, so its executable sits inside
    // Contents/MacOS rather than directly alongside the tests.
    const QString dir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#ifdef Q_OS_WIN
    candidates << dir + QStringLiteral("/pist.exe");
#elif defined(Q_OS_MACOS)
    candidates << dir + QStringLiteral("/pist.app/Contents/MacOS/pist")
               << dir + QStringLiteral("/pist");
#else
    candidates << dir + QStringLiteral("/pist");
#endif

    QString app;
    for (const QString &candidate : candidates) {
        if (QFileInfo(candidate).isExecutable()) {
            app = candidate;
            break;
        }
    }
    if (app.isEmpty())
        QSKIP("pist binary not found next to the test executable");

    QProcess process;
    process.start(app, {QStringLiteral("--diagnose")});
    QVERIFY(process.waitForStarted(5000));
    QVERIFY(process.waitForFinished(15000));

    const QString output = QString::fromUtf8(process.readAllStandardOutput())
                         + QString::fromUtf8(process.readAllStandardError());

    QVERIFY2(process.exitCode() == 0, qPrintable("diagnose failed: " + output));
    QVERIFY2(output.contains(QLatin1String("Assembler")), qPrintable(output));
    QVERIFY2(output.contains(QLatin1String("Emulator")), qPrintable(output));
    QVERIFY2(output.contains(QLatin1String("TOS ROMs found")), qPrintable(output));
    QVERIFY2(output.contains(QLatin1String("Search paths")), qPrintable(output));

    // It must name a path or say NOT FOUND — never leave the section blank, which
    // would make a packaging check pass vacuously.
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1String("Assembler (")))
            QVERIFY2(line.contains(QLatin1String("NOT FOUND"))
                         || line.contains(QLatin1Char('/')) || line.contains(QLatin1Char('\\')),
                     qPrintable("no assembler path or NOT FOUND: " + line));
    }
}

void TstGui::setupDialogShowsMissingPieces()
{
    // Strip PATH and PIST_TOS_DIR so the missing-state is identical on every
    // machine, and redirect QStandardPaths so a previously fetched tool in the
    // real per-user directory cannot leak in. All three are restored so later
    // tests in this process are unaffected.
    const QByteArray savedPath = qgetenv("PATH");
    const QByteArray savedTosDir = qgetenv("PIST_TOS_DIR");
    qputenv("PATH", "");
    qputenv("PIST_TOS_DIR", "");
    QStandardPaths::setTestModeEnabled(true);

    // Other suites' fixtures may have left an "installed" fake tool or ROM in
    // the shared test-mode data root; clear both so missing really is missing.
    // Below setTestModeEnabled deliberately: only with it on do these resolve
    // to the test-mode locations — above it they would name the real per-user
    // directories and delete a user's genuinely fetched tools.
    QDir(toolchain::suggestedInstallDir()).removeRecursively();
    QDir(paths::suggestedRomDir()).removeRecursively();

    // The dismissal flag follows QSettings into the shared test-mode root, so
    // a previous run's flag would make this order-dependent; clear exactly the
    // key the production code reads.
    QSettings().remove(SetupDialog::dismissalKey());

    {
        SetupDialog dialog;
        QVERIFY(SetupDialog::anythingMissing());

        auto *vasmStatus = dialog.findChild<QLabel *>(QStringLiteral("vasmStatus"));
        auto *vasmButton = dialog.findChild<QPushButton *>(QStringLiteral("vasmFetchButton"));
        auto *emuStatus = dialog.findChild<QLabel *>(QStringLiteral("emulatorStatus"));
        auto *romStatus = dialog.findChild<QLabel *>(QStringLiteral("romStatus"));
        auto *romButton = dialog.findChild<QPushButton *>(QStringLiteral("romFetchButton"));
        QVERIFY(vasmStatus && vasmButton && emuStatus && romStatus && romButton);
        QVERIFY(emuStatus->text().startsWith(QLatin1String("Not found")));

        // A system Hatari package ships ROMs under /usr/share/hatari, which no
        // environment strip can hide (Qt's test mode only redirects user
        // locations), so both row states are pinned conditionally.
        if (findTosRoms().isEmpty()) {
            QVERIFY(romStatus->text().startsWith(QLatin1String("None found")));
            QVERIFY(romButton->isVisibleTo(&dialog));
            QVERIFY(romButton->isEnabled());
        } else {
            QVERIFY(romStatus->text().startsWith(QLatin1String("Found")));
            QVERIFY(!romButton->isVisibleTo(&dialog));
        }
        QVERIFY(vasmStatus->text().startsWith(QLatin1String("Not found")));

        // The vasm fetch button stays disabled here because the stripped PATH
        // also hides make/cc, and offering a build that cannot run would be
        // the bug.
        QVERIFY(vasmButton->isVisibleTo(&dialog));
        QVERIFY(!vasmButton->isEnabled());

        // Unprompted startup shows it while anything is missing, exactly once:
        // any close records the dismissal and it never opens unprompted again.
        QVERIFY(SetupDialog::shouldPromptAtStartup());
        dialog.done(0);
        QVERIFY(!SetupDialog::shouldPromptAtStartup());
    }

    QStandardPaths::setTestModeEnabled(false);
    qputenv("PATH", savedPath);
    qputenv("PIST_TOS_DIR", savedTosDir);
}


void TstGui::setupInstallTakesEffectWithoutRestart()
{
    // Same stripped, test-mode environment as setupDialogShowsMissingPieces.
    const QByteArray savedPath = qgetenv("PATH");
    const QByteArray savedTosDir = qgetenv("PIST_TOS_DIR");
    qputenv("PATH", "");
    qputenv("PIST_TOS_DIR", "");
    QStandardPaths::setTestModeEnabled(true);
    QDir(toolchain::suggestedInstallDir()).removeRecursively();
    QDir(paths::suggestedRomDir()).removeRecursively();

    MainWindow window;
    QCOMPARE(window.assemblerPath(), QStringLiteral("vasmm68k_mot"));

    // What the setup dialog's vasm fetch leaves behind, with the build faked:
    // an executable in the per-user tools directory discovery searches.
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("vasmm68k_mot.exe");
#else
    const QString name = QStringLiteral("vasmm68k_mot");
#endif
    const QString fixture = m_work->path() + QLatin1Char('/') + name;
    {
        QFile f(fixture);

        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\necho fixture\n");
    }
    QString error;
    const QString installed = toolchain::installToolBinary(fixture, &error);
    QVERIFY2(!installed.isEmpty(), qPrintable(error));

    window.refreshToolchain();
    QCOMPARE(QDir::cleanPath(window.assemblerPath()), QDir::cleanPath(installed));

    QStandardPaths::setTestModeEnabled(false);
    qputenv("PATH", savedPath);
    qputenv("PIST_TOS_DIR", savedTosDir);
}

void TstGui::appearancePreferencesApply()
{
    QSettings settings;
    settings.remove(QStringLiteral("appearance/theme"));
    settings.remove(QStringLiteral("appearance/fontSize"));
    settings.remove(QStringLiteral("appearance/fontFamily"));

    MainWindow window;
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);

    // Font size: an explicit value wins over the platform default.
    const int defaultSize = editor->font().pointSize();
    settings.setValue(QStringLiteral("appearance/fontSize"), defaultSize + 6);
    editor->applyFontPreferences();
    QCOMPARE(editor->font().pointSize(), defaultSize + 6);

    // Font family: a real installed family is applied to the editor.
    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    QVERIFY(!family.isEmpty());
    QVERIFY(pist::appearance::editorFontChoices().contains(QStringLiteral("Monospace")));
    settings.setValue(QStringLiteral("appearance/fontFamily"), family);
    editor->applyFontPreferences();
    QCOMPARE(editor->font().family(), family);

    // Unset theme defaults to dark (the designed look).
    QVERIFY(pist::appearance::theme() == QLatin1String("dark"));
    pist::appearance::applyTheme();
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 128);
    QVERIFY(pist::appearance::darkModeActive());

    // Theme: dark must measurably darken the application palette and switch
    // the syntax colours to the dark set; light must undo both.
    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("dark"));
    pist::appearance::applyTheme();
    editor->applyFontPreferences();
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 128);
    QVERIFY(pist::appearance::darkModeActive());

    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
    pist::appearance::applyTheme();
    editor->applyFontPreferences();
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() >= 128);
    QVERIFY(!pist::appearance::darkModeActive());

    // Restore the platform default for the rest of the suite.
    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("system"));
    settings.remove(QStringLiteral("appearance/fontSize"));
    settings.remove(QStringLiteral("appearance/fontFamily"));
    pist::appearance::applyTheme();
}

void TstGui::documentTabsManageOpenFiles()
{
    const QString dir = m_work->path() + QStringLiteral("/tabs");
    QVERIFY(QDir().mkpath(dir));
    const QString a = dir + QStringLiteral("/a.s");
    const QString b = dir + QStringLiteral("/b.s");
    for (const QString &path : {a, b}) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\tnop\n");
        f.close();
    }

    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY2(tabs, "documents must live in a tab widget");

    // The pristine start tab is reused by the first open rather than stranded.
    QCOMPARE(tabs->count(), 1);
    window.openPath(a);
    QCOMPARE(tabs->count(), 1);
    auto *editorA = window.findChild<CodeEditor *>();
    QVERIFY(editorA);
    QCOMPARE(editorA->filePath(), a);

    window.openPath(b);
    QCOMPARE(tabs->count(), 2);
    auto *editorB = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(editorB);
    QCOMPARE(editorB->filePath(), b);

    // Reopening a file raises its tab instead of opening a duplicate.
    window.openPath(a);
    QCOMPARE(tabs->count(), 2);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(editorA));

    // The label carries the modified marker and drops it when the document is
    // no longer modified.
    editorA->insertPlainText(QStringLiteral("\trts\n"));
    QCOMPARE(tabs->tabText(tabs->indexOf(editorA)), QStringLiteral("a.s *"));
    editorA->document()->setModified(false);
    QVERIFY(!tabs->tabText(tabs->indexOf(editorA)).endsWith(QLatin1String(" *")));

    // Closing tabs must never empty the central widget: the last close leaves
    // a pristine editor behind.
    bool ok = false;
    int index = tabs->indexOf(editorA);
    ok = QMetaObject::invokeMethod(&window, "onTabCloseRequested", Q_ARG(int, index));
    QVERIFY(ok);
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->currentWidget(), static_cast<QWidget *>(editorB));

    index = tabs->indexOf(editorB);
    ok = QMetaObject::invokeMethod(&window, "onTabCloseRequested", Q_ARG(int, index));
    QVERIFY(ok);
    QCOMPARE(tabs->count(), 1);
    auto *pristine = qobject_cast<CodeEditor *>(tabs->widget(0));
    QVERIFY(pristine);
    QVERIFY(pristine->filePath().isEmpty());
}

void TstGui::imageTabsOpenBesideAssembly()
{
    const QString dir = m_work->path() + QStringLiteral("/img");
    QVERIFY(QDir().mkpath(dir));
    const QString source = dir + QStringLiteral("/prog.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\tnop\n\trts\n");
    src.close();

    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    const QString pim = dir + QStringLiteral("/sprite.pim");
    QString error;
    QVERIFY2(doc.save(pim, &error), qPrintable(error));

    MainWindow window;
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);

    window.openPath(source);
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<CodeEditor *>(tabs->currentWidget()));

    window.openPath(pim);
    QCOMPARE(tabs->count(), 2);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY2(image, "opening a .pim must create an ImageEditor tab");
    QCOMPARE(image->filePath(), pim);
    QCOMPARE(tabs->tabText(tabs->indexOf(image)), QStringLiteral("sprite.pim"));

    bool sawBrush = false;
    for (auto *button : image->findChildren<QToolButton *>()) {
        if (!button->property("tool").isValid())
            continue;
        QVERIFY2(!button->icon().isNull(), "drawing tools are icons, not text labels");
        if (button->property("tool").toInt() == int(DrawTool::Brush))
            sawBrush = true;
    }
    QVERIFY(sawBrush);

    auto *onion = image->findChild<QComboBox *>(QStringLiteral("imageOnion"));
    QVERIFY2(onion, "onion-skin selector is part of the sprite editor");
    QCOMPARE(onion->count(), 3);
    auto *layers = image->findChild<QListWidget *>(QStringLiteral("imageLayers"));
    QVERIFY2(layers, "layer stack is part of the sprite editor");
    QCOMPARE(layers->count(), 1);
    QVERIFY(image->findChild<QToolButton *>(QStringLiteral("imagePlay")));
    QVERIFY2(image->findChild<QAction *>(QStringLiteral("imageShiftLeft")),
             "transform group must include shift left");
    QVERIFY(image->findChild<QAction *>(QStringLiteral("imageShiftRight")));
    QVERIFY(image->findChild<QAction *>(QStringLiteral("imageShiftUp")));
    QVERIFY(image->findChild<QAction *>(QStringLiteral("imageShiftDown")));

    QList<QToolButton *> swatches;
    QToolButton *checkedSwatch = nullptr;
    for (auto *button : image->findChildren<QToolButton *>()) {
        if (!button->property("cube").isValid())
            continue;
        swatches.append(button);
        if (button->isChecked())
            checkedSwatch = button;
    }
    QVERIFY2(checkedSwatch, "the active palette colour is marked on its swatch");
    QVERIFY(swatches.size() >= 3);
    QToolButton *otherSwatch = nullptr;
    for (auto *button : swatches) {
        if (button != checkedSwatch && button->property("cube").toInt() >= 0) {
            otherSwatch = button;
            break;
        }
    }
    QVERIFY(otherSwatch);
    QTest::mouseClick(otherSwatch, Qt::LeftButton);
    QVERIFY(otherSwatch->isChecked());
    QVERIFY(!checkedSwatch->isChecked());
    auto *canvas = image->findChild<ImageCanvas *>();
    QVERIFY(canvas);
    QVERIFY2(canvas->cursor().shape() != Qt::ArrowCursor,
             "the canvas uses a paint-tip cursor, not the window arrow");
    const int beforeZoom = canvas->cellSize();
    QVERIFY(QMetaObject::invokeMethod(image, "zoomIn"));
    QVERIFY2(canvas->cellSize() > beforeZoom, "Zoom in must enlarge the pixel grid");

    QVERIFY(QMetaObject::invokeMethod(image, "addFrame"));
    QCOMPARE(image->document().frameCount(), 2);
    QVERIFY(image->isModifiedSinceLoad());
    QCOMPARE(tabs->tabText(tabs->indexOf(image)), QStringLiteral("sprite.pim *"));
    QVERIFY(image->saveFile(pim));
    QVERIFY(!image->isModifiedSinceLoad());
    QVERIFY(!tabs->tabText(tabs->indexOf(image)).endsWith(QLatin1String(" *")));

    // Focusing the image must not strand Build: the assembly source is still
    // the project source.
    QMetaObject::invokeMethod(&window, "build");
    QVERIFY(window.debugConsoleText().contains(QLatin1String("--- build ---")));

    window.openPath(pim);
    QCOMPARE(tabs->count(), 2);

    bool ok = QMetaObject::invokeMethod(&window, "onTabCloseRequested",
                                        Q_ARG(int, tabs->indexOf(image)));
    QVERIFY(ok);
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<CodeEditor *>(tabs->currentWidget()));

    ok = QMetaObject::invokeMethod(&window, "onTabCloseRequested", Q_ARG(int, 0));
    QVERIFY(ok);
    QCOMPARE(tabs->count(), 1);
    auto *pristine = qobject_cast<CodeEditor *>(tabs->widget(0));
    QVERIFY(pristine);
    QVERIFY(pristine->filePath().isEmpty());

    // Opening a .pim as the only document still leaves a pristine assembly
    // editor when that last tab is closed.
    window.openPath(pim);
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<ImageEditor *>(tabs->currentWidget()));
    ok = QMetaObject::invokeMethod(&window, "onTabCloseRequested", Q_ARG(int, 0));
    QVERIFY(ok);
    QCOMPARE(tabs->count(), 1);
    QVERIFY(qobject_cast<CodeEditor *>(tabs->widget(0)));
}

void TstGui::newImageDialogResolvesFileName()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    NewImageDialog dialog(nullptr, tmp.path());
    auto *name = dialog.findChild<QLineEdit *>(QStringLiteral("newImageFileName"));
    QVERIFY(name);
    QVERIFY(dialog.filePath().isEmpty());
    name->setText(QStringLiteral("hero"));
    const QString path = dialog.filePath();
    QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("hero.pim"));
    QCOMPARE(QFileInfo(path).absolutePath(), QFileInfo(tmp.path()).absoluteFilePath());
}

void TstGui::bitplaneExportDialogMapsChoices()
{
    // Two phases, the second an animation, and the animation selected: the
    // export follows the editor's phase, not a fixed frame.
    const QVector<int> active = defaultActiveIndices(PaletteKind::Ste);
    BitplaneExportDialog dialog({{QStringLiteral("Atlas"), 16, 16, 1},
                                 {QStringLiteral("Walk"), 16, 16, 3}},
                                1, PaletteKind::Ste, active, 0);
    const auto box = [&dialog](const char *name) {
        auto *check = dialog.findChild<QCheckBox *>(QString::fromLatin1(name));
        Q_ASSERT(check);
        return check;
    };
    auto *map = dialog.findChild<QPlainTextEdit *>(QStringLiteral("bitplaneMap"));
    auto *source = dialog.findChild<QLabel *>(QStringLiteral("bitplaneSource"));
    auto *phases = dialog.findChild<QComboBox *>(QStringLiteral("bitplanePhase"));
    auto *combo = dialog.findChild<QComboBox *>(QStringLiteral("bitplanePreShifts"));
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    auto *transparent = dialog.findChild<QComboBox *>(QStringLiteral("bitplaneTransparent"));
    QVERIFY(map && source && phases && combo && transparent && buttons);

    // The transparent colour is chosen from the document's palette, and starts
    // on the background register: "None" plus one entry per active colour.
    QCOMPARE(transparent->count(), active.size() + 1);
    QCOMPARE(transparent->currentData().toInt(), 0);
    QCOMPARE(dialog.options().transparent, 0);
    transparent->setCurrentIndex(transparent->findData(3));
    QCOMPARE(dialog.options().transparent, 3);
    transparent->setCurrentIndex(0); // None
    QCOMPARE(dialog.options().transparent, -1);
    transparent->setCurrentIndex(transparent->findData(0));

    QCOMPARE(phases->count(), 2);
    QVERIFY(phases->currentText().startsWith(QStringLiteral("Walk")));
    QCOMPARE(dialog.phase(), 1);
    QVERIFY(source->text().contains(QStringLiteral("3 frame")));

    // Defaults: the palette, the sprite and its mask — not the (much larger)
    // pre-shifted copies.
    BitplaneDataOptions options = dialog.options();
    QVERIFY(options.palette);
    QVERIFY(options.sprite);
    QVERIFY(options.masked);
    QVERIFY(!options.shifted);
    QVERIFY(!options.shiftedMasked);
    QCOMPARE(options.preShifts, 4);

    // The map is the file's block table with the labels the source will use:
    // pre-shift rows stay out until one is asked for, and every frame past the
    // first is a stride away rather than listed again.
    QVERIFY(map->toPlainText().contains(QStringLiteral("sprite_masked_f0")));
    QVERIFY(!map->toPlainText().contains(QStringLiteral("sprite_shift0")));
    QVERIFY(map->toPlainText().contains(QStringLiteral("each further frame is")));
    QVERIFY(!combo->isEnabled());

    // Eight pre-shifts means eight copies at 2 px steps, and the run arrives in
    // the map as one row naming them all.
    box("bitplaneShifted")->setChecked(true);
    box("bitplaneShiftedMasked")->setChecked(true);
    combo->setCurrentIndex(combo->count() - 1);
    options = dialog.options();
    QVERIFY(options.shifted && options.shiftedMasked);
    QCOMPARE(options.preShifts, 8);
    QVERIFY(map->toPlainText().contains(QStringLiteral("sprite_shift0..7_f0")));
    QVERIFY(map->toPlainText().contains(QStringLiteral("sprite_masked_shift0..7_f0")));
    QVERIFY(combo->isEnabled());

    // A single-frame phase has nothing to animate, so the map says nothing
    // about frames.
    phases->setCurrentIndex(0);
    QCOMPARE(dialog.phase(), 0);
    QVERIFY(!map->toPlainText().contains(QStringLiteral("each further frame is")));

    // Nothing selected is not a file: Ok goes away rather than writing a
    // zero-byte blob.
    for (const char *name : {"bitplanePalette", "bitplaneSprite", "bitplaneMasked",
                             "bitplaneShifted", "bitplaneShiftedMasked"})
        box(name)->setChecked(false);
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(buttons->button(QDialogButtonBox::Cancel)->isEnabled());

    // A scroller needs a sprite to scroll: with every sprite block off the
    // option greys out and cannot be requested.
    auto *demo = dialog.findChild<QCheckBox *>(QStringLiteral("bitplaneScrollDemo"));
    QVERIFY(demo);
    box("bitplanePalette")->setChecked(true);
    QVERIFY(!demo->isEnabled());
    QVERIFY(!dialog.writesScrollDemo());

    box("bitplaneSprite")->setChecked(true);
    QVERIFY(demo->isEnabled());
    demo->setChecked(true);
    QVERIFY(dialog.writesScrollDemo());
    // ...and taking the sprite away again withdraws it.
    box("bitplaneSprite")->setChecked(false);
    QVERIFY(!demo->isChecked());
    QVERIFY(!dialog.writesScrollDemo());
}

void TstGui::floppyImageOpensAndSavesBack()
{
    const QString dir = m_work->path() + QStringLiteral("/floppyimg");
    QVERIFY(QDir().mkpath(dir));

    // A tiny Degas image to live on the disk: one red pixel at (0, 0).
    ImageDocument piece = ImageDocument::create(32, 32, PaletteKind::Ste);
    const int red = piece.active().at(1);
    const int blue = piece.active().at(2);
    piece.setPixel(0, red);
    QString error;
    const QByteArray pi1 = exportPi1(piece, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));

    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("PIECE.PI1");
    file.data = pi1;
    items.append(file);
    const QString image = dir + QStringLiteral("/art.st");
    QVERIFY2(floppy::writeImage(image, items, &error), qPrintable(error));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    browser->setFloppyImages({image, QString()});

    // Opening registers the file as a sprite sheet; the document itself is
    // a new, untitled .pim — the imported file is a sheet target, not the
    // document.
    auto *tab = qobject_cast<ImageEditor *>(window.openFloppyEntry(0, QStringLiteral("PIECE.PI1")));
    QVERIFY2(tab, "a .pi1 entry must open in an image tab");
    QVERIFY2(tab->filePath().isEmpty(),
             "an imported sheet must not adopt the file as its document path");
    QCOMPARE(tab->document().sheets().size(), 1);

    // Slice the sprite out of the sheet into a phase and repaint it.
    QCOMPARE(tab->addPhaseFromSheet(0, QStringLiteral("sprite"), 0, 0, 32, 32, 1), 1);
    QCOMPARE(tab->document().currentPhase(), 1);
    QCOMPARE(tab->document().pixels().at(0), red);
    tab->document().setPixel(0, blue);

    // Export Sprite Sheet recomposes the sheet and writes it back into the
    // image entry — that is how edits reach the disk.
    const QString sheetPath = tab->document().sheets().at(0).path;
    QVERIFY2(window.exportSpriteSheetTo(sheetPath), "sheet export must succeed");

    QByteArray raw;
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    QByteArray saved;
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("PIECE.PI1"), &saved, &error),
             qPrintable(error));
    QCOMPARE(saved.size(), 32034);

    ImportedSheet sheet;
    QVERIFY2(importPi1(saved, PaletteKind::Ste, &sheet, &error), qPrintable(error));
    QCOMPARE(sheet.pixels.at(0), blue);
    QStringList onDisk;
    for (const floppy::Entry &e : floppy::listImage(image, &error))
        onDisk.append(e.path);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(onDisk.size(), 1);
}

void TstGui::phasePlacementAndSheetMode()
{
    const QString dir = m_work->path() + QStringLiteral("/phasesheets");
    QVERIFY(QDir().mkpath(dir));

    ImageDocument doc = ImageDocument::create(32, 32, PaletteKind::Ste);
    QCOMPARE(doc.addSheet(QStringLiteral("chars.pi1"), 320, 200), 0);
    QCOMPARE(doc.addPhase(QStringLiteral("dragon"), 64, 64), 1);
    QVERIFY(doc.setPhasePlacement(1, 0, 20, 50));
    const QString pim = dir + QStringLiteral("sheet.pim");
    QString error;
    QVERIFY2(doc.save(pim, &error), qPrintable(error));

    MainWindow window;
    window.openPath(pim);
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    auto *list = image->findChild<QListWidget *>(QStringLiteral("imagePhases"));
    QVERIFY(list);
    QCOMPARE(list->count(), 2);

    // Select the dragon phase: the editing context follows it.
    list->setCurrentRow(1);
    QCOMPARE(image->document().currentPhase(), 1);
    QCOMPARE(image->document().width(), 64);

    // Numeric placement edits land in the document.
    auto *x = image->findChild<QSpinBox *>(QStringLiteral("imagePhaseX"));
    auto *y = image->findChild<QSpinBox *>(QStringLiteral("imagePhaseY"));
    auto *sheetBox = image->findChild<QComboBox *>(QStringLiteral("imagePhaseSheet"));
    QVERIFY(x && y && sheetBox);
    QCOMPARE(x->value(), 20);
    x->setValue(22);
    QCOMPARE(image->document().phases().at(1).x, 22);
    sheetBox->setCurrentIndex(0);   // "(unplaced)"
    QCOMPARE(image->document().phases().at(1).sheet, -1);
    sheetBox->setCurrentIndex(1);   // back onto the sheet
    QCOMPARE(image->document().phases().at(1).sheet, 0);

    // Spritesheet mode: the composed sheet view, then drag the strip.
    auto *mode = image->findChild<QAction *>(QStringLiteral("imageSheetMode"));
    QVERIFY(mode);
    mode->setChecked(true);
    auto *sheetCanvas = image->findChild<SheetCanvas *>();
    QVERIFY2(sheetCanvas, "sheet mode must expose the composed sheet view");
    // Phase 0 (32×32) is unplaced, so the staging gutter shifts the sheet:
    // gutter = 4 + 32 + 16, plus a 12px top margin.
    const int scale = sheetCanvas->scale();
    const int originX = (4 + 32 + 16 + 22) * scale;
    const int originY = (12 + 50) * scale;
    QTest::mousePress(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                      QPoint(originX + 1, originY + 1));
    QTest::mouseMove(sheetCanvas, QPoint(originX + 10 * scale + 1, originY + 5 * scale + 1));
    QTest::mouseRelease(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(originX + 10 * scale + 1, originY + 5 * scale + 1));
    QCOMPARE(image->document().phases().at(1).x, 32);
    QCOMPARE(image->document().phases().at(1).y, 55);

    // The placement survives the round-trip through the file.
    QVERIFY2(image->saveFile(pim), qPrintable(image->lastError()));
    ImageDocument reloaded;
    QVERIFY2(reloaded.load(pim, &error), qPrintable(error));
    QCOMPARE(reloaded.phases().at(1).x, 32);
    QCOMPARE(reloaded.phases().at(1).y, 55);
}

void TstGui::newSheetAndStagingDrag()
{
    // The demo.pim situation: two phases, neither placed, no sheets.
    ImageDocument doc = ImageDocument::create(32, 32, PaletteKind::Ste);
    QCOMPARE(doc.addPhase(QStringLiteral("Bm"), 16, 16), 1);
    const QString pim = m_work->path() + QStringLiteral("/unplaced.pim");
    QString error;
    QVERIFY2(doc.save(pim, &error), qPrintable(error));

    MainWindow window;
    window.openPath(pim);
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    QVERIFY(image->document().sheets().isEmpty());

    auto *mode = image->findChild<QAction *>(QStringLiteral("imageSheetMode"));
    QVERIFY(mode);
    mode->setChecked(true);
    auto *sheetCanvas = image->findChild<SheetCanvas *>();
    QVERIFY(sheetCanvas);

    // No sheet exists yet: New sheet creates the 320×200 target.
    auto *newSheet = image->findChild<QAction *>(QStringLiteral("imageNewSheet"));
    QVERIFY(newSheet);
    QVERIFY(newSheet->isEnabled());
    newSheet->trigger();
    QCOMPARE(image->document().sheets().size(), 1);
    QCOMPARE(image->document().sheets().at(0).width, 320);

    // The staged "Bm" strip drags from the gutter straight onto the sheet.
    QVERIFY(image->document().setCurrentPhase(1));
    const int scale = sheetCanvas->scale();
    // Both phases are unplaced: Bm is staging ordinal 1, at [4, 12 + 16 + 20].
    QTest::mousePress(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                      QPoint(6 * scale, 50 * scale));
    // Drop with the strip origin at sheet cell (10, 10).
    QTest::mouseMove(sheetCanvas, QPoint(64 * scale, 24 * scale));
    QTest::mouseRelease(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(64 * scale, 24 * scale));
    QCOMPARE(image->document().phases().at(1).sheet, 0);
    QCOMPARE(image->document().phases().at(1).x, 10);
    QCOMPARE(image->document().phases().at(1).y, 10);
}

void TstGui::stagedPhaseSlicesOnPlacement()
{
    const QString dir = m_work->path() + QStringLiteral("/staged");
    QVERIFY(QDir().mkpath(dir));

    // Six marked 32x32 cells on a sheet file.
    ImageDocument strip = ImageDocument::create(192, 32, PaletteKind::Ste);
    for (int k = 0; k < 6; ++k)
        strip.setPixel(k * 32, strip.active().at(k + 1));
    const QString pi1Path = dir + QStringLiteral("/six.pi1");
    QString error;
    const QByteArray pi1 = exportPi1(strip, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));
    {
        QFile out(pi1Path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(pi1), qint64(pi1.size()));
    }

    MainWindow window;
    window.openPath(pi1Path);
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    QCOMPARE(image->document().sheets().size(), 1);

    auto *mode = image->findChild<QAction *>(QStringLiteral("imageSheetMode"));
    QVERIFY(mode);
    mode->setChecked(true);
    auto *sheetCanvas = image->findChild<SheetCanvas *>();
    QVERIFY(sheetCanvas);
    const int scale = sheetCanvas->scale();

    // Drag the staged default phase (32x32, one empty frame) to sheet
    // cell [0, 0]: gutter origin (52, 12), strip at [4, 12].
    QTest::mousePress(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                      QPoint(6 * scale, 14 * scale));
    // Keep the pointer 2px right/down of the strip origin so the origin
    // lands at the sheet's [0, 0].
    QTest::mouseMove(sheetCanvas, QPoint(54 * scale, 14 * scale));
    QTest::mouseRelease(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(54 * scale, 14 * scale));
    QCOMPARE(image->document().phases().at(0).sheet, 0);
    QCOMPARE(image->document().phases().at(0).x, 0);
    QCOMPARE(image->document().phases().at(0).y, 0);

    // The placement slices the art under the strip.
    QCOMPARE(image->document().frame(0).at(0), strip.active().at(1));

    // Adding a frame extends the strip AND slices the next cell.
    auto *addFrame = image->findChild<QToolButton *>(QStringLiteral("imageAddFrame"));
    QVERIFY(addFrame);
    addFrame->click();
    QCOMPARE(image->document().frameCount(), 2);
    QCOMPARE(image->document().frame(1).at(0), strip.active().at(2));

    // Dragging the strip re-slices the untouched frames under the new
    // position (the strip is a window onto the sheet). The placed strip
    // sits at sheet origin (52, 12) in widget coordinates.
    QTest::mousePress(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                      QPoint(54 * scale, 14 * scale));
    QTest::mouseMove(sheetCanvas, QPoint(86 * scale, 14 * scale));
    QTest::mouseRelease(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(86 * scale, 14 * scale));
    QCOMPARE(image->document().phases().at(0).x, 32);
    QCOMPARE(image->document().frame(0).at(0), strip.active().at(2));
    QCOMPARE(image->document().frame(1).at(0), strip.active().at(3));

    // A frame that has been drawn on keeps its pixels across the move.
    QVERIFY(image->document().setCurrentFrame(1));
    image->document().setPixel(0, image->document().active().at(15));
    QTest::mousePress(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                      QPoint(86 * scale + 2, 14 * scale));
    QTest::mouseMove(sheetCanvas, QPoint(118 * scale + 2, 14 * scale));
    QTest::mouseRelease(sheetCanvas, Qt::LeftButton, Qt::NoModifier,
                        QPoint(118 * scale + 2, 14 * scale));
    QCOMPARE(image->document().phases().at(0).x, 64);
    QCOMPARE(image->document().frame(0).at(0), strip.active().at(3));
    QCOMPARE(image->document().frame(1).at(0), image->document().active().at(15));
}

void TstGui::phaseCellSizeAndStripGrowth()
{
    ImageDocument doc = ImageDocument::create(32, 32, PaletteKind::Ste);
    const QString pim = m_work->path() + QStringLiteral("/cells.pim");
    QString error;
    QVERIFY2(doc.save(pim, &error), qPrintable(error));

    MainWindow window;
    window.openPath(pim);
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);

    // The panel exposes the current phase's cell size, and edits resize it.
    auto *cellW = image->findChild<QSpinBox *>(QStringLiteral("imagePhaseCellW"));
    auto *cellH = image->findChild<QSpinBox *>(QStringLiteral("imagePhaseCellH"));
    QVERIFY(cellW && cellH);
    QCOMPARE(cellW->value(), 32);
    cellW->setValue(16);
    cellH->setValue(24);
    QCOMPARE(image->document().width(), 16);
    QCOMPARE(image->document().height(), 24);

    // Adding frames through the frames panel grows the strip: with the phase
    // placed, the composed sheet paints both cells.
    QCOMPARE(image->document().addSheet(QStringLiteral("out.pi1"), 320, 200), 0);
    QVERIFY(image->document().setPhasePlacement(0, 0, 0, 0));
    auto *addFrame = image->findChild<QToolButton *>(QStringLiteral("imageAddFrame"));
    QVERIFY(addFrame);
    addFrame->click();
    QCOMPARE(image->document().frameCount(), 2);

    QString composeError;
    ImageDocument composed = composeSheet(image->document(), 0, &composeError);
    QVERIFY2(composeError.isEmpty(), qPrintable(composeError));
    // A painted pixel in the second frame must compose at [16, ...], one cell
    // to the right of the first.
    image->document().setCurrentFrame(1);
    image->document().setPixel(0, image->document().active().at(2));
    composed = composeSheet(image->document(), 0, &composeError);
    QVERIFY2(composeError.isEmpty(), qPrintable(composeError));
    QCOMPARE(composed.pixels().at(16), image->document().active().at(2));
}

void TstGui::sliceDialogBuildsPhaseFromSheet()
{
    const QString dir = m_work->path() + QStringLiteral("/slice");
    QVERIFY(QDir().mkpath(dir));

    // A 192x32 strip: six 32x32 cells, each marked with its own colour.
    ImageDocument strip = ImageDocument::create(192, 32, PaletteKind::Ste);
    for (int k = 0; k < 6; ++k)
        strip.setPixel(k * 32, strip.active().at(k + 1));
    const QString pi1Path = dir + QStringLiteral("/strip.pi1");
    QString error;
    const QByteArray pi1 = exportPi1(strip, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));
    {
        QFile out(pi1Path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(pi1), qint64(pi1.size()));
    }

    MainWindow window;
    window.openPath(pi1Path);
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    QCOMPARE(image->document().sheets().size(), 1);

    // Add phase opens the slice dialog over the imported sheet; fill it in
    // from a single-shot timer that runs inside the dialog's event loop.
    QTimer::singleShot(0, image, [image]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        auto *count = dialog->findChild<QSpinBox *>(QStringLiteral("sliceCount"));
        auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("sliceName"));
        QVERIFY(count && name);
        name->setText(QStringLiteral("walk"));
        count->setValue(6);
        dialog->accept();
    });
    auto *addBtn = image->findChild<QToolButton *>(QStringLiteral("imageAddPhase"));
    QVERIFY(addBtn);
    addBtn->click();

    // The sliced phase carries all six marked frames.
    QVERIFY(image->document().setCurrentPhase(1));
    QCOMPARE(image->document().phases().at(1).name, QStringLiteral("walk"));
    QCOMPARE(image->document().frameCount(), 6);
    for (int k = 0; k < 6; ++k)
        QCOMPARE(image->document().frame(k).at(0), strip.active().at(k + 1));
}

void TstGui::imageSelectionCopyPaste()
{
    ImageEditor editor;
    editor.show();
    editor.newDocument(8, 8, PaletteKind::Ste);
    auto *canvas = editor.findChild<ImageCanvas *>();
    QVERIFY(canvas);
    const auto idx = [](int x, int y) { return y * 8 + x; };
    // Centre of a cell in widget coordinates, at whatever zoom the editor
    // settled on (the deferred fit-to-view may override an explicit size).
    const auto cellPoint = [&canvas](int x, int y) {
        const int c = canvas->cellSize();
        return QPoint(x * c + c / 2, y * c + c / 2);
    };

    // A solid 2x2 block at (1,1)-(2,2).
    for (int y = 1; y <= 2; ++y)
        for (int x = 1; x <= 2; ++x)
            editor.document().setPixel(idx(x, y), 1);

    QToolButton *selectButton = nullptr;
    for (auto *button : editor.findChildren<QToolButton *>()) {
        if (button->property("tool").isValid()
            && button->property("tool").toInt() == int(DrawTool::Select))
            selectButton = button;
    }
    QVERIFY2(selectButton, "the select tool joins the toolbar");
    QTest::mouseClick(selectButton, Qt::LeftButton);

    // A marquee drag around the block selects it.
    QTest::mouseMove(canvas, cellPoint(1, 1));
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(1, 1));
    QTest::mouseMove(canvas, cellPoint(2, 2));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(2, 2));
    QCOMPARE(canvas->selection(), QRect(1, 1, 2, 2));

    // Dragging inside the selection moves the block two cells down-right.
    QVERIFY(QMetaObject::invokeMethod(&editor, "copySelection"));
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(1, 1));
    QTest::mouseMove(canvas, cellPoint(3, 3));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(3, 3));
    QCOMPARE(canvas->selection(), QRect(3, 3, 2, 2));
    for (int y = 1; y <= 2; ++y)
        for (int x = 1; x <= 2; ++x)
            QCOMPARE(editor.document().pixels().at(idx(x, y)), kTransparent);
    for (int y = 3; y <= 4; ++y)
        for (int x = 3; x <= 4; ++x)
            QCOMPARE(editor.document().pixels().at(idx(x, y)), 1);

    // Copy the moved block, deselect, then paste: the patch lands at the
    // origin and becomes the selection.
    QVERIFY(QMetaObject::invokeMethod(&editor, "copySelection"));
    QAction *deselect = editor.findChild<QAction *>(QStringLiteral("imageDeselect"));
    QVERIFY(deselect);
    deselect->trigger();
    QVERIFY(canvas->selection().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&editor, "pasteClipboard"));
    QCOMPARE(canvas->selection(), QRect(0, 0, 2, 2));
    for (int y = 0; y <= 1; ++y)
        for (int x = 0; x <= 1; ++x)
            QCOMPARE(editor.document().pixels().at(idx(x, y)), 1);

    // Arrow keys nudge the selection's pixels one cell.
    QTest::keyClick(canvas, Qt::Key_Right);
    QCOMPARE(canvas->selection(), QRect(1, 0, 2, 2));
    QCOMPARE(editor.document().pixels().at(idx(0, 0)), kTransparent);
    QCOMPARE(editor.document().pixels().at(idx(0, 1)), kTransparent);
    QCOMPARE(editor.document().pixels().at(idx(2, 0)), 1);
    QCOMPARE(editor.document().pixels().at(idx(2, 1)), 1);

    // Delete clears the selection contents; undo brings the pixels back.
    QVERIFY(QMetaObject::invokeMethod(&editor, "deleteSelection"));
    QCOMPARE(editor.document().pixels().at(idx(2, 0)), kTransparent);
    editor.undo();
    QCOMPARE(editor.document().pixels().at(idx(2, 0)), 1);

    // A click outside the selection collapses it.
    QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(7, 7));
    QVERIFY(canvas->selection().isEmpty());
}

void TstGui::importAdoptsTheFilePalette()
{
    // A PI1 whose 16 registers are deliberately not the editor default:
    // importing must adopt the file's registers, in file order, so the
    // colours its pixels use are not reported as overspill.
    ImageDocument art = ImageDocument::create(8, 8, PaletteKind::Ste);
    QVector<int> custom;
    for (int i = 0; i < 15; ++i)
        custom.append(0x111 * (i + 1));
    custom.append(0x001);
    QVERIFY2(art.active() != custom, "test palette must differ from the default");

    art.setActive(custom);
    art.setPixel(0, custom.at(3));
    const QString dir = m_work->path() + QStringLiteral("/importpal");
    QVERIFY(QDir().mkpath(dir));
    const QString pi1Path = dir + QStringLiteral("/pal.pi1");
    QString error;
    const QByteArray pi1 = exportPi1(art, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));
    QFile out(pi1Path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE(out.write(pi1), qint64(pi1.size()));
    out.close();

    ImageEditor editor;
    editor.show();
    QVERIFY(editor.importFile(pi1Path, false));
    QCOMPARE(editor.document().active(), custom);

    // The whole point: a phase sliced out of the imported sheet paints with
    // the file's registers, none of which are overspill now.
    QVERIFY(editor.addPhaseFromSheet(0, QStringLiteral("cut"), 0, 0, 4, 4, 1) >= 0);
    QVERIFY(editor.document().overspill().isEmpty());
}

void TstGui::sheetPixelsSurviveReopen()
{
    const QString dir = m_work->path() + QStringLiteral("/reopen");
    QVERIFY(QDir().mkpath(dir));

    // The sheet file on disk: a 96x32 strip of three marked cells.
    ImageDocument strip = ImageDocument::create(96, 32, PaletteKind::Ste);
    for (int k = 0; k < 3; ++k)
        strip.setPixel(k * 32, strip.active().at(k + 1));
    const QString pi1Path = dir + QStringLiteral("/strip.pi1");
    QString error;
    const QByteArray pi1 = exportPi1(strip, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));
    {
        QFile out(pi1Path);
        QVERIFY(out.open(QIODevice::WriteOnly));
        QCOMPARE(out.write(pi1), qint64(pi1.size()));
    }

    // A document that references the sheet by path.
    ImageDocument doc = ImageDocument::create(32, 32, PaletteKind::Ste);
    QCOMPARE(doc.addSheet(pi1Path, 320, 200), 0);
    const QString pim = dir + QStringLiteral("withsheet.pim");
    QVERIFY2(doc.save(pim, &error), qPrintable(error));

    // A fresh process-equivalent: a new window loading the document knows
    // nothing about the import, yet slicing must work off the recorded file.
    MainWindow window;
    window.openPath(pim);
    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    QCOMPARE(image->document().sheets().at(0).path, pi1Path);

    QCOMPARE(image->addPhaseFromSheet(0, QStringLiteral("walk"), 0, 0, 32, 32, 3), 1);
    QVERIFY(image->document().setCurrentPhase(1));
    QCOMPARE(image->document().frameCount(), 3);
    for (int k = 0; k < 3; ++k)
        QCOMPARE(image->document().frame(k).at(0), strip.active().at(k + 1));

    // Selecting a phase retargets the animation preview.
    auto *list = image->findChild<QListWidget *>(QStringLiteral("imagePhases"));
    QVERIFY(list);
    auto *previewBox = image->findChild<QComboBox *>(QStringLiteral("imagePreviewPhase"));
    QVERIFY(previewBox);
    list->setCurrentRow(0);
    QCOMPARE(previewBox->currentData().toInt(), 0);
    list->setCurrentRow(1);
    QCOMPARE(previewBox->currentData().toInt(), 1);
}


void TstGui::editorFindAndReplace()
{
    CodeEditor editor;
    editor.show();
    editor.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&editor));
    editor.setPlainText(QStringLiteral("move.w d0,d1\nmovem.l d2-d7,-(sp)\nmoveq #0,d0\nMOVE d0,d1\n"));
    editor.showFindBar(false);
    QVERIFY(editor.findBarVisible());

    auto *find = editor.findChild<QLineEdit *>(QStringLiteral("editorFindText"));
    auto *status = editor.findChild<QLabel *>(QStringLiteral("editorFindStatus"));
    QVERIFY(find && status);

    // As the needle is typed the hits are counted and the first one at or after
    // the caret is selected: "move", "movem", "moveq" and the shouted "MOVE".
    find->setText(QStringLiteral("move"));
    QCOMPARE(editor.findMatchCount(), 4);
    QCOMPARE(editor.findMatchIndex(), 0);
    QCOMPARE(editor.textCursor().selectedText(), QStringLiteral("move"));
    QVERIFY(status->text().contains(QLatin1String("1 of 4")));

    // Next walks them and wraps; Previous walks back and wraps the other way.
    editor.findNext();
    QCOMPARE(editor.findMatchIndex(), 1);
    QCOMPARE(editor.textCursor().selectionStart(), 13);   // "movem", second line
    editor.findNext();
    editor.findNext();
    editor.findNext();
    QCOMPARE(editor.findMatchIndex(), 0);   // wrapped
    editor.findPrevious();
    QCOMPARE(editor.findMatchIndex(), 3);   // wrapped backwards

    // Whole words only: not the "move" inside "movem" or "moveq", but "move.w"
    // and a bare "MOVE" are words.
    auto *word = editor.findChild<QToolButton *>(QStringLiteral("editorFindWord"));
    QVERIFY(word);
    word->setChecked(true);
    QCOMPARE(editor.findMatchCount(), 2);

    // Case sensitivity, on the other hand, drops the shouted one.
    auto *caseBox = editor.findChild<QToolButton *>(QStringLiteral("editorFindCase"));
    QVERIFY(caseBox);
    caseBox->setChecked(true);
    QCOMPARE(editor.findMatchCount(), 1);
    caseBox->setChecked(false);
    word->setChecked(false);
    QCOMPARE(editor.findMatchCount(), 4);

    // A needle with no hits says so, and does not leave a stale selection.
    find->setText(QStringLiteral("nothing here"));
    QCOMPARE(editor.findMatchCount(), 0);
    QCOMPARE(editor.findMatchIndex(), -1);
    QVERIFY(status->text().contains(QLatin1String("no matches")));

    // Replace one: the replace row belongs to the replace form, and the hit under
    // the selection is swapped, then the search steps on to the next one.
    auto *replace = editor.findChild<QLineEdit *>(QStringLiteral("editorReplaceText"));
    auto *replaceOne = editor.findChild<QPushButton *>(QStringLiteral("editorReplaceOne"));
    auto *replaceAll = editor.findChild<QPushButton *>(QStringLiteral("editorReplaceAll"));
    QVERIFY(replace && replaceOne && replaceAll);
    QVERIFY(!replace->isVisible());
    editor.showFindBar(true);
    QVERIFY(replace->isVisible());
    find->setText(QStringLiteral("move"));
    replace->setText(QStringLiteral("jump"));
    replaceOne->click();
    QVERIFY(editor.toPlainText().startsWith(QStringLiteral("jump.w d0,d1")));
    QVERIFY(status->text().contains(QLatin1String("replaced")));
    QCOMPARE(editor.findMatchCount(), 3);   // the one just written is gone

    // Replace All: every remaining hit, back to front, as one undo step — and
    // only one, so the single replace above survives its undo.
    replaceAll->click();
    QVERIFY(!editor.toPlainText().contains(QStringLiteral("move")));
    QVERIFY(editor.toPlainText().contains(QStringLiteral("jumpq #0,d0")));
    QCOMPARE(editor.findMatchCount(), 0);
    editor.undo();
    QCOMPARE(editor.toPlainText(),
             QStringLiteral("jump.w d0,d1\nmovem.l d2-d7,-(sp)\nmoveq #0,d0\nMOVE d0,d1\n"));

    // Escape from the find field closes the bar, drops the highlights and puts
    // the caret back in the text.
    find->setText(QStringLiteral("d0"));
    QVERIFY(editor.findMatchCount() > 0);
    QTest::keyClick(find, Qt::Key_Escape);
    QVERIFY(!editor.findBarVisible());
    QCOMPARE(editor.findMatchCount(), 0);
    QVERIFY(editor.hasFocus());

    // Shift+Enter in the field is the other direction, and the replace row only
    // shows when it was asked for.
    editor.showFindBar(true);
    find->setText(QStringLiteral("d1"));
    QCOMPARE(editor.findMatchIndex(), 0);
    QTest::keyClick(find, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(editor.findMatchIndex(), editor.findMatchCount() - 1);
    editor.hideFindBar();
    editor.showFindBar(false);
    QVERIFY(!editor.findChild<QWidget *>(QStringLiteral("editorReplaceText"))->isVisible());
}

void TstGui::searchMenuFollowsTheEditor()
{
    MainWindow window;
    window.show();
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    auto *findAction = window.findChild<QAction *>(QStringLiteral("findAction"));
    auto *replaceAction = window.findChild<QAction *>(QStringLiteral("replaceAction"));
    auto *nextAction = window.findChild<QAction *>(QStringLiteral("findNextAction"));
    auto *previousAction = window.findChild<QAction *>(QStringLiteral("findPreviousAction"));
    QVERIFY(findAction && replaceAction && nextAction && previousAction);

    // The menu is there, and the shortcuts are the ones asked for.
    bool inSearchMenu = false;
    for (QAction *menu : window.menuBar()->actions()) {
        if (!menu->menu() || !menu->text().contains(QLatin1String("Search")))
            continue;
        inSearchMenu = menu->menu()->actions().contains(findAction)
                       && menu->menu()->actions().contains(replaceAction);
    }
    QVERIFY2(inSearchMenu, "Find and Replace belong to the Search menu");
    QCOMPARE(findAction->shortcut(), QKeySequence(QKeySequence::Find));
    QCOMPARE(replaceAction->shortcut(), QKeySequence(Qt::CTRL | Qt::Key_H));
    QCOMPARE(nextAction->shortcut(), QKeySequence(Qt::Key_F3));

    // A session starts on a text tab, so they are live from the first frame.
    QVERIFY(findAction->isEnabled());
    QVERIFY(replaceAction->isEnabled());

    auto *tabs = window.findChild<QTabWidget *>();
    QVERIFY(tabs);
    auto *editor = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(editor);
    editor->setFocus();

    // The shortcut itself, not just the action: Ctrl+F opens the bar, Ctrl+H
    // opens it with the replace row, F3 walks on.
    QTest::keyClick(editor, Qt::Key_F, Qt::ControlModifier);
    QVERIFY(editor->findBarVisible());
    editor->hideFindBar();
    QTest::keyClick(editor, Qt::Key_H, Qt::ControlModifier);
    QVERIFY(editor->findBarVisible());
    QVERIFY(editor->findChild<QWidget *>(QStringLiteral("editorReplaceText"))->isVisible());
    editor->hideFindBar();

    // An image tab has nothing to search, so the actions go away with it.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pim = dir.filePath(QStringLiteral("sprite.pim"));
    QString error;
    QVERIFY2(ImageDocument::create(8, 8, PaletteKind::Ste).save(pim, &error), qPrintable(error));
    window.openPath(pim);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    QVERIFY(!findAction->isEnabled());
    QVERIFY(!replaceAction->isEnabled());

    // ...and come back with the editor.
    tabs->setCurrentIndex(tabs->indexOf(editor));
    QVERIFY(findAction->isEnabled());
}

QTEST_MAIN(TstGui)

#include "tst_gui.moc"
