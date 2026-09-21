// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/Diagnostic.h"
#include "debug/Breakpoint.h"
#include "debug/Watchpoint.h"
#include "build/LineMap.h"
#include "build/ProgramLineMap.h"
#include "emu/HatariProbe.h"
#include "emu/DebugBackend.h"
#include "emu/MachineState.h"
#include "emu/SessionConfig.h"
#include "project/ProjectSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>

class QTabWidget;

#include <QString>

class QAction;
class QDockWidget;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTreeWidget;

namespace pist {

class BuildService;
class CodeEditor;
class BreakpointPanel;
class InstructionRefView;
class SymbolsView;
class RemoteControl;
class DisassemblyView;
class ProfilerView;
class FileBrowser;
class IDebugBackend;
class ImageEditor;
class MemoryView;
class RegistersView;
class StackView;
class HardwareView;
class EmulatorDisplayWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

public slots:
    /// Open a source file by path, without a dialog. Used for the command line
    /// and for anything that already knows what it wants to open.
    bool openPath(const QString &path);

    /// openPath without the failure modal: the remote `open` verb drives this,
    /// because a modal offscreen (or against an agent with nobody to dismiss
    /// it) blocks the reply forever. Failure is reported by the return value.
    bool openPathQuiet(const QString &path);

    /// Add a memory pane in its own tabbed dock, with a fresh routing tag.
    void addMemoryPane(quint32 initialAddress = 0);

    /// Open the tool/ROM setup dialog. From Project Settings, and at startup
    /// when something is missing.
    void showToolSetup();

    /// GEM-style About box, from the Atari menu.
    void showAbout();

    /// Open the setup dialog only when a required piece is missing. Called
    /// once from main() after the window is shown, so a first run offers the
    /// guided fetch instead of failing the first build with it.
    void showSetupIfNeeded();

    /// Wire the remote-control event sink (main.cpp owns the RemoteControl).
    void setEventSink(RemoteControl *sink) { m_eventSink = sink; }

    /// Re-resolve assembler/emulator paths and the capability probe, updating
    /// the build service and status bar. Called at construction and after the
    /// setup dialog, so a just-installed tool works without a restart.
    void refreshToolchain();

    /// The build & debug console contents, for scripted verification.
    QString debugConsoleText() const;

    /// Send a command typed into the debug console input. The response is
    /// appended to the console log when it arrives (matched by command text).
    void sendConsoleCommand(const QString &command);

    /// The assembler path builds will use, for tests and diagnostics.
    QString assemblerPath() const;

    /// A plain-text snapshot of the machine state (registers, PC, running
    /// state), for the remote-control interface. Read-only.
    QString stateSummary() const;

signals:
    /// The asynchronous build finished, for the remote-control interface to
    /// answer a `build` command. Success carries no diagnostics; a failure is
    /// accompanied by the console text.
    void buildCompleted(bool success);
    /// showProfileResults has populated the profiler (ok) or failed to parse
    /// the save (!ok). The remote `profile stop` waits on this, so a results
    /// call right after it can never see the previous run's data.
    void profileResultsReady(bool ok);

    /// The emulator session started or stopped, so the remote-control interface
    /// can answer `run` when the session is actually up rather than merely
    /// requested.
    void sessionRunningChanged(bool running);

    /// A response to an arbitrary debugger command sent via debugCommand.
    void debugCommandFinished(const QString &command, const QString &response);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void showDockMoveMenu(QDockWidget *dock, const QPoint &globalPos);

    /// The dock a title-bar/tab press targets, or null when the press is on a
    /// dock's content (or on no dock). The default title bar is painted by the
    /// dock rather than being a child widget, and a tabbed dock has no title
    /// bar at all, so this matches a dock tab's text to a title and treats a
    /// press inside a dock but outside its content as a title-bar press.
    QDockWidget *dockAtPress(QWidget *pressed, const QPoint &globalPos);

    /// While a title-bar/tab drag may be in progress, make the embedded
    /// emulator's foreign window input-transparent so the drag keeps tracking
    /// across the video; restore it when the press is released.
    void setDragVideoPassthrough(bool on);

private slots:
    void openFile();
    void saveFile();

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void openProject();
    void saveProject();
    void editSettings();
    void build();
    void run();
    void launchEmulator();
    void stopSession();
    void step();
    void stepOver();
    /// Step out of the current subroutine: one-shot breakpoint at the return
    /// address read from the stack, then resume (no Hatari primitive exists).
    void stepOut();
    /// One-shot breakpoint at the cursor line's code address, then resume.
    void runToCursor();
    /// Profiling is armed/collected while stopped: Hatari starts collection on
    /// continue and zeroes it if any breakpoint is armed mid-run, so both
    /// actions refuse a running machine.
    void profileStart();
    bool profileStop();
    /// Enable the profile actions from the session/profiling state: Start and
    /// Profile to cursor need stopped-and-not-profiling, Stop and Show needs
    /// stopped-and-profiling. Called on stoppedChanged and every state change.
    void syncProfileActions();
    /// The guided profile: arm a one-shot at the cursor line, start collecting
    /// and resume — the results show themselves on the stop. Composes with the
    /// user's breakpoints; an earlier stop ends the run with partial results.
    void profileToCursor();
    void showProfileResults();
    /// F4 / Shift+F4: step through the Problems pane without the mouse,
    /// wrapping, skipping diagnostics that carry no source line.
    void nextDiagnostic();
    void previousDiagnostic();
    void stepDiagnostic(int direction);
    void pauseSession();
    void resume();

    void removeBreakpoint(const QString &file, int line);
    void goToBreakpoint(const QString &file, int line);
    bool toggleBreakpointAtLine(int line);

    void editBreakpointCondition(int line);
    void clearAllBreakpoints();
    /// The breakpoints dock's "Clear all": removes breakpoints AND watchpoints
    /// (its button enables when only watchpoints are present), unlike the Run-menu
    /// "Clear Breakpoints" action, which stays breakpoint-only.
    void clearAllDebugTargets();
    void addWatchpoint();
    void removeWatchpoint(int index);

public:
    /// Write a memory byte through the debugger (`w b <addr>=<value>`), when
    /// stopped. Shared by the memory views' editing and the remote `setmem`.
    /// `pane` is the memory pane to refresh after the write; null (the remote
    /// path) refreshes the first pane.
    bool setMemoryByte(quint32 address, quint32 value, MemoryView *pane = nullptr);

    /// Send an arbitrary debugger command (for the remote `cmd`). The response
    /// arrives via debugCommandFinished.
    void debugCommand(const QString &command);

    /// Write a register through the debugger (`r <reg>=<value>`), when stopped.
    /// Shared by the register view's editing and the remote-control `setreg`
    /// command. Returns false (and logs) when the machine is not stopped.
    bool setRegister(const QString &regName, quint32 value);

    /// Whether the current editor's file has an instruction at `line`,
    /// according to the program map. True when there is no map yet — arming
    /// will decide then, so "can't tell" must not read as "cannot fire".
    /// For the remote `breakpoint` reply.
    bool lineHasCode(int line) const;

    /// Toggle a breakpoint at (file, line) directly — the base-name keying the
    /// whole IDE uses. Shared by toggleBreakpointAtLine (current editor) and
    /// toggleBreakpointAtLabel (any module).
    bool toggleBreakpoint(const QString &file, int line);

    /// Toggle a breakpoint at a symbol's definition. `detail` carries the
    /// reply text either way: "ok <file>:<line> [= 0x…]" or an "error …"
    /// naming why (unknown name, or a position-less symbol). For the remote
    /// `breakpoint <label>` form.
    bool toggleBreakpointAtLabel(const QString &name, QString *detail);
    /// The current editor document as JSON {path, text}, for the remote
    /// `read` verb — what the IDE is showing, so an agent needn't guess.
    /// JSON rather than raw text because the block protocol terminates on a
    /// lone `.` line, which assembly source can legitimately contain.
    /// Empty object when no source is open.
    QJsonObject documentJson() const;

    /// Machine state as JSON: {running, stopped}, plus pc/d0-7/a0-7/sr when a
    /// register batch has landed. For the remote `statejson` verb, which the
    /// MCP shim serves as structured content.
    QJsonObject stateJson() const;

    /// The Problems pane as JSON objects {file, line, message}, for the
    /// remote `problems` verb.
    QJsonArray problemsJson() const;

    /// Every open document as JSON objects {path, modified, current}, for the
    /// remote `tabs` verb — the agent's map of what the IDE has open.
    QJsonArray tabsJson() const;

    /// Save the current document without a dialog. False when it has no path
    /// yet (a name can't be chosen remotely) or the write failed. For the
    /// remote `save` verb.
    bool saveCurrentDocument();

    /// Profiler hot lines as JSON objects {line, count}, sorted by descending
    /// count, for the remote `profile results` verb. Empty when no profile
    /// has been collected.
    QJsonArray profilerResultsJson() const;

    /// The build's symbols as JSON objects {name, file?, line?, address?},
    /// optionally name-filtered (case-insensitive). Addresses appear only once
    /// the program map has live bases — SymbolsView's honesty rule. For the
    /// remote `symbols` verb.
    QJsonArray symbolsJson(const QString &filter) const;

    /// Parse and add a watchpoint by address text (e.g. "$12345" or "$12345.l").
    /// Separated from the dialog so the remote-control interface and tests can use
    /// it without a prompt. Returns false and sets error on a bad address.
    bool addWatchpointAddress(const QString &text, QString *error);

    /// Compose the current phase's sheet and write it to `path`, writing
    /// back into a mounted floppy when the sheet came from one.
    bool exportSpriteSheetTo(const QString &path);

    /// Open an entry from a mounted floppy image (the file browser's
    /// double-click). Text opens in a text tab; a still image (.pi1, .neo,
    /// .iff, .png, .pim) opens as an image tab over the imported sheet. The
    /// entry is extracted to a file in the session directory, and saving that
    /// tab writes it back into the image. Returns the tab, or null when the
    /// entry cannot be opened (no disk, not text, read error).
    QWidget *openFloppyEntry(int drive, const QString &entryPath);

private:
    void createActions();
    void createMenus();
    void createDocks();
    void createToolBar();
    void createStatusBar();

    /// Session chip: Not running, Running, or Stopped — file:line. The Hatari
    /// capability probe lives on the chip's tooltip.
    void updateSessionChip();

    /// Caret chip: file:line:column. While stopped, a caret that has moved off
    /// the execution line shows both ("caret 22 · PC 18").
    void updateCaretChip();
    void updateRegisterStrip();

    /// Point the instruction reference, and the one-line strip under the
    /// editor, at the word under the caret. Runs whether or not the
    /// Instructions dock is the tab on top.
    void followCursorReference(CodeEditor *editor);

    /// Bring the Instructions dock forward. The strip under the editor does this.
    void raiseInstructionRef();

    /// View → Layout. Saves the current arrangement first so Restore my
    /// layout can undo it. The embed checkbox stays the owner of the
    /// emulator dock.
    void applyLayoutPreset(const QString &preset);
    void restorePreviousLayout();

    /// Open `file` (already open, or found beside the project) and move the
    /// caret to `line`. A missing file is reported on the status bar.
    bool navigateToSourceLine(const QString &file, int line);
    QString resolveNavigablePath(const QString &file) const;

    /// Connect every backend signal to its handler. Runs once at construction
    /// and again whenever the selected debug transport changes (a project can
    /// switch between the native and HRDB backends), so it must be safe to
    /// call on a freshly created backend.
    void wireBackend();

    /// Reset everything that must not leak from one debug session into the
    /// next: the arming state and resolved bases (GEMDOS relocates the
 /// program on every run), the cached machine state (the remote `state`
    /// command must not answer for a dead session) and any pending remote
    /// command. Called at launch and again when the session ends, so a
    /// session's state has exactly one lifetime and one owner.
    void resetSessionState();

    /// Report a build that refused to start (no source, failed save, missing
    /// linker): a refusal is still a finished build as far as callers are
    /// concerned — run()'s launch intent must be dropped rather than left
    /// armed for the next successful build, and a remote-control `build`
    /// must be answered now, not after its timeout.
    void refuseBuild(const QString &title, const QString &reason, bool critical);

    /// Load the project settings that sit beside a source file, if any.
    void loadProjectForSource(const QString &sourcePath);

    /// Persist `m_settings` beside the current source, if a source is known.
    void persistSettings();

    /// Keep the project-files pane's Disk A/B groups in line with settings.
    void syncFileBrowserDisks();

    /// Shared tail of the two export actions.
    void exportImageWith(bool spriteSafe);

    /// A text document opened from a floppy image: where the extracted file
    /// lives on the host, and which image entry it writes back to on save.
    struct FloppyDoc {
        QString imagePath;
        QString entryPath;
    };

    /// The deterministic session file for an image entry, so reopening an
    /// entry raises its existing tab instead of extracting a second copy.
    QString extractedFloppyPath(const QString &imagePath, const QString &entryPath) const;

    /// After a successful save of an extracted floppy document, write the
    /// text back into its image and refresh the pane. No-op for other paths.
    void writeBackFloppyDoc(const QString &path);

    /// Update the title and the Save action to reflect the modified state.
    void updateModifiedState();

    /// Ask to save when any open document has unsaved work. Returns false if
    /// the user cancelled, in which case the caller must abandon its action.
    bool maybeSave();

    /// The single-document form, for closing one tab.
    bool maybeSaveEditor(CodeEditor *editor);
    bool maybeSaveImage(ImageEditor *editor);

    /// Create and wire a text-editor tab, loading `path` into it; empty means a
    /// pristine tab. Reuses a pristine tab when there is exactly one.
    /// Returns null when the file could not be loaded.
    CodeEditor *addEditorTab(const QString &path, bool quiet = false);

    /// Create and wire an image-editor tab. `path` is a `.pim` or an importable
    /// ST still-image; empty means a new untitled sprite.
    ImageEditor *addImageTab(const QString &path, bool quiet = false);

    /// Shared body of openPath (interactive) and openPathQuiet (remote): the
    /// quiet form returns false where the interactive one shows a modal.
    bool openPathImpl(const QString &path, bool quiet);

    /// Connect one editor's signals. Runs for every editor tab created.
    void wireEditor(CodeEditor *editor);
    void wireImage(ImageEditor *editor);
    void wireImageReExport(ImageEditor *editor);

    /// Every open text editor, in tab order.
    QList<CodeEditor *> openEditors() const;
    QList<ImageEditor *> openImages() const;

    /// The open editor showing `path`, or null.
    CodeEditor *editorForPath(const QString &path) const;
    ImageEditor *imageForPath(const QString &path) const;

    /// Sync a tab's label with its document's name and modified state.
    void updateTabTitle(QWidget *widget);

    /// Assembly source used for Build/Run: the current text editor if it has a
    /// path, otherwise the project source, otherwise any open `.s`.
    QString buildSourcePath() const;

    void newImage();
    void newImageIn(const QString &directory);
    QString suggestedImageDirectory() const;
    void importImage();
    void exportImage();
    void exportImageSpriteSafe();
    void exportSpriteSheet();
    /// Ask which blocks a `.dat` should hold, then write them for the current
    /// frame of the focused sprite tab.
    void exportBitplaneData();
    /// Find / find-and-replace in the focused text editor, from the Search menu
    /// (the editor owns the bar; these follow the current tab).
    void showFindBar();
    void showReplaceBar();
    void findNextInEditor();
    void findPreviousInEditor();

    void openRecentSource();

    void onBuildFinished(bool success, const QList<pist::Diagnostic> &diagnostics);
    void onStateUpdated(const pist::MachineState &state);
    void locationFromPc(quint32 pc);

    /// Called once the debugger stops. This is the point at which the program's
    /// load address is known, so breakpoints can finally be resolved and armed.
    void onDebuggerStopped();
    void armBreakpoints();

    /// Rebuild the program's source mapping from the listings this build
    /// produced, and load the linker's placement map when there was a link.
    void rebuildProgramMap();

    /// Fold the addresses the ArmPlan resolved back into the stored breakpoint
    /// list, so the panel shows where each one actually landed.
    QList<Breakpoint> mergeResolved(const ArmPlan &plan) const;
    void refreshBreakpointMarkers();

    /// Whether the emulator's display can be embedded in this session: it needs
    /// PiST to be an X11 (xcb) client, and the control socket that carries the
    /// video-size report. When false the option is disabled and the emulator
    /// runs as a separate window regardless of the setting.
    bool canEmbedDisplay(const HatariCapabilities &caps) const;

    /// The embedded/separate display preference. Applies on the next Run; a
    /// running session keeps the mode it was launched with. Persisted as an
    /// application setting, because it is a view choice, not a project one.
    void setDisplayEmbedded(bool on);
    void updateEmbedActionState();

    /// Per-session working directory, kept short so the control socket path fits
    /// in sockaddr_un::sun_path.
    QString makeSessionDir();

    /// Create a dock with the shared Photoshop-style setup: movable, floatable,
    /// closable, and a stable objectName for layout persistence.
    QDockWidget *makeDock(const QString &title, const QString &objectName, QWidget *widget);

    /// Put `dock`'s show/hide action on the View menu, with the other dock
    /// toggles and above Reset layout. A no-op until that section exists, and
    /// a no-op when the action is already there — the first memory pane is
    /// created before the section, and the walk at the end of createDocks
    /// lists it.
    void addDockToViewMenu(QDockWidget *dock);

    /// Restore the factory dock layout (the default tab groupings), discarding
    /// the user's current arrangement. Invoked from the View menu.
    void resetToDefaultLayout();

    /// Widths of a first run: the editor keeps about 60% of a 1280-wide
    /// window. The bottom group keeps its size hint, a short strip.
    /// Captured into the factory state so Reset layout returns to it.
    void applyFactoryDockSizes();

    /// Size the window (restored geometry, or 1280×860), capture the factory
    /// dock state, then put a saved arrangement back if there is one.
    void finalizeLayout();

    /// Re-apply theme, icons, and monospace fonts after appearance preferences
    /// change, and once at construction so the first window is already themed.
    void applyAppearance();
    void applyIcons();

    /// PiST or Common debug keys. F5 is shared by Run and Continue in the
    /// Common scheme, so only the one that should fire currently holds it.
    void applyShortcutScheme();
    void updateRunContinueShortcut();
    void refreshToolbarStatusTips();
    void toggleBreakpointAtCaret();

    /// The open documents. m_editor is the *current* text editor and is null
    /// when the current tab is not a text editor; m_image is the current image
    /// editor and is null otherwise.
    QTabWidget *m_tabs = nullptr;
    CodeEditor *m_editor = nullptr;
    ImageEditor *m_image = nullptr;
    BuildService *m_build = nullptr;


    /// The most recent machine state, kept so the remote-control interface can
    /// answer `state` without touching the emulator.
    MachineState m_lastState;
    /// The dock and widget that host the emulator's display in embedded mode.
    /// The dock is hidden in separate-window mode.
    QDockWidget *m_displayDock = nullptr;
    EmulatorDisplayWidget *m_display = nullptr;

    /// The embedded/separate display preference. Persisted via QSettings.
    bool m_embeddedDisplay = false;

    /// True while the embedded video is input-transparent for an in-progress
    /// title-bar/tab drag, so the restore only runs once and only when needed.
    bool m_dragVideoPassthrough = false;

    /// The factory dock arrangement, captured after the default tab groupings are
    /// applied, so "Reset layout" can restore it. Saved/restored via QSettings.
    QByteArray m_defaultLayoutState;

    /// True until the first show of a window that has no saved arrangement.
    /// Dock sizes set before the window is on screen do not stick.
    bool m_applyFactorySizes = false;

    /// Client size saved alongside saveGeometry. restoreGeometry returns
    /// success and then still adjusts the width, so the explicit size is
    /// reapplied on show.
    QSize m_restoredSize;

    /// The View menu, kept so the per-dock show/hide actions can be appended once
    /// the docks exist (menus are created before docks).
    class QMenu *m_viewMenu = nullptr;
    QAction *m_restoreLayoutAction = nullptr;
    HatariCapabilities m_caps;
    IDebugBackend *m_host = nullptr;

    /// The Problems pane's contents as structured data — file, line, message
    /// and severity, with linker offsets already resolved to source lines, so
    /// the remote `problems` verb reports what the pane shows rather than
    /// re-deriving it. The widget shows the same severity as a coloured
    /// square on the file and a tint on the message.
    struct ProblemEntry
    {
        QString file;
        int line = 0;
        QString message;
        bool error = false;
    };
    QList<ProblemEntry> m_problemEntries;
    DisassemblyView *m_disassembly = nullptr;
    RegistersView *m_registers = nullptr;
    MemoryView *m_memory = nullptr;
    class ConsoleInput *m_consoleInput = nullptr;

    /// Every open memory pane, keyed by the dump-routing tag each one carries, so
    /// a dump is routed back to the pane that asked for it. m_memory is the first
    /// pane (tag 0) and drives the auto-refresh / first-stop navigation.
    QHash<int, MemoryView *> m_memoryPanes;
    QDockWidget *m_memoryDock = nullptr;
    int m_nextMemoryTag = 0;

    StackView *m_stack = nullptr;
    class PcHistoryView *m_pcHistory = nullptr;
    HardwareView *m_hardware = nullptr;
    class SymbolsView *m_symbolsView = nullptr;
    BreakpointPanel *m_breakpointPanel = nullptr;
    class InstructionRefView *m_instrRef = nullptr;
    /// One line under the source editor: mnemonic and summary, or an OS call
    /// with its stack. Hidden on an image tab. Clicking it raises Instructions.
    QPushButton *m_instrStrip = nullptr;
    /// Two lines of registers, shown under the editor while the machine is
    /// stopped. The Registers dock keeps the full editable table.
    QLabel *m_registerStrip = nullptr;
    FileBrowser *m_fileBrowser = nullptr;
    QVector<struct SymbolEntry> m_symbols;
    QStringList m_consoleVerbs;
    /// Where session events are published for remote-control watchers
    /// (control/RemoteControl); null when no one wired one up (tests).
    RemoteControl *m_eventSink = nullptr;
    class ProfilerView *m_profiler = nullptr;
    QDockWidget *m_profilerDock = nullptr;
    /// A `profile save` is in flight; its commandFinished parses the file.
    bool m_profileSavePending = false;
    /// Hatari is collecting profile counts (set by Profile Start / Profile to
    /// cursor, cleared by Profile Stop and session end). Drives which of the
    /// three profile actions make sense right now.
    bool m_profilingActive = false;
    /// A "profile to cursor line" run is collecting; the next stop saves and
    /// shows, and deletes the one-shot if some other stop won the race.
    bool m_profileGuided = false;
    quint32 m_profileGuidedAddr = 0;
    /// A stop was announced; the next state batch carries its PC, so the
    /// stopped event is published from onStateUpdated with the detail filled.
    bool m_stopEventPending = false;
    QPlainTextEdit *m_log = nullptr;
    QTreeWidget *m_problems = nullptr;
    QDockWidget *m_problemsDock = nullptr;

    /// Text documents currently edited out of a floppy image, keyed by the
    /// extracted session file the editor tab holds open.
    QHash<QString, FloppyDoc> m_floppyDocs;

    QLabel *m_statusToolchain = nullptr;
    QLabel *m_statusSession = nullptr;
    QLabel *m_statusBuild = nullptr;
    QLabel *m_statusCaret = nullptr;

    /// Source location of the current stop, for the session chip. Empty when
    /// the PC has not resolved to a line.
    QString m_stoppedFile;
    int m_stoppedLine = 0;

    /// Source mapping for the whole program. Replaces a single LineMap because a
    /// linked program has one listing per module, each needing its own base.
    ProgramLineMap m_programMap;
    LineMap::SectionBases m_bases;

    /// Breakpoints are stored per file:line, never per address: the program is
    /// relocated by GEMDOS on every run (docs/PLAN.md §5 rule 6).
    QList<Breakpoint> m_breakpoints;

    /// Address watchpoints: break when a memory value changes (see
    /// debug/Watchpoint.h). Armed and cleared alongside the source breakpoints.
    QList<Watchpoint> m_watchpoints;

    /// Set once the entry stop has been handled for the current session, so the
    /// arming sequence runs exactly once.
    bool m_sessionArmed = false;


    /// The debugger command a remote `cmd` is waiting on, so its response is
    /// routed to debugCommandFinished.
    QString m_pendingDebugCommand;

    /// Set once breakpoints have been armed against the live bases this session.
    /// Arming has to wait for the basepage response (not a timer, which always
    /// loses that race), so this guards doing it exactly once, when they arrive.
    bool m_breakpointsArmedThisSession = false;

    /// A stack dump requested by stepOut() is in flight; the next
    /// stackDumpReady arms the return-address breakpoint instead of only
    /// feeding the stack view.
    bool m_stepOutPending = false;

    /// Set by Run, consumed by onBuildFinished. Needed because the build is
    /// asynchronous: the launch has to wait for it, not run alongside it.
    bool m_launchAfterBuild = false;

    /// Session directory of the current (or most recent) run, so it can be
    /// removed when the session ends instead of accumulating in the temp
    /// location. See paths::removeSessionDir.
    QString m_currentSessionDir;

    /// Project configuration. Persisted to a `.pistproject` file beside the
    /// source, so settings travel with the project (docs/PLAN.md §5 rule 7 —
    /// never via a user's hatari.cfg).
    ProjectSettings m_settings;

    QAction *m_actOpen = nullptr;
    QAction *m_actNewFile = nullptr;
    QAction *m_actNewImage = nullptr;
    QAction *m_actImportImage = nullptr;
    QAction *m_actExportImage = nullptr;
    QAction *m_actExportImageSafe = nullptr;
    QAction *m_actExportSpriteSheet = nullptr;
    QAction *m_actExportBitplanes = nullptr;
    QAction *m_actReExportBitplanes = nullptr;
    QAction *m_actFind = nullptr;
    QAction *m_actFindNext = nullptr;
    QAction *m_actFindPrevious = nullptr;
    QAction *m_actReplace = nullptr;
    QAction *m_actOpenProject = nullptr;
    QAction *m_actProfileStart = nullptr;
    QAction *m_actProfileStop = nullptr;
    QAction *m_actProfileToCursor = nullptr;
    QAction *m_actSaveProject = nullptr;
    QAction *m_actSettings = nullptr;
    QAction *m_actSave = nullptr;
    QAction *m_actBuild = nullptr;
    QAction *m_actRun = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actEmbedDisplay = nullptr;
    QAction *m_actNextDiagnostic = nullptr;
    QAction *m_actPrevDiagnostic = nullptr;
    QAction *m_actStep = nullptr;
    QAction *m_actStepOver = nullptr;
    QAction *m_actStepOut = nullptr;
    QAction *m_actRunToCursor = nullptr;
    QAction *m_actResume = nullptr;
    QAction *m_actClearBreakpoints = nullptr;
    QAction *m_actToggleBreakpoint = nullptr;
    QAction *m_actGotoLine = nullptr;
    QAction *m_actPause = nullptr;
    QAction *m_actAddWatchpoint = nullptr;
};

} // namespace pist
