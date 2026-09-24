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
#include "editor/AsmHighlighter.h"
#include "editor/InstrRef.h"
#include "image/ImageDocument.h"
#include "image/StFormats.h"
#include "ui/ImageEditor.h"
#include "ui/ImageCanvas.h"
#include "ui/SheetCanvas.h"
#include "ui/NewImageDialog.h"
#include "ui/BitplaneExportDialog.h"
#include "ui/InstructionRefView.h"
#include "emu/EmulatorHost.h"
#include "emu/Paths.h"
#include "control/RemoteControl.h"
#include "emu/TosRom.h"
#include "ui/FileBrowser.h"
#include "ui/RegistersView.h"
#include "emu/MachineState.h"
#include "ui/MainWindow.h"
#include "build/BuildService.h"
#include "build/FloppyImage.h"
#include "ui/DisassemblyView.h"
#include "ui/HardwareView.h"
#include "ui/MemoryView.h"
#include "emu/MemoryDump.h"
#include "ui/PcHistoryView.h"
#include "ui/SetupDialog.h"
#include "ui/SettingsDialog.h"
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
#include <QListView>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QMenu>
#include <QMenuBar>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QTabBar>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTimer>
#include <QSpinBox>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QProcess>
#include <QRegularExpression>
#include <QTreeView>
#include <QAbstractItemModel>
#include <QImage>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QFontDatabase>
#include <QSettings>
#include <QtTest>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTreeWidget>

#include <cmath>
#include <functional>

using namespace pist;

namespace {

/// What the emulator integration is missing, or empty when it can run. The
/// same prerequisites tst_emulatorhost checks, so PIST_REQUIRE_EMULATOR means
/// one thing across the suite.
QString emulatorMissing()
{
    QStringList missing;
    if (QStandardPaths::findExecutable(QStringLiteral("hatari")).isEmpty())
        missing << QStringLiteral("hatari");
    const TosRom rom = selectPreferredRom(findTosRoms(), Machine::St);
    if (rom.path.isEmpty() || !rom.supportsAutostart())
        missing << QStringLiteral("an autostart-capable TOS ROM for an ST");
    return missing.join(QStringLiteral(", "));
}

/// The bytes of `path`, empty when it cannot be read. Used to compare a file
/// before and after a round-trip byte for byte.
QByteArray fileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

/// Whether a symbolic link can be made in `dir` at all. The copy/move tests
/// below are about links and would assert nothing on a platform or filesystem
/// that cannot create one (Windows without developer mode).
bool canCreateSymlinks(const QString &dir)
{
    const QString target = dir + QStringLiteral("/.symlink-probe-target");
    const QString link = dir + QStringLiteral("/.symlink-probe-link");
    if (!writeFile(target, QByteArrayLiteral("x")))
        return false;
    const bool ok = QFile::link(target, link) && QFileInfo(link).isSymLink();
    QFile::remove(link);
    QFile::remove(target);
    return ok;
}

/// Whether `pixmap` paints anything at all. A pixmap with nothing drawn on it is
/// still a valid, non-null QPixmap, so an icon that came out blank would pass a
/// null check; the logo tests ask for ink instead.
bool hasOpaquePixels(const QPixmap &pixmap)
{
    const QImage image = pixmap.toImage();
    if (image.isNull())
        return false;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0)
                return true;
        }
    }
    return false;
}

/// QStandardPaths::setTestModeEnabled is process-wide, and the setup-dialog
/// tests need it only for their own body. Restoring it in the destructor means
/// an assertion that returns early cannot leave test mode on for the rest of
/// the suite, which would silently redirect every later test's tool and ROM
/// lookups into the test-mode data root.
/// WCAG relative luminance, so a colour change can be judged as text on a
/// background rather than as a swatch that looks fine next to a keyword.
double contrastRatio(const QColor &fg, const QColor &bg)
{
    auto channel = [](int component) {
        const double s = component / 255.0;
        return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
    };
    auto luminance = [&](const QColor &c) {
        return 0.2126 * channel(c.red()) + 0.7152 * channel(c.green())
             + 0.0722 * channel(c.blue());
    };
    const double lighter = std::max(luminance(fg), luminance(bg));
    const double darker = std::min(luminance(fg), luminance(bg));
    return (lighter + 0.05) / (darker + 0.05);
}

struct TestModeScope
{
    TestModeScope() { QStandardPaths::setTestModeEnabled(true); }
    ~TestModeScope() { QStandardPaths::setTestModeEnabled(false); }
    TestModeScope(const TestModeScope &) = delete;
    TestModeScope &operator=(const TestModeScope &) = delete;
};

/// Restore an environment variable on every path out of the test, including the
/// early return an assertion takes: a structured environment stripped for a
/// test that then fails would stay stripped for the rest of the suite, and the
/// tests after it would silently look for their tools in the wrong place.
class EnvScope
{
public:
    explicit EnvScope(const char *name) : m_name(name), m_saved(qgetenv(name)) {}
    ~EnvScope()
    {
        if (m_saved.isNull())
            qunsetenv(m_name);
        else
            qputenv(m_name, m_saved);
    }
    EnvScope(const EnvScope &) = delete;
    EnvScope &operator=(const EnvScope &) = delete;

private:
    const char *m_name;
    QByteArray m_saved;
};

/// True once `finished` has recorded a reply for exactly `command`.
///
/// The deterministic form of "let that console command complete" for the
/// emulator tests: the reply is the event the next action must not overtake, and
/// a fixed sleep is only a guess about when it lands.
bool commandAnswered(const QSignalSpy &finished, const QString &command)
{
    for (const QList<QVariant> &call : finished)
        if (call.at(0).toString() == command)
            return true;
    return false;
}

/// The declarations of `selector`'s rule inside a stylesheet, empty when the
/// rule is absent.
///
/// Lets a test pin what a rule is *for* — the separator gives the splitter a
/// grab area in both orientations — without copying the sheet's source text, so
/// a cosmetic restyle does not fail a behaviour test.
QString styleRule(const QString &sheet, const QString &selector)
{
    const int at = sheet.indexOf(selector);
    if (at < 0)
        return {};
    const int open = sheet.indexOf(QLatin1Char('{'), at);
    const int close = sheet.indexOf(QLatin1Char('}'), open);
    if (open < 0 || close < 0)
        return {};
    return sheet.mid(open, close - open);
}

/// True when `rule` declares `property` with a positive pixel value.
bool declaresPositivePixels(const QString &rule, const QString &property)
{
    const QRegularExpression re(property + QStringLiteral(R"(\s*:\s*(\d+)px)"));
    const QRegularExpressionMatch match = re.match(rule);
    return match.hasMatch() && match.captured(1).toInt() > 0;
}

/// Ends whatever emulator session a test leaves running, on every path out of
/// the test function. A failed QCOMPARE returns immediately, skipping the
/// test's own host->stop(), and a window destroyed with a live session tears its
/// children down in an order that runs the backend's session-end handler
/// against siblings that are already gone: measured as a SIGSEGV/SIGABRT inside
/// that handler, which loses the test binary and leaves a real Hatari spinning
/// into the next test. Declared after the window so it runs before it, this
/// stops the session while every widget is still whole, so a failure mid-test
/// leaves nothing behind.
class EmulatorStopper
{
public:
    explicit EmulatorStopper(MainWindow &window) : m_window(window) {}

    ~EmulatorStopper()
    {
        // Whichever transport the test ended up on: the window replaces its
        // backend when the configured transport differs from the live one.
        auto *backend = m_window.findChild<IDebugBackend *>();
        if (backend && backend->isRunning())
            backend->stop();
    }

    EmulatorStopper(const EmulatorStopper &) = delete;
    EmulatorStopper &operator=(const EmulatorStopper &) = delete;

private:
    MainWindow &m_window;
};

} // namespace

/// Skip the calling test — or fail it under PIST_REQUIRE_EMULATOR — when the
/// emulator integration cannot run. Skipping is the right default on a
/// developer's machine, which may legitimately have none of this; in CI a skip
/// is a lie, because the run reports green while the emulator path went
/// untested, so CI sets the variable and a missing prerequisite fails loudly
/// instead. A macro because QSKIP/QFAIL return from the *calling* function.
#define REQUIRE_EMULATOR_OR_SKIP()                                             \
    do {                                                                       \
        const QString pist_missing = emulatorMissing();                        \
        if (!pist_missing.isEmpty()) {                                         \
            if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR")) {       \
                QFAIL(qPrintable(QStringLiteral(                                 \
                    "PIST_REQUIRE_EMULATOR is set but the emulator integration "\
                    "cannot run: missing %1").arg(pist_missing)));              \
            }                                                                  \
            QSKIP(qPrintable(QStringLiteral("needs %1 (set $PIST_TOS_DIR if "   \
                                            "needed)").arg(pist_missing)));     \
        }                                                                      \
    } while (false)

class TstGui : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanupTestCase();
    void windowConstructs();
    void aboutMenuIsFirstAndOpensGemDialog();
    void assemblesAndMapsLines();
    void editorShowsExecutionLineAndBreakpoints();
    void clearAllRemovesWatchpoints();
    void importAppendAddsSheetReplaceReplaces();
    void strokeDedupMakesUndoRestoreOriginal();
    void sheetCanvasBlitsPhaseComposite();
    void removeWatchpointWithoutSessionDoesNotError();

    /// Run must eventually start an emulator session — and must do so *after* the
    /// asynchronous build finishes, not alongside it.
    void runStartsAnEmulatorSession();
    /// An auto-selected ROM the chosen machine cannot run makes Hatari override
    /// `--machine`, which it reports only as an error line — so the run must say
    /// so itself. The two boundary cases ride in the same slot: a matching
    /// auto-selected ROM and an explicitly configured ROM must stay quiet.
    void autoSelectedRomForAnotherMachineWarnsAboutTheOverride();
    void breakpointSetBeforeRunFiresAndEditorFollows();
    void breakpointAddedWhileRunningFiresThisSession();
    void stepOutAndRunToCursorReachTheirTargets();
    void openRecentMenuListsAndOpensFiles();
    void ctrlClickOpensIncludesAndJumpsToLabels();
    void debugConsoleRecallsHistoryWithArrowKeys();
    void instructionReferenceFollowsTheCursor();
    void osCallReferenceFollowsTheCursor();
    void osCallBindingInsertsAtTheCursor();
    void diagnosticKeyboardFlowToursProblems();
    void navigationOpensTheOtherFile();
    void fileMenuAndToolbarMatchTheJobs();
    void symbolsPanelListsLabelsAfterBuild();
    void profilerCollectsAndMapsHotLines();
    void profileToCursorCollectsAndShowsResults();
    /// The guided run's one-shot is removed when another breakpoint ends the
    /// run: left armed at the cursor line it would stop the program there later
    /// for no reason.
    void guidedProfileDropsAnUnfiredOneShot();
    void profilerButtonsExplainThemselvesInTheDock();
    void remoteControlWatchersSeeSessionEvents();
    void dockLayoutPersistsAcrossRestart();
    void viewMenuListsEveryDock();
    void factoryLayoutShowsRegistersAndTheEditor();
    void windowGeometryPersistsAcrossRestart();
    void savedLayoutBeatsTheFactorySplit();
    void statusBarShowsCaretAndBuild();
    void statusBarNamesTheStop();
    void registerDockShowsFlagsUntilTheMachineStops();
    void layoutPresetsHideDocksAndRestore();
    /// The Emulator panel hosts another process's window, or nothing at all
    /// before a session: it must say which, rather than show a black void.
    void emulatorPanelExplainsItselfWhenEmpty();
    void gitDockStaysUnderProjectFiles();
    /// Git discovery follows the project the user opened, never the process
    /// working directory: a window with nothing open must run no git at all,
    /// because running `git status` inside whatever repository contains the
    /// launch directory is how a suite run left `.git/index.lock` in PiST's own
    /// checkout (MIN-81).
    void gitDiscoveryFollowsTheOpenedProject();
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
    /// The positional program path uses the host separator, so Hatari's
    /// Windows build can split directory from filename.
    void programPathUsesHostSeparators();
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
    void floppyRewriteOfAForeignDiskAsks();
    /// Copy, cut and paste through the browser panes: duplicate and move on
    /// the hard drive (with pathRenamed for open documents), and across the
    /// hard-drive and floppy panes.
    void fileBrowserCopyMovePanes();
    /// A copy-out whose host write fails reports the step that failed, so the
    /// browser's message has a reason after the colon instead of the empty or
    /// stale string the path used to carry (MIN-85).
    void floppyCopyOutNamesTheFailingWrite();
    /// A copy reproduces a symbolic link's content instead of silently
    /// dropping it, and refuses outright when it cannot.
    void fileBrowserCopyMaterialisesSymlinks();
    /// The move fallback below a failed rename must not delete — or claim to
    /// have deleted — a source it could not remove.
    void fileBrowserMoveFallbackNeverLosesTheSource();
    /// A floppy clipboard addresses entries of the image that was mounted when
    /// it was filled: ejecting that disk, or putting another one in the drive,
    /// must take the clipboard — and Paste — with it.
    void fileBrowserClipboardDiesWithItsDisk();
    /// A move across volumes materialises a nested symlink rather than handing
    /// it to whatever QFile::rename does internally. A guard, not a red: see
    /// the test's comment for what was measured.
    void fileBrowserCrossDeviceMoveMaterialisesLinks();
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
    /// Exporting the composed sheet as .pim writes the composition: the sheet
    /// exporter's format table used to have no Pim case.
    void sheetExportWritesTheComposedSheetAsPim();
    /// The slice dialog cuts an N-frame strip out of an imported sheet into a
    /// new phase's frames.
    void sliceDialogBuildsPhaseFromSheet();
    /// The same slice onto a phase whose cell size differs from the current
    /// phase's: every frame — the first one included — carries the sheet's
    /// pixels, at the new phase's cell size. A GUARD, not a red: measured
    /// against the pre-CRIT-2 addPhase (which built the frame at the previous
    /// phase's cell) the slice still passed, because painting the first cell
    /// remeshes the frame to the new size — see the body.
    void slicingAPhaseFromASheetOfAnotherCellSize();
    /// Re-slicing is refused for a phase index that no longer exists, the guard
    /// its sibling already carries: the index is a caller's, and QVector::at()
    /// on a stale one aborts the process in a debug build (MIN-88).
    void reSlicingAnUnknownPhaseIsRefused();
    /// The select tool: a marquee drag sets the selection, dragging inside it
    /// moves the pixels, and copy/paste/nudge/delete edit the active layer
    /// through the undo stack.
    void imageSelectionCopyPaste();
    /// The select tool's cut-out is an overlay on the frame, not part of it:
    /// the canvas must paint the pixels again once the drag is over.
    void selectionCutOutDoesNotStickToTheCanvas();
    /// The canvas selection belongs to the document it was made on: replacing
    /// the document, or opening a file over it, drops it.
    void swappingTheDocumentDropsTheOldSelection();
    void undoEditsThePhaseItWasMadeIn();
    void refusedBuildAnswersAndDropsLaunchIntent();
    /// A remote-initiated build or run must answer on buildCompleted without a
    /// dialog: a modal's event loop holds the reply until the operation
    /// timeout when nobody can dismiss it.
    void remoteBuildAndRunRefusalsStayQuiet();
    /// The launch half of a remote `run`: build, then a program the launch
    /// cannot find. The refusal must answer the reply, not open a modal.
    void remoteRunLaunchRefusalStaysQuiet();
    /// resetSessionState() owns everything that must not cross a session
    /// boundary, including a step-out and a profile save left waiting when the
    /// session died: the next session's entry stop must not consume them.
    void sessionResetDropsAnUnansweredStepOut();
    void stateSummaryClearsAfterSessionEnds();
    void projectAssemblerPathOverridesDiscovery();
    /// Importing an ST image adopts the file's palette registers as the
    /// active set, so the colours its pixels use are not overspill.
    void importAdoptsTheFilePalette();
    /// A debugger command typed into the console's entry line must be sent
    /// through the backend and its response appended to the console log.
    void consoleCommandRoundTrips();
    void editorTracksUnsavedChanges();
    /// A Latin-1 source keeps its bytes through a load/save round-trip, and a
    /// UTF-8 or ASCII one is written back unchanged.
    void editorRoundTripsNonUtf8Sources();
    /// Saving into a location that does not exist yet creates the parents.
    void editorSaveCreatesMissingDirectories();
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
    /// The Atari logo pixmap still draws both glyphs after the outline was
    /// parsed once per pixmap instead of once per draw.
    void atariLogoRendersBothGlyphs();
    void editorGotoIndentAndShortcutScheme();
    void quietColoursClearTheirBackground();
    void instructionStripFollowsTheCaret();
    /// Documents open in tabs: pristine-tab reuse, raise-not-duplicate, the
    /// modified marker on the label, and the never-empty invariant.
    void documentTabsManageOpenFiles();
    /// Opening a `.pim` creates an ImageEditor tab that coexists with `.s`
    /// tabs; Build still finds the assembly source when the image is focused.
    void imageTabsOpenBesideAssembly();
    /// Placement fields belong to spritesheet mode; onion sits with the frame
    /// buttons; play sits above the preview; icon rows pack instead of stretching.
    void spriteEditorChromeSitsWhereItIsUsed();
    /// New Image… requires a file name and writes the blank `.pim` before
    /// the editor opens, rather than defaulting every sprite to sprite.pim.
    void newImageDialogResolvesFileName();
    /// The bitplane export dialog's boxes and pre-shift picker are what the
    /// encoder is handed, and its map is the file's block table.
    void bitplaneExportDialogMapsChoices();
    /// Find and replace in the editor: incremental hits, case/word options,
    /// wrapping, one-step undo for Replace All, and the bar closing cleanly.
    void editorFindAndReplace();
    /// Next/Previous navigate from the caret, and the counter describes the hit
    /// the caret is on — across an edit that moved the hits under the search.
    void findNextFollowsTheCaretAcrossAnEdit();
    /// The "1 replaced" note belongs to the replace: the next navigation is a
    /// search step again and the live counter comes back.
    void replaceNoteClearsOnNavigation();
    /// A hit list in the thousands is capped for the live search (the label says
    /// "more than"), only the hits on screen are decorated, and Replace All
    /// still reaches every hit.
    void findDecoratesOnlyWhatIsOnScreen();
    /// The number gutter shares the viewport's coordinate space, so a click on
    /// the row line N's text is on toggles line N — including while the
    /// go-to bar has moved the viewport down.
    void gutterClickLandsOnTheLineBesideIt();
    /// Every document mutation is an undo command: an appended import (and the
    /// palette it adopts) survives an undo/redo round-trip through the commands
    /// pushed before it, as does a placement made from the panel.
    void undoRoundTripKeepsAnImportedSheet();
    /// Arming has one gate. An edit that reaches the pre-base window (a
    /// watchpoint added while the emulator is still booting to the entry stop)
    /// must not mark the session armed, or no source breakpoint is ever sent.
    void preBaseWatchpointEditStillArmsBreakpoints();
    /// The Search menu exists, its shortcuts land on the focused editor, and an
    /// image tab has nothing to search.
    void searchMenuFollowsTheEditor();
    /// Problems, hardware, memory, breakpoints and the console say what they
    /// are; a disassembly row and a PC-history line hand their address out.
    /// Split per surface: as one test the first failure hid every surface after
    /// it, and the report could not name what broke.
    void panelPlaceholdersNameTheNextStep();
    void breakpointPanelNamesTheShortcuts();
    void commonShortcutSchemeRenamesThePanelsKeys();
    void buildAndSearchMenusCarryTheirActions();
    void problemsDockListsABuildWarning();
    void themeSheetFramesItsDocks();
    void settingsDialogAndEditorChromeExposeTheirControls();
    void hardwareAndDisassemblyViewsNameWhatTheyShow();
    void pcHistoryDoubleClickReportsTheAddress();

    /// The highlighter's mnemonic family is derived from the 68000 reference
    /// plus the 68020/FPU extras, matched as whole words — the hand-written list
    /// it replaced had drifted, and `blo.s` in the shipped demo went uncoloured.
    void highlighterColoursMnemonicsOfTheReference();
    void highlighterColoursTheWholeReferenceAndTheExtras();
    /// Motorola syntax's single-quoted strings are strings: vasm's mot module
    /// treats `'…'` exactly as `"…"`, and every string in the demos is
    /// single-quoted, so the string rule must win over the mnemonic rule that
    /// finds `ST` inside `'PiST scroll demo'`.
    void singleQuotedStringsWinOverTheirContents();
    /// The ordering guard for the same rule: the string rule is applied last, so
    /// a quoted `add 12` is a string, not a mnemonic plus a number. (Like the
    /// gutter case this passes pre-fix as well — what it pins is the order of the
    /// rules, which a later rule reordering would break.)
    void doubleQuotedStringsStillWinOverTheirContents();
    /// A `;` inside a string does not start a comment (AsmLex owns that rule);
    /// outside one it does, even with a quote in the comment text.
    void aSemicolonInsideAStringIsNotAComment();
    /// vasm reads the first field as a label wherever it starts, so an indented
    /// label definition is a label.
    void anIndentedLabelIsALabel();
    /// REGRESSION GUARD, not a RED test: the gutter repaints when the error
    /// lines are set, the mark is visible in it, and clearing restores the plain
    /// rendering. Qt's updateRequest chain already reached the gutter on HEAD;
    /// what this pins is the whole path, not a mechanism.
    void settingErrorLinesRepaintsTheGutter();
    /// A subject change with no session running must not leave the previous
    /// subject's transcript on screen under the new label.
    void hardwareViewDropsTheTranscriptOfTheSubjectItLeft();
    /// A ROM the user picked must survive the machine change that rebuilds the
    /// list, because that is the ROM OK persists.
    void settingsDialogKeepsTheChosenRomAcrossAMachineChange();
    /// The dialog's minimum fits a short screen, the OK button stays inside
    /// the window at that minimum, and nothing about the content resizes the
    /// dialog once it is shown.
    void settingsDialogFitsAShortScreenWithReachableButtons();

private:
    QString m_vasm;
    QString m_tos;
    QTemporaryDir *m_work = nullptr;
    QString m_source;
};

void TstGui::initTestCase()
{
    m_vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (m_vasm.isEmpty()) {
        // A skip here takes the whole suite with it, so under
        // PIST_REQUIRE_EMULATOR it must fail instead: a CI leg missing the
        // assembler would otherwise report green having run nothing.
        if (!qEnvironmentVariableIsEmpty("PIST_REQUIRE_EMULATOR"))
            QFAIL("PIST_REQUIRE_EMULATOR is set but vasmm68k_mot is not on PATH");
        QSKIP("needs vasmm68k_mot");
    }

    m_work = new QTemporaryDir;
    QVERIFY(m_work->isValid());
    m_source = m_work->path() + QStringLiteral("/prog.s");

    // The suite must not carry state between runs through a shared settings
    // store. It never did touch the developer's PiST.conf — these binaries set
    // no organisation or application name, so QSettings resolved to an
    // "Unknown Organization" placeholder instead — but that file persisted
    // across runs, and last/source drives the constructor's deferred
    // openRecentSource() while setup/promptDismissed gates the setup dialog.
    // (This used to name last/project, a key the IDE wrote but never read; it is
    // deleted — MIN-51 — and the store's real readers are the two above.)
    // main() redirects QSettings to a throwaway directory for the whole run.
    QVERIFY2(QSettings().fileName().startsWith(QDir::tempPath()),
             qPrintable(QStringLiteral("QSettings resolves to %1, outside %2")
                            .arg(QSettings().fileName(), QDir::tempPath())));

    // Every window built by this suite sweeps the extracted-document cache at
    // construction (pruneStaleSessions() -> pruneExtractedDocuments() over
    // documentExtractDir()), and with the real cache root that means a test run
    // prunes and repopulates the developer's ~/.cache/PiST/documents.
    // QStandardPaths test mode cannot be used for this — main()'s note: it moves
    // the data root that the tool and ROM lookups derive from — so the cache
    // root is redirected on its own, before the first window exists. Linux only:
    // XDG_CACHE_HOME is the XDG variable, and the cache location is not
    // environment-selectable on macOS or Windows.
#ifdef Q_OS_LINUX
    qputenv("XDG_CACHE_HOME", QFile::encodeName(m_work->path() + QStringLiteral("/cache")));
    QVERIFY2(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                 .startsWith(QDir::tempPath()),
             qPrintable(QStringLiteral("the cache root resolves to %1, outside %2")
                            .arg(QStandardPaths::writableLocation(
                                     QStandardPaths::GenericCacheLocation),
                                 QDir::tempPath())));
#endif
}

// Every test starts from an empty settings store. main() already points
// QSettings at a throwaway directory, but that store is shared by the whole
// run, so a key one test writes (layout state, appearance, the open-recent MRU,
// the setup dismissal) changes what the next test's fresh MainWindow restores.
// That coupling is how a test flaked on the success path: an early return
// between a write and its hand-written cleanup skipped the cleanup. Clearing
// here makes each test independent of what ran before it, and the per-test
// defensive removes are gone with it.
void TstGui::init()
{
    QSettings().clear();
}

void TstGui::cleanupTestCase()
{
    // The suite's scratch tree is a member, so it outlives every test and would
    // otherwise only be reclaimed by the QTemporaryDir destructor at process
    // exit — a full tree of listings, .prg files and session directories left in
    // /tmp on every run, including a crashed one.
    delete m_work;
    m_work = nullptr;
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
        // The branding strings are cosmetic; the contract is that the dialog
        // carries all four. The year was pinned to the literal "2026", which
        // would have failed the first January after it was written, so it is a
        // four-digit year instead of a copy of the source text.
        QVERIFY(title->text().contains(QLatin1String("PIST")));
        QCOMPARE(version->text(), QStringLiteral(PIST_VERSION));
        QVERIFY(QRegularExpression(QStringLiteral(R"(\d{4})")).match(copyright->text()).hasMatch());
        QVERIFY(!credit->text().isEmpty());
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
    REQUIRE_EMULATOR_OR_SKIP();

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
    EmulatorStopper stopper(window);
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

// Hatari resolves a machine/ROM mismatch itself: it overrides `--machine` to
// match the ROM and reports it only as an error line, so a run that silently
// becomes a different machine than the one selected is the failure this warns
// about. Only the auto-selected ROM is checked — a ROM the user configured is
// their own decision, marked "(not for <machine>)" where it is chosen — so both
// boundaries ride along: a matching auto-selected ROM, and an explicit one.
void TstGui::autoSelectedRomForAnotherMachineWarnsAboutTheOverride()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QList<TosRom> roms = findTosRoms();
    const TosRom autoRom = selectPreferredRom(roms, Machine::Falcon);
    if (autoRom.path.isEmpty())
        QSKIP("no TOS ROM found to auto-select");
    if (autoRom.supportsMachine(Machine::Falcon))
        QSKIP("a Falcon-compatible ROM exists, so no override would happen");
    const TosRom stRom = selectPreferredRom(roms, Machine::St);
    if (stRom.path.isEmpty() || !stRom.supportsMachine(Machine::St))
        QSKIP("no ST-capable ROM, so the matching-ROM boundary cannot be exercised");

    // The same project shape for all three phases, differing only in what is
    // configured: a spinning program (so the session is stoppable), no include
    // paths (so no modal can interrupt), and a quiet Run through the slot.
    const auto writeProject = [this](const QString &name, Machine machine, const QString &romPath) {
        const QString path = m_work->path() + QLatin1Char('/') + name;
        QFile src(path);
        if (!src.open(QIODevice::WriteOnly | QIODevice::Text))
            return QString();
        src.write("\ttext\n"
                  "start:\tmoveq\t#1,d0\n"
                  "loop:\tbra.s\tloop\n"
                  "\teven\n"
                  "\tend\n");
        src.close();

        ProjectSettings settings;
        settings.sourceFile = path;
        settings.machine = machine;
        settings.monitor = QStringLiteral("mono");
        settings.memSizeMiB = 1;
        settings.tosPath = romPath;
        QString error;
        if (!settings::save(settings, settings::projectFileFor(path), &error)) {
            qWarning().noquote() << error;
            return QString();
        }
        return path;
    };

    // The reported case: Falcon selected, no ROM configured, and no Falcon ROM
    // on disk — selectPreferredRom returns the newest ROM anyway, so that the
    // message can name it.
    const QString falconSource =
        writeProject(QStringLiteral("autooverride.s"), Machine::Falcon, QString());
    QVERIFY(!falconSource.isEmpty());
    {
        MainWindow window;
        EmulatorStopper stopper(window);
        window.openPath(falconSource);
        QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));

        // The warning is emitted before Hatari spawns, so it must not be waited
        // for through a Falcon + older-TOS boot (which would never arrive).
        QTRY_VERIFY_WITH_TIMEOUT(
            window.debugConsoleText().contains(QLatin1String("will override the machine")),
            30000);
        // It names both the ROM that will win and the machine that loses.
        QVERIFY(window.debugConsoleText().contains(autoRom.fileName));
        QVERIFY(window.debugConsoleText().contains(machineDisplayName(Machine::Falcon)));

        // Safe before the session is up, and it ends whatever did start.
        if (auto *host = window.findChild<EmulatorHost *>())
            host->stop();
    }

    // Boundary: an auto-selected ROM the machine can run is no override. An
    // implementation that warns whenever the ROM is not the machine's own would
    // fail here.
    const QString stSource = writeProject(QStringLiteral("automatch.s"), Machine::St, QString());
    QVERIFY(!stSource.isEmpty());
    {
        MainWindow window;
        EmulatorStopper stopper(window);
        window.openPath(stSource);
        QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
        // The ROM check sits between the build and the spawn, so reaching the
        // spawn proves it ran — the negative assertion below is not vacuous.
        QTRY_VERIFY_WITH_TIMEOUT(
            window.debugConsoleText().contains(QLatin1String("Session started in")), 30000);
        QVERIFY2(!window.debugConsoleText().contains(QLatin1String("will override the machine")),
                 qPrintable(window.debugConsoleText()));
        if (auto *host = window.findChild<EmulatorHost *>())
            host->stop();
    }

    // Boundary: with the ROM configured explicitly, the same pairing stays
    // quiet — the note is for the auto-selection only, and an implementation
    // that dropped that condition fails here.
    const QString explicitSource = writeProject(QStringLiteral("explicitrom.s"), Machine::Falcon,
                                                autoRom.path);
    QVERIFY(!explicitSource.isEmpty());
    {
        MainWindow window;
        EmulatorStopper stopper(window);
        window.openPath(explicitSource);
        QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
        QTRY_VERIFY_WITH_TIMEOUT(
            window.debugConsoleText().contains(QLatin1String("Session started in")), 30000);
        QVERIFY2(!window.debugConsoleText().contains(QLatin1String("will override the machine")),
                 qPrintable(window.debugConsoleText()));
        if (auto *host = window.findChild<EmulatorHost *>())
            host->stop();
    }
}

void TstGui::refusedBuildAnswersAndDropsLaunchIntent()
{
    MainWindow window; // no source open
    QSignalSpy completed(&window, &MainWindow::buildCompleted);
    QTimer::singleShot(0, &window, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->accept();
    });
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    // A refused build is a completed build: answered now, not after a
    // timeout, and without leaving a launch armed.
    QCOMPARE(completed.count(), 1);
    QCOMPARE(completed.first().first().toBool(), false);

    // The refusal must not leave m_launchAfterBuild set: a later successful
    // build has no reason to start an emulator the user never asked for.
    const QString source = m_work->path() + QStringLiteral("/later.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tmoveq\t#1,d0\n\trts\n\teven\n\tend\n");
    src.close();
    window.openPath(source);

    QSignalSpy completed2(&window, &MainWindow::buildCompleted);
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(completed2.count(), 1, 30000);
    QCOMPARE(completed2.first().first().toBool(), true);

    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);
    QTest::qWait(1500);
    QVERIFY(!host->isRunning());
}

// The remote-control `build` and `run` verbs answer on buildCompleted, and
// their reply loop cannot return while a modal is open: a refusal that opens
// QMessageBox holds the reply until a human dismisses it, which for an agent on
// the other end means the full operation timeout. Both refusals must reach
// buildCompleted and the console with no modal widget anywhere.
void TstGui::remoteBuildAndRunRefusalsStayQuiet()
{
    MainWindow window;
    EmulatorStopper stopper(window);
    window.show();

    // A modal is the bug under test, so watch for one from inside whatever
    // event loop opens it: record it, then close it so the test can fail on the
    // observation instead of sitting behind it until the watchdog.
    QCOMPARE(QApplication::activeModalWidget(), nullptr);
    bool sawModal = false;
    QTimer poller;
    poller.setInterval(10);
    QObject::connect(&poller, &QTimer::timeout, [&sawModal] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            sawModal = true;
            box->close();
        }
    });
    poller.start();

    // No source is open, so the build refuses. RemoteControl invokes the slot by
    // name and its wait ends on buildCompleted.
    QSignalSpy completed(&window, &MainWindow::buildCompleted);
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
    QCOMPARE(completed.first().first().toBool(), false);
    QVERIFY(window.debugConsoleText().contains(QLatin1String("build refused")));
    QVERIFY2(!sawModal, "a modal opened for a remote build refusal");

    // Run refuses through the build it starts, and must answer the same way.
    completed.clear();
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
    QCOMPARE(completed.first().first().toBool(), false);
    QVERIFY2(!sawModal, "a modal opened for a remote run refusal");
    QCOMPARE(QApplication::activeModalWidget(), nullptr);
}

// The launch half of a remote `run`: the build succeeds and the program it was
// supposed to produce is gone, so launchEmulator refuses. That refusal used to
// be a modal only, which left the agent's `run` reply waiting on the operation
// timeout; it must answer buildCompleted(false) — the channel RemoteControl
// waits on after the build — and log the reason.
void TstGui::remoteRunLaunchRefusalStaysQuiet()
{
    const QString source = m_work->path() + QStringLiteral("/nolaunch.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tmoveq\t#0,d0\n\trts\n\teven\n\tend\n");
    src.close();

    MainWindow window;
    EmulatorStopper stopper(window);
    window.show();
    window.openPath(source);
    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);

    bool sawModal = false;
    QTimer poller;
    poller.setInterval(10);
    QObject::connect(&poller, &QTimer::timeout, [&sawModal] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            sawModal = true;
            box->close();
        }
    });
    poller.start();

    QSignalSpy completed(&window, &MainWindow::buildCompleted);

    // The launch runs straight after the build reports, so take the program away
    // in that gap: the run then stops at launchEmulator's "the build did not
    // produce …" precondition, which is where the modal used to appear.
    const QString prg = settings::outputPathsFor(source).program;
    QObject::connect(&window, &MainWindow::buildCompleted, this, [&prg](bool ok) {
        if (ok)
            QFile::remove(prg);
    });

    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() >= 1, 30000);
    QCOMPARE(completed.first().first().toBool(), true);
    // The refusal is the answer the remote `run` waits for.
    QTRY_VERIFY_WITH_TIMEOUT(completed.count() >= 2, 10000);
    QCOMPARE(completed.last().first().toBool(), false);
    QVERIFY2(!sawModal, "a modal opened for a remote run refusal");
    QCOMPARE(QApplication::activeModalWidget(), nullptr);
    QVERIFY(window.debugConsoleText().contains(QLatin1String("did not produce")));
    QVERIFY(!QFileInfo::exists(prg));
    QVERIFY(!host->isRunning());
}

// After a build the symbols dock lists the program's labels with their source
// locations, parsed from the listing the build just wrote — and activating one
// navigates the editor to its definition.
void TstGui::symbolsPanelListsLabelsAfterBuild()
{
    const QString source = m_work->path() + QStringLiteral("/syms.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                     // line 1
              "start:\tmoveq\t#1,d0\n"       // line 2
              "\tbsr\thelper\n"              // line 3
              "\trts\n"                      // line 4
              "helper:\tmoveq\t#2,d1\n"      // line 5
              "\trts\n"                      // line 6
              "\teven\n"
              "\tend\n");
    src.close();

    MainWindow window;
    window.show();
    window.openPath(source);
    QSignalSpy completed(&window, &MainWindow::buildCompleted);
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 30000);
    QCOMPARE(completed.first().first().toBool(), true);

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("symbolsDock"));
    QVERIFY(dock);
    auto *tree = dock->findChild<QTreeWidget *>();
    QVERIFY(tree);

    auto rowFor = [&](const QString &name) -> int {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == name)
                return i;
        return -1;
    };
    const int startRow = rowFor(QStringLiteral("start"));
    const int helperRow = rowFor(QStringLiteral("helper"));
    QVERIFY(startRow >= 0);
    QVERIFY(helperRow >= 0);
    QCOMPARE(tree->topLevelItem(startRow)->text(2), QStringLiteral("syms.s:2"));
    QCOMPARE(tree->topLevelItem(helperRow)->text(2), QStringLiteral("syms.s:5"));

    // Activation navigates the editor to the definition, like a breakpoint.
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    emit tree->itemActivated(tree->topLevelItem(helperRow), 0);
    QCOMPARE(editor->textCursor().blockNumber(), 4);  // line 5, 0-based 4
}

// A `watch` subscription turns the control connection into an event stream:
// running/stopped edges arrive with the stop's PC, so an agent never polls to
// notice a breakpoint.
void TstGui::remoteControlWatchersSeeSessionEvents()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/watch.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tmoveq\t#0,d0\nloop:\taddq.w\t#1,d0\n\tbra.s\tloop\n\tend\n");
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
    EmulatorStopper stopper(window);
    window.show();
    RemoteControl control(&window);
    window.setEventSink(&control);
    QVERIFY2(control.listen(0, &error), qPrintable(error));

    QTcpSocket watcher;
    watcher.connectToHost(QHostAddress::LocalHost, control.boundPort());
    QVERIFY(watcher.waitForConnected(3000));
    // One write is fine: the server reads line-wise, auth first.
    watcher.write("auth " + control.token().toUtf8() + "\nwatch\n");
    // waitForReadyRead does not spin the window's event loop, where the
    // server runs — poll in short slices instead of one long wait.
    QString events;
    QTRY_VERIFY_WITH_TIMEOUT(
        (watcher.waitForReadyRead(100), events += QString::fromUtf8(watcher.readAll()),
         events.contains(QLatin1String("ok\n"))),
        5000);

    window.openPath(source);
    // A modal from a failed Run would hang the offscreen suite until the
    // watchdog; answer it if one appears, and dump the console on failure.
    QTimer::singleShot(0, &window, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->accept();
    });
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    const bool entered = QTest::qWaitFor(
        [&] {
            return watcher.waitForReadyRead(100)
                   && (events += QString::fromUtf8(watcher.readAll()))
                          .contains(QLatin1String("event stopped pc=0x"));
        },
        30000);
    if (!entered)
        qDebug().noquote() << window.debugConsoleText();
    QVERIFY(entered);
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(
        (watcher.waitForReadyRead(100), events += QString::fromUtf8(watcher.readAll()),
         events.contains(QLatin1String("event running"))),
        10000);

    auto *host = window.findChild<EmulatorHost *>();

    QVERIFY(host);
    host->stop();
}
// The profiling flow end to end against a real Hatari: a breakpoint armed
// before Profile Start (arming one mid-run would reset the counters), collect
// over the run, Profile Stop saves and parses, and the hot lines land in the
// profiler dock and the gutter heat.
void TstGui::profilerCollectsAndMapsHotLines()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/prof.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                     // line 1
              "start:\tmoveq\t#0,d0\n"       // line 2
              "loop:\taddq.w\t#1,d0\n"       // line 3  <- the hot line
              "\tcmp.w\t#100,d0\n"           // line 4
              "\tblo.s\tloop\n"              // line 5
              "done:\tbra.s\tdone\n"         // line 6
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
    EmulatorStopper stopper(window);
    window.show();
    window.openPath(source);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(host && editor);

    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);

    // The stopping breakpoint goes in BEFORE Profile Start — arming anything
    // after `profile on` resets the counters (Profile_CpuStart memsets).
    auto *input = window.findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    QVERIFY(input);
    input->setText(QStringLiteral("b d0 = 50 :once"));
    QSignalSpy commandReplies(host, &IDebugBackend::commandFinished);
    QTest::keyClick(input, Qt::Key_Return);
    // Wait for the command's own reply rather than guessing a delay: the
    // breakpoint has to be armed before `profile on`, which memsets the
    // counters. A sleep can only be right by luck.
    QTRY_VERIFY_WITH_TIMEOUT(commandAnswered(commandReplies, QStringLiteral("b d0 = 50 :once")),
                             15000);

    QVERIFY(QMetaObject::invokeMethod(&window, "profileStart", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    if (!QTest::qWaitFor([&] { return editor->currentExecutionLine() == 4; }, 30000))
        qDebug().noquote() << window.debugConsoleText();
    QCOMPARE(editor->currentExecutionLine(), 4);

    QVERIFY(QMetaObject::invokeMethod(&window, "profileStop", Qt::DirectConnection));

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("profilerDock"));
    QVERIFY(dock);
    auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("profilerTree"));
    QVERIFY(tree);

    // Wait for the parse outcome itself: rows, or the parser's error in the
    // console. (Hatari's own stop-time stats say "executed instructions", so
    // matching on "instructions" alone would race profileStop's commands.)
    QTRY_VERIFY_WITH_TIMEOUT(
        tree->topLevelItemCount() > 0
        || window.debugConsoleText().contains(QLatin1String("[profile] no"))
        || window.debugConsoleText().contains(QLatin1String("[profile] not")),
        15000);
    // Stop before the skip path too: bailing with a live session aborts the
    // process on teardown (observed on CI as exit 134 after the QSKIP).
    const bool haveRows = tree->topLevelItemCount() > 0;
    if (!haveRows) {
        // The save needs the external disassembler (the UAE core writes
        // profile text to the trace file, not the save file): a Hatari
        // without Capstone produces no instruction lines.
        host->stop();
        QSKIP("this Hatari build has no Capstone disassembler for profile save");
    }

    // The loop body dominates: its line is a child row under its routine.
    QStringList lines, routines;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *root = tree->topLevelItem(i);
        routines << root->text(0);
        for (int c = 0; c < root->childCount(); ++c)
            lines << root->child(c)->text(0);
    }
    QVERIFY2(lines.contains(QStringLiteral("3")), qPrintable(lines.join(',')));
    QVERIFY2(routines.contains(QStringLiteral("loop")), qPrintable(routines.join(',')));
    QVERIFY(editor->hasLineHeat());

    host->stop();
}

// The guided flow: cursor on the measurement's end line, one action, and the
// results save and show themselves when the run stops there — no Profile
// Stop step, no manual choreography.
void TstGui::profileToCursorCollectsAndShowsResults()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/profguided.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                     // line 1
              "start:\tmoveq\t#0,d0\n"       // line 2
              "loop:\taddq.w\t#1,d0\n"       // line 3  <- the hot line
              "\tcmp.w\t#100,d0\n"           // line 4
              "\tblo.s\tloop\n"              // line 5
              "done:\tbra.s\tdone\n"         // line 6  <- measurement ends here
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
    EmulatorStopper stopper(window);
    window.show();
    window.openPath(source);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(host && editor);

    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);

    auto *status = window.findChild<QLabel *>(QStringLiteral("profilerStatus"));
    QVERIFY(status);
    auto *toCursorButton = window.findChild<QToolButton *>(QStringLiteral("profilerToCursorButton"));
    QVERIFY(toCursorButton);

    // A refusal message (cursor on a line with no code) must be replaced by
    // the success feedback, not survive into the real run.
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(editor->document()->findBlockByNumber(0).position());
    editor->setTextCursor(cursor);
    QTest::mouseClick(toCursorButton, Qt::LeftButton);
    QVERIFY(status->text().contains(QStringLiteral("No code")));

    // Cursor on the measurement's end, one click: arm the one-shot, profile
    // on, continue — the stop saves and shows without another click.
    cursor.setPosition(editor->document()->findBlockByNumber(5).position());
    editor->setTextCursor(cursor);
    QTest::mouseClick(toCursorButton, Qt::LeftButton);
    QVERIFY(status->text().contains(QStringLiteral("Collecting to")));

    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 6, 30000);

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("profilerDock"));
    QVERIFY(dock);
    auto *tree = dock->findChild<QTreeWidget *>(QStringLiteral("profilerTree"));
    QVERIFY(tree);
    QTRY_VERIFY_WITH_TIMEOUT(
        tree->topLevelItemCount() > 0
        || window.debugConsoleText().contains(QLatin1String("[profile] no"))
        || window.debugConsoleText().contains(QLatin1String("[profile] not")),
        15000);

    // Capture every observable before stopping the session: an assertion that
    // fails with a live session is the documented teardown hang (exit 134).
    const bool haveRows = tree->topLevelItemCount() > 0;
    QStringList lines;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *root = tree->topLevelItem(i);
        for (int c = 0; c < root->childCount(); ++c)
            lines << root->child(c)->text(0);
    }
    const QString console = window.debugConsoleText();
    const bool heat = editor->hasLineHeat();

    // The guided stop ended collection: Start and Profile-to-cursor make
    // sense again, Stop has nothing left to save.
    auto *startButton = window.findChild<QToolButton *>(QStringLiteral("profilerStartButton"));
    auto *stopButton = window.findChild<QToolButton *>(QStringLiteral("profilerStopButton"));
    const bool startEnabled = startButton && startButton->isEnabled();
    const bool stopEnabled = stopButton && stopButton->isEnabled();
    const bool toCursorEnabled = toCursorButton->isEnabled();
    host->stop();

    if (!haveRows)
        QSKIP("this Hatari build has no Capstone disassembler for profile save");
    QVERIFY(startEnabled);
    QVERIFY(!stopEnabled);
    QVERIFY(toCursorEnabled);
    QVERIFY2(lines.contains(QStringLiteral("3")), qPrintable(lines.join(',')));
    QVERIFY(console.contains(QLatin1String("Collecting to")));
    QVERIFY(heat);
}

// The guided run's teardown has to drop the one-shot it armed. When the run
// ends at the one-shot itself it is already consumed (`:once` removes it on the
// hit), but when another breakpoint stops the machine first, the one-shot is
// still armed at the cursor line — and the program would stop there later for no
// reason at all. That is what the teardown is for, and what it used to send
// instead was `db pc = $X :once`: `db` is dspbreak, a *DSP* breakpoint command,
// so on an ST it printed "DSP isn't present or initialized." and deleted
// nothing.
void TstGui::guidedProfileDropsAnUnfiredOneShot()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/guidedstray.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                     // line 1
              "start:\tmoveq\t#0,d0\n"       // line 2
              "loop:\taddq.w\t#1,d0\n"       // line 3
              "\tcmp.w\t#20,d0\n"            // line 4  <- the other breakpoint stops here
              "\tblo.s\tloop\n"              // line 5
              "done:\tbra.s\tdone\n"         // line 6  <- the guided run's target
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
    EmulatorStopper stopper(window);
    window.show();
    window.openPath(source);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(host && editor);

    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);

    // A one-shot of the user's own, armed at the entry stop, which fires before
    // the machine has executed far enough to reach the cursor line. It is what
    // "another breakpoint won the race" means for the guided run.
    auto *input = window.findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    QVERIFY(input);
    input->setText(QStringLiteral("b d0 = 10 :once"));
    QSignalSpy commandReplies(host, &IDebugBackend::commandFinished);
    QTest::keyClick(input, Qt::Key_Return);
    // Deterministic completion wait, as above: the one-shot must be armed
    // before the guided run's own commands reach the debugger.
    QTRY_VERIFY_WITH_TIMEOUT(commandAnswered(commandReplies, QStringLiteral("b d0 = 10 :once")),
                             15000);

    // Cursor on the measurement's end (line 6) and one click: arm the one-shot
    // there, profile on, continue.
    auto *toCursorButton = window.findChild<QToolButton *>(QStringLiteral("profilerToCursorButton"));
    QVERIFY(toCursorButton);
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(editor->document()->findBlockByNumber(5).position());
    editor->setTextCursor(cursor);
    QTest::mouseClick(toCursorButton, Qt::LeftButton);

    QTRY_VERIFY_WITH_TIMEOUT(host->isStopped(), 30000);
    // The run has to have been ended by the *other* breakpoint: a stop at the
    // cursor line would leave nothing stray behind, and this test would pass
    // without exercising the teardown at all.
    QVERIFY2(editor->currentExecutionLine() != 6,
             "the guided one-shot fired, so there is no stray breakpoint to clean up");
    // Let the teardown's own commands (the save, and the breakpoint rebuild)
    // flush while the machine is stopped.
    QTest::qWait(1000);

    // Continue: with the stray one-shot gone the program spins at `done` (line
    // 6) and never stops. Left armed — what `db` did — the machine stops there
    // as soon as the loop exits.
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTest::qWait(2000);
    const bool stoppedAtTheCursor = host->isStopped();
    const int line = editor->currentExecutionLine();
    host->stop();

    QVERIFY2(!stoppedAtTheCursor,
             qPrintable(QStringLiteral("the guided run's one-shot was still armed: the machine "
                                       "stopped at line %1 after the run had ended")
                            .arg(line)));
}

// The dock's buttons follow the profiling state machine: with no session
// they are all disabled (nothing to stop, nothing to collect from), and the
// empty state in the status line teaches the flow. The click-level refusal
// and success feedback is covered by profileToCursorCollectsAndShowsResults.
void TstGui::profilerButtonsExplainThemselvesInTheDock()
{
    MainWindow window;
    window.show();

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("profilerDock"));
    QVERIFY(dock);
    auto *status = dock->findChild<QLabel *>(QStringLiteral("profilerStatus"));
    QVERIFY(status);
    QVERIFY(!status->text().isEmpty()); // the empty state teaches the flow

    for (const char *name : {"profilerStartButton", "profilerStopButton", "profilerToCursorButton"}) {
        auto *button = dock->findChild<QToolButton *>(QString::fromLatin1(name));
        QVERIFY2(button, name);
        QVERIFY2(!button->isEnabled(), name); // no session: nothing to profile
    }

    dock->show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *start = dock->findChild<QToolButton *>(QStringLiteral("profilerStartButton"));
    auto *stop = dock->findChild<QToolButton *>(QStringLiteral("profilerStopButton"));
    QVERIFY(start && stop);
    QVERIFY2(start->width() <= start->sizeHint().width() + 8,
             "profiler buttons pack instead of stretching across the dock");
    QVERIFY(stop->x() > start->x());
    QVERIFY2(stop->x() - (start->x() + start->width()) <= 8,
             "profiler buttons sit next to each other");
}

void TstGui::clearAllRemovesWatchpoints()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QCoreApplication::processEvents();

    QString error;
    // A lone watchpoint, no breakpoints. The dock's "Clear all" enables for it
    // (its enable test is "any breakpoint OR any watchpoint"), so it must clear
    // the watchpoint too — the shared clearAllBreakpoints handler used to leave
    // it, so the button stayed enabled.
    QVERIFY2(window.addWatchpointAddress(QStringLiteral("$1234.w"), &error), qPrintable(error));

    QPushButton *clear = nullptr;
    for (QPushButton *b : window.findChildren<QPushButton *>()) {
        if (b->text() == QStringLiteral("Clear all")) {
            clear = b;
            break;
        }
    }
    QVERIFY2(clear, "the breakpoints dock has a Clear all button");
    QVERIFY2(clear->isEnabled(), "Clear all must enable for a lone watchpoint");

    clear->click();
    QCoreApplication::processEvents();

    QVERIFY2(!clear->isEnabled(),
             "Clear all left the watchpoint behind — it cleared only breakpoints");
}

void TstGui::importAppendAddsSheetReplaceReplaces()
{
    ImageDocument art = ImageDocument::create(16, 16, PaletteKind::Ste);
    art.setPixel(0, art.active().at(1));
    const QString dir = m_work->path() + QStringLiteral("/import-append");
    QVERIFY(QDir().mkpath(dir));
    const QString pi1Path = dir + QStringLiteral("/a.pi1");
    QString error;
    const QByteArray pi1 = exportPi1(art, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));
    QFile out(pi1Path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    out.write(pi1);
    out.close();

    ImageEditor editor;
    editor.show();
    QVERIFY(editor.importFile(pi1Path, false));
    QCOMPARE(editor.document().sheets().size(), 1);

    // "Add frame" keeps the current image and adds the import as another sheet.
    QVERIFY(editor.importFile(pi1Path, true));
    QCOMPARE(editor.document().sheets().size(), 2);

    // "Replace" discards the current document, so the import is the whole image
    // again — one sheet, not a third. Before append was honoured, every import
    // added a sheet (1, 2, 3), so the dialog's Replace was a no-op.
    QVERIFY(editor.importFile(pi1Path, false));
    QCOMPARE(editor.document().sheets().size(), 1);
}

// SnapshotCommand::undo assigns the whole document, so a mutation that skips the
// undo stack sits *under* the next command's `before`: undoing that one silently
// wipes it, and its `after` cannot bring it back. An appended import is two such
// mutations at once — the sheet target and the palette it adopts — and the
// placement panel was a third.
void TstGui::undoRoundTripKeepsAnImportedSheet()
{
    // A PI1 whose registers are deliberately not the editor default, so the
    // palette the import adopts is part of what has to come back.
    // Stfm, not Ste: a PI1 stores 3-bit words and an Ste palette is not
    // representable in them — MAJ-38 made the codec honest about that, so these
    // two fixtures were only green under the buggy 4-bit words. These indices
    // are valid 3-bit triples: the deliberately non-default palette stays
    // non-default and the round trip is index-exact again.
    ImageDocument art = ImageDocument::create(8, 8, PaletteKind::Stfm);
    QVector<int> custom;
    for (int i = 0; i < 15; ++i)
        custom.append(0x11 * (i + 1));
    custom.append(0x001);
    art.setActive(custom);
    art.setPixel(0, custom.at(3));
    const QString dir = m_work->path() + QStringLiteral("/undoimport");
    QVERIFY(QDir().mkpath(dir));
    const QString pi1Path = dir + QStringLiteral("/sheet.pi1");
    QString error;
    const QByteArray pi1 = exportPi1(art, 0, &error);
    QVERIFY2(!pi1.isEmpty(), qPrintable(error));
    QFile out(pi1Path);
    QVERIFY(out.open(QIODevice::WriteOnly));
    QCOMPARE(out.write(pi1), qint64(pi1.size()));
    out.close();

    ImageEditor editor;
    editor.show();
    editor.newDocument(8, 8, PaletteKind::Stfm);
    const QVector<int> defaultPalette = editor.document().active();

    // One command under the import. Undoing the import must stop on top of this
    // one, not reach through it: the add-frame's `before` predates the import.
    auto *addFrame = editor.findChild<QToolButton *>(QStringLiteral("imageAddFrame"));
    QVERIFY(addFrame);
    addFrame->click();
    QCOMPARE(editor.document().frameCount(), 2);

    QVERIFY(editor.importFile(pi1Path, true));
    QCOMPARE(editor.document().sheets().size(), 1);
    QCOMPARE(editor.document().active(), custom);
    const QByteArray imported = editor.document().toJson();

    // Ctrl+Z undoes the import: the sheet it added goes, the adopted palette
    // goes back — and the frames that were already there stay untouched. Pre-fix
    // there was no import command, so this undo assigned the add-frame's
    // pre-import document and the frame count fell to 1 with the import gone.
    editor.undo();
    QCOMPARE(editor.document().sheets().size(), 0);
    QCOMPARE(editor.document().active(), defaultPalette);
    QCOMPARE(editor.document().frameCount(), 2);

    // And the add-frame is still reachable below it.
    editor.undo();
    QCOMPARE(editor.document().frameCount(), 1);

    // Redo restores both, byte for byte — the sheet, its path and the palette.
    editor.redo();
    editor.redo();
    QCOMPARE(editor.document().toJson(), imported);

    // The placement panel holds the same contract: one panel edit is one command
    // covering the placement and the pixels it slices out of the sheet.
    auto *sheetBox = editor.findChild<QComboBox *>(QStringLiteral("imagePhaseSheet"));
    auto *x = editor.findChild<QSpinBox *>(QStringLiteral("imagePhaseX"));
    QVERIFY(sheetBox && x);
    sheetBox->setCurrentIndex(1);   // the imported sheet
    QCOMPARE(editor.document().phases().at(0).sheet, 0);
    x->setValue(4);
    QCOMPARE(editor.document().phases().at(0).x, 4);

    editor.undo();
    QCOMPARE(editor.document().phases().at(0).x, 0);
    editor.undo();
    QCOMPARE(editor.document().phases().at(0).sheet, -1);
    editor.redo();
    editor.redo();
    QCOMPARE(editor.document().phases().at(0).sheet, 0);
    QCOMPARE(editor.document().phases().at(0).x, 4);
}

void TstGui::strokeDedupMakesUndoRestoreOriginal()
{
    // Guards the O(1) stroke dedup: a cell painted twice in one stroke must
    // record its *original* value once (so undo restores it), not the colour
    // painted on the second visit. A drag that crosses (2,2), leaves, and returns
    // visits it in two paintIndices calls.
    ImageEditor editor;
    editor.show();
    editor.newDocument(8, 8, PaletteKind::Ste);
    auto *canvas = editor.findChild<ImageCanvas *>();
    QVERIFY(canvas);
    const auto idx = [](int x, int y) { return y * 8 + x; };
    const auto cellPoint = [&canvas](int x, int y) {
        const int c = canvas->cellSize();
        return QPoint(x * c + c / 2, y * c + c / 2);
    };
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(2, 2));
    QTest::mouseMove(canvas, cellPoint(3, 2));
    QTest::mouseMove(canvas, cellPoint(2, 2));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(2, 2));
    QVERIFY2(editor.document().pixels().at(idx(2, 2)) != kTransparent, "the stroke painted (2,2)");

    editor.undo();
    // Without the dedup, the second visit recorded the painted colour as the
    // "before" value and undo would leave it here instead of transparent.
    QCOMPARE(editor.document().pixels().at(idx(2, 2)), kTransparent);
}

void TstGui::sheetCanvasBlitsPhaseComposite()
{
    // The per-frame QImage blit must draw the same pixels the old per-pixel
    // fillRect loop did — a broken rect/transform/colour would leave the strip
    // blank or mis-tinted. Paint a 2x2 block, render the sheet, find the colour.
    ImageDocument doc = ImageDocument::create(4, 4, PaletteKind::Ste);
    doc.addSheet(QStringLiteral("s.pi1"), 32, 32);
    QVERIFY(doc.setPhasePlacement(0, 0, 0, 0));
    const int colour = doc.active().at(1);
    doc.fillIndices({0, 1, 4, 5}, colour, 0);
    const Rgb rgb = cubeRgb(PaletteKind::Ste, colour);
    const QRgb want = qRgb(rgb.r, rgb.g, rgb.b);

    SheetCanvas canvas;
    canvas.setDocument(&doc);
    canvas.setSheetIndex(0);
    canvas.setScale(8);
    canvas.resize(canvas.sizeHint());
    QImage img(canvas.size(), QImage::Format_ARGB32);
    img.fill(Qt::black);
    canvas.render(&img);

    bool found = false;
    for (int y = 0; y < img.height() && !found; ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixelColor(x, y).rgb() == want) {
                found = true;
                break;
            }
    QVERIFY2(found, "the painted phase cells must be blitted into the sheet view");
}

void TstGui::removeWatchpointWithoutSessionDoesNotError()
{
    MainWindow window;
    window.show();
    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);
    QString error;
    QVERIFY(window.addWatchpointAddress(QStringLiteral("$1234.w"), &error));

    QSignalSpy errors(host, &EmulatorHost::errorOccurred);
    // No session running: removing a watchpoint must not push debugger commands
    // (armBreakpoints used to run unconditionally, and each command emits "No
    // emulator session is running." — a false error on a plain edit).
    QMetaObject::invokeMethod(&window, "removeWatchpoint", Qt::DirectConnection, Q_ARG(int, 0));
    QCOMPARE(errors.count(), 0);
}

void TstGui::stateSummaryClearsAfterSessionEnds()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/state.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tmoveq\t#1,d0\nloop:\tbra.s\tloop\n\teven\n\tend\n");
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
    EmulatorStopper stopper(window);
    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);
    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(host->isRunning(), 30000);

    // At the entry stop the machine state is cached; after the session ends
    // the remote `state` command must not answer with a dead session's
    // registers.
    QTRY_VERIFY_WITH_TIMEOUT(!window.stateSummary().startsWith(
                                 QStringLiteral("no session state")),
                             30000);

    // The hardware transcript and the PC history are fetched at every stop, so
    // both hold this session's text by now.
    auto *hardware = window.findChild<HardwareView *>();
    auto *pcHistory = window.findChild<PcHistoryView *>();
    QVERIFY(hardware && pcHistory);
    auto *hardwareText = hardware->findChild<QPlainTextEdit *>();
    auto *historyText = pcHistory->findChild<QPlainTextEdit *>();
    QVERIFY(hardwareText && historyText);
    QTRY_VERIFY_WITH_TIMEOUT(!hardwareText->toPlainText().isEmpty(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(!historyText->toPlainText().isEmpty(), 30000);

    host->stop();
    QTRY_VERIFY_WITH_TIMEOUT(!host->isRunning(), 10000);
    QCOMPARE(window.stateSummary(), QStringLiteral("no session state\n"));

    // The PC bar belongs to the session that just ended too: it was painted
    // from the last stop's PC and nothing else clears it, so left on screen it
    // claims a line the machine is no longer at — and a wait for "the program
    // counter reached line N" matches it instantly (MIN-83).
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 0, 10000);

    // Both transcripts describe the session that just ended, and neither is
    // re-requested without one: a dead machine's hardware report would read as
    // the live state (NIT-5).
    QTRY_VERIFY_WITH_TIMEOUT(hardwareText->toPlainText().isEmpty(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(historyText->toPlainText().isEmpty(), 10000);
    auto *summary = hardware->findChild<QLabel *>(QStringLiteral("hardwareSummary"));
    QVERIFY(summary);
    QVERIFY2(summary->text().isEmpty() && summary->isHidden(),
             qPrintable(summary->text()));
}

// resetSessionState() is the single owner of everything that must not leak from
// one debug session into the next, and a step-out left waiting on its stack dump
// when the session dies is exactly that. Its flag used to survive the reset, so
// the next session's own entry-stop stack dump was consumed as the step-out's
// answer: a one-shot `b pc = $…` armed at the old stack's idea of a return
// address, and a resume — the new session running away from its entry stop with
// a breakpoint nobody asked for. The profile-save flag goes with it.
void TstGui::sessionResetDropsAnUnansweredStepOut()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/resetstate.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                 // line 1
              "start:\tmoveq\t#0,d0\n"   // line 2 — the entry stop
              "loop:\tbra.s\tloop\n"     // line 3
              "\tend\n");
    src.close();

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    // Two sessions in one window, so the transport is pinned rather than left
    // to "auto": launchEmulator() *replaces* its backend when the transport the
    // probe selects differs from the live one, and the host this test holds
    // would then be a dangling pointer. Pinning it also keeps the test on the
    // backend whose signals it asserts (profileSaveFinished is native-only); on
    // a machine whose hatari is the HRDB fork the launch is refused with that
    // reason rather than reporting a crash from freed memory.
    settings.debugBackend = QStringLiteral("native");
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    EmulatorStopper stopper(window);
    window.show();
    window.openPath(source);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(host && editor);

    // Session one: stop at entry, let that stop's own stack dump land, then leave
    // a step-out and a profile save unanswered by killing the session in the same
    // event-loop turn — neither response can ever arrive.
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);
    QTest::qWait(1000);
    QVERIFY(QMetaObject::invokeMethod(&window, "stepOut", Qt::DirectConnection));
    QVERIFY(QMetaObject::invokeMethod(&window, "profileStop", Qt::DirectConnection));
    QVERIFY2(!window.debugConsoleText().contains(QLatin1String("[step out] needs")),
             "stepOut() must take the stopped-session path (the flag under test)");
    QVERIFY2(!window.debugConsoleText().contains(QLatin1String("[profile] Needs a stopped")),
             "profileStop() must take the stopped-session path (the flag under test)");
    host->stop();
    QVERIFY(!host->isRunning());

    // Session two: the entry stop's stack dump must only feed the stack view.
    // The console accumulates per window, not per session, so the session-two
    // slice is what the text grew by while it ran. The editor's execution line
    // is still 2 from session one, so session two's entry cannot be observed by
    // line number — a stale 2 would make the wait pass while TOS is still
    // booting and the machine then reads as "resumed". Wait for the host's own
    // stop event instead.
    QSignalSpy results(&window, &MainWindow::profileResultsReady);
    const int consoleOffset = window.debugConsoleText().size();
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    // Session two's host, re-fetched rather than reused: a launch that changed
    // the transport replaced the backend, and the pointer captured above would
    // be dangling (see the transport pin above).
    host = window.findChild<EmulatorHost *>();
    QVERIFY(host);
    QSignalSpy entryStop(host, &IDebugBackend::stoppedChanged);
    QTRY_VERIFY_WITH_TIMEOUT(!entryStop.isEmpty() && entryStop.last().at(0).toBool(), 30000);
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);
    QTest::qWait(1500);  // long enough for a stale flag to have fired

    const QString sessionTwo = window.debugConsoleText().mid(consoleOffset);
    const bool staleStepOut = sessionTwo.contains(QLatin1String("[step out]"));
    const bool resumed = !host->isStopped();
    const int entryLine = editor->currentExecutionLine();
    // Observations captured; end the session before asserting so a failure
    // cannot leave a live emulator for teardown.
    host->stop();

    QVERIFY2(!staleStepOut,
             qPrintable(QStringLiteral("the stale step-out flag fired: %1")
                            .arg(sessionTwo)));
    QVERIFY2(!resumed,
             qPrintable(QStringLiteral(
                            "the new session was resumed from its entry stop; "
                            "session two console: %1")
                            .arg(sessionTwo)));
    QCOMPARE(entryLine, 2);

    // The old profile save must be gone too: a save completing now is this
    // session's business. The completion is delivered exactly as the backend
    // delivers it — what is under test is the flag's lifetime, not Hatari's
    // profile-save output.
    emit host->profileSaveFinished(
        QStringLiteral("%1/profile.txt").arg(QDir::tempPath()));
    QCOMPARE(results.count(), 0);
}

void TstGui::projectAssemblerPathOverridesDiscovery()
{
    const QString source = m_work->path() + QStringLiteral("/ovr.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\trts\n\teven\n\tend\n");
    src.close();
    // A private copy of the assembler: discovery cannot find it, so the
    // build using it proves the project override reached the build service.
    // The .exe suffix matters on Windows: executability there is the suffix,
    // so an extensionless copy is not runnable and the override is rejected.
    const QString copy = m_work->path() + QStringLiteral("/vasm-override.exe");
    QVERIFY(QFile::copy(m_vasm, copy));
    QVERIFY(QFile::setPermissions(copy, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                              | QFileDevice::ExeOwner));

    ProjectSettings settings;
    settings.sourceFile = source;
    settings.assemblerPath = copy;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error),
             qPrintable(error));

    MainWindow window;
    window.openPath(source);
    QCOMPARE(window.assemblerPath(), copy);
}

// The HRDB fork is an *additional* prerequisite on top of the emulator ones, and
// the suite's contract for a missing prerequisite is fail-loud: under
// PIST_REQUIRE_EMULATOR a missing fork fails rather than skips, the same as a
// missing assembler in initTestCase. (MIN-66: the workflow's comment used to
// describe HRDB as outside that contract and skipping instead; the tests are the
// contract of record.)
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
    EmulatorStopper stopper(window);
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
    EmulatorStopper stopper(window);
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
    view.applyDump(parseMemoryDump(dump));
    QCOMPARE(edits.size(), 0);

    // Editing enabled (the stopped state, when a user edit can be in flight):
    // the rewrite must still emit nothing — the bytes were not user edits.
    view.setEditingEnabled(true);

    view.applyDump(parseMemoryDump(dump));
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
    view.applyDump(parseMemoryDump(dump));

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
    view.applyDump(parseMemoryDump(QStringLiteral(
        "00012590: 70 ab 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  p.a.t.`.r.Nu..HI\n")));
    // A double-click on a cell whose 4-byte window looks like an address must
    // EDIT, not navigate — pointer-following is Alt+double-click. Use bytes
    // 00 01 25 96 (an address-like long) in row 1's first long position by
    // re-dumping a window that has them.
    QSignalSpy nav(&view, &MemoryView::dumpRequested);
    view.applyDump(parseMemoryDump(QStringLiteral(
        "00012590: 00 01 25 96 70 01 61 04 74 03 60 fe 72 02 4e 75  ....p.a.t.`.r.Nu\n")));
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
    view.applyDump(parseMemoryDump(QStringLiteral(
        "00012590: 00 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  ..a.t.`.r.Nu..HI\n")));

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
    view.applyDump(parseMemoryDump(QStringLiteral(
        "00012590: 00 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  ..a.t.`.r.Nu..HI\n")));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("70"));

    view.applyDump(parseMemoryDump(QStringLiteral(
        "00012590: 70 01 61 04 74 03 60 fe 72 02 4e 75 00 00 48 49  p.a.t.`.r.Nu..HI\n")));
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("70"));
}

void TstGui::memoryEditSurvivesRefreshLive()
{
    REQUIRE_EMULATOR_OR_SKIP();

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
    EmulatorStopper stopper(window);
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
                      parseMemoryDump(QStringLiteral(
                          "00001000: 00 01 25 96 00 00 00 43 00 00 00 00 00 00 00 00\n")),
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
    REQUIRE_EMULATOR_OR_SKIP();

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
    EmulatorStopper stopper(window);
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
    // When the session never starts, the console transcript is the only
    // evidence a headless CI runner leaves — dump it on the failure path.
    if (!QTest::qWaitFor([&] { return editor->currentExecutionLine() == 2; }, 30000))
        qDebug().noquote() << window.debugConsoleText();
    QCOMPARE(editor->currentExecutionLine(), 2);
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 3, 30000);

    host->stop();
}

// A breakpoint added while the program is running must still fire in that
// session. The edit arrives with the machine running — the debugger is at a
// prompt only on a stop — and the commands armBreakpoints() queues then wait in
// the host, which holds stdin writes until the debugger is back, and are
// dispatched at the next stop. The gate used to require isStopped(), so the edit
// was dropped for the rest of the session: the panel and the gutter listed the
// breakpoint, the emulator never heard of it, and the symptom was a breakpoint
// that never fired — while a watchpoint added the same way was armed.
void TstGui::breakpointAddedWhileRunningFiresThisSession()
{
    REQUIRE_EMULATOR_OR_SKIP();

    // Line 6 stops the first pass (armed before Run, the ordinary way) and line
    // 7 is the breakpoint added mid-run. The countdown on 3-5 holds the machine
    // running while the edit is made, so it lands in a run rather than against
    // the stop that ends the countdown.
    const QString source = m_work->path() + QStringLiteral("/midrun.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                        // line 1
              "start:\tmoveq\t#0,d0\n"          // line 2  <- the entry stop
              "delay:\tmove.l\t#$00200000,d1\n" // line 3
              "wait:\tsubq.l\t#1,d1\n"          // line 4
              "\tbne.s\twait\n"                 // line 5
              "mark:\tnop\n"                    // line 6  <- armed before Run
              "spin:\taddq.w\t#1,d0\n"          // line 7  <- added while running
              "\tbra.s\tspin\n"                 // line 8
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
    EmulatorStopper stopper(window);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY2(host, "MainWindow must own an EmulatorHost");
    QVERIFY2(editor, "MainWindow must own a CodeEditor");
    window.openPath(source);

    QVERIFY(QMetaObject::invokeMethod(&window, "toggleBreakpointAtLine", Qt::DirectConnection,
                                      Q_ARG(int, 6)));
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 2, 30000);

    // Resume and let the machine actually start running before editing it.
    // resume() writes the continue once the entry attach's commands have
    // drained, so isStopped() is still true in the frame that called it.
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(!host->isStopped(), 10000);
    // The countdown is the whole runway for the edit: if it has already ended,
    // the stop would be processed next and the edit would take the stopped path
    // instead — the window under test missed, so say so rather than assert
    // something weaker.
    QVERIFY2(!host->isStopped(), "the countdown ended before the edit — the machine was stopped");
    QVERIFY(QMetaObject::invokeMethod(&window, "toggleBreakpointAtLine", Qt::DirectConnection,
                                      Q_ARG(int, 7)));

    // The countdown ends at the pre-armed line 6, and that stop is where the
    // queued re-arm is dispatched. Resuming from it must reach line 7: with the
    // edit dropped the program spins on 7-8 and never stops again.
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 6, 30000);
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 7, 20000);
}

// Arming has one gate: source breakpoints are planned only once the live bases
// have arrived, and only then is the session marked armed. An edit that reaches
// the pre-base window — emulator up, booting TOS to the entry breakpoint, no
// `info basepage` answer yet — used to mark the session armed against an
// unresolved program map, which produces no `b` command at all and, because
// onStateUpdated() arms only while that flag is clear, left the whole run with
// no source breakpoint armed and no second chance to arm one. The remote-control
// `run` verb replies at process start, so an agent's immediate `watchpoint`
// lands exactly there.
void TstGui::preBaseWatchpointEditStillArmsBreakpoints()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/prebase.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                     // line 1
              "start:\tmoveq\t#0,d0\n"       // line 2  <- entry stop
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
    EmulatorStopper stopper(window);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY2(host, "MainWindow must own an EmulatorHost");
    QVERIFY2(editor, "MainWindow must own a CodeEditor");

    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "toggleBreakpointAtLine", Qt::DirectConnection,
                                      Q_ARG(int, 3)));

    // Watch the process come up instead of polling for it: from that instant to
    // the entry stop is the TOS boot, and that gap is the window under test.
    QSignalSpy processStarted(host, &EmulatorHost::runningChanged);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    if (!host->isRunning())
        QVERIFY2(processStarted.wait(30000), "the emulator never started");

    // The pre-base window: process up, no state read yet, so no bases. A run
    // that raced past the entry stop here would not be testing this path at all,
    // so say so rather than quietly asserting nothing.
    QVERIFY2(editor->currentExecutionLine() == 0,
             "the entry stop already landed — the pre-base window was missed");

    // The edit that used to poison the session: a watchpoint added while the
    // debugger is still on its way to the entry breakpoint.
    QVERIFY2(window.addWatchpointAddress(QStringLiteral("$1234.w"), &error), qPrintable(error));

    // The entry stop still completes, and the source breakpoint armed with it:
    // resuming must stop on line 3 rather than run the loop forever.
    if (!QTest::qWaitFor([&] { return editor->currentExecutionLine() == 2; }, 30000))
        qDebug().noquote() << window.debugConsoleText();
    QCOMPARE(editor->currentExecutionLine(), 2);
    QVERIFY(QMetaObject::invokeMethod(&window, "resume", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 3, 20000);

    host->stop();
}

// Step out and run to cursor are built from one-shot breakpoints over the
// existing arm/resume path, so the observable contract is where the editor
// lands: run-to-cursor stops on the cursor's line, step-out stops on the
// line after the call that entered the subroutine.
void TstGui::stepOutAndRunToCursorReachTheirTargets()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/steps.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                    // line 1
              "start:\tbsr\tsub\n"          // line 2
              "spin:\tbra.s\tspin\n"        // line 3  <- step-out lands here
              "sub:\tmoveq\t#1,d0\n"        // line 4  <- run-to-cursor lands here
              "\trts\n"                     // line 5
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
    EmulatorStopper stopper(window);
    auto *host = window.findChild<EmulatorHost *>();
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(host);
    QVERIFY(editor);

    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    if (!QTest::qWaitFor([&] { return editor->currentExecutionLine() == 2; }, 30000))
        qDebug().noquote() << window.debugConsoleText();
    QCOMPARE(editor->currentExecutionLine(), 2);

    // Run to the cursor on line 4, inside the subroutine: the bsr at the
    // entry line executes, and the one-shot breakpoint traps at `sub`.
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(editor->document()->findBlockByNumber(3).position());
    editor->setTextCursor(cursor);
    QVERIFY(QMetaObject::invokeMethod(&window, "runToCursor", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 4, 30000);

    // Stopped inside `sub`, the top of the stack is the return address —
    // step out must land on line 3, the instruction after the bsr.
    QVERIFY(QMetaObject::invokeMethod(&window, "stepOut", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(editor->currentExecutionLine(), 3, 30000);


    host->stop();
}
// F4/Shift+F4 tour the Problems pane without the mouse: each step selects the
// next diagnostic carrying a source line (wrapping, skipping build-level
// messages) and the editor follows, exactly like double-clicking the item. The
// rows are the build's own diagnostics — the pane's rows are views of the
// structured entries the build produced, so the fixture goes through the build
// rather than through hand-made tree items, which no longer navigate.
void TstGui::diagnosticKeyboardFlowToursProblems()
{
    const QString source = m_work->path() + QStringLiteral("/diag.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\n"                        // line 1
              "start:\tmoveq\t#1,d0\n"          // line 2
              "\tbogus_op\t#1,d1\n"             // line 3  <- error
              "\tmoveq\t#3,d2\n"                // line 4
              "\tbogus2\n"                      // line 5  <- error
              "\trts\n"                         // line 6
              "\tend\n");                       // line 7
    src.close();

    MainWindow window;
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("problemsDock"));
    QVERIFY(dock);
    auto *problems = dock->findChild<QTreeWidget *>();
    QVERIFY(problems);
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    window.openPath(source);

    // Two assembly errors, so the tour has two rows with a source line to land
    // on. A quiet build: the assembler's failure is reported in the pane.
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(problems->topLevelItemCount(), 2, 30000);

    // And a build-level message: no file, no line (the shape the link-map note
    // and an unlocated fatal both take). The service is driven directly because
    // the window's own build path clears the pane first — this is the mix the
    // tour has to walk.
    auto *build = window.findChild<BuildService *>();
    QVERIFY(build);
    build->setSourceFile(QString());
    build->build();
    QTRY_COMPARE_WITH_TIMEOUT(problems->topLevelItemCount(), 3, 30000);

    auto editorLine = [&] { return editor->textCursor().blockNumber() + 1; };

    QVERIFY(QMetaObject::invokeMethod(&window, "nextDiagnostic", Qt::DirectConnection));
    QCOMPARE(problems->currentIndex().row(), 0);
    QCOMPARE(editorLine(), 3);

    QVERIFY(QMetaObject::invokeMethod(&window, "nextDiagnostic", Qt::DirectConnection));
    QCOMPARE(problems->currentIndex().row(), 1);
    QCOMPARE(editorLine(), 5);

    QVERIFY(QMetaObject::invokeMethod(&window, "nextDiagnostic", Qt::DirectConnection));
    QCOMPARE(problems->currentIndex().row(), 0);  // the line-less row was skipped, wrapped

    QVERIFY(QMetaObject::invokeMethod(&window, "previousDiagnostic", Qt::DirectConnection));
    QCOMPARE(problems->currentIndex().row(), 1);  // backwards wraps too
    QCOMPARE(editorLine(), 5);
}

// A breakpoint, a problem row and F4 used to do nothing when the line lived
// in a file that was not the current editor.
void TstGui::navigationOpensTheOtherFile()
{
    const QString dir = m_work->path() + QStringLiteral("/nav");
    QVERIFY(QDir().mkpath(dir));
    const QString a = dir + QStringLiteral("/a.s");
    const QString b = dir + QStringLiteral("/b.s");
    {
        QFile f(a);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\ttext\nstart:\tnop\n\trts\n\tend\n");
    }
    {
        QFile f(b);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        // Line 4 is the unknown mnemonic the problem row below comes from.
        f.write("\ttext\n\tnop\nhelper:\tnop\n\tbogus_op\n\trts\n\tend\n");
    }

    MainWindow window;
    window.openPath(a);
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);

    QVERIFY(QMetaObject::invokeMethod(&window, "goToBreakpoint", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("b.s")), Q_ARG(int, 3)));
    auto *current = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(current);
    QCOMPARE(QFileInfo(current->filePath()).fileName(), QStringLiteral("b.s"));
    QCOMPARE(current->textCursor().blockNumber() + 1, 3);

    // The problem row for the last step: b.s is the current document here, so a
    // quiet build fills the pane with its own diagnostic (b.s line 4).
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    auto *problems = window.findChild<QDockWidget *>(QStringLiteral("problemsDock"))
                         ->findChild<QTreeWidget *>();
    QVERIFY(problems);
    QTRY_COMPARE_WITH_TIMEOUT(problems->topLevelItemCount(), 1, 30000);

    QVERIFY(QMetaObject::invokeMethod(&window, "goToBreakpoint", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("a.s")), Q_ARG(int, 2)));
    current = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(current);
    QCOMPARE(QFileInfo(current->filePath()).fileName(), QStringLiteral("a.s"));
    QCOMPARE(current->textCursor().blockNumber() + 1, 2);
    QCOMPARE(tabs->count(), 2);

    QVERIFY(QMetaObject::invokeMethod(&window, "goToBreakpoint", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("missing.s")), Q_ARG(int, 1)));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("missing.s")));
    current = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QCOMPARE(QFileInfo(current->filePath()).fileName(), QStringLiteral("a.s"));

    QVERIFY(QMetaObject::invokeMethod(&window, "nextDiagnostic", Qt::DirectConnection));
    current = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QCOMPARE(QFileInfo(current->filePath()).fileName(), QStringLiteral("b.s"));
    QCOMPARE(current->textCursor().blockNumber() + 1, 4);
}

void TstGui::fileMenuAndToolbarMatchTheJobs()
{
    MainWindow window;
    auto *file = window.findChild<QMenu *>(QStringLiteral("fileMenu"));
    QVERIFY(file);
    QVERIFY(!file->actions().isEmpty());
    QCOMPARE(file->actions().first()->objectName(), QStringLiteral("newFileAction"));

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    const int before = tabs->count();
    file->actions().first()->trigger();
    QCOMPARE(tabs->count(), before + 1);
    auto *editor = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(editor);
    QVERIFY(editor->filePath().isEmpty());

    auto *exportMenu = window.findChild<QMenu *>(QStringLiteral("exportMenu"));
    QVERIFY2(exportMenu, "File must gather the image exports under Export");
    bool sawExportImage = false;
    for (QAction *action : exportMenu->actions()) {
        if (action->text().contains(QLatin1String("Export Image"))) {
            sawExportImage = true;
            QVERIFY2(!action->isEnabled(), "an export stays disabled on a source tab");
        }
    }
    QVERIFY(sawExportImage);
    for (QAction *action : file->actions())
        QVERIFY(!action->text().contains(QLatin1String("Export Image")));

    QAction *stepOut = nullptr;
    QAction *stepOver = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->shortcut() == QKeySequence(Qt::SHIFT | Qt::Key_F11))
            stepOut = action;
        if (action->shortcut() == QKeySequence(Qt::Key_F11))
            stepOver = action;
    }
    QVERIFY(stepOut && stepOver);
    QVERIFY2(!stepOut->icon().isNull(), "Step Out needs a toolbar glyph");
    QVERIFY2(stepOver->statusTip().contains(QLatin1String("F11")),
             qPrintable(stepOver->statusTip()));
    QVERIFY(stepOut->statusTip().contains(QLatin1String("F11")));

    SettingsDialog dialog{ProjectSettings()};
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Settings"));
}

// File ▸ Open Recent lists the persisted MRU sources (skipping files that no
// longer exist) and opening an entry loads it — the table-stakes follow-up to
// reopen-last-on-start.
void TstGui::openRecentMenuListsAndOpensFiles()
{
    const QString one = m_work->path() + QStringLiteral("/one.s");
    const QString two = m_work->path() + QStringLiteral("/two.s");
    for (const QString &p : {one, two}) {
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
        f.write("\ttext\nstart:\trts\n\tend\n");
    }
    const QString gone = m_work->path() + QStringLiteral("/deleted.s");
    QSettings().setValue(QStringLiteral("last/recentSources"),
                         QStringList{two, gone, one});

    MainWindow window;
    auto *menu = window.findChild<QMenu *>(QStringLiteral("openRecentMenu"));
    QVERIFY(menu);

    menu->popup(QPoint());
    QStringList titles;
    for (QAction *a : menu->actions())
        titles << a->text();
    menu->close();

    QCOMPARE(titles.size(), 2);  // the deleted file was pruned
    QVERIFY(titles.at(0).contains(QStringLiteral("two.s")));  // MRU order kept
    QVERIFY(titles.at(1).contains(QStringLiteral("one.s")));

    menu->popup(QPoint());
    menu->actions().at(1)->trigger();
    menu->close();

    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    QTRY_COMPARE(editor->filePath(), one);
}

// The instruction reference dock follows the word under the cursor while it
// is visible: an instruction word selects its entry, a label leaves the panel
// alone.
void TstGui::instructionReferenceFollowsTheCursor()
{
    const QString source = m_work->path() + QStringLiteral("/instr.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tmoveq\t#1,d0\n\taddq.w\t#1,d0\n\trts\n\tend\n");
    src.close();

    MainWindow window;
    window.openPath(source);
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("instructionRefDock"));
    QVERIFY(dock);
    auto *view = dock->findChild<pist::InstructionRefView *>();
    QVERIFY(view);
    dock->show();  // the follow-cursor path is gated on visibility

    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);

    window.show();  // follow-cursor is gated on the dock being visible
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(editor->document()->findBlockByNumber(2).position() + 2);
    editor->setTextCursor(cursor);
    QTRY_COMPARE(view->currentMnemonic(), QStringLiteral("ADDQ"));

    cursor.setPosition(editor->document()->findBlockByNumber(1).position());
    editor->setTextCursor(cursor);
    // The panel is left alone here, so there is nothing to wait *for*: flush the
    // event loop so a queued follow-cursor update would still land before the
    // assertion, instead of sleeping a guessed 50 ms first.
    QCoreApplication::processEvents();
    QCOMPARE(view->currentMnemonic(), QStringLiteral("ADDQ"));  // a label: no-op
}


// The same dock resolves OS calls: cursor on a trap line (or a push feeding
// it) shows the call being made, with the actual arguments in the detail.
void TstGui::osCallReferenceFollowsTheCursor()
{
    const QString source = m_work->path() + QStringLiteral("/oscall.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tmove.l\t#msg,-(a7)\n\tmove.w\t#9,-(a7)\n\ttrap\t#1\n"
              "\taddq.l\t#6,a7\n\tend\n");
    src.close();

    MainWindow window;
    window.openPath(source);
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("instructionRefDock"));
    QVERIFY(dock);
    auto *view = dock->findChild<pist::InstructionRefView *>();
    QVERIFY(view);
    dock->show();

    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    auto *detail = dock->findChild<QLabel *>(QStringLiteral("instructionRefDetail"));
    QVERIFY(detail);

    window.show();

    // On the trap line: the call resolves, not the generic TRAP entry, and
    // the detail names what the call is being made with.
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(editor->document()->findBlockByNumber(3).position() + 2);
    editor->setTextCursor(cursor);
    QTRY_COMPARE(view->currentMnemonic(), QStringLiteral("gemdos:9"));
    QVERIFY(detail->text().contains(QStringLiteral("Cconws(const char *buf)")));
    QVERIFY(detail->text().contains(QStringLiteral("#msg")));

    // On the function-number push: the same call, even though the word under
    // the cursor is a real instruction (MOVE).
    cursor.setPosition(editor->document()->findBlockByNumber(2).position() + 2);
    editor->setTextCursor(cursor);
    QTRY_COMPARE(view->currentMnemonic(), QStringLiteral("gemdos:9"));

    // Back on an ordinary instruction line, the word wins again.
    cursor.setPosition(editor->document()->findBlockByNumber(4).position() + 2);
    editor->setTextCursor(cursor);
    QTRY_COMPARE(view->currentMnemonic(), QStringLiteral("ADDQ"));

    // The argument push shares its line with the label: it still resolves the
    // call — that is the "explain this line" case the feature exists for.
    cursor.setPosition(editor->document()->findBlockByNumber(1).position());
    editor->setTextCursor(cursor);
    QTRY_COMPARE(view->currentMnemonic(), QStringLiteral("gemdos:9"));

    // A directive line resolves nothing and leaves the panel alone. As above:
    // flush rather than sleep, since nothing new is expected.
    cursor.setPosition(editor->document()->findBlockByNumber(0).position());
    editor->setTextCursor(cursor);
    QCoreApplication::processEvents();
    QCOMPARE(view->currentMnemonic(), QStringLiteral("gemdos:9"));
}

// The dock's "Insert binding" button drops the selected call's assembler
// binding into the source, above the cursor's line.
void TstGui::osCallBindingInsertsAtTheCursor()
{
    const QString source = m_work->path() + QStringLiteral("/binding.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\tnop\n\tend\n");
    src.close();

    MainWindow window;
    window.openPath(source);
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("instructionRefDock"));
    QVERIFY(dock);
    auto *view = dock->findChild<pist::InstructionRefView *>();
    QVERIFY(view);
    auto *button = dock->findChild<QPushButton *>(QStringLiteral("instructionRefInsert"));
    QVERIFY(button);
    dock->show();

    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    window.show();

    // Place the cursor first, then pick the call: cursor-follow would
    // otherwise replace the dock's selection with the word under the cursor.
    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(editor->document()->findBlockByNumber(1).position() + 2);
    editor->setTextCursor(cursor);
    QVERIFY(!button->isEnabled()); // an instruction row has no binding

    view->showOsCall(1, 9);
    QTRY_VERIFY(button->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);

    QCOMPARE(editor->document()->findBlockByNumber(1).text(), QStringLiteral("\tpea\tbuf"));
    QCOMPARE(editor->document()->findBlockByNumber(2).text(),
             QStringLiteral("\tmove.w\t#9,-(sp)\t; GEMDOS Cconws"));
    QCOMPARE(editor->document()->findBlockByNumber(3).text(), QStringLiteral("\ttrap\t#1"));
    QCOMPARE(editor->document()->findBlockByNumber(4).text(), QStringLiteral("\taddq.l\t#6,sp"));
    QCOMPARE(editor->document()->findBlockByNumber(5).text(), QStringLiteral("start:\tnop"));
    QVERIFY(editor->document()->isModified());
}

// Ctrl+click on an include opens the target file (resolved via the current
// directory and the project's include paths); Ctrl+click on a symbol jumps to
// its definition in the same document.
void TstGui::ctrlClickOpensIncludesAndJumpsToLabels()
{
    const QString lib = m_work->path() + QStringLiteral("/lib.s");
    QFile lf(lib);
    QVERIFY(lf.open(QIODevice::WriteOnly | QIODevice::Text));
    lf.write("\ttext\nhelper:\tmoveq\t#3,d0\n\trts\n\tend\n");
    lf.close();

    const QString main = m_work->path() + QStringLiteral("/main.s");
    QFile mf(main);
    QVERIFY(mf.open(QIODevice::WriteOnly | QIODevice::Text));
    mf.write("\ttext\n"                      // line 1
             "\tinclude\t\"lib.s\"\n"        // line 2
             "start:\tmoveq\t#1,d0\n"        // line 3
             "\tbra.s\tdone\n"               // line 4
             "\tmoveq\t#2,d1\n"              // line 5
             "done:\trts\n"                  // line 6
             "\tend\n");
    mf.close();

    MainWindow window;
    window.show();
    window.openPath(main);
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);

    auto ctrlClickLine = [&](int line, int col) {
        QTextCursor c = editor->textCursor();
        c.setPosition(editor->document()->findBlockByNumber(line - 1).position() + col);
        const QPoint pos = editor->cursorRect(c).center();
        QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::ControlModifier, pos);
    };

    // On the include line: lib.s opens (in a new editor tab, so the window —
    // not the first editor pointer — is asked for it).
    ctrlClickLine(2, 14);
    auto haveEditorFor = [&] (const QString &path) {
        for (CodeEditor *e : window.findChildren<CodeEditor *>())
            if (e->filePath() == path)
                return true;
        return false;
    };
    QTRY_VERIFY(haveEditorFor(lib));

    // Back in main.s: Ctrl+click the `done` operand jumps to its definition.
    window.openPath(main);
    QTRY_VERIFY(editor->isVisible());

    ctrlClickLine(4, 8);
    QCOMPARE(editor->textCursor().blockNumber(), 5);  // line 6, 0-based 5

    // Ctrl+click on an instruction (no such label) does not move the cursor.
    ctrlClickLine(3, 3);  // on `moveq`
    QCOMPARE(editor->textCursor().blockNumber(), 2);
}
// The debugger console recalls its session history with the arrow keys:
// newest-first from Up, repeats collapse, and Down past the newest restores
// the half-typed line.
void TstGui::debugConsoleRecallsHistoryWithArrowKeys()
{
    MainWindow window;
    window.show();
    auto *input = window.findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    input->setEnabled(true);  // the shell enables it when a session starts
    QVERIFY(input);

    QTest::keyClicks(input, QStringLiteral("r"));
    QTest::keyClick(input, Qt::Key_Return);   // no session: logged, but recorded
    QTest::keyClicks(input, QStringLiteral("d"));
    QTest::keyClick(input, Qt::Key_Return);
    QTest::keyClicks(input, QStringLiteral("d"));  // a repeat: collapses
    QTest::keyClick(input, Qt::Key_Return);

    QTest::keyClicks(input, QStringLiteral("m $1"));
    QTest::keyClick(input, Qt::Key_Up);
    QCOMPARE(input->text(), QStringLiteral("d"));
    QTest::keyClick(input, Qt::Key_Up);
    QCOMPARE(input->text(), QStringLiteral("r"));
    QTest::keyClick(input, Qt::Key_Up);             // clamped at the oldest
    QCOMPARE(input->text(), QStringLiteral("r"));
    QTest::keyClick(input, Qt::Key_Down);
    QCOMPARE(input->text(), QStringLiteral("d"));
    QTest::keyClick(input, Qt::Key_Down);
    QCOMPARE(input->text(), QStringLiteral("m $1"));  // the half-typed line back
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
    // A first run hides the debug docks, including Memory. The tab exists once
    // the pane is shown, and it starts in the bottom area.
    dock->show();
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

    // Factory layout tabs Git with Project files, and a tabbed dock has no
    // painted title bar. Split Git off so this test still presses that bar.
    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    auto *git = window.findChild<QDockWidget *>(QStringLiteral("gitDock"));
    QVERIFY(dock);
    if (git)
        window.addDockWidget(Qt::RightDockWidgetArea, git);
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
    memory->show();

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
    dock->show();
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
    for (const char *name : {"registersDock", "disassemblyDock", "stackDock"}) {
        auto *dock = window.findChild<QDockWidget *>(QLatin1String(name));
        QVERIFY(dock);
        dock->show();
    }
    QCoreApplication::processEvents();

    // isVisibleTo is false for a tab that is not the selected one. What matters
    // is a header the view itself has not hidden (MemoryView hides its own).
    const auto selfHidden = [](const QWidget *w) {
        if (!w->isHidden())
            return false;
        const QWidget *parent = w->parentWidget();
        return !parent || !parent->isHidden();
    };
    int checked = 0;
    for (QTableWidget *table : window.findChildren<QTableWidget *>()) {
        QHeaderView *h = table->horizontalHeader();
        if (selfHidden(h))
            continue;
        QWidget *owner = table;
        bool dockHidden = false;
        while (owner) {
            if (qobject_cast<QDockWidget *>(owner) && owner->isHidden())
                dockHidden = true;
            owner = owner->parentWidget();
        }
        if (dockHidden)
            continue;
        QCOMPARE(h->defaultAlignment(), Qt::AlignLeft | Qt::AlignVCenter);
        ++checked;
    }
    QVERIFY2(checked >= 3, "expected several visible table headers to check");
}

void TstGui::consoleCommandRoundTrips()
{
    REQUIRE_EMULATOR_OR_SKIP();

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
    EmulatorStopper stopper(window);
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

// Closing a dock is only reversible from the View menu. A dock omitted from
// that menu (the old hand-written list stopped at PC history) cannot be
// reopened without Reset layout, which also throws away every other
// arrangement. Every dock the window owns must be on the menu, including a
// memory pane created later.
void TstGui::viewMenuListsEveryDock()
{
    MainWindow window;

    QMenu *view = nullptr;
    for (QAction *menu : window.menuBar()->actions()) {
        if (menu->menu() && menu->text().remove(QLatin1Char('&')) == QLatin1String("View"))
            view = menu->menu();
    }
    QVERIFY2(view, "the View menu must exist");

    const QList<QDockWidget *> docks = window.findChildren<QDockWidget *>();
    QVERIFY2(docks.size() >= 13, "the window should have built its docks");

    QStringList missing;
    for (QDockWidget *dock : docks) {
        if (!view->actions().contains(dock->toggleViewAction()))
            missing << dock->objectName();
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("View menu omits: %1").arg(missing.join(QStringLiteral(", ")))));

    // The action on the menu is the dock's own toggle, so it actually shows
    // and hides the panel. The window has to be shown first: Qt only syncs a
    // toggle action's checked state from visibility once the dock is on
    // screen, and triggering an unsynced action does not close it.
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *breakpoints = window.findChild<QDockWidget *>(QStringLiteral("breakpointsDock"));
    QVERIFY(breakpoints);
    QAction *toggle = breakpoints->toggleViewAction();
    const bool shown = breakpoints->isVisibleTo(&window);
    toggle->trigger();
    QCOMPARE(breakpoints->isVisibleTo(&window), !shown);
    toggle->trigger();
    QCOMPARE(breakpoints->isVisibleTo(&window), shown);

    QMetaObject::invokeMethod(&window, "addMemoryPane", Qt::DirectConnection);
    auto *second = window.findChild<QDockWidget *>(QStringLiteral("memoryDock1"));
    QVERIFY2(second, "the second memory pane must exist as memoryDock1");
    QVERIFY2(view->actions().contains(second->toggleViewAction()),
             "a memory pane opened later must join the View menu");
    const bool secondShown = second->isVisibleTo(&window);
    second->toggleViewAction()->trigger();
    QCOMPARE(second->isVisibleTo(&window), !secondShown);
}

// A first run is the Editing arrangement: the debug docks stay hidden, the
// editor keeps the centre, and the bottom group stays a short strip. Showing
// the debug group still opens with Registers on top of Profiler. Reset layout
// returns to the hidden factory state.
void TstGui::factoryLayoutShowsRegistersAndTheEditor()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *registers = window.findChild<QDockWidget *>(QStringLiteral("registersDock"));
    auto *profiler = window.findChild<QDockWidget *>(QStringLiteral("profilerDock"));
    auto *problems = window.findChild<QDockWidget *>(QStringLiteral("problemsDock"));
    QVERIFY(registers && profiler && problems);
    QVERIFY(registers->isHidden());
    QVERIFY(profiler->isHidden());
    QVERIFY(!problems->isHidden());

    // 60% of the window, with room for frames and the splitter handles.
    QVERIFY2(window.centralWidget()->width() * 100 >= window.width() * 55,
             qPrintable(QStringLiteral("editor %1px in a %2px window")
                            .arg(window.centralWidget()->width())
                            .arg(window.width())));
    // A strip, not a second editor. The dock's own size hint lands near 110px
    // in an 860-tall window; half the window would be the failure.
    QVERIFY2(problems->height() >= 80 && problems->height() * 3 < window.height(),
             qPrintable(QStringLiteral("bottom group is %1px in a %2px window")
                            .arg(problems->height())
                            .arg(window.height())));

    // The debug docks are still one tab group. Raising Profiler is what a
    // first run used to do by accident; Reset must put them away again.
    registers->show();
    profiler->show();
    profiler->raise();
    QTabBar *debugTabs = nullptr;
    for (QTabBar *bar : window.findChildren<QTabBar *>()) {
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == QLatin1String("Profiler"))
                debugTabs = bar;
        }
    }
    QVERIFY2(debugTabs, "the debug docks must be tabbed together");
    QCOMPARE(debugTabs->tabText(debugTabs->currentIndex()), QStringLiteral("Profiler"));
    auto *reset = window.findChild<QAction *>(QStringLiteral("resetLayoutAction"));
    QVERIFY2(reset, "View menu must offer Reset layout");
    reset->trigger();
    QVERIFY(registers->isHidden());
    QVERIFY(profiler->isHidden());
    QVERIFY(!problems->isHidden());
}

void TstGui::windowGeometryPersistsAcrossRestart()
{
    int width = 0;
    int height = 0;
    {
        // Inside the offscreen screen (800×800). The plugin nudges a
        // requested size, so the contract is the size the window actually
        // took, and that it stayed near the request.
        MainWindow window;
        window.resize(760, 520);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.resize(760, 520);
        width = window.width();
        height = window.height();
        QVERIFY(window.close());
    }
    QVERIFY(qAbs(width - 760) < 80);
    QVERIFY(qAbs(height - 520) < 80);
    {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QCOMPARE(window.width(), width);
        QCOMPARE(window.height(), height);
    }
}

// A saved arrangement is the user's. The factory split must not put Registers
// back on top of a layout that closed it.
void TstGui::savedLayoutBeatsTheFactorySplit()
{
    {
        MainWindow window;
        auto *registers = window.findChild<QDockWidget *>(QStringLiteral("registersDock"));
        QVERIFY(registers);
        registers->setVisible(false);
        QVERIFY(window.close());
    }
    {
        MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *registers = window.findChild<QDockWidget *>(QStringLiteral("registersDock"));
        QVERIFY(registers);
        QVERIFY2(!registers->isVisibleTo(&window),
                 "a saved layout that hid Registers must stay hidden");
    }
}

// The status bar used to show the assembler file name and Hatari's capability
// probe. The probe is a tooltip now. What stays on the bar is the session, a
// build result that does not expire, and where the caret is.
void TstGui::statusBarShowsCaretAndBuild()
{
    MainWindow window;
    auto *session = window.findChild<QLabel *>(QStringLiteral("statusSession"));
    auto *build = window.findChild<QLabel *>(QStringLiteral("statusBuild"));
    auto *caret = window.findChild<QLabel *>(QStringLiteral("statusCaret"));
    QVERIFY2(session && build && caret, "the status bar must carry session, build and caret chips");

    QCOMPARE(session->text(), QStringLiteral("Not running"));
    QVERIFY2(!session->text().contains(QLatin1String("control socket")),
             qPrintable(session->text()));
    QVERIFY2(session->toolTip().contains(QLatin1String("Hatari")),
             qPrintable(session->toolTip()));

    QCOMPARE(caret->text(), QStringLiteral("untitled:1:1"));
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    editor->setPlainText(QStringLiteral("\ttext\nstart:\tnop\n"));
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::Down);
    cursor.movePosition(QTextCursor::Right);
    editor->setTextCursor(cursor);
    QCOMPARE(caret->text(), QStringLiteral("untitled:2:2"));

    const QString source = m_work->path() + QStringLiteral("/status.s");
    {
        QFile src(source);
        QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
        src.write("\ttext\nstart:\tmoveq\t#1,d0\n\trts\n\teven\n\tend\n");
    }
    window.openPath(source);
    QSignalSpy completed(&window, &MainWindow::buildCompleted);
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 30000);
    QCOMPARE(completed.first().first().toBool(), true);
    QCOMPARE(build->text(), QStringLiteral("Build ok"));

    // Already-open files are not reloaded, so the failure has to be the
    // buffer the build will save. The pristine tab is still around, and
    // findChild would hand that one back.
    CodeEditor *sourceEditor = nullptr;
    for (CodeEditor *candidate : window.findChildren<CodeEditor *>()) {
        if (candidate->filePath() == source)
            sourceEditor = candidate;
    }
    QVERIFY(sourceEditor);
    sourceEditor->setPlainText(QStringLiteral("\ttext\nthis is not assembly\n"));
    // setPlainText loads text the way open does, and that leaves the document
    // clean. The build only saves a buffer it believes the user changed.
    sourceEditor->document()->setModified(true);
    QVERIFY(sourceEditor->isModifiedSinceLoad());
    QSignalSpy failed(&window, &MainWindow::buildCompleted);
    QVERIFY(QMetaObject::invokeMethod(&window, "build", Qt::DirectConnection));
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 30000);
    QCOMPARE(failed.first().first().toBool(), false);
    QCOMPARE(build->text(), QStringLiteral("Build failed"));
}

// A stop names the source line. The capability probe stays in the tooltip.
void TstGui::statusBarNamesTheStop()
{
    REQUIRE_EMULATOR_OR_SKIP();

    const QString source = m_work->path() + QStringLiteral("/statusrun.s");
    {
        QFile src(source);
        QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
        src.write("\ttext\n"
                  "start:\tmoveq\t#1,d0\n"
                  "loop:\tbra.s\tloop\n"
                  "\teven\n"
                  "\tend\n");
    }
    ProjectSettings settings;
    settings.sourceFile = source;
    settings.machine = Machine::St;
    settings.monitor = QStringLiteral("mono");
    settings.memSizeMiB = 1;
    QString error;
    QVERIFY2(settings::save(settings, settings::projectFileFor(source), &error), qPrintable(error));

    MainWindow window;
    EmulatorStopper stopper(window);
    auto *session = window.findChild<QLabel *>(QStringLiteral("statusSession"));
    auto *caret = window.findChild<QLabel *>(QStringLiteral("statusCaret"));
    QVERIFY(session && caret);
    auto *host = window.findChild<EmulatorHost *>();
    QVERIFY(host);

    window.openPath(source);
    QVERIFY(QMetaObject::invokeMethod(&window, "run", Qt::DirectConnection));
    QTRY_VERIFY_WITH_TIMEOUT(host->isStopped(), 30000);
    QTRY_VERIFY_WITH_TIMEOUT(
        session->text().startsWith(QStringLiteral("Stopped — statusrun.s:")), 5000);
    QVERIFY2(session->toolTip().contains(QLatin1String("Hatari")), qPrintable(session->toolTip()));

    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);
    const int execution = editor->currentExecutionLine();
    QVERIFY(execution > 0);
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    if (editor->textCursor().blockNumber() + 1 != execution) {
        QVERIFY2(caret->text().contains(QLatin1String("caret "))
                     && caret->text().contains(QLatin1String("PC ")),
                 qPrintable(caret->text()));
    }

    auto *strip = window.findChild<QLabel *>(QStringLiteral("registerStrip"));
    QVERIFY(strip);
    QTRY_VERIFY_WITH_TIMEOUT(!strip->isHidden() && strip->text().contains(QLatin1String("D0")),
                             5000);

    host->stop();
    QTRY_COMPARE_WITH_TIMEOUT(session->text(), QStringLiteral("Not running"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(strip->isHidden(), 5000);
}

void TstGui::registerDockShowsFlagsUntilTheMachineStops()
{
    MainWindow window;
    auto *placeholder = window.findChild<QLabel *>(QStringLiteral("registersPlaceholder"));
    QVERIFY(placeholder);
    QVERIFY(placeholder->text().contains(QStringLiteral("machine stops")));
    QVERIFY(!placeholder->isHidden());
    auto *strip = window.findChild<QLabel *>(QStringLiteral("registerStrip"));
    QVERIFY(strip && strip->isHidden());

    auto *view = window.findChild<RegistersView *>();
    QVERIFY(view);
    MachineState state;
    state.regs.valid = true;
    state.regs.sr = 0x2704;
    state.regs.flagN = true;
    state.pc = 0x00012396;
    view->setState(state);

    QVERIFY(placeholder->isHidden());
    auto *n = window.findChild<QLabel *>(QStringLiteral("flagChipN"));
    auto *z = window.findChild<QLabel *>(QStringLiteral("flagChipZ"));
    QVERIFY(n && z);
    // A lit flag chip is filled and a clear one is not. The exact green is a
    // cosmetic choice, so it is not pinned to the stylesheet's source text; the
    // structure is what the user sees.
    QVERIFY2(n->styleSheet() != z->styleSheet(),
             "a set flag chip must be styled differently from a clear one");
    QVERIFY(n->styleSheet().contains(QLatin1String("background")));
    QVERIFY(z->styleSheet().contains(QLatin1String("transparent")));
    bool sawSr = false;
    for (QTableWidget *table : view->findChildren<QTableWidget *>()) {
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem *item = table->item(row, 1);
            if (item && item->text() == QLatin1String("2704"))
                sawSr = true;
        }
    }
    QVERIFY2(sawSr, "the SR cell keeps the status word beside the flag chips");
    QVERIFY(strip->isHidden());
}

// View → Layout rearranges the docks and can put the previous arrangement
// back. The sprite editor's palette carries colour indexes, and frames are a
// horizontal filmstrip of thumbnails rather than a tall list.
void TstGui::layoutPresetsHideDocksAndRestore()
{
    QSettings settings;

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *registers = window.findChild<QDockWidget *>(QStringLiteral("registersDock"));
    auto *disassembly = window.findChild<QDockWidget *>(QStringLiteral("disassemblyDock"));
    auto *emulator = window.findChild<QDockWidget *>(QStringLiteral("emulatorDisplayDock"));
    auto *project = window.findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    QVERIFY(registers && disassembly && emulator && project);
    QVERIFY(registers->isHidden());

    auto *editing = window.findChild<QAction *>(QStringLiteral("layoutEditing"));
    auto *debugging = window.findChild<QAction *>(QStringLiteral("layoutDebugging"));
    auto *sprite = window.findChild<QAction *>(QStringLiteral("layoutSprite"));
    auto *restore = window.findChild<QAction *>(QStringLiteral("restoreMyLayoutAction"));
    QVERIFY(editing && debugging && sprite && restore);
    QVERIFY(!restore->isEnabled());

    // The factory state is already Editing, so show the docks first. Editing
    // then has something to hide, and Restore brings that shown state back.
    registers->show();
    disassembly->show();
    QVERIFY(!registers->isHidden());
    editing->trigger();
    QVERIFY(registers->isHidden());
    QVERIFY(disassembly->isHidden());
    QVERIFY(restore->isEnabled());

    restore->trigger();
    QVERIFY(!registers->isHidden());
    QVERIFY(!restore->isEnabled());

    debugging->trigger();
    QVERIFY(!registers->isHidden());
    QVERIFY(!disassembly->isHidden());
    QCOMPARE(window.dockWidgetArea(disassembly), Qt::RightDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(registers), Qt::BottomDockWidgetArea);
    QVERIFY(emulator->isHidden());

    sprite->trigger();
    QVERIFY(registers->isHidden());
    QVERIFY(project->isHidden());
    QVERIFY(emulator->isHidden());

    const QString dir = m_work->path() + QStringLiteral("/layoutsprite");
    QVERIFY(QDir().mkpath(dir));
    const QString pim = dir + QStringLiteral("/sprite.pim");
    QString error;
    QVERIFY2(ImageDocument::create(16, 16, PaletteKind::Ste).save(pim, &error),
             qPrintable(error));
    window.openPath(pim);
    QTRY_VERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("Sprite")));
    QVERIFY(settings.value(QStringLiteral("layout/offeredSprite")).toBool());

    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    auto *image = qobject_cast<ImageEditor *>(tabs->currentWidget());
    QVERIFY(image);
    auto *swatches = image->findChild<QWidget *>(QStringLiteral("imageSwatches"));
    QVERIFY(swatches);
    bool sawErase = false;
    bool sawNumber = false;
    for (QToolButton *button : swatches->findChildren<QToolButton *>()) {
        const int cube = button->property("cube").toInt();
        if (cube == kTransparent) {
            QCOMPARE(button->text(), QStringLiteral("0"));
            QCOMPARE(button->toolTip(), QStringLiteral("Erase"));
            sawErase = true;
        } else if (!button->text().isEmpty()) {
            QCOMPARE(button->text(), QString::number(cube));
            QVERIFY(button->toolTip().startsWith(QStringLiteral("Colour ")));
            QVERIFY(button->toolTip().contains(QStringLiteral("—")));
            sawNumber = true;
        }
    }
    QVERIFY(sawErase);
    QVERIFY(sawNumber);

    auto *frames = image->findChild<QListWidget *>(QStringLiteral("imageFrames"));
    QVERIFY(frames);
    QCOMPARE(frames->flow(), QListView::LeftToRight);
    QCOMPARE(frames->viewMode(), QListView::IconMode);
    QVERIFY(frames->count() >= 1);
    QVERIFY(!frames->item(0)->icon().isNull());

    auto *phases = image->findChild<QListWidget *>(QStringLiteral("imagePhases"));
    auto *picker = image->findChild<QComboBox *>(QStringLiteral("imagePhasePicker"));
    QVERIFY(phases && phases->isHidden());
    QVERIFY(picker && !picker->isHidden());
    QVERIFY(!picker->currentText().isEmpty());

    bool sawColour = false;
    for (QLabel *label : image->findChildren<QLabel *>()) {
        if (label->text().contains(QStringLiteral("colour ")))
            sawColour = true;
    }
    QVERIFY2(sawColour, "the canvas status names the active colour index");
}

// The Emulator panel is a container for a window that belongs to another
// process, and before any session — or on a platform that cannot embed at all —
// there is nothing in it. A bare black rectangle there reads as a broken
// emulator rather than as an empty panel, which is exactly how the Windows
// "docked emulator is a black screen" report presented itself, so the panel
// paints what it is waiting for.
void TstGui::emulatorPanelExplainsItselfWhenEmpty()
{
    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("emulatorDisplayDock"));
    QVERIFY(dock);
    dock->show();
    QWidget *display = dock->widget();
    QVERIFY(display);
    QVERIFY(QTest::qWaitFor([display] { return display->width() > 100; }, 2000));

    const QImage image = display->grab().toImage();
    int lit = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) > 16)
                ++lit;
        }
    }
    QVERIFY2(lit > 100, "an empty emulator panel paints its explanation, not a black void");
}

// Git is a project pane, not a debug one: it shares Project files' tab and
// stays underneath it, and Sprite puts it away with that pane.
void TstGui::gitDockStaysUnderProjectFiles()
{
    QSettings settings;
    settings.setValue(QStringLiteral("git/blame"), false);

    MainWindow window;
    window.resize(1000, 700);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *git = window.findChild<QDockWidget *>(QStringLiteral("gitDock"));
    auto *project = window.findChild<QDockWidget *>(QStringLiteral("projectFilesDock"));
    QVERIFY(git && project);
    QCOMPARE(window.dockWidgetArea(git), window.dockWidgetArea(project));

    auto *blame = window.findChild<QAction *>(QStringLiteral("gitBlameAction"));
    QVERIFY(blame && blame->isCheckable());
    QVERIFY(!blame->isChecked());

    auto sharesTab = [&window] {
        for (QTabBar *bar : window.findChildren<QTabBar *>()) {
            if (!bar->isVisible())
                continue;
            bool sawProject = false;
            bool sawGit = false;
            int projectAt = -1;
            int current = bar->currentIndex();
            for (int i = 0; i < bar->count(); ++i) {
                if (bar->tabText(i) == QLatin1String("Project files") && bar->isTabVisible(i)) {
                    sawProject = true;
                    projectAt = i;
                }
                if (bar->tabText(i) == QLatin1String("Git") && bar->isTabVisible(i))
                    sawGit = true;
            }
            if (sawProject && sawGit)
                return projectAt == current;
        }
        return false;
    };
    QVERIFY2(sharesTab(), "Git must be tabbed under Project files");

    auto *debugging = window.findChild<QAction *>(QStringLiteral("layoutDebugging"));
    auto *sprite = window.findChild<QAction *>(QStringLiteral("layoutSprite"));
    auto *editing = window.findChild<QAction *>(QStringLiteral("layoutEditing"));
    QVERIFY(debugging && sprite && editing);

    debugging->trigger();
    QVERIFY2(sharesTab(), "Debugging leaves Git under Project files");

    sprite->trigger();
    QVERIFY(project->isHidden());
    QVERIFY(git->isHidden());
    // The tab bar drops a removed dock on the next layout pass.
    QTest::qWait(0);
    QStringList gitTabs;
    for (QTabBar *bar : window.findChildren<QTabBar *>()) {
        if (!bar->isVisible())
            continue;
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == QLatin1String("Git") && bar->isTabVisible(i))
                gitTabs << QStringLiteral("%1 (bar %2)").arg(bar->tabText(i)).arg(bar->isVisible());
        }
    }
    QVERIFY2(gitTabs.isEmpty(), qPrintable(gitTabs.join(QStringLiteral(", "))));

    editing->trigger();
    QVERIFY2(sharesTab(), "Editing puts Git back under Project files");
}

// The git panel follows the project the user opened. It used to be aimed at
// whatever the file browser's model root happened to be, which is the process
// working directory until a file is opened — so a window with no project ran
// `git status` inside whichever repository contained the launch directory. This
// suite runs from a build tree inside PiST's own checkout, which is why ~90
// window constructions per run aimed git at the real `.git`, and a status child
// abandoned at teardown leaves `.git/index.lock` there (MIN-81).
//
// Measured with a fake `git` first on PATH that records the working directory of
// every invocation it is handed. Both directions are deterministic without a
// sleep: with the fix the log stays empty (nothing is run at all), and the
// awaited condition is checked immediately, so the passing case costs one event
// loop turn rather than the timeout.
void TstGui::gitDiscoveryFollowsTheOpenedProject()
{
    const QString bin = m_work->path() + QStringLiteral("/fakebin");
    QVERIFY(QDir().mkpath(bin));
    const QString log = bin + QStringLiteral("/invocations.log");
#ifdef Q_OS_WIN
    const QString git = bin + QStringLiteral("/git.bat");
    // cmd.exe's recording of the same `<cwd>|<args>` line the shell fake
    // writes: `^|` survives `echo` as a literal pipe — an unescaped `|` would
    // split the command — and the redirection sits in front of `echo` so cmd
    // cannot echo a space before the arguments. What cmd appends is CRLF: the
    // CR trails the args, outside the field the checker splits on (the CI log
    // shows it as a trailing `?`), so field 0 stays exactly the working
    // directory for QFileInfo::canonicalFilePath() to resolve.
    QVERIFY(writeFile(git, QByteArrayLiteral("@echo off\r\n>>\"") + QFile::encodeName(log)
                               + QByteArrayLiteral("\" echo %CD%^|%*\r\n")));
#else
    const QString git = bin + QStringLiteral("/git");
    // `pwd -P`, not $PWD: the shell variable is inherited from the parent and
    // does not follow QProcess's chdir, so it would report PiST's directory
    // rather than the child's.
    QVERIFY(writeFile(git, QStringLiteral("#!/bin/sh\necho \"$(pwd -P)|$*\" >> %1\nexit 1\n")
                               .arg(log)
                               .toUtf8()));
    QVERIFY(QFile::setPermissions(git, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | QFileDevice::ExeOwner));
#endif

    // Every git child must run in a directory this test opened — the scratch
    // tree — and never in the directory the process happens to be started from.
    // Compared canonically, so a symlinked /tmp (macOS) matches either way.
    const QString scratch = QFileInfo(m_work->path()).canonicalFilePath();
    const auto gitStayedInTheScratchTree = [&]() {
        const QStringList lines =
            QString::fromUtf8(fileBytes(log)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QString workDir = line.section(QLatin1Char('|'), 0, 0);
            if (!QFileInfo(workDir).canonicalFilePath().startsWith(scratch))
                return false;
        }
        return true;
    };

    const EnvScope pathScope("PATH");
    const QString previous = qEnvironmentVariable("PATH");
    qputenv("PATH", QFile::encodeName(bin + QDir::listSeparator() + previous));

    {
        // Nothing opened: the window has no project directory, so there is
        // nothing for git to be pointed at.
        MainWindow window;
        QTRY_VERIFY_WITH_TIMEOUT(gitStayedInTheScratchTree(), 2000);
        QVERIFY2(fileBytes(log).isEmpty(),
                 qPrintable(QStringLiteral("a window with nothing open must run no git at all: %1")
                                .arg(QString::fromUtf8(fileBytes(log)))));
    }

    {
        // A document opened from the scratch tree: git is pointed at *that*
        // directory, which is the project, and the log records it there.
        QFile source(m_source);
        QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
        source.write("\tnop\n");
        source.close();

        MainWindow window;
        window.openPath(m_source);
        QTRY_VERIFY_WITH_TIMEOUT(!fileBytes(log).isEmpty(), 2000);
        QVERIFY2(!fileBytes(log).isEmpty(), "opening a project must run git discovery");
        QVERIFY2(gitStayedInTheScratchTree(),
                 qPrintable(QStringLiteral("git ran outside the project:\n%1")
                                .arg(QString::fromUtf8(fileBytes(log)))));
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

    QVERIFY2(joined.contains(QStringLiteral("--disk-a ")
                              + QDir::toNativeSeparators(QStringLiteral("/disks/boot.st"))),
             qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("--disk-b ")
                              + QDir::toNativeSeparators(QStringLiteral("/disks/data.st"))),
             qPrintable(joined));

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
    QVERIFY2(clashArgv.contains(QDir::toNativeSeparators(QStringLiteral("/disks/magazine.st"))),
             qPrintable(clashArgv.join(QLatin1Char(' '))));
    QVERIFY2(!clashArgv.contains(QDir::toNativeSeparators(QStringLiteral("/tmp/auto.st"))),
             qPrintable(clashArgv.join(QLatin1Char(' '))));

    SessionConfig spaced;
    spaced.hatariPath = QStringLiteral("hatari");
    spaced.programPath = QStringLiteral("/tmp/prog.prg");
    // Fabricated under the test's own temp tree: the shape that failed in the
    // wild (spaces and parentheses) is what this checks, and a hard-coded
    // personal path would make the fixture depend on whose machine runs it.
    spaced.floppyImages = {m_work->path()
                           + QStringLiteral(
                               "/ST Format Magazine Issue 08 (1990-03)(Future Publishing).st")};
    QVERIFY(spaced.toArgv().contains(QDir::toNativeSeparators(spaced.floppyImages.at(0))));
}

void TstGui::programPathUsesHostSeparators()
{
    // Hatari 2.6.1 Opt_HandleArgument splits the positional with
    // strrchr(path, PATHSEP). PATHSEP is '\\' on Windows, and a Qt path has
    // none, so the GEMDOS drive becomes the process working directory and the
    // autostart name is the whole "C:/proj/hello.prg" string.
    SessionConfig config;
    config.hatariPath = QStringLiteral("hatari");
    config.gemdosDir = QStringLiteral("C:/proj");
    config.programPath = QStringLiteral("C:/proj/hello.prg");
    config.tosPath = QStringLiteral("C:/roms/tos.img");
    config.bootstrapScriptPath = QStringLiteral("C:/session/boot.prg");

    const QStringList argv = config.toArgv();
    QCOMPARE(argv.last(), QDir::toNativeSeparators(config.programPath));
    QVERIFY(argv.contains(QDir::toNativeSeparators(config.gemdosDir)));
    QVERIFY(argv.contains(QDir::toNativeSeparators(config.tosPath)));
    QVERIFY(argv.contains(QDir::toNativeSeparators(config.bootstrapScriptPath)));
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

    // The first open seeds the browser with the file's directory, so a fresh
    // window still lands on the project being edited.
    window.openPath(src);

    auto *view = browser->findChild<QTreeView *>(QStringLiteral("hardDriveView"));
    QVERIFY(view);
    QVERIFY2(view->model(), "the browser must have a model");
    // The file system model populates in a thread, so the root and the
    // selection land when the directory read finishes.
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->data(view->rootIndex(), Qt::UserRole + 1).toString(),
                              dir, 5000);

    // The file being edited is the selected entry.
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(),
                              src, 5000);

    // A file inside the project — here one level down — is revealed by
    // expanding the tree to it; the root itself does not move.
    const QString sub = dir + QStringLiteral("/sub");
    QVERIFY(QDir().mkpath(sub));
    const QString nested = sub + QStringLiteral("/nested.s");
    QFile n(nested);
    QVERIFY(n.open(QIODevice::WriteOnly | QIODevice::Text));
    n.write("\tnop\n");
    n.close();
    window.openPath(nested);
    QCOMPARE(view->model()->data(view->rootIndex(), Qt::UserRole + 1).toString(), dir);
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(),
                              nested, 5000);
    QVERIFY(view->isExpanded(view->currentIndex().parent()));

    // A file outside the project leaves the browser where the user put it:
    // the root is a choice, not a shadow of whatever tab is focused.
    const QString elsewhere = m_work->path() + QStringLiteral("/elsewhere");
    QVERIFY(QDir().mkpath(elsewhere));
    const QString foreign = elsewhere + QStringLiteral("/foreign.s");
    QFile g(foreign);
    QVERIFY(g.open(QIODevice::WriteOnly | QIODevice::Text));
    g.write("\tnop\n");
    g.close();
    window.openPath(foreign);
    QCOMPARE(view->model()->data(view->rootIndex(), Qt::UserRole + 1).toString(), dir);
    QCOMPARE(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(), nested);

    // The root moves only on an explicit choice: the Browse… button opens a
    // folder picker, and dismissing it changes nothing.
    auto *browse = browser->findChild<QPushButton *>(QStringLiteral("hardDriveBrowse"));
    QVERIFY(browse);
    QTimer::singleShot(0, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    browse->click();
    QCOMPARE(view->model()->data(view->rootIndex(), Qt::UserRole + 1).toString(), dir);
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
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(),
                              src, 5000);

    // Typing another folder into the path field moves the selection to that
    // folder: the context menu (New File…, Paste) acts on the current index,
    // so with the old selection left in place a new file was created in dirA
    // while the user was looking at dirB.
    browser->showDirectory(dirB);
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->data(view->currentIndex(), Qt::UserRole + 1).toString(),
                              dirB, 5000);
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
    QVERIFY(diskA && diskB && changeA && ejectA);
    QVERIFY2(!exportBtn, "export stays on the hard-drive context menu");

    auto *nameA = browser->findChild<QLabel *>(QStringLiteral("diskAName"));
    QVERIFY(nameA);
    QCOMPARE(nameA->text(), QStringLiteral("A: no disk"));
    QVERIFY(diskA->isHidden());
    auto *projectTitle = browser->findChild<QLabel *>(QStringLiteral("projectTitle"));
    QVERIFY(projectTitle);
    QCOMPARE(projectTitle->text(), QFileInfo(src).dir().dirName());
    QCOMPARE(projectTitle->toolTip(), QFileInfo(src).absolutePath());

    QSignalSpy spy(browser, &FileBrowser::floppyImageChanged);
    browser->setFloppyImages({image, QString()});
    QCOMPARE(browser->floppyImages().at(0), image);
    QVERIFY(ejectA->isEnabled());
    QVERIFY(!diskA->isHidden());
    QCOMPARE(nameA->text(), QStringLiteral("A: boot.st"));

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
        if (!f.open(QIODevice::ReadOnly))
            return QByteArray();
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

// A copy-out that cannot write to the host reported whatever `error` last held,
// which after a successful read is nothing at all: the browser's
// "Could not copy PROG.PRG from disk.st:" ended in a colon with no reason, and
// after an earlier damaged entry it reported *that* entry's damage for this one.
// The failing step names itself now, so the message says what actually went
// wrong.
void TstGui::floppyCopyOutNamesTheFailingWrite()
{
    const QString dir = m_work->path() + QStringLiteral("/copyout");
    QVERIFY(QDir().mkpath(dir));

    QString error;
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("PROG.PRG");
    file.data = QByteArrayLiteral("payload");
    items.append(file);
    const QString image = dir + QStringLiteral("/disk.st");
    QVERIFY2(floppy::writeImage(image, items, &error), qPrintable(error));

    // A destination directory this user cannot write into. Where the write is
    // allowed anyway — root, or a filesystem without permissions — there is no
    // failure to report, so the case skips instead of passing vacuously.
    const QString targetDir = dir + QStringLiteral("/locked");
    QVERIFY(QDir().mkpath(targetDir));
    QVERIFY(QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    {
        QFile probe(targetDir + QStringLiteral("/probe"));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            probe.remove();
            QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner);
            QSKIP("this user can write into a directory it has no write bit on");
        }
    }

    struct Host : floppy::Transfer::Host
    {
        QString image;
        QString mountedImage(int) const override { return image; }
        bool confirmRewrite(const QString &) override { return true; }
    } host;
    host.image = image;

    floppy::Transfer transfer(&host);
    const floppy::Transfer::Result result =
        transfer.imageToHost(0, {QStringLiteral("PROG.PRG")}, targetDir, false);
    QVERIFY2(result.outcome == floppy::Transfer::CopyFailed,
             "a failed copy-out must report the copy as failed");
    QVERIFY2(!result.error.isEmpty(),
             "the reason must not be empty: that is the colon with nothing after it");
    QVERIFY2(result.error.contains(QStringLiteral("PROG.PRG")),
             qPrintable(QStringLiteral("the reason must name the file it could not write: %1")
                            .arg(result.error)));

    QFile::setPermissions(targetDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                        | QFileDevice::ExeOwner);
}

// A copy must reproduce a symbolic link's content, not silently drop it. The
// old copy skipped every link and still reported success — which mattered
// because a move falls back to copy-then-delete, so a skipped link was deleted
// with the source tree it lived in.
void TstGui::fileBrowserCopyMaterialisesSymlinks()
{
    const QString dir = m_work->path() + QStringLiteral("/symlinks");
    const QString src = dir + QStringLiteral("/src");
    const QString dst = dir + QStringLiteral("/dst");
    QVERIFY(QDir().mkpath(src));
    QVERIFY(QDir().mkpath(dst));
    QVERIFY(writeFile(src + QStringLiteral("/keep.txt"), QByteArrayLiteral("kept")));
    if (!canCreateSymlinks(src))
        QSKIP("this platform/filesystem does not support symbolic links");
    QVERIFY(QFile::link(src + QStringLiteral("/keep.txt"), src + QStringLiteral("/link.txt")));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);

    browser->copyHardDrivePaths({src}, false);
    QVERIFY(browser->pasteIntoDirectory(dst));
    const QString copied = dst + QStringLiteral("/src");
    QCOMPARE(fileBytes(copied + QStringLiteral("/keep.txt")), QByteArrayLiteral("kept"));
    // Materialised as the regular file it resolved to: the bytes are what the
    // user sees, and nothing has to follow a link out of the copied tree.
    QVERIFY2(QFileInfo(copied + QStringLiteral("/link.txt")).isFile(),
             "the link's content must be copied, not skipped");
    QVERIFY(!QFileInfo(copied + QStringLiteral("/link.txt")).isSymLink());
    QCOMPARE(fileBytes(copied + QStringLiteral("/link.txt")), QByteArrayLiteral("kept"));

    // A link to a directory has no single file to reproduce. The copy must
    // refuse — and say so — rather than report success with the entry missing.
    const QString tree = dir + QStringLiteral("/tree");
    const QString dst2 = dir + QStringLiteral("/dst2");
    QVERIFY(QDir().mkpath(tree + QStringLiteral("/real")));
    QVERIFY(QDir().mkpath(dst2));
    QVERIFY(writeFile(tree + QStringLiteral("/real/inner.txt"), QByteArrayLiteral("inner")));
    QVERIFY(QFile::link(tree + QStringLiteral("/real"), tree + QStringLiteral("/dirlink")));

    browser->copyHardDrivePaths({tree}, false);
    QTimer::singleShot(0, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    QVERIFY2(!browser->pasteIntoDirectory(dst2),
             "a copy that cannot reproduce an entry must fail, not drop it silently");
}

// The move fallback (copy, then delete the source) runs when a rename fails.
// It must only delete after a copy that reproduced everything, and must never
// announce a deletion it did not perform: pre-fix it emitted pathDeleted for a
// source that was still on disk, so MainWindow closed the document as "deleted
// on disk" and the user's file sat there unopened.
void TstGui::fileBrowserMoveFallbackNeverLosesTheSource()
{
    const QString root = m_work->path() + QStringLiteral("/fallback");
    const QString srcDir = root + QStringLiteral("/src");
    const QString dstDir = root + QStringLiteral("/dst");
    QVERIFY(QDir().mkpath(srcDir));
    QVERIFY(QDir().mkpath(dstDir));
    QVERIFY(writeFile(srcDir + QStringLiteral("/keep.txt"), QByteArrayLiteral("kept")));
    if (!canCreateSymlinks(srcDir))
        QSKIP("this platform/filesystem does not support symbolic links");
    QVERIFY(QFile::link(srcDir + QStringLiteral("/keep.txt"), srcDir + QStringLiteral("/link.txt")));

    // A read-only parent directory makes the rename fail (no write permission
    // on the source's parent), so the move takes its copy-then-delete
    // fallback; the delete then fails too, for the same reason. That failure is
    // the point: the move did not happen and must be reported as such.
    const auto ownerFull = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
    const QString probeDir = root + QStringLiteral("/probe");
    QVERIFY(QDir().mkpath(probeDir));
    QVERIFY(writeFile(probeDir + QStringLiteral("/probe"), QByteArrayLiteral("x")));
    QVERIFY(QFile::setPermissions(probeDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    const bool enforced = !QFile::remove(probeDir + QStringLiteral("/probe"));
    QVERIFY(QFile::setPermissions(probeDir, ownerFull));
    if (!enforced)
        QSKIP("directory permissions are not enforced (running as root?)");
    QVERIFY(QFile::setPermissions(srcDir, QFileDevice::ReadOwner | QFileDevice::ExeOwner));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    QSignalSpy renamed(browser, &FileBrowser::pathRenamed);
    QSignalSpy deleted(browser, &FileBrowser::pathDeleted);
    browser->copyHardDrivePaths({srcDir}, true);
    // The refusal is reported with a modal warning; close it so the paste can
    // return.
    QTimer::singleShot(0, [] {
        if (QWidget *modal = QApplication::activeModalWidget())
            modal->close();
    });
    const bool moved = browser->pasteIntoDirectory(dstDir);
    QVERIFY(QFile::setPermissions(srcDir, ownerFull));

    QCOMPARE(deleted.count(), 0);
    QCOMPARE(renamed.count(), 0);
    QVERIFY2(!moved, "a move whose source could not be removed is not a successful move");
    QVERIFY2(QFileInfo(srcDir + QStringLiteral("/keep.txt")).isFile(),
             "the source must survive a move that could not complete");
    QVERIFY2(QFileInfo(srcDir + QStringLiteral("/link.txt")).isSymLink(),
             "the source tree must be untouched, symlink included");
}

// Copying entries off a floppy addresses paths inside the image that was mounted
// at the time. Once that disk is ejected — or another image takes its place in
// the same drive — the clipboard points at entries with no disk behind them, and
// Paste used to stay enabled regardless: pasting then did nothing at all while
// claiming to be available. The clipboard is dropped when its source is gone,
// and the refusal is a refusal rather than a silent no-op.
void TstGui::fileBrowserClipboardDiesWithItsDisk()
{
    const QString dir = m_work->path() + QStringLiteral("/clipdisk");
    QVERIFY(QDir().mkpath(dir));
    const QString src = dir + QStringLiteral("/prog.s");
    QVERIFY(writeFile(src, QByteArrayLiteral("\tnop\n")));

    QString error;
    // One disk-image builder: an image holding a single file, or an empty one.
    const auto makeImage = [&](const QString &name, const QString &entry) {
        QVector<floppy::Item> items;
        if (!entry.isEmpty()) {
            floppy::Item file;
            file.destPath = entry;
            file.data = QByteArrayLiteral("payload");
            items.append(file);
        }
        const QString path = dir + QLatin1Char('/') + name;
        return floppy::writeImage(path, items, &error) ? path : QString();
    };
    const QString imageA = makeImage(QStringLiteral("a.st"), QStringLiteral("ONE.TXT"));
    const QString imageB = makeImage(QStringLiteral("b.st"), QString());
    const QString other = makeImage(QStringLiteral("other.st"), QStringLiteral("TWO.TXT"));
    QVERIFY2(!imageA.isEmpty() && !imageB.isEmpty() && !other.isEmpty(), qPrintable(error));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    window.openPath(src);   // roots the hard-drive pane, so a paste target exists
    browser->setFloppyImages({imageA, imageB});

    // Reads the hard-drive context menu's Paste item the way the user sees it:
    // menu.exec() runs a nested event loop, so the probe rides a zero-delay
    // timer inside it, records the action, then closes the popup to return.
    const auto pasteEnabledInMenu = [&window]() {
        auto *view = window.findChild<QTreeView *>(QStringLiteral("hardDriveView"));
        if (!view)
            return false;
        bool enabled = false;
        QTimer::singleShot(0, [&enabled] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu)
                return;
            for (QAction *action : menu->actions()) {
                if (action->text() == QStringLiteral("Paste"))
                    enabled = action->isEnabled();
            }
            menu->close();
        });
        const QPoint at(5, 5);
        QContextMenuEvent event(QContextMenuEvent::Mouse, at,
                                view->viewport()->mapToGlobal(at));
        QApplication::sendEvent(view->viewport(), &event);
        return enabled;
    };

    browser->copyFloppyEntries(0, {QStringLiteral("ONE.TXT")}, false);
    QVERIFY2(pasteEnabledInMenu(), "a copied floppy entry must offer Paste");

    // Ejecting A leaves the entry path with no disk behind it.
    browser->setFloppyImages({QString(), imageB});
    QVERIFY2(!pasteEnabledInMenu(), "Paste must die with the disk it copied from");
    QVERIFY2(!browser->pasteIntoDirectory(dir),
             "a paste from an ejected disk must refuse, not quietly do nothing");

    // Another disk in the same drive is the same problem: an entry path is
    // relative to the image that held it.
    browser->setFloppyImages({other, imageB});
    browser->copyFloppyEntries(0, {QStringLiteral("TWO.TXT")}, false);
    QVERIFY2(pasteEnabledInMenu(), "a fresh copy must offer Paste again");
    browser->setFloppyImages({imageA, imageB});
    QVERIFY2(!pasteEnabledInMenu(), "a different disk in the drive must take Paste with it");
}

// A move across volumes must materialise a symbolic link inside the moved tree
// instead of handing it to whatever QFile::rename does internally, which is Qt's
// own copy path with Qt's link and permission semantics rather than the
// browser's.
//
// HONEST LABEL: a guard, not a red on this Qt/filesystem. Measured on Qt 6.10.2,
// a *directory* rename across volumes already fails (EXDEV, and Qt's fallback
// for a failed rename only covers files), so the browser's copy+delete fallback
// ran before the fix too and this test passed then. It WOULD be red where a
// cross-device rename succeeds and does its own copy underneath — the Qt 6.5-era
// premise the finding was filed under. What it now guards is that the
// cross-volume route is unconditional, so the fallback's behaviour cannot regress.
void TstGui::fileBrowserCrossDeviceMoveMaterialisesLinks()
{
    const QString tempRoot = QStorageInfo(m_work->path()).rootPath();

    // A second mounted volume to move across. isReadOnly() is the mount's flag
    // and not this user's permission on it, so the candidate is probed by
    // actually creating the scratch directory.
    QString scratch;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || volume.isReadOnly())
            continue;
        if (volume.rootPath() == tempRoot)
            continue;
        const QString candidate = volume.rootPath()
            + QStringLiteral("/pist-crossdev-%1").arg(QCoreApplication::applicationPid());
        if (QDir().mkpath(candidate)) {
            scratch = candidate;
            break;
        }
    }
    if (scratch.isEmpty())
        QSKIP("needs a second writable mounted volume to move across");

    const QString srcDir = scratch + QStringLiteral("/src");
    const QString dstDir = m_work->path() + QStringLiteral("/crossdev-dst");
    QVERIFY(QDir().mkpath(srcDir));
    QVERIFY(QDir().mkpath(dstDir));
    QVERIFY(writeFile(srcDir + QStringLiteral("/keep.txt"), QByteArrayLiteral("kept")));
    if (!canCreateSymlinks(srcDir)) {
        QDir(scratch).removeRecursively();
        QSKIP("this platform/filesystem does not support symbolic links");
    }
    QVERIFY(QFile::link(srcDir + QStringLiteral("/keep.txt"),
                        srcDir + QStringLiteral("/inner.txt")));

    MainWindow window;
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    QSignalSpy deleted(browser, &FileBrowser::pathDeleted);

    browser->copyHardDrivePaths({srcDir}, true);
    QVERIFY(browser->pasteIntoDirectory(dstDir));
    QVERIFY(!QFileInfo::exists(srcDir));
    // The link is reproduced as the file it resolved to, in the tree's new home.
    QCOMPARE(QFileInfo(dstDir + QStringLiteral("/src/inner.txt")).isSymLink(), false);
    QCOMPARE(fileBytes(dstDir + QStringLiteral("/src/inner.txt")), QByteArrayLiteral("kept"));
    // The source went with the move, not as a deletion: MainWindow closes the
    // documents it is told were deleted on disk.
    QCOMPARE(deleted.count(), 0);

    QDir(scratch).removeRecursively();
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

    // The write-back link dies with the tab that carried it. Closing DOC.TXT
    // and then saving that same path as an ordinary document (the extracted
    // file reopened, not through the disk pane) must leave the image alone:
    // kept behind, the closed document's mapping outlived the tab and pushed
    // whatever was saved at that path back into the image.
    const QString extracted = editor->filePath();
    QVERIFY(!extracted.isEmpty());
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    QVERIFY(QMetaObject::invokeMethod(&window, "onTabCloseRequested", Qt::DirectConnection,
                                      Q_ARG(int, tabs->indexOf(editor))));
    QCOMPARE(tabs->indexOf(editor), -1);

    QVERIFY(window.openPath(extracted));
    auto *reopened = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(reopened);
    QCOMPARE(reopened->filePath(), extracted);
    reopened->setPlainText(QStringLiteral("ordinary\n"));
    saveAction->trigger();
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    QVERIFY2(floppy::readFileRaw(raw, QStringLiteral("DOC.TXT"), &saved, &error),
             qPrintable(error));
    QCOMPARE(saved, QByteArray("changed\n"));
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

// Atari ST sources are ASCII or Latin-1/ST-charset. A high-bit byte is not
// valid UTF-8, so a bare UTF-8 QTextStream decoded it to U+FFFD with status Ok
// and the next save wrote EF BF BD over the byte the user never touched. The
// encoding the load detected is the one the save uses, so an unedited file
// round-trips byte for byte.
//
// Line endings are the same class of fact: the document holds '\n' whatever the
// file held, so the style the load saw is written back at save. Both halves are
// asserted per fixture, because a CRLF file with a Latin-1 byte exercises the
// two together.
void TstGui::editorRoundTripsNonUtf8Sources()
{
    const QString dir = m_work->path() + QStringLiteral("/encodings");
    QVERIFY(QDir().mkpath(dir));

    // Latin-1: 0xE9 is 'é' and forms no valid UTF-8 sequence.
    QByteArray latin1 = QByteArrayLiteral("dc.b 'caf");
    latin1.append(char(0xE9));
    latin1.append(QByteArrayLiteral("',13,10\n"));
    const QString latin1Path = dir + QStringLiteral("/latin1.s");
    QVERIFY(writeFile(latin1Path, latin1));

    CodeEditor editor(appearance::editorTheme());
    QVERIFY(editor.loadFile(latin1Path));
    QVERIFY2(editor.toPlainText().contains(QChar(0x00E9)),
             qPrintable(QStringLiteral("0xE9 must load as its character, not a replacement one: %1")
                            .arg(editor.toPlainText())));
    QVERIFY2(!editor.toPlainText().contains(QChar(0xFFFD)),
             "a decoded source must not contain the replacement character");
    QVERIFY(editor.saveFile(latin1Path));
    QCOMPARE(fileBytes(latin1Path), latin1);

    // UTF-8 with multi-byte characters — including one Latin-1 could encode,
    // which must still be written back as UTF-8 because that is what the file
    // was loaded as.
    const QByteArray utf8 = QStringLiteral("\tnop\t; \u2500\u2500 caf\u00E9\n").toUtf8();
    const QString utf8Path = dir + QStringLiteral("/utf8.s");
    QVERIFY(writeFile(utf8Path, utf8));
    QVERIFY(editor.loadFile(utf8Path));
    QCOMPARE(editor.toPlainText(), QString::fromUtf8(utf8));
    QVERIFY(editor.saveFile(utf8Path));
    QCOMPARE(fileBytes(utf8Path), utf8);

    // Pure ASCII, the third case in the invariant.
    const QByteArray ascii = QByteArrayLiteral("\tnop\n\trts\n");
    const QString asciiPath = dir + QStringLiteral("/ascii.s");
    QVERIFY(writeFile(asciiPath, ascii));
    QVERIFY(editor.loadFile(asciiPath));
    QVERIFY(editor.saveFile(asciiPath));
    QCOMPARE(fileBytes(asciiPath), ascii);

    // Line endings are part of the bytes. A CRLF source (the common shape for
    // anything edited on Windows or by an Atari tool) loaded and saved unedited
    // must come back identical: pre-fix the document's '\n' was written as-is,
    // so every line ending in the file changed, and a source saved back into a
    // floppy image carried the rewrite into the image.
    const QByteArray crlf = QByteArrayLiteral("\tnop\r\n\trts\r\n");
    const QString crlfPath = dir + QStringLiteral("/crlf.s");
    QVERIFY(writeFile(crlfPath, crlf));
    QVERIFY(editor.loadFile(crlfPath));
    QCOMPARE(editor.toPlainText(), QStringLiteral("\tnop\n\trts\n"));
    QVERIFY(editor.saveFile(crlfPath));
    QVERIFY2(fileBytes(crlfPath) == crlf,
             qPrintable(QStringLiteral("a CRLF source must round-trip: %1")
                            .arg(QString::fromLatin1(fileBytes(crlfPath).toHex(' ')))));

    // The two features are independent, so CRLF has to survive alongside the
    // Latin-1 encoding the load detected (a 0xE9 byte, which 0x0D and 0x0A do
    // not disturb).
    QByteArray latin1Crlf = QByteArrayLiteral("dc.b 'caf");
    latin1Crlf.append(char(0xE9));
    latin1Crlf.append(QByteArrayLiteral("',13,10\r\n"));
    const QString latin1CrlfPath = dir + QStringLiteral("/latin1-crlf.s");
    QVERIFY(writeFile(latin1CrlfPath, latin1Crlf));
    QVERIFY(editor.loadFile(latin1CrlfPath));
    QVERIFY(editor.saveFile(latin1CrlfPath));
    QCOMPARE(fileBytes(latin1CrlfPath), latin1Crlf);

    // And an edited CRLF document stays CRLF rather than turning into a mix:
    // the style is a property of the file, not of the lines that were already
    // there. Every separator in the saved bytes is a CRLF pair — one per line
    // break in the document — so a single '\n' written bare would show up as a
    // shortfall here. (appendPlainText opens a new line, hence the extra break.)
    QVERIFY(editor.loadFile(crlfPath));
    editor.appendPlainText(QStringLiteral("\tend"));
    QVERIFY(editor.saveFile(crlfPath));
    QCOMPARE(fileBytes(crlfPath).count(QByteArrayLiteral("\r\n")),
             editor.toPlainText().count(QLatin1Char('\n')));
    QVERIFY2(fileBytes(crlfPath).startsWith(QByteArrayLiteral("\tnop\r\n\trts\r\n")),
             qPrintable(QString::fromLatin1(fileBytes(crlfPath).toHex(' '))));
}

// An extracted document is saved to a path the user has just chosen, which may
// not exist yet: the save creates the parents instead of failing on them.
void TstGui::editorSaveCreatesMissingDirectories()
{
    const QString dir = m_work->path() + QStringLiteral("/missing/parent/deep");
    const QString path = dir + QStringLiteral("/doc.s");
    QVERIFY(!QFileInfo::exists(dir));

    CodeEditor editor(appearance::editorTheme());
    editor.setPlainText(QStringLiteral("\tnop\n"));
    QVERIFY2(editor.saveFile(path), qPrintable(editor.lastError()));
    QCOMPARE(editor.filePath(), path);
    QCOMPARE(fileBytes(path), QByteArrayLiteral("\tnop\n"));
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
    // real per-user directory cannot leak in. The scopes restore all of it on
    // every path out, so an assertion that fails cannot leave the rest of the
    // suite looking at a stripped environment.
    const EnvScope pathScope("PATH");
    const EnvScope tosDirScope("PIST_TOS_DIR");
    qputenv("PATH", "");
    qputenv("PIST_TOS_DIR", "");
    const TestModeScope testMode;

    // Other suites' fixtures may have left an "installed" fake tool or ROM in
    // the shared test-mode data root; clear both so missing really is missing.
    // Below the scope guard deliberately: only with test mode on do these
    // resolve to the test-mode locations — above it they would name the real
    // per-user directories and delete a user's genuinely fetched tools.
    QDir(toolchain::suggestedInstallDir()).removeRecursively();
    QDir(paths::suggestedRomDir()).removeRecursively();

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
}


void TstGui::setupInstallTakesEffectWithoutRestart()
{
    // Same stripped, test-mode environment as setupDialogShowsMissingPieces,
    // restored by the scopes on every path out.
    const EnvScope pathScope("PATH");
    const EnvScope tosDirScope("PIST_TOS_DIR");
    qputenv("PATH", "");
    qputenv("PIST_TOS_DIR", "");
    const TestModeScope testMode;
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
}

void TstGui::appearancePreferencesApply()
{
    QSettings settings;
    MainWindow window;
    auto *editor = window.findChild<CodeEditor *>();
    QVERIFY(editor);

    // Font size: an explicit value wins over the platform default.
    const int defaultSize = editor->font().pointSize();
    settings.setValue(QStringLiteral("appearance/fontSize"), defaultSize + 6);
    editor->setTheme(appearance::editorTheme());
    QCOMPARE(editor->font().pointSize(), defaultSize + 6);

    // Font family: a real installed family is applied to the editor.
    const QString family =
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    QVERIFY(!family.isEmpty());
    QVERIFY(pist::appearance::editorFontChoices().contains(QStringLiteral("Monospace")));
    settings.setValue(QStringLiteral("appearance/fontFamily"), family);
    editor->setTheme(appearance::editorTheme());
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
    editor->setTheme(appearance::editorTheme());
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 128);
    QVERIFY(pist::appearance::darkModeActive());

    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("light"));
    pist::appearance::applyTheme();
    editor->setTheme(appearance::editorTheme());
    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() >= 128);
    QVERIFY(!pist::appearance::darkModeActive());

    // Leave the palette at the platform default again. The settings store is
    // cleared between tests (init()), but the application palette is
    // process-wide and would otherwise stay light for whatever runs next.
    pist::appearance::applyTheme();
}

// The logo pixmap carries the ST glyph, and the wordmark flag adds the ATARI
// letters under it. The refactor behind this test parses the SVG outline once
// per pixmap instead of once per draw; the drawing itself is meant to be
// unchanged, so this is a regression guard rather than a red: it pins that both
// flags still produce a pixmap, that they still select different glyphs, that
// neither comes out blank, and that the window/menu icon builds at every size it
// advertises.
void TstGui::atariLogoRendersBothGlyphs()
{
    const QPixmap wordmark = appearance::atariLogoPixmap(88, true);
    const QPixmap plain = appearance::atariLogoPixmap(88, false);
    QVERIFY(!wordmark.isNull() && !plain.isNull());
    QVERIFY2(wordmark.toImage() != plain.toImage(),
             "the wordmark flag must still select the glyph it always did");
    QVERIFY2(hasOpaquePixels(wordmark) && hasOpaquePixels(plain),
             "a wrong or empty outline would leave the logo blank");

    for (int size : {16, 20, 24, 32})
        QVERIFY(!appearance::atariLogoIcon().pixmap(size, size).isNull());
}

// Ctrl+G is a one-line bar, Return copies the indent it just left, and the
// default keys stay PiST (F8 breakpoint, F9 continue, F10 step into). Common
// is opt-in.
void TstGui::editorGotoIndentAndShortcutScheme()
{
    QSettings settings;
    const QString src = m_work->path() + QStringLiteral("/keys.s");
    {
        QFile file(src);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("nop\n\tnop\n    rts\n");
    }

    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    window.openPath(src);
    auto *tabs = window.findChild<QTabWidget *>(QStringLiteral("documentTabs"));
    QVERIFY(tabs);
    auto *editor = qobject_cast<CodeEditor *>(tabs->currentWidget());
    QVERIFY(editor);
    editor->setFocus();

    QTest::keyClick(editor, Qt::Key_G, Qt::ControlModifier);
    QVERIFY(editor->gotoBarVisible());
    auto *edit = editor->findChild<QLineEdit *>(QStringLiteral("editorGotoLine"));
    QVERIFY(edit);
    edit->setText(QStringLiteral("3"));
    QTest::keyClick(edit, Qt::Key_Return);
    QTRY_COMPARE(editor->textCursor().blockNumber(), 2);
    QVERIFY(!editor->gotoBarVisible());

    editor->setPlainText(QStringLiteral("\tnop"));
    QTextCursor cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(editor->toPlainText(), QStringLiteral("\tnop\n\t"));

    auto *toggle = window.findChild<QAction *>(QStringLiteral("toggleBreakpointAction"));
    auto *step = window.findChild<QAction *>(QStringLiteral("stepAction"));
    auto *resume = window.findChild<QAction *>(QStringLiteral("continueAction"));
    QVERIFY(toggle && step && resume);
    QCOMPARE(toggle->shortcut(), QKeySequence(Qt::Key_F8));
    QCOMPARE(step->shortcut(), QKeySequence(Qt::Key_F10));
    QCOMPARE(resume->shortcut(), QKeySequence(Qt::Key_F9));

    const int line = editor->textCursor().blockNumber() + 1;
    toggle->trigger();
    QVERIFY(editor->breakpointLines().contains(line));
    toggle->trigger();
    QVERIFY(!editor->breakpointLines().contains(line));

    settings.setValue(QStringLiteral("appearance/shortcuts"), QStringLiteral("common"));
    {
        MainWindow common;
        QCOMPARE(common.findChild<QAction *>(QStringLiteral("stepAction"))->shortcut(),
                 QKeySequence(Qt::Key_F11));
        QCOMPARE(common.findChild<QAction *>(QStringLiteral("stepOverAction"))->shortcut(),
                 QKeySequence(Qt::Key_F10));
        QCOMPARE(common.findChild<QAction *>(QStringLiteral("toggleBreakpointAction"))->shortcut(),
                 QKeySequence(Qt::Key_F9));
        QCOMPARE(common.findChild<QAction *>(QStringLiteral("runAction"))->shortcut(),
                 QKeySequence(Qt::Key_F5));
        QCOMPARE(common.findChild<QAction *>(QStringLiteral("continueAction"))->shortcut(),
                 QKeySequence());

        SettingsDialog dialog{ProjectSettings()};
        auto *scheme = dialog.findChild<QComboBox *>(QStringLiteral("shortcutScheme"));
        auto *width = dialog.findChild<QLabel *>(QStringLiteral("tabWidthValue"));
        QVERIFY(scheme && width);
        QCOMPARE(scheme->currentData().toString(), QStringLiteral("common"));
        QCOMPARE(width->text(), QStringLiteral("8"));
    }
}

// Comments, gutter numerals, zero bytes and the muted "disabled / pending"
// state are text, so they have to clear 4.5:1 on the surface they are painted
// on, and stay quieter than the body ink beside them.
void TstGui::quietColoursClearTheirBackground()
{
    QSettings settings;
    const QVariant previous = settings.value(QStringLiteral("appearance/theme"));
    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("dark"));
    pist::appearance::applyTheme();
    QVERIFY(pist::appearance::darkModeActive());

    const appearance::Colors c = appearance::colors();
    const QColor base = QApplication::palette().color(QPalette::Base);
    const QColor window = QApplication::palette().color(QPalette::Window);
    const QColor ink = QApplication::palette().color(QPalette::Text);

    const struct {
        const char *name;
        QColor fg;
        QColor bg;
    } pairs[] = {
        {"comment", c.comment, base},
        {"gutter", c.gutterText, c.gutter},
        {"zero", c.zero, base},
        {"muted", c.muted, window},
    };
    for (const auto &pair : pairs) {
        const double ratio = contrastRatio(pair.fg, pair.bg);
        const double body = contrastRatio(ink, pair.bg);
        QVERIFY2(ratio >= 4.5,
                 qPrintable(QStringLiteral("%1 is %2:1, under 4.5")
                                .arg(QLatin1String(pair.name))
                                .arg(ratio, 0, 'f', 2)));
        QVERIFY2(body > ratio + 2.0,
                 qPrintable(QStringLiteral("%1 at %2:1 is no longer quiet next to body text at %3:1")
                                .arg(QLatin1String(pair.name))
                                .arg(ratio, 0, 'f', 2)
                                .arg(body, 0, 'f', 2)));
    }

    if (previous.isValid())
        settings.setValue(QStringLiteral("appearance/theme"), previous);
    else
        settings.remove(QStringLiteral("appearance/theme"));
    pist::appearance::applyTheme();
}

// The instruction reference used to update only while its dock was the
// selected tab. The strip under the editor is the line you can read without
// opening that dock, and clicking it brings the dock forward.
void TstGui::instructionStripFollowsTheCaret()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    auto *editor = window.findChild<CodeEditor *>();
    auto *ref = window.findChild<InstructionRefView *>();
    auto *strip = window.findChild<QPushButton *>(QStringLiteral("instructionStrip"));
    QVERIFY(editor && ref && strip);
    QVERIFY(strip->isVisible());

    editor->setPlainText(QStringLiteral("\tmoveq\t#1,d0\n"
                                        "\tpea\tmsg(pc)\n"
                                        "\tmove.w\t#9,-(sp)\n"
                                        "\ttrap\t#1\n"
                                        "; a comment\n"));

    QTextCursor moveq = editor->document()->find(QStringLiteral("moveq"));
    QVERIFY(!moveq.isNull());
    editor->setTextCursor(moveq);
    QCOMPARE(ref->currentMnemonic(), QStringLiteral("MOVEQ"));
    QVERIFY2(strip->text().startsWith(QLatin1String("MOVEQ")), qPrintable(strip->text()));
    QVERIFY(strip->text().contains(QLatin1String("Move quick")));

    // A first run hides the debug docks, so Instructions is not showing. The
    // reference must still have followed.
    auto *instrDock = window.findChild<QDockWidget *>(QStringLiteral("instructionRefDock"));
    QVERIFY(instrDock);
    QVERIFY(instrDock->isHidden());

    QTextCursor trap = editor->document()->find(QStringLiteral("trap"));
    QVERIFY(!trap.isNull());
    editor->setTextCursor(trap);
    QCOMPARE(ref->currentMnemonic(), QStringLiteral("gemdos:9"));
    QVERIFY2(strip->text().contains(QLatin1String("Cconws")), qPrintable(strip->text()));
    QVERIFY2(strip->text().contains(QLatin1String("Stack:")), qPrintable(strip->text()));

    QTextCursor comment = editor->document()->find(QStringLiteral("comment"));
    QVERIFY(!comment.isNull());
    editor->setTextCursor(comment);
    QVERIFY(strip->text().isEmpty());
    QCOMPARE(ref->currentMnemonic(), QStringLiteral("gemdos:9"));

    QTest::mouseClick(strip, Qt::LeftButton);
    QTabBar *debugTabs = nullptr;
    for (QTabBar *bar : window.findChildren<QTabBar *>()) {
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == QLatin1String("Instructions"))
                debugTabs = bar;
        }
    }
    QVERIFY(debugTabs);
    QCOMPARE(debugTabs->tabText(debugTabs->currentIndex()), QStringLiteral("Instructions"));
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

void TstGui::spriteEditorChromeSitsWhereItIsUsed()
{
    ImageEditor editor;
    editor.resize(900, 620);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    auto *addFrame = editor.findChild<QToolButton *>(QStringLiteral("imageAddFrame"));
    auto *onion = editor.findChild<QComboBox *>(QStringLiteral("imageOnion"));
    auto *play = editor.findChild<QToolButton *>(QStringLiteral("imagePlay"));
    auto *preview = editor.findChild<QLabel *>(QStringLiteral("imagePreview"));
    auto *x = editor.findChild<QSpinBox *>(QStringLiteral("imagePhaseX"));
    auto *w = editor.findChild<QSpinBox *>(QStringLiteral("imagePhaseCellW"));
    auto *addPhase = editor.findChild<QToolButton *>(QStringLiteral("imageAddPhase"));
    auto *picker = editor.findChild<QComboBox *>(QStringLiteral("imagePhasePicker"));
    auto *mode = editor.findChild<QAction *>(QStringLiteral("imageSheetMode"));
    QVERIFY(addFrame && onion && play && preview && x && w && addPhase && picker && mode);

    auto *placement = editor.findChild<QWidget *>(QStringLiteral("imagePhasePlacement"));
    QVERIFY(placement);
    QVERIFY2(placement->isHidden(), "placed/x/y/w/h belong to spritesheet mode");
    QVERIFY(!x->isVisible());
    mode->setChecked(true);
    QVERIFY(!placement->isHidden());
    QVERIFY(x->isVisible());
    QVERIFY(w->isVisible());
    mode->setChecked(false);
    QVERIFY(placement->isHidden());

    const QPoint frameAt = addFrame->mapTo(&editor, QPoint());
    const QPoint onionAt = onion->mapTo(&editor, QPoint());
    const QPoint playAt = play->mapTo(&editor, QPoint());
    const QPoint previewAt = preview->mapTo(&editor, QPoint());
    const QPoint phaseAt = addPhase->mapTo(&editor, QPoint());
    const QPoint pickerAt = picker->mapTo(&editor, QPoint());

    QVERIFY2(qAbs(frameAt.y() - onionAt.y()) < 12,
             "onion-skin sits in the frame-button row");
    QVERIFY(playAt.y() + play->height() <= previewAt.y() + 4);
    QVERIFY2(qAbs(playAt.x() - previewAt.x()) < 48,
             "play and fps sit in the preview column");
    QVERIFY2(qAbs(phaseAt.y() - pickerAt.y()) < 12,
             "phase +/− sit after the phase selector");
    QVERIFY(phaseAt.x() > pickerAt.x());
    QVERIFY2(addFrame->width() <= addFrame->sizeHint().width() + 8,
             "frame buttons pack instead of stretching across the filmstrip");

    QVERIFY(!editor.findChild<QComboBox *>(QStringLiteral("imagePreviewPhase")));
    QCOMPARE(preview->size(), QSize(96, 96));
    play->click();
    QVERIFY2(play->isChecked(), "the play button must start playback");
    // The preview box is a fixed 96×96 whatever playback state it is in; this
    // used to sleep 150 ms and compare, which asserted the same invariant only
    // after a guessed delay.
    QCOMPARE(preview->size(), QSize(96, 96));
    play->click();
    QVERIFY(!play->isChecked());

    auto *layers = editor.findChild<QListWidget *>(QStringLiteral("imageLayers"));
    QVERIFY(layers);
    QVERIFY2(layers->height() <= 150,
             "layers list must not eat leftover column height");
    const QPoint layersAt = layers->mapTo(&editor, QPoint());
    QVERIFY2(pickerAt.y() - (layersAt.y() + layers->height()) < 80,
             "This phase sits under a compact layers list");
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

void TstGui::sheetExportWritesTheComposedSheetAsPim()
{
    // The sheet exporter's format switch was a copy of the single-frame
    // exporter's and had drifted: it carried no Pim case, so exporting the
    // composed sheet as .pim answered "Unknown export format" while the other
    // path wrote the document. Both now encode through one table.
    const QString dir = m_work->path() + QStringLiteral("/sheetpim");
    QVERIFY(QDir().mkpath(dir));

    ImageEditor editor;
    editor.show();
    editor.newDocument(16, 16, PaletteKind::Ste);
    QCOMPARE(editor.document().addSheet(QString(), 320, 200), 0);
    QVERIFY(editor.document().setPhasePlacement(0, 0, 8, 12));
    const int colour = editor.document().active().at(6);
    editor.document().setPixel(0, colour);

    const QString path = dir + QStringLiteral("/sheet.pim");
    QVERIFY2(editor.exportSheetFile(path, 0, false), qPrintable(editor.lastError()));
    QVERIFY(QFileInfo(path).size() > 0);

    // The composition comes back: the phase's pixel at [8, 12] of a 320×200
    // frame, which is what the still-image formats of the same sheet hold.
    ImageDocument reloaded;
    QString error;
    QVERIFY2(reloaded.load(path, &error), qPrintable(error));
    QCOMPARE(reloaded.width(), 320);
    QCOMPARE(reloaded.height(), 200);
    QCOMPARE(reloaded.pixels().at(12 * 320 + 8), colour);
}

void TstGui::sliceDialogBuildsPhaseFromSheet()
{
    const QString dir = m_work->path() + QStringLiteral("/slice");
    QVERIFY(QDir().mkpath(dir));

    // A 192x32 strip: six 32x32 cells, each marked with its own colour.
    // The strip travels through a PI1, and PI1 stores 3-bit words (MAJ-38) —
    // so every palette entry's channels come from {0,2,4,6,9,11,13,15}, the
    // values the 3-bit codec (bit-replication expansion) round-trips exactly
    // (7 and 8, for instance, come back as 6 and 9). Anything else would
    // legitimately come back quantised, and the index comparison below would
    // be comparing against a colour that never survived the file.
    ImageDocument strip = ImageDocument::create(192, 32, PaletteKind::Ste);
    strip.setActive({0x000, 0xFFF, 0xDDD, 0xBBB, 0x999, 0x666, 0x444, 0x222});
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

// A sheet slice onto a document whose own cell is a different shape. The entry
// (MAJ-55) filed the first sliced cell as rejected here, because the document
// used to build a new phase's frame at the *previous* phase's cell size and
// replaceActiveLayer() refuses a buffer that does not match the phase.
//
// Measured against a scratch build with the pre-CRIT-2 addPhase restored
// (frame 0 created while the 8x8 phase was still current, composite 64 for a
// 16x16 phase): replaceActiveLayer(cell) *accepted* the cell — it size-checks
// the incoming buffer against the new phase, which matches — and its remesh
// resized the frame to the cell. So the slicing path repaired itself, and this
// test passes with and without CRIT-2's document fix. It is a GUARD: what it
// pins is that a slice with differing cell sizes carries the sheet's pixels into
// every frame at the new phase's size, first frame included.
void TstGui::slicingAPhaseFromASheetOfAnotherCellSize()
{
    const int cell = 16;
    ImageDocument doc = ImageDocument::create(8, 8, PaletteKind::Ste);
    // The precondition the entry is about, asserted rather than assumed: the
    // current phase's cell is not the one being sliced.
    QCOMPARE(doc.phases().at(0).cellW, 8);
    QCOMPARE(doc.phases().at(0).cellH, 8);

    // A 64x16 sheet holding four solid 16x16 cells, each in its own colour.
    ImportedSheet sheet;
    sheet.width = 4 * cell;
    sheet.height = cell;
    sheet.kind = PaletteKind::Ste;
    sheet.active = doc.active();
    sheet.pixels.fill(kTransparent, sheet.width * sheet.height);
    QVector<QVector<int>> expected;
    for (int k = 0; k < 4; ++k) {
        QVector<int> block(cell * cell, doc.active().at(k + 1));
        for (int row = 0; row < cell; ++row)
            for (int col = 0; col < cell; ++col)
                sheet.pixels[row * sheet.width + k * cell + col] = doc.active().at(k + 1);
        expected.append(block);
    }

    SheetSlicing slicing;
    slicing.insert(0, sheet, doc.paletteKind());
    QVERIFY2(slicing.hasSheet(0), "the sheet's pixels must be cached before slicing");

    const int index = slicing.addPhaseFromSheet(doc, 0, QStringLiteral("walk"), 0, 0, cell, cell,
                                                expected.size());
    QCOMPARE(index, 1);
    QCOMPARE(doc.phases().size(), 2);
    QCOMPARE(doc.phases().at(1).cellW, cell);
    QCOMPARE(doc.phases().at(1).cellH, cell);
    QCOMPARE(doc.phases().at(1).frames.size(), expected.size());
    QVERIFY(doc.setCurrentPhase(index));
    for (int k = 0; k < expected.size(); ++k) {
        QVERIFY(doc.setCurrentFrame(k));
        const QVector<int> &frame = doc.frame(k);
        QCOMPARE(frame.size(), cell * cell);
        // The buffer the canvas and the exporters index with, and not only the
        // composite: an undersized one over-reads in Release.
        QCOMPARE(doc.activeLayerPixels().size(), cell * cell);
        QVERIFY2(frame == expected.at(k),
                 qPrintable(QStringLiteral("frame %1 must be the sheet's cell %1: %2")
                                .arg(k)
                                .arg(QString::number(frame.value(0, -9)))));
    }
}

// reSliceUntouchedFrames takes the phase index from its caller, and the class is
// public and reusable, so an index that no longer names a phase is reachable —
// it is on the other side of an undo that removed one. Pre-fix the index went
// straight into QVector::at(), which aborts a debug build (measured:
// "ASSERT failure in QList<T>::at: index out of range"); the sibling
// slicePlacedPhaseFrames already refuses the same input.
void TstGui::reSlicingAnUnknownPhaseIsRefused()
{
    ImageDocument doc = ImageDocument::create(8, 8, PaletteKind::Ste);
    SheetSlicing slicing;

    QVERIFY2(!slicing.reSliceUntouchedFrames(doc, -1, 0, 0, 0),
             "a negative phase index is not a phase");
    QVERIFY2(!slicing.reSliceUntouchedFrames(doc, doc.phases().size(), 0, 0, 0),
             "a phase index past the end is not a phase");
    // And the live index still answers the ordinary question: a phase with no
    // sheet behind it has nothing to re-slice.
    QVERIFY(!slicing.reSliceUntouchedFrames(doc, 0, 0, 0, 0));
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

void TstGui::selectionCutOutDoesNotStickToTheCanvas()
{
    // A move drag paints the source cells as "cut out". That is an overlay on
    // the frame, not a change to it: the canvas caches the frame image and
    // rebuilds it only when the pixels change, so an overlay written into the
    // cache would still be there on the repaint that follows the drag.
    ImageEditor editor;
    editor.show();
    editor.newDocument(4, 4, PaletteKind::Ste);
    auto *canvas = editor.findChild<ImageCanvas *>();
    QVERIFY(canvas);
    const int colour = editor.document().active().at(3);
    editor.document().fillIndices({0, 1, 4, 5}, colour);

    QToolButton *selectButton = nullptr;
    for (auto *button : editor.findChildren<QToolButton *>()) {
        if (button->property("tool").isValid()
            && button->property("tool").toInt() == int(DrawTool::Select))
            selectButton = button;
    }
    QVERIFY2(selectButton, "the select tool joins the toolbar");
    QTest::mouseClick(selectButton, Qt::LeftButton);

    const auto cellPoint = [&canvas](int x, int y) {
        const int c = canvas->cellSize();
        return QPoint(x * c + c / 2, y * c + c / 2);
    };
    canvas->setSelection(QRect(0, 0, 2, 2));
    QCoreApplication::processEvents();

    // Press inside the selection (the cut-out is live), let the canvas paint,
    // then release without moving: the pixels never changed, so what the
    // canvas shows afterwards is whatever it cached.
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(0, 0));
    QCoreApplication::processEvents();
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(0, 0));
    QCoreApplication::processEvents();

    QImage shot(canvas->size(), QImage::Format_ARGB32);
    shot.fill(Qt::black);
    canvas->render(&shot);
    const int c = canvas->cellSize();
    const Rgb rgb = cubeRgb(PaletteKind::Ste, colour);
    QCOMPARE(shot.pixelColor(c / 2, c / 2).rgb(), qRgb(rgb.r, rgb.g, rgb.b));
}

// The canvas selection names cells of the document it was made on. Every path
// that swaps the document — New Image, replaceDocument, opening a file over it —
// used to leave the old selection in place, so the marquee kept highlighting
// coordinates that belonged to a document nobody is looking at any more.
void TstGui::swappingTheDocumentDropsTheOldSelection()
{
    ImageEditor editor;
    auto *canvas = editor.findChild<ImageCanvas *>();
    QVERIFY(canvas);
    QVERIFY(canvas->selection().isEmpty());

    canvas->setSelection(QRect(1, 1, 4, 4));
    QCOMPARE(canvas->selection(), QRect(1, 1, 4, 4));
    editor.replaceDocument(ImageDocument::create(16, 16, PaletteKind::Stfm));
    QVERIFY2(canvas->selection().isEmpty(),
             "the selection addressed cells of the document that was replaced");

    // The same when the new document comes off disk.
    const QString dir = m_work->path() + QStringLiteral("/swapdoc");
    QVERIFY(QDir().mkpath(dir));
    const QString pim = dir + QStringLiteral("/sprite.pim");
    QString error;
    QVERIFY2(ImageDocument::create(16, 16, PaletteKind::Stfm).save(pim, &error), qPrintable(error));

    canvas->setSelection(QRect(2, 2, 3, 3));
    QCOMPARE(canvas->selection(), QRect(2, 2, 3, 3));
    QVERIFY(editor.loadFile(pim));
    QVERIFY2(canvas->selection().isEmpty(),
             "the selection addressed cells of the document that was closed");
}

void TstGui::undoEditsThePhaseItWasMadeIn()
{
    ImageEditor editor;
    editor.show();
    editor.newDocument(8, 8, PaletteKind::Ste);
    // A second phase sharing the cell size: an undo applied to the wrong
    // phase lands in range and corrupts silently instead of failing loudly.
    QCOMPARE(editor.document().addPhase(QStringLiteral("B"), 8, 8), 1);
    // addPhase leaves the new phase current; draw on A, not on B.
    QVERIFY(editor.document().setCurrentPhase(0));

    auto *canvas = editor.findChild<ImageCanvas *>();
    QVERIFY(canvas);
    const auto idx = [](int x, int y) { return y * 8 + x; };
    const auto cellPoint = [&canvas](int x, int y) {
        const int c = canvas->cellSize();
        return QPoint(x * c + c / 2, y * c + c / 2);
    };
    // A stroke on phase A (a PaintCommand on the undo stack).
    QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(2, 2));
    QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, cellPoint(2, 2));
    const int colour = editor.document().pixels().at(idx(2, 2));
    QVERIFY2(colour != kTransparent, "the stroke painted a colour");

    // A flip (a LayerPixelsCommand on the undo stack): the pixel moves to
    // the mirrored column.
    QVERIFY(QMetaObject::invokeMethod(&editor, "flipHorizontal"));
    QCOMPARE(editor.document().pixels().at(idx(2, 2)), kTransparent);
    QCOMPARE(editor.document().pixels().at(idx(5, 2)), colour);

    // Undo the flip while phase B is current: the command must edit the
    // phase it was made in, not whichever one is on screen.
    QVERIFY(editor.document().setCurrentPhase(1));
    editor.undo();
    QVERIFY(editor.document().setCurrentPhase(0));
    QCOMPARE(editor.document().pixels().at(idx(2, 2)), colour);
    QCOMPARE(editor.document().pixels().at(idx(5, 2)), kTransparent);

    // Undo the stroke too (the PaintCommand half): A returns to its pristine
    // state — pre-fix the flip's pixel survives at (5,2) because the undo
    // landed in B instead.
    QVERIFY(editor.document().setCurrentPhase(1));
    editor.undo();
    QVERIFY(editor.document().setCurrentPhase(0));
    for (int i = 0; i < editor.document().pixelCount(); ++i)
        QCOMPARE(editor.document().pixels().at(i), kTransparent);

    // And phase B was never written to by either undo.
    QVERIFY(editor.document().setCurrentPhase(1));
    for (int i = 0; i < editor.document().pixelCount(); ++i)
        QCOMPARE(editor.document().pixels().at(i), kTransparent);
}

void TstGui::importAdoptsTheFilePalette()
{
    // A PI1 whose 16 registers are deliberately not the editor default:
    // importing must adopt the file's registers, in file order, so the
    // colours its pixels use are not reported as overspill.
    // Stfm, not Ste: a PI1 stores 3-bit words and an Ste palette is not
    // representable in them — MAJ-38 made the codec honest about that, so these
    // two fixtures were only green under the buggy 4-bit words. These indices
    // are valid 3-bit triples: the deliberately non-default palette stays
    // non-default and the round trip is index-exact again.
    ImageDocument art = ImageDocument::create(8, 8, PaletteKind::Stfm);
    QVector<int> custom;
    for (int i = 0; i < 15; ++i)
        custom.append(0x11 * (i + 1));
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
    // The fixture is Stfm and the comparison is on raw cube indices, so the
    // document's kind must match: importFile decodes the file's 3-bit words
    // into the TARGET document's kind.
    editor.newDocument(32, 32, PaletteKind::Stfm);
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

    QVERIFY(!image->findChild<QComboBox *>(QStringLiteral("imagePreviewPhase")));
    auto *list = image->findChild<QListWidget *>(QStringLiteral("imagePhases"));
    auto *picker = image->findChild<QComboBox *>(QStringLiteral("imagePhasePicker"));
    QVERIFY(list && picker);
    list->setCurrentRow(0);
    QCOMPARE(picker->currentIndex(), 0);
    list->setCurrentRow(1);
    QCOMPARE(picker->currentIndex(), 1);
}


void TstGui::editorFindAndReplace()
{
    CodeEditor editor(appearance::editorTheme());
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

// Next and Previous are meant to step from the caret. The hit index they stepped
// from, though, is re-derived from the anchor the search began at whenever the
// document changes: an edit above the hits (a pasted line, a reformat) therefore
// sent Next backwards to where the search opened, and left the "n of m" label
// describing a hit the caret was not on.
void TstGui::findNextFollowsTheCaretAcrossAnEdit()
{
    CodeEditor editor(appearance::editorTheme());
    editor.show();
    editor.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&editor));
    editor.setPlainText(QStringLiteral("move.w d0,d1\nmovem.l d2-d7,-(sp)\nmoveq #0,d0\nMOVE d0,d1\n"));
    editor.showFindBar(false);

    auto *find = editor.findChild<QLineEdit *>(QStringLiteral("editorFindText"));
    auto *status = editor.findChild<QLabel *>(QStringLiteral("editorFindStatus"));
    QVERIFY(find && status);

    find->setText(QStringLiteral("move"));
    QCOMPARE(editor.findMatchCount(), 4);
    editor.findNext();
    editor.findNext();
    QCOMPARE(editor.textCursor().selectionStart(), 33);   // the third hit

    // A hit-bearing line inserted above the caret. The selection is a range in
    // the document, so it rides the edit down with the text it selects.
    QTextCursor c(editor.document());
    c.setPosition(0);
    c.insertText(QStringLiteral("\tMOVE a0,a1\n"));
    QCOMPARE(editor.findMatchCount(), 5);
    const int caret = editor.textCursor().selectionStart();
    QCOMPARE(caret, 45);

    // Next must continue from the caret — not from where the re-run search
    // started — and the label must describe the hit the caret lands on.
    editor.findNext();
    QVERIFY(editor.textCursor().selectionStart() > caret);
    QCOMPARE(editor.textCursor().selectionStart(), 57);   // the fifth hit
    QVERIFY(status->text().contains(QLatin1String("5 of 5")));

    // And Previous walks back to the hit the caret came from.
    editor.findPrevious();
    QCOMPARE(editor.textCursor().selectionStart(), 45);
}

// "1 replaced" is a report on the replace, not the search's position. It used to
// stay up while the user walked the hits, so the label kept announcing a replace
// the caret had long left; the live counter has to come back on navigation.
void TstGui::replaceNoteClearsOnNavigation()
{
    CodeEditor editor(appearance::editorTheme());
    editor.show();
    editor.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&editor));
    editor.setPlainText(QStringLiteral("move.w d0,d1\nmovem.l d2-d7,-(sp)\nmoveq #0,d0\nMOVE d0,d1\n"));
    editor.showFindBar(true);

    auto *find = editor.findChild<QLineEdit *>(QStringLiteral("editorFindText"));
    auto *replace = editor.findChild<QLineEdit *>(QStringLiteral("editorReplaceText"));
    auto *replaceOne = editor.findChild<QPushButton *>(QStringLiteral("editorReplaceOne"));
    auto *status = editor.findChild<QLabel *>(QStringLiteral("editorFindStatus"));
    QVERIFY(find && replace && replaceOne && status);

    find->setText(QStringLiteral("move"));
    replace->setText(QStringLiteral("jump"));
    replaceOne->click();
    QVERIFY(status->text().contains(QLatin1String("1 replaced")));
    QCOMPARE(editor.findMatchCount(), 3);   // the one just written is gone

    // The next navigation is a search step again: the note goes, and the counter
    // for the hit the caret is on takes its place.
    editor.findNext();
    QVERIFY2(!status->text().contains(QLatin1String("replaced")),
             "the replace note belongs to the replace, not to the search");
    QVERIFY(status->text().contains(QLatin1String("of")));
    QCOMPARE(editor.findMatchIndex(), 1);   // "2 of 3"
}

// A common needle in a large document has thousands of hits, and the live list is
// rebuilt on every keystroke and every cursor move. It stops at a cap — and only
// the hits on screen become decorations — while a replace still acts on every
// one of them. The cap must show itself nowhere but the label's "more than", and
// it must not cost a replacement.
//
// Mix of red and guard: the capped count, the "more than" label and the
// viewport-sized decoration list are red pre-fix (the live list held all 2500 and
// decorated all of them); the Replace All half below passed pre-fix too and is
// kept as a guard — it fails if the uncapped rescan behind the replace is
// removed, which is exactly what a capped list would break.
void TstGui::findDecoratesOnlyWhatIsOnScreen()
{
    CodeEditor editor(appearance::editorTheme());
    editor.resize(400, 120);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    QString text;
    for (int line = 0; line < 2500; ++line)
        text += QStringLiteral("x\n");
    editor.setPlainText(text);
    editor.showFindBar(false);

    auto *find = editor.findChild<QLineEdit *>(QStringLiteral("editorFindText"));
    auto *status = editor.findChild<QLabel *>(QStringLiteral("editorFindStatus"));
    QVERIFY(find && status);
    find->setText(QStringLiteral("x"));

    QCOMPARE(editor.findMatchCount(), 2000);
    QVERIFY2(status->text().contains(QLatin1String("more than")),
             "a capped count is a floor, and the label must say so");
    QVERIFY2(editor.extraSelections().size() < 40,
             "only the hits on screen may be turned into decorations");

    // Scrolling brings another window of hits onto the screen, and the
    // decorations are rebuilt from the visible range rather than staying at the
    // top of the document. The scroll has to stay inside the capped list: hits
    // past the cap are not in the live list at all.
    QScrollBar *bar = editor.verticalScrollBar();
    QVERIFY(bar->maximum() > 0);
    bar->setValue(bar->maximum() / 2);
    int firstVisible = 0;
    int lastVisible = 0;
    editor.visibleLineRange(firstVisible, lastVisible);
    QVERIFY2(lastVisible > 100, "the scroll must actually have moved the viewport");

    int decoratedHits = 0;
    for (const QTextEdit::ExtraSelection &sel : editor.extraSelections()) {
        if (!sel.cursor.hasSelection())
            continue;   // the caret-line selection carries no range
        const int line = sel.cursor.blockNumber() + 1;
        QVERIFY2(line >= firstVisible && line <= lastVisible,
                 "a hit is decorated wherever it is, not where it is on screen");
        ++decoratedHits;
    }
    QVERIFY2(decoratedHits > 0, "the scrolled-to window's hits must be decorated");

    // Exactness the cap must not break: Replace All is the whole document's.
    auto *replace = editor.findChild<QLineEdit *>(QStringLiteral("editorReplaceText"));
    auto *replaceAll = editor.findChild<QPushButton *>(QStringLiteral("editorReplaceAll"));
    QVERIFY(replace && replaceAll);
    editor.showFindBar(true);
    replace->setText(QStringLiteral("y"));
    replaceAll->click();
    QVERIFY(!editor.toPlainText().contains(QLatin1Char('x')));
    QVERIFY(status->text().contains(QLatin1String("2500 replaced")));
}

// The gutter carries the viewport's coordinate space, both ways: the paint walks
// blockBoundingGeometry(block).translated(contentOffset()).top() — viewport
// coordinates — and lineAtY() maps a click back through the same numbers. Laid
// out from the raw contents rect while the go-to bar holds the viewport down by
// the bar's height, it sat exactly that much too high: every number (and the
// breakpoint dot, the PC bar, the profiler heat, the blame lane) drew beside the
// line below the one it annotates, so clicking the number the user sees next to
// line N toggled N + (bar height / row height) — a breakpoint armed on the wrong
// line, silently.
void TstGui::gutterClickLandsOnTheLineBesideIt()
{
    CodeEditor editor(appearance::editorTheme());
    QString text;
    for (int line = 1; line <= 40; ++line)
        text += QStringLiteral("line_%1:\tnop\n").arg(line);
    editor.setPlainText(text);
    // Tall enough that the line under test is on screen whatever the editor
    // font works out to be.
    editor.resize(420, 560);
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    editor.showGotoBar();
    QVERIFY(editor.gotoBarVisible());

    auto *gutter = editor.findChild<QWidget *>(QStringLiteral("lineNumberArea"));
    QVERIFY(gutter);

    // The row line 8's text is on — which is the row its number is meant to sit
    // beside, and where the user aims. Taken in viewport coordinates, carried to
    // the screen and back, so the click is the user's point whatever geometry
    // the gutter was given: deriving it from the gutter's own mapping would move
    // the click along with the bug and always "hit".
    const int line = 8;
    const QTextCursor there(editor.document()->findBlockByNumber(line - 1));
    const QRect row = editor.cursorRect(there);
    const QPoint onScreen = editor.viewport()->mapToGlobal(QPoint(0, row.center().y()));
    const int gutterY = gutter->mapFromGlobal(onScreen).y();
    QVERIFY2(gutterY >= 0 && gutterY < gutter->height(),
             "the row under test must be inside the gutter to be clicked");

    QSignalSpy clicks(&editor, &CodeEditor::gutterClicked);
    QTest::mouseClick(gutter, Qt::LeftButton, Qt::NoModifier,
                      QPoint(gutter->width() / 2, gutterY));
    QCOMPARE(clicks.count(), 1);
    QCOMPARE(clicks.at(0).at(0).toInt(), line);
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
    // Open a real file rather than relying on the pristine tab: an untouched
    // tab is REPLACED when the image opens below (addImageTab), and whether the
    // tab is pristine otherwise depends on QSettings left by whichever test ran
    // before — which is how this test flaked.
    const QString source = m_work->path() + QStringLiteral("/search.s");
    QFile src(source);
    QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
    src.write("\ttext\nstart:\trts\n\tend\n");
    src.close();
    window.openPath(source);
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

// Each of these was one 180-line test over nine surfaces: the first failure hid
// every surface after it, and the report could not say which one broke. One test
// per surface, and the stylesheet assertion reads the rule's contract instead of
// its source text (see styleRule).

void TstGui::panelPlaceholdersNameTheNextStep()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    bool sawAddress = false;
    for (QLineEdit *edit : window.findChildren<QLineEdit *>()) {
        QVERIFY(edit->placeholderText() != QLatin1String("00012596"));
        if (edit->placeholderText() == QLatin1String("address"))
            sawAddress = true;
    }
    QVERIFY2(sawAddress, "the memory address field's placeholder is 'address'");

    auto *console = window.findChild<QLineEdit *>(QStringLiteral("consoleInput"));
    QVERIFY(console);
    QVERIFY(console->placeholderText().contains(QStringLiteral("Debugger command")));
    QVERIFY(console->toolTip().contains(QStringLiteral("b breakpoint")));
}

void TstGui::breakpointPanelNamesTheShortcuts()
{
    MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *breakpoints = window.findChild<QDockWidget *>(QStringLiteral("breakpointsDock"));
    QVERIFY(breakpoints);
    breakpoints->show();
    auto *empty = window.findChild<QLabel *>(QStringLiteral("breakpointEmpty"));
    QVERIFY(empty);
    QVERIFY(empty->isVisible());
    QVERIFY(empty->text().contains(QStringLiteral("F8")));
    QVERIFY(empty->text().contains(QStringLiteral("gutter")));
    auto *breakpointTable = breakpoints->findChild<QTableWidget *>();
    QVERIFY(breakpointTable && breakpointTable->isHidden());
}

// The shortcut scheme is a setting the empty panel reads: Common is opt-in, and
// the panel has to name the keys that scheme actually uses.
void TstGui::commonShortcutSchemeRenamesThePanelsKeys()
{
    struct RestoreScheme {
        ~RestoreScheme() { QSettings().remove(QStringLiteral("appearance/shortcuts")); }
    } restoreScheme;
    QSettings().setValue(QStringLiteral("appearance/shortcuts"), QStringLiteral("common"));
    MainWindow commonWindow;
    auto *commonEmpty = commonWindow.findChild<QLabel *>(QStringLiteral("breakpointEmpty"));
    QVERIFY(commonEmpty);
    QVERIFY(commonEmpty->text().contains(QStringLiteral("F9")));
    QVERIFY(!commonEmpty->text().contains(QStringLiteral("F8")));
}

void TstGui::buildAndSearchMenusCarryTheirActions()
{
    MainWindow window;

    auto *buildMenu = window.findChild<QMenu *>(QStringLiteral("buildMenu"));
    auto *runMenu = window.findChild<QMenu *>(QStringLiteral("runMenu"));
    QVERIFY(buildMenu && runMenu);
    QAction *buildAction = nullptr;
    QAction *nextDiagnostic = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->shortcut() == QKeySequence(Qt::Key_F7))
            buildAction = action;
        if (action->shortcut() == QKeySequence(Qt::Key_F4))
            nextDiagnostic = action;
    }
    QVERIFY(buildAction && nextDiagnostic);
    QVERIFY(buildMenu->actions().contains(buildAction));
    QVERIFY(buildMenu->actions().contains(nextDiagnostic));
    QVERIFY(!runMenu->actions().contains(buildAction));

    for (QAction *menu : window.menuBar()->actions()) {
        if (!menu->menu())
            continue;
        const QString title = menu->text().remove(QLatin1Char('&'));
        QVERIFY(title != QLatin1String("Tools"));
        if (title == QLatin1String("Search")) {
            for (QAction *action : menu->menu()->actions())
                QVERIFY(!action->text().contains(QStringLiteral("Diagnostic")));
        }
    }
}

// A build that warns must land in Problems with the colour the theme gives a
// warning, not merely a row.
void TstGui::problemsDockListsABuildWarning()
{
    MainWindow window;
    auto *buildMenu = window.findChild<QMenu *>(QStringLiteral("buildMenu"));
    QVERIFY(buildMenu);
    QAction *buildAction = nullptr;
    for (QAction *action : window.findChildren<QAction *>()) {
        if (action->shortcut() == QKeySequence(Qt::Key_F7))
            buildAction = action;
    }
    QVERIFY(buildAction);

    const QString warnSrc = m_work->path() + QStringLiteral("/warn.s");
    {
        QFile src(warnSrc);
        QVERIFY(src.open(QIODevice::WriteOnly | QIODevice::Text));
        src.write("\ttext\n\tdc.b 1\n\tmove.w d0,d0\n\tend\n");
    }
    window.openPath(warnSrc);
    QSignalSpy built(&window, &MainWindow::buildCompleted);
    buildAction->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(built.count() >= 1, 20000);
    auto *problemsDock = window.findChild<QDockWidget *>(QStringLiteral("problemsDock"));
    QVERIFY(problemsDock);
    auto *problems = qobject_cast<QTreeWidget *>(problemsDock->widget());
    QVERIFY(problems);
    QTreeWidgetItem *warning = nullptr;
    for (int i = 0; i < problems->topLevelItemCount(); ++i) {
        if (problems->topLevelItem(i)->text(2).contains(QStringLiteral("auto-aligned")))
            warning = problems->topLevelItem(i);
    }
    QVERIFY2(warning, "an odd move.w must land in Problems as a warning");
    QCOMPARE(warning->foreground(2).color(), appearance::colors().warning);
    QVERIFY(!warning->icon(0).isNull());
    QCOMPARE(warning->text(1), QStringLiteral("3"));
}

// The chrome sheet is what gives dock separators a grab area. Dark and light
// both carry it; "system" must leave the platform's own look alone.
void TstGui::themeSheetFramesItsDocks()
{
    QSettings settings;
    for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
        settings.setValue(QStringLiteral("appearance/theme"), theme);
        appearance::applyTheme();
        const QString rule =
            styleRule(qApp->styleSheet(), QStringLiteral("QMainWindow::separator"));
        QVERIFY2(!rule.isEmpty(),
                 qPrintable(QStringLiteral("the %1 theme must style the dock separators")
                                .arg(theme)));
        QVERIFY2(declaresPositivePixels(rule, QStringLiteral("width")),
                 qPrintable(QStringLiteral("the %1 separator needs a horizontal grab area: %2")
                                .arg(theme, rule)));
        QVERIFY2(declaresPositivePixels(rule, QStringLiteral("height")),
                 qPrintable(QStringLiteral("the %1 separator needs a vertical grab area: %2")
                                .arg(theme, rule)));
    }

    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("system"));
    appearance::applyTheme();
    QVERIFY2(qApp->styleSheet().isEmpty(),
             "the system theme must leave the platform's own chrome alone");

    // Back to the default for whatever runs next. The store is cleared between
    // tests, but the application palette is process-wide.
    settings.remove(QStringLiteral("appearance/theme"));
    appearance::applyTheme();
}

void TstGui::settingsDialogAndEditorChromeExposeTheirControls()
{
    SettingsDialog dialog{ProjectSettings()};
    auto *sample = dialog.findChild<QLabel *>(QStringLiteral("editorFontSample"));
    QVERIFY(sample);
    QCOMPARE(sample->text(), QStringLiteral("move.w #9,-(a7) ; Cconws"));
    QVERIFY(!sample->font().family().isEmpty());
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("setupToolsButton")));
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("tabWidthValue")));

    ImageEditor image;
    auto *transform = image.findChild<QToolButton *>(QStringLiteral("imageTransform"));
    auto *shift = image.findChild<QAction *>(QStringLiteral("imageShiftLeft"));
    auto *bar = image.findChild<QToolBar *>();
    QVERIFY(transform && transform->menu() && shift && bar);
    QVERIFY(transform->menu()->actions().contains(shift));
    QVERIFY(!bar->actions().contains(shift));
}

void TstGui::hardwareAndDisassemblyViewsNameWhatTheyShow()
{
    HardwareView hardware;
    auto *chips = hardware.findChild<QComboBox *>();
    QVERIFY(chips);
    QCOMPARE(chips->itemText(1), QStringLiteral("mfp"));
    QVERIFY(chips->itemData(1, Qt::ToolTipRole).toString().contains(QStringLiteral("68901")));
    hardware.setInfo(hataritext::parseHardwareInfo(QStringLiteral(
        "Video base : 0x00012596\n"
        "VBL counter : 1\n"
        "HBL line : 0\n"
        "V-overscan : none\n"
        "Refresh rate : 50 Hz\n"
        "Frame skips : 0\n")));
    auto *summary = hardware.findChild<QLabel *>(QStringLiteral("hardwareSummary"));
    QVERIFY(summary);
    QVERIFY(summary->text().contains(QStringLiteral("$00012596")));
    QVERIFY(summary->text().contains(QStringLiteral("50 Hz")));
    QVERIFY(summary->text().contains(QStringLiteral("overscan none")));
    QVERIFY(!summary->text().contains(QStringLiteral("palette")));
    hardware.setInfo(hataritext::parseHardwareInfo(QStringLiteral("MFP registers follow")));
    QVERIFY(summary->text().isEmpty());
    QVERIFY(summary->isHidden());

    DisassemblyView listing;
    MachineState state;
    DisasmLine line;
    line.address = 0x12396;
    line.instruction = QStringLiteral("rts");
    state.disassembly.append(line);
    state.pc = line.address;
    listing.setState(state);
    QSignalSpy disasmSpy(&listing, &DisassemblyView::addressActivated);
    auto *table = listing.findChild<QTableWidget *>();
    QVERIFY(table && table->item(0, 0));
    // Synthetic double-clicks do not reach a table offscreen. The row-to-address
    // mapping is the contract; Qt's gesture delivery is not.
    emit table->cellDoubleClicked(0, 0);
    QCOMPARE(disasmSpy.count(), 1);
    QCOMPARE(disasmSpy.at(0).at(0).toUInt(), 0x12396u);
}

void TstGui::pcHistoryDoubleClickReportsTheAddress()
{
    PcHistoryView history;
    history.setHistory(QStringLiteral("00012396  move.w d0,d1\n"));
    history.resize(420, 160);
    history.show();
    QVERIFY(QTest::qWaitForWindowExposed(&history));
    QSignalSpy historySpy(&history, &PcHistoryView::addressActivated);
    auto *edit = history.findChild<QPlainTextEdit *>();
    QVERIFY(edit);
    const QTextCursor cursor(edit->document()->findBlockByNumber(0));
    const QPoint at = edit->cursorRect(cursor).center();
    QMouseEvent dbl(QEvent::MouseButtonDblClick, at, edit->viewport()->mapToGlobal(at),
                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QVERIFY(QApplication::sendEvent(edit->viewport(), &dbl));
    QCOMPARE(historySpy.count(), 1);
    QCOMPARE(historySpy.at(0).at(0).toUInt(), 0x12396u);
}

void TstGui::floppyRewriteOfAForeignDiskAsks()
{
    const QString dir = m_work->path() + QStringLiteral("/foreign");
    QVERIFY(QDir().mkpath(dir));
    const QString host = dir + QStringLiteral("/new.txt");
    {
        QFile f(host);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("new");
    }

    // A disk with its own boot code: writing to it rebuilds the whole image
    // with PiST's canonical layout, so the browser must ask first, and a "no"
    // must leave the image byte-identical in the parts that matter.
    QVector<floppy::Item> items;
    floppy::Item file;
    file.destPath = QStringLiteral("ONE.TXT");
    file.data = QByteArrayLiteral("one");
    items.append(file);
    const QString image = dir + QStringLiteral("/game.st");
    QString error;
    QVERIFY2(floppy::writeImage(image, items, &error), qPrintable(error));
    QByteArray raw;
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    raw.replace(3, 8, QByteArrayLiteral("GAMEDSK "));
    QVERIFY2(floppy::saveRaw(image, raw, &error), qPrintable(error));

    MainWindow window;
    // The constructor queues "reopen where the user left off" on a zero-delay
    // timer, which loads a project and re-syncs the disk slots from its (empty)
    // settings. Flush that timer now, rather than sleeping on a guess, so it
    // cannot land after the mount and wipe it at the next event loop — here the
    // confirmation dialog's.
    QCoreApplication::processEvents();
    auto *browser = window.findChild<FileBrowser *>();
    QVERIFY(browser);
    browser->setFloppyImages({image, QString()});
    browser->copyHardDrivePaths({host}, false);

    // Decline: nothing is written and the boot code survives.
    QTimer::singleShot(0, browser, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::No)->click();
    });
    QVERIFY(!browser->pasteIntoFloppy(0, QString()));
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    QCOMPARE(raw.mid(3, 8), QByteArrayLiteral("GAMEDSK "));
    QCOMPARE(floppy::listImage(image, &error).size(), 1);

    // Accept: the file lands, and the disk is now PiST's canonical layout —
    // which is exactly what the warning said would happen.
    browser->copyHardDrivePaths({host}, false);
    QTimer::singleShot(0, browser, [] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->click();
    });
    QVERIFY(browser->pasteIntoFloppy(0, QString()));
    const QVector<floppy::Entry> after = floppy::listImage(image, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(after.size(), 2);
    QVERIFY2(floppy::loadRaw(image, &raw, &error), qPrintable(error));
    QVERIFY(floppy::looksLikeCanonical720k(raw.left(512), raw.size()));
}

namespace {

/// The colour the highlighter paints behind character `pos` of block `block`.
///
/// The highlighter applies its formats to the block's *layout*, and only once
/// the document has been laid out — so the widget must be shown and events
/// processed before the ranges are readable. (QSyntaxHighlighter::format(pos) is
/// protected, and measured on Qt 6.10 it returns a default format in this setup
/// even after highlighting; layout()->formats() is the observable that carries
/// the colours.)
QColor highlightColourAt(const QPlainTextEdit &edit, int pos, int block = 0)
{
    const QTextBlock b = edit.document()->findBlockByNumber(block);
    for (const QTextLayout::FormatRange &r : b.layout()->formats()) {
        if (pos >= r.start && pos < r.start + r.length)
            return r.format.foreground().color();
    }
    return QColor();
}

/// One fresh widget per case: setting the same text again on the same document
/// does not re-highlight.
void highlight(QPlainTextEdit *edit, const QString &source)
{
    new AsmHighlighter(edit->document(), appearance::editorTheme()); // owned by the document
    edit->setPlainText(source);
    edit->resize(600, 120);
    edit->show();
    QCoreApplication::processEvents();
    QVERIFY2(!edit->document()->firstBlock().layout()->formats().isEmpty(),
             "the highlighter's formats never reached the layout");
}

/// Counts the paint events a widget receives. The gutter is a plain QWidget, so
/// its repaints are observable only through an event filter.
class PaintCounter : public QObject
{
public:
    int paints = 0;

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint)
            ++paints;
        return false;
    }
};

} // namespace

void TstGui::highlighterColoursMnemonicsOfTheReference()
{
    QPlainTextEdit edit;
    // `blo.s` is the shipped demo's own spelling (demo/hello.s), and the mnemonic
    // family is matched as whole words, so the size suffix is outside the match.
    highlight(&edit, QStringLiteral("\tblo.s\tcount"));
    QCOMPARE(highlightColourAt(edit, 1), appearance::colors().keyword);
    QCOMPARE(highlightColourAt(edit, 4), QColor());   // the `.` of the suffix
}

void TstGui::highlighterColoursTheWholeReferenceAndTheExtras()
{
    // A mnemonic the 68000 reference lists...
    QVERIFY(instructionRef(QStringLiteral("bhs")) != nullptr);
    QPlainTextEdit fromTable;
    highlight(&fromTable, QStringLiteral("\tbhs\tdone"));
    QCOMPARE(highlightColourAt(fromTable, 1), appearance::colors().keyword);

    // ...and one only the 68020+/FPU extras carry, which the reference omits.
    QVERIFY(instructionRef(QStringLiteral("bkpt")) == nullptr);
    QPlainTextEdit fromExtras;
    highlight(&fromExtras, QStringLiteral("\tbkpt\t#1"));
    QCOMPARE(highlightColourAt(fromExtras, 1), appearance::colors().keyword);
}

void TstGui::singleQuotedStringsWinOverTheirContents()
{
    QPlainTextEdit edit;
    const QString source = QStringLiteral("\tdc.b\t'PiST scroll demo'");
    highlight(&edit, source);

    const int quote = source.indexOf(QLatin1Char('\''));
    const int lastQuote = source.lastIndexOf(QLatin1Char('\''));
    const int st = source.indexOf(QLatin1String("ST"));
    QVERIFY(quote > 0 && lastQuote > quote && st > quote);

    // `ST` inside `PiST` is a 68000 mnemonic spelled like a substring, and the
    // string rule is applied after the mnemonic rule, so the string wins.
    QCOMPARE(highlightColourAt(edit, st), appearance::colors().string);
    QCOMPARE(highlightColourAt(edit, quote), appearance::colors().string);
    for (int pos = quote; pos <= lastQuote; ++pos)
        QCOMPARE(highlightColourAt(edit, pos), appearance::colors().string);
}

void TstGui::doubleQuotedStringsStillWinOverTheirContents()
{
    QPlainTextEdit edit;
    const QString source = QStringLiteral("\tdc.b\t\"add 12\"");
    highlight(&edit, source);

    const int add = source.indexOf(QLatin1String("add"));
    const int twelve = source.indexOf(QLatin1String("12"));
    QVERIFY(add > 0 && twelve > add);
    QCOMPARE(highlightColourAt(edit, add), appearance::colors().string);
    QCOMPARE(highlightColourAt(edit, twelve), appearance::colors().string);
}

void TstGui::aSemicolonInsideAStringIsNotAComment()
{
    QPlainTextEdit edit;
    const QString source = QStringLiteral("\tdc.b\t';'");
    highlight(&edit, source);
    const int semicolon = source.indexOf(QLatin1Char(';'));
    QVERIFY(semicolon > 0);
    QCOMPARE(highlightColourAt(edit, semicolon), appearance::colors().string);

    // Control: outside a string the same character is a comment, and a quote
    // inside a comment does not start one.
    QPlainTextEdit control;
    const QString controlSource = QStringLiteral("\tmoveq\t#1,d0\t; note '");
    highlight(&control, controlSource);
    const int controlSemicolon = controlSource.indexOf(QLatin1Char(';'));
    QVERIFY(controlSemicolon > 0);
    QCOMPARE(highlightColourAt(control, controlSemicolon), appearance::colors().comment);
}

void TstGui::anIndentedLabelIsALabel()
{
    QPlainTextEdit edit;
    const QString source = QStringLiteral("\tloop:\tmoveq\t#0,d0");
    highlight(&edit, source);

    const int moveq = source.indexOf(QLatin1String("moveq"));
    QVERIFY(moveq > 0);
    QCOMPARE(highlightColourAt(edit, 1), appearance::colors().label);
    QCOMPARE(highlightColourAt(edit, moveq), appearance::colors().keyword);
}

void TstGui::settingErrorLinesRepaintsTheGutter()
{
    PaintCounter counter;   // declared first, so the editor's filter target dies before it
    CodeEditor editor(appearance::editorTheme());
    editor.resize(520, 220);
    editor.setPlainText(QStringLiteral("start:\tmoveq\t#0,d0\n"
                                       "loop:\taddq.w\t#1,d0\n"
                                       "\tbra.s\tloop\n"
                                       "\trts\n"
                                       "\tend\n"));
    editor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&editor));

    auto *gutter = editor.findChild<QWidget *>(QStringLiteral("lineNumberArea"));
    QVERIFY2(gutter, "CodeEditor must have a lineNumberArea child");
    gutter->installEventFilter(&counter);
    // The harness must be able to see a repaint at all, or every assertion
    // below would pass for the wrong reason.
    counter.paints = 0;
    gutter->repaint();
    QVERIFY2(counter.paints > 0, "paint events never reached the harness");
    const QImage plain = gutter->grab().toImage();

    counter.paints = 0;
    editor.setErrorLines({4});
    QCoreApplication::processEvents();
    QVERIFY2(counter.paints > 0, "marking an error line must repaint the gutter");
    const QImage marked = gutter->grab().toImage();

    // The control step: a breakpoint mark goes through the same path, so a
    // harness that stopped delivering paints fails here.
    counter.paints = 0;
    editor.setBreakpointLines({2});
    QCoreApplication::processEvents();
    QVERIFY2(counter.paints > 0, "marking a breakpoint must repaint the gutter");
    // These two grabs differ only by the breakpoint dot, and that dot is
    // painted as geometry — the breakpoint branch of
    // CodeEditor::lineNumberAreaPaintEvent draws it, not a glyph, exactly so
    // it does not depend on a font — so the check holds even where drawText
    // puts down no pixels at all.
    QVERIFY2(gutter->grab().toImage() != marked,
             "the breakpoint mark must be visible in the gutter");

    counter.paints = 0;
    editor.setErrorLines({});
    editor.setBreakpointLines({});
    QCoreApplication::processEvents();
    QVERIFY2(counter.paints > 0, "clearing the markers must repaint the gutter");
    QCOMPARE(gutter->grab().toImage(), plain);

    // Checked last, so a platform without fonts still runs everything above.
    // The error mark is the glyph "!" (drawText in
    // CodeEditor::lineNumberAreaPaintEvent), and Qt ships no fonts: the
    // offscreen platform on Windows loads none — the QFontDatabase warning
    // this suite prints ("Cannot find font directory .../lib/fonts") is its
    // way of saying so — so drawText paints nothing and `marked` cannot
    // differ from `plain` for any reason this test controls. Where fonts do
    // exist this must still fail if the marker stops being drawn.
    if (QFontDatabase::families().isEmpty())
        QSKIP("no font is available to draw the error marker's '!' glyph "
              "(Qt ships no fonts and the offscreen platform loads none)");
    QVERIFY2(marked != plain, "the error marker must be visible in the gutter");
}

void TstGui::hardwareViewDropsTheTranscriptOfTheSubjectItLeft()
{
    HardwareView hardware;
    auto *chips = hardware.findChild<QComboBox *>();
    auto *text = hardware.findChild<QPlainTextEdit *>();
    auto *summary = hardware.findChild<QLabel *>(QStringLiteral("hardwareSummary"));
    QVERIFY(chips && text && summary);

    // The default subject is video; its transcript is what the host fetches.
    QCOMPARE(hardware.subject(), QStringLiteral("video"));
    hardware.setInfo(hataritext::parseHardwareInfo(QStringLiteral("Video base : 0x00012596\n"
                                    "VBL counter : 1\n"
                                    "Refresh rate : 50 Hz\n")));
    QVERIFY(text->toPlainText().contains(QLatin1String("Video base")));
    // The header is exactly the report's own fields, in the pane's wording —
    // the view parses nothing, so a field the report does not carry (here the
    // overscan) cannot appear and one it does cannot be dropped (MAJ-45).
    QCOMPARE(summary->text(), QStringLiteral("Screen $00012596 · 50 Hz"));

    // No session is running, so nothing re-requests `info mfp`: whatever is on
    // screen after the switch is what the user reads under the new label.
    chips->setCurrentText(QStringLiteral("mfp"));
    QCOMPARE(hardware.subject(), QStringLiteral("mfp"));
    QCOMPARE(text->toPlainText(), QString());
    QCOMPARE(summary->text(), QString());
    QVERIFY(summary->isHidden());
}

void TstGui::settingsDialogKeepsTheChosenRomAcrossAMachineChange()
{
    const QList<TosRom> roms = findTosRoms();
    if (roms.isEmpty())
        QSKIP("no TOS ROM found to choose");

    ProjectSettings settings;
    settings.machine = Machine::St;
    settings.tosPath = roms.first().path;
    SettingsDialog dialog{settings};

    // The dialog's machine and ROM combos, addressed by what they show rather
    // than by name: the machines are listed in full, and the ROM list leads with
    // Automatic.
    QComboBox *machine = nullptr;
    QComboBox *rom = nullptr;
    for (QComboBox *box : dialog.findChildren<QComboBox *>()) {
        if (box->count() == allMachines().size()
            && box->itemText(0) == machineDisplayName(Machine::St))
            machine = box;
        else if (box->itemText(0).startsWith(QLatin1String("Automatic")))
            rom = box;
    }
    QVERIFY2(machine && rom, "the settings dialog must show a machine and a ROM list");
    QCOMPARE(rom->currentData().toString(), roms.first().path);

    // The user picks another ROM than the stored one: the last entry is a plain
    // list choice when there is a second ROM, and Automatic otherwise.
    const QString stored = roms.first().path;
    const bool anotherRom = rom->count() > 1 && rom->itemData(rom->count() - 1).toString() != stored;
    rom->setCurrentIndex(anotherRom ? rom->count() - 1 : 0);
    const QString picked = rom->currentData().toString();
    QVERIFY2(picked != stored, "the pick must differ from the stored ROM, or this proves nothing");
    // The pick is what OK would persist, before anything re-runs the list.
    QCOMPARE(dialog.settings().tosPath, picked);

    // Changing the machine rebuilds the ROM list (onMachineChanged).
    const int machineIndex = machine->currentIndex();
    machine->setCurrentIndex(machineIndex == 0 ? 1 : 0);
    QCOMPARE(rom->currentData().toString(), picked);
    QCOMPARE(dialog.settings().tosPath, picked);

    // Boundary: Automatic (empty data) is a choice too, so the machine change
    // must not resurrect the stored ROM over it.
    rom->setCurrentIndex(0);
    QCOMPARE(dialog.settings().tosPath, QString());
    machine->setCurrentIndex(machine->currentIndex() == 0 ? 1 : 0);
    QCOMPARE(rom->currentData().toString(), QString());
    QCOMPARE(dialog.settings().tosPath, QString());
}

// The Settings dialog must fit a short screen: Windows at 125–150% scaling on
// a 768 px display leaves only about 470 logical pixels of height, and a
// dialog whose minimum demands more than that gets its frame clamped below
// what the layout needs — the OK/Cancel buttons end up under the bottom edge.
// Every relayout then re-applied the over-tall minimum and dragged the window
// around, which is the "jumps around" half of the report.
void TstGui::settingsDialogFitsAShortScreenWithReachableButtons()
{
    SettingsDialog dialog{ProjectSettings()};
    dialog.show();
    QCoreApplication::processEvents();

    // The minimum is bounded by the dialog's own chrome (tab bar, setup
    // button, button box), not by the tallest page's content — comfortably
    // under the height a 150%-scaled laptop has left.
    QVERIFY(dialog.minimumSizeHint().height() <= 320);

    // Shrink the dialog to exactly that minimum: OK must be visible with its
    // whole geometry inside the window, so the buttons stay reachable without
    // resizing anything first.
    dialog.resize(dialog.minimumSizeHint());
    QCoreApplication::processEvents();
    auto *buttons = dialog.findChild<QDialogButtonBox *>();
    QVERIFY(buttons);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    QVERIFY(ok);
    QVERIFY(ok->isVisible());
    const QPoint okTopLeft = ok->mapTo(&dialog, QPoint(0, 0));
    QVERIFY(dialog.rect().contains(okTopLeft));
    QVERIFY(okTopLeft.y() + ok->height() <= dialog.height());

    // Once shown, the content may not move the window: cycling the machine
    // combo rewrites the word-wrapped ROM note, and switching tabs swaps the
    // page behind the size hints. Neither may produce a single resize.
    struct ResizeCounter : QObject {
        int resizes = 0;
        bool eventFilter(QObject *, QEvent *event) override
        {
            if (event->type() == QEvent::Resize)
                ++resizes;
            return false;
        }
    } counter;
    dialog.installEventFilter(&counter);

    // The machine combo, addressed by what it shows: the machines are listed
    // in full, while the CPU combo counts the same but starts with a vasm -m
    // value.
    QComboBox *machine = nullptr;
    for (QComboBox *box : dialog.findChildren<QComboBox *>()) {
        if (box->count() == allMachines().size()
            && box->itemText(0) == machineDisplayName(Machine::St))
            machine = box;
    }
    QVERIFY(machine);
    for (int i = 0; i < machine->count(); ++i) {
        machine->setCurrentIndex(i);
        QCoreApplication::processEvents();
    }

    auto *tabs = dialog.findChild<QTabWidget *>();
    QVERIFY(tabs);
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        QCoreApplication::processEvents();
    }

    dialog.removeEventFilter(&counter);
    QCOMPARE(counter.resizes, 0);
}

// Settings are redirected to a throwaway directory for the whole run, so the
// suite neither reads nor writes a store it shares with previous runs (or with
// the developer): MainWindow restores layout/state from QSettings, the setup
// dialog persists setup/promptDismissed, and the appearance test writes
// appearance/*. Without this they land in ~/.config/Unknown Organization/
// tst_gui.conf — and one setApplicationName("PiST") away from the real thing.
//
// Both calls are needed. setPath only covers the format it is registered for,
// and a default-constructed QSettings uses NativeFormat: the registry on
// Windows, a plist on macOS. Forcing IniFormat is what makes the redirect —
// and initTestCase's assertion on fileName() — hold on every platform.
//
// QStandardPaths::setTestModeEnabled is deliberately NOT used here: it also
// moves the *data* location that toolchain::suggestedInstallDir() and
// paths::suggestedRomDir() derive from, which would hide a vasm or a TOS ROM
// installed through PiST's own setup dialog — and the emulator gate now fails
// rather than skips when PIST_REQUIRE_EMULATOR is set.
int main(int argc, char *argv[])
{
    // Headless with no QT_QPA_PLATFORM, QApplication aborts hard (qFatal) before
    // a single test can skip — which loses the whole binary, not one test. CI
    // sets the variable; a container or a bare `xvfb`-less shell does not, so
    // default to the offscreen plugin when there is no display to connect to.
    // An explicit setting always wins.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")
        && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QTemporaryDir settings;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());

    QApplication app(argc, argv);
    TstGui testCase;
    return QTest::qExec(&testCase, argc, argv);
}

#include "tst_gui.moc"
