// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/Diagnostic.h"
#include "build/LineMap.h"
#include "build/ProgramDiff.h"
#include "build/ProgramLineMap.h"
#include "control/ControlHost.h"
#include "editor/CompletionModel.h"
#include "editor/CursorReference.h"
#include "emu/HatariProbe.h"
#include "emu/DebugBackend.h"
#include "emu/MachineState.h"
#include "emu/SessionConfig.h"
#include "project/ProjectSettings.h"
#include "ui/RemoteStateAdapter.h"
#include "ui/SymbolPaletteDialog.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QImage>
#include <QMainWindow>

class QTabWidget;
class QStackedWidget;

#include <QString>

class QAction;
class QDockWidget;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;

#include <functional>

namespace pist {

class BuildService;
class BreakpointWatchpointModel;
class CodeEditor;
class BreakpointPanel;
class DebugSessionController;
class DocumentWatcher;
class ProfilerController;
class SessionLauncher;
class StopHookRunner;
class InstructionRefView;
class SymbolsView;
class RemoteControl;
class DisassemblyView;
class ProfilerView;
class FileBrowser;
class FindInFilesView;
class GitPanel;
class IDebugBackend;
class ImageEditor;
class MemoryView;
class AnalysisView;
class BinaryView;
class RegistersView;
class StackView;
class HardwareView;
class EmulatorDisplayWidget;
class EmuAudio;
class WelcomeView;

/// The IDE's main window. It is the shell: it owns the docks, the tabs and the
/// menus, and it is the one place the modules meet.
///
/// The MAJ-41 seams below (the session controller, the launcher, the
/// breakpoint and watchpoint models, the profiler and the remote-state
/// adapter) are constructed here and handed *only the operations they perform
/// on this window*, as their own `Host` structs of callbacks (MIN-89). They
/// used to reach the private members directly through granted access; a seam
/// now compiles against its collaborator list alone, which is what the
/// declarations below and the host aggregates in the constructor spell out.
///
/// MainWindow is also the control layer's `ControlHost` (control/ControlHost.h):
/// the remote-control verbs are its own methods, so the protocol drives the IDE
/// through an interface the control module owns rather than through this
/// header (MIN-86).
class MainWindow : public QMainWindow, public control::ControlHost
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    /// The tag the remote-control `readmem` verb's dump carries. Memory panes
    /// are tagged from 0 up (addMemoryPane), so a negative tag can never belong
    /// to one: the dump is answered to the verb that asked instead of being
    /// applied to a view.
    static constexpr int kRemoteReadTag = -1;
    /// A memory-dock "Save bytes…" dump. Like kRemoteReadTag it belongs to no
    /// pane: the bytes are written to the path the user chose.
    static constexpr int kSaveBytesTag = -2;

public slots:
    /// Open a source file by path, without a dialog. Used for the command line
    /// and for anything that already knows what it wants to open.
    bool openPath(const QString &path);
    bool openDiskHit(const QString &imagePath, const QString &entryPath, int offset,
                     const QString &kind, QString *error);

    /// openPath without the failure modal: the remote `open` verb drives this,
    /// because a modal offscreen (or against an agent with nobody to dismiss
    /// it) blocks the reply forever. Failure is reported by the return value.
    bool openPathQuiet(const QString &path) override;

    /// Open the bundled demo project — the tour of what the IDE does, written as a
    /// program that uses it (labels to complete and jump to, TOS calls to hover, a
    /// word to watch by name, a stop hook that logs every stop). The action is
    /// disabled, and this does nothing, when no demo is reachable: an installed tree
    /// without one, or a checkout with `demo/` moved.
    void openDemoProject();

    /// The first-run doorway: open the demo when the window is still empty — the
    /// app was started with no file and there was nothing to reopen. Called once by
    /// `main()`, queued after the window's own start-up step, which is what reopens
    /// the last document. A window built by a test never calls it, which is why the
    /// policy lives here rather than in the window's constructor.
    void openDemoIfIdle();

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

    /// Typed debugger reads for the remote-control interface, whose `readmem`
    /// and `disasm` verbs return the answer itself rather than filling a pane
    /// or the snapshot. A memory read's rows arrive on
    /// debugReadMemoryFinished, a disassembly's text on debugReadFinished, each
    /// with the address that was asked about.
    void debugReadMemory(quint32 address, int length) override;
    void debugReadDisassembly(quint32 address) override;

    /// The assembler path builds will use, for tests and diagnostics.
    QString assemblerPath() const;

    /// A plain-text snapshot of the machine state (registers, PC, running
    /// state), for the remote-control interface. Read-only.
    QString stateSummary() const override;

public:
    /// The remote `setmem`: the memory write with no pane to refresh, since
    /// the caller has no view. The pane-aware setMemoryByte below is the same
    /// write for the memory views.
    bool writeMemoryByte(quint32 address, quint32 value) override;

    /// The remote `screenshot`: raise the window, capture it (a media
    /// session's frame comes from the framebuffer, the emulator having no
    /// window) and save it to `path`. False when the capture or the write
    /// failed.
    bool saveScreenshot(const QString &path) override;
    bool ripImage(const QString &path, quint32 offset, int width, int height,
                  const QString &outPath, bool hasPalette, quint32 paletteOffset,
                  QString *error) override;
    bool ripSample(const QString &path, quint32 offset, int length, int rate,
                   bool signedSamples, const QString &outPath, QString *error) override;
    bool writeBinaryFile(const QString &path, const QByteArray &bytes, QString *error) override;
    bool writeCapturedScreen(const QString &outPath, const QByteArray &resolutionByte,
                             const QByteArray &paletteWords, const QByteArray &planes,
                             QString *error) override;
    bool writeCapturedDma(const QString &outPath, int rateHz, const QByteArray &samples,
                          QString *error) override;
    bool captureScreenTo(const QString &outPath, QString *error) override;
    bool captureDmaTo(const QString &outPath, QString *error) override;
    bool ripAvr(const QString &path, quint32 offset, const QString &outPath, QString *error) override;
    QJsonArray scanFileMagic(const QString &path, QString *error) override;
    QJsonArray scanDiskMagic(const QString &imagePath, QString *error) override;
    QString identifyPacker(const QString &path, quint32 offset, QString *error) override;
    QJsonObject sndhInfo(const QString &path, quint32 offset, QString *error) override;

    // The control layer's event subscriptions (control/ControlHost.h): each is
    // the QObject::connect MainWindow's own signal would take, with `context`
    // as the connection's owner, returned so the waiter can disconnect. The
    // signals themselves stay on this class — the IDE's other code connects to
    // them directly, and these hooks exist so the control layer never has to
    // name a MainWindow to do the same.

    /// The asynchronous build finished.
    QMetaObject::Connection onBuildCompleted(QObject *context,
                                             std::function<void(bool)> handler) override;
    /// The emulator session started or stopped.
    QMetaObject::Connection onSessionRunningChanged(QObject *context,
                                                    std::function<void(bool)> handler) override;
    /// A profile save was parsed, or could not be.
    QMetaObject::Connection onProfileResultsReady(QObject *context,
                                                  std::function<void(bool)> handler) override;
    /// A debugger command's response arrived.
    QMetaObject::Connection onDebugCommandFinished(
        QObject *context, std::function<void(const QString &, const QString &)> handler) override;
    /// A typed read's answer arrived, with the address it was asked about.
    QMetaObject::Connection onDebugReadFinished(
        QObject *context, std::function<void(quint32, const QString &)> handler) override;
    QMetaObject::Connection onDebugReadMemoryFinished(
        QObject *context, std::function<void(quint32, const QList<MemoryRow> &)> handler) override;

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

    /// The answer to a typed remote read (debugReadMemory/debugReadDisassembly),
    /// carrying the address it was asked about so a late answer cannot satisfy
    /// the next request.
    ///
    /// The two reads report differently because what they asked for differs: a
    /// disassembly is text, a memory dump is the rows the backend parsed from
    /// it (MAJ-45).
    void debugReadFinished(quint32 address, const QString &response);
    void debugReadMemoryFinished(quint32 address, const QList<MemoryRow> &rows);

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


private slots:
    void openFile();
    void openBinary();
    void openBinaryAt(const QString &path);
    void openAnalysis();
    void openAnalysisAt(const QString &path);
    void openIffBytes(const QByteArray &bytes);
    void captureScreen();
    void captureDma();
    void scanDiskImage(const QString &imagePath);
    void saveMemoryBytes(quint32 address);
    void requestSaveBytes(quint32 address, int length, const QString &path);
    void saveFile();
    /// File ▸ New File: name, directory, template, then a real file on disk
    /// (docs/untitled-plan.md §4). There is no untitled buffer to fall back on,
    /// so this is the only way a new source starts.
    void newFile();
    /// The same transaction rooted at `directory` — what the sidebar's
    /// context-menu entry asks for, since it knows the directory clicked.
    void newFileIn(const QString &directory);
    /// File ▸ Close File / Close All: the deliberate route to the
    /// zero-document state, which the tab's × button used to be the only way
    /// to reach.
    void closeCurrentFile();
    void closeAllFiles();

private slots:
    void onTabChanged(int index);
    void onTabCloseRequested(int index);
    void openProject();
    void saveProject();
    void editSettings();
    /// Build the source. The remote-control `build` verb invokes this slot by
    /// name (QMetaObject::invokeMethod), so a refusal here is quiet — console
    /// and status bar — because a modal nobody can dismiss (an agent on the
    /// other end, or an offscreen window) would hold the reply until its
    /// operation timeout: the failure openPathQuiet exists to avoid. The
    /// menu/toolbar/shortcut entry is buildInteractive().
    void build() override;
    /// Build with the release assembler arguments, quietly. The remote-control
    /// `buildrelease` verb invokes this slot by name, so a refusal is reported on
    /// the console and status bar rather than in a modal nobody can dismiss; the
    /// menu/toolbar entry is buildReleaseInteractive().
    void buildRelease() override;
    /// Run: build now, launch when it finishes. RemoteControl's `run` verb
    /// invokes this slot by name, so its refusals are quiet for the same
    /// reason as build()'s. The menu/toolbar/shortcut entry is
    /// runInteractive().
    void run() override;
    /// Build/Run from the menu, toolbar and their shortcuts: the same
    /// operations with the modal refusals they have always shown.
    void buildInteractive();
    /// Build with the release assembler arguments (no debug symbols), from the
    /// Build menu. Interactive refusals, like buildInteractive().
    void buildReleaseInteractive();
    void runInteractive();
    void stopSession() override;
    void step() override;
    void stepOver() override;
    /// Step out of the current subroutine: one-shot breakpoint at the return
    /// address read from the stack, then resume (no Hatari primitive exists).
    void stepOut();
    /// One-shot breakpoint at the cursor line's code address, then resume.
    void runToCursor();
    /// Run ▸ Follow execution: hand the caret to the program counter — it goes
    /// to the PC's line now, and every later stop brings it along until a caret
    /// move of the user's own takes it back. The session chip's click and the
    /// gutter's PC wedge do exactly this too (docs/cursor-plan.md §3.3).
    void followExecution();
    /// Profiling is armed/collected while stopped: Hatari starts collection on
    /// continue and zeroes it if any breakpoint is armed mid-run, so both
    /// actions refuse a running machine.
    void profileStart() override;
    bool profileStop() override;
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
    void resume() override;

    void removeBreakpoint(const QString &file, int line);
    void goToBreakpoint(const QString &file, int line);
    bool toggleBreakpointAtLine(int line) override;

    void editBreakpointCondition(int line);
    /// Gutter menu: the Lua script this breakpoint runs when it fires, instead of
    /// the project's hook (empty clears it back).
    void editBreakpointScript(int line);
    void clearAllBreakpoints();
    /// The breakpoints dock's "Clear all": removes breakpoints AND watchpoints
    /// (its button enables when only watchpoints are present), unlike the Run-menu
    /// "Clear Breakpoints" action, which stays breakpoint-only.
    void clearAllDebugTargets();
    void addWatchpoint();
    void removeWatchpoint(int index);
    /// Run ▸ Stop hooks…: choose the script, switch it on, decide whether it may
    /// continue. Persisted in the project file (docs/stop-hooks-plan.md §9).
    void editStopHooks();

public:
    /// Write the project's stop-hook script beside the source (or adopt one that
    /// is already there) and open it in the editor, returning its path — empty
    /// when there is no source to name it after or the write failed.
    ///
    /// Public because the stop-hooks dialog calls it and `tst_gui` drives it
    /// directly: the naming rule, the starter bytes and the no-overwrite rule
    /// are this operation's whole contract, and none of them need a modal to be
    /// worth testing.
    QString createStopHookScript();

    /// Write a memory byte through the debugger (the `writeMemoryByte` intent:
    /// `w b $<addr> $<val>` on the native transport, the fork's `memset`
    /// otherwise), when stopped. The memory views' editing path; the remote
    /// `setmem` verb reaches the same write through writeMemoryByte. `pane` is
    /// the memory pane to refresh after the write; null (writeMemoryByte)
    /// refreshes the first pane.
    bool setMemoryByte(quint32 address, quint32 value, MemoryView *pane = nullptr);

    /// Send a debugger command as written (for the remote `cmd`) — free text,
    /// because it is the client's own. The response arrives via
    /// debugCommandFinished.
    void debugCommand(const QString &command) override;

    /// Write a register through the debugger (the `writeRegister` intent), when
    /// stopped. Shared by the register view's editing and the remote-control
    /// `setreg` command. Returns false (and logs) when the machine is not
    /// stopped.
    bool setRegister(const QString &regName, quint32 value) override;

    /// Whether the current editor's file has an instruction at `line`,
    /// according to the program map. True when there is no map yet — arming
    /// will decide then, so "can't tell" must not read as "cannot fire".
    /// For the remote `breakpoint` reply.
    bool lineHasCode(int line) const override;

    /// Toggle a breakpoint at (file, line) directly — the base-name keying the
    /// whole IDE uses. Shared by toggleBreakpointAtLine (current editor) and
    /// toggleBreakpointAtLabel (any module).
    bool toggleBreakpoint(const QString &file, int line);

    /// Toggle a breakpoint at a symbol's definition. `detail` carries the
    /// reply text either way: "ok <file>:<line> [= 0x…]" or an "error …"
    /// naming why (unknown name, or a position-less symbol). For the remote
    /// `breakpoint <label>` form.
    bool toggleBreakpointAtLabel(const QString &name, QString *detail) override;
    /// The current editor document as JSON {path, text}, for the remote
    /// `read` verb — what the IDE is showing, so an agent needn't guess.
    /// JSON rather than raw text because the block protocol terminates on a
    /// lone `.` line, which assembly source can legitimately contain.
    /// Empty object when no source is open.
    QJsonObject documentJson() const override;

    /// Machine state as JSON: {running, stopped}, plus pc/d0-7/a0-7/sr when a
    /// register batch has landed. For the remote `statejson` verb, which the
    /// MCP shim serves as structured content.
    QJsonObject stateJson() const override;

    /// The Problems pane as JSON objects {file, line, message}, for the
    /// remote `problems` verb.
    QJsonArray problemsJson() const override;

    /// Every open document as JSON objects {path, modified, current}, for the
    /// remote `tabs` verb — the agent's map of what the IDE has open.
    QJsonArray tabsJson() const override;

    /// Save the current document without a dialog. False when no document is
    /// open, or when the write failed. For the remote `save` verb.
    bool saveCurrentDocument() override;

    /// Profiler hot lines as JSON objects {line, count}, sorted by descending
    /// count, for the remote `profile results` verb. Empty when no profile
    /// has been collected.
    QJsonArray profilerResultsJson() const override;

    /// The hover's card for a position in an editor: the same facts the reference
    /// dock shows, in fewer words, and **empty when nothing resolves** — the rule
    /// both surfaces keep. The tooltip handler is its one caller in the window; the
    /// tests read it directly, because a tooltip widget's text is not ours to drive.
    QString referenceCardAt(CodeEditor *editor, const QPoint &viewportPos) const;

    /// The build's symbols as JSON objects {name, file?, line?, address?},
    /// optionally name-filtered (case-insensitive). Addresses appear only once
    /// the program map has live bases — SymbolsView's honesty rule. For the
    /// remote `symbols` verb.
    QJsonArray symbolsJson(const QString &filter) const override;

    /// The stop hook as JSON {enabled, path, autoContinue, last {ran, ok,
    /// timedOut, pc, file, line, logs, writes, message?}} — the project's script
    /// and what its last run did. For the remote `hook` verb, which the MCP shim
    /// serves as structured content.
    QJsonObject hookJson() const override;

    /// How the current program differs from the snapshot of the previous one.
    /// Not a pane's text, so it is computed here rather than read through
    /// RemoteStateAdapter: the adapter reports what a view is already showing.
    QJsonObject buildDiffJson() const override;
    QJsonArray assetsJson() const override;
    bool declareAsset(const QJsonObject &spec, QString *error) override;
    bool removeAsset(const QString &output, QString *error) override;
    QString stoppedSourceLocation() const override;

    /// Parse and add a watchpoint by text: an address ("$12345", "$12345.l") or a
    /// label ("counter.l"), which the arm plan resolves again on every run.
    /// Separated from the dialog so the remote-control interface and tests can use
    /// it without a prompt. `added` receives what was stored, with the address
    /// filled in when it is known now. Returns false and sets error otherwise.
    bool addWatchpointAddress(const QString &text, Watchpoint *added, QString *error) override;

    /// Remove watchpoints: every one when `text` is empty, otherwise those it
    /// names. False and a reason when the text matches nothing.
    bool clearWatchpoints(const QString &text, int *removed, QString *error) override;

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
    /// Build the panels, wire them to each other, then put them on the View
    /// menu. Three passes over one arrangement, in that order: every panel has
    /// to exist before anything is wired to it, and the menu lists the docks
    /// that exist.
    void createDocks();
    void buildPanels();
    void wirePanels();
    void buildViewMenu();
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
    /// The text window the OS-call scanner needs around `blockNumber`, resolved
    /// for `column`: the one way the reference dock and the hover card ask what a
    /// position in the source *is* (docs/glm-features.md §4).
    CursorReference referenceAroundCursor(CodeEditor *editor, int blockNumber, int column) const;

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

    /// Report a build that refused to start (no source, failed save, missing
    /// linker): a refusal is still a finished build as far as callers are
    /// concerned — run()'s launch intent must be dropped rather than left
    /// armed for the next successful build, and a remote-control `build`
    /// must be answered now, not after its timeout. A quiet build (see
    /// buildImpl) reports the reason on the console and the status bar instead
    /// of opening a modal.
    void refuseBuild(const QString &title, const QString &reason, bool critical);

    /// Report a run that could not start. A quiet run (see m_quietDialogs)
    /// reports the reason on the console and the status bar and answers
    /// buildCompleted(false) — the failure channel RemoteControl's `run` reply
    /// wait ends on — rather than leaving the reply to its operation timeout;
    /// an interactive run keeps the modal it has always shown.
    void refuseRun(const QString &title, const QString &reason, bool critical);

    /// Shared bodies of the quiet (remote-invoked) and interactive entries.
    /// `quiet` suppresses the modal dialogs on the refusal paths; it is carried
    /// on the window because a run's launch happens after the build's async
    /// hop, where the argument is gone. `release` picks the project's release
    /// assembler arguments over the debug ones.
    void buildImpl(bool quiet, bool release);
    /// Compare the current program with the snapshot taken before the last
    /// build replaced it. Preview only: the dialog does not write to the machine.
    void compareWithLastBuild();

    /// Why `gatherBuildDiff` could not produce a comparison. `Ok` means `diff`
    /// is a successful comparison. `BadImage` means a file is not a TOS image
    /// (`detail` is the parser's error).
    enum class BuildDiffMiss { Ok, NoSource, NoPrevious, NoCurrent, Unreadable, BadImage };
    BuildDiffMiss gatherBuildDiff(ProgramDiff *diff, ProgramDiffHints *hints, QString *detail) const;
    void runImpl(bool quiet);

    /// Load the project settings that sit beside a source file, if any.
    ///
    /// `quiet` is the remote-control path: a refusal to trust a hook suppresses
    /// it for this session rather than opening a modal nobody can dismiss.
    void loadProjectForSource(const QString &sourcePath, bool quiet = false);

    /// Whether the currently loaded project's hook may run: no hook, a project
    /// that shipped with PiST, or one the user has already allowed. Otherwise asks
    /// once and records the answer. `quiet` skips the question and refuses.
    bool hookMayRunFor(const QString &projectPath, bool quiet = false);

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

    /// Create and wire a text-editor tab, loading `path` into it. The path is
    /// required: a document is a file on disk (docs/untitled-plan.md §2), so
    /// there is no empty-path form left to create. Returns null when the file
    /// could not be loaded.
    CodeEditor *addEditorTab(const QString &path, bool quiet = false);

    /// Create and wire an image-editor tab. `path` is a `.pim` or an importable
    /// ST still-image, and is required here too. An *image* may become
    /// path-less afterwards — an imported sheet has no file yet — which is the
    /// deliberate asymmetry docs/untitled-plan.md §9 keeps; nothing creates one
    /// that way.
    ImageEditor *addImageTab(const QString &path, bool quiet = false);

    /// A read-only hex tab. `path` is the host file, empty for bytes taken from
    /// a disk image. `originKey` raises this tab instead of opening a second
    /// one: the absolute path, or the disk image plus the entry.
    BinaryView *addBinaryTab(const QByteArray &bytes, const QString &name,
                             const QString &path, const QString &originKey);
    void wireBinary(BinaryView *view);

    /// A read-only disassembly of a TOS executable. The same path raises the
    /// tab that is already open; a binary tab of that path is a different
    /// document and stays put.
    AnalysisView *addAnalysisTab(const QByteArray &bytes, const QString &name,
                                 const QString &path);

    /// Shared body of openPath (interactive) and openPathQuiet (remote): the
    /// quiet form returns false where the interactive one shows a modal.
    bool openPathImpl(const QString &path, bool quiet);

    /// Swap the central area between the documents page and the welcome view,
    /// refreshing the welcome view when it is the one on show.
    void updateCentralPage();

    /// Rebuild the welcome view from one place: the recents, the demo projects
    /// that exist here, toolchain readiness, and the usage shelf.
    void refreshWelcome();

    /// Close the tab at `index`, asking to save a modified document first.
    /// False when the user cancelled, so Close All stops where the user
    /// stopped. `promptToSave` false closes without that offer, for a caller
    /// that has already put the question and been answered — the delete
    /// rescue's "Close tab", whose own prompt says the changes go with it.
    bool closeTab(int index, bool promptToSave = true);

    /// The directory a new source file starts in: the project's when the
    /// sidebar has one, else the chain suggestedImageDirectory() follows.
    QString suggestedSourceDirectory() const;

    /// Keep the document watcher on exactly the open documents' paths
    /// (docs/untitled-plan.md §7.1).
    void syncWatchedDocuments();

    /// Write `editor`/`image` to `path`, then re-arm the document watcher:
    /// `files::write()` replaces the file by staging a temporary and renaming it
    /// over the target, which is what drops a watch on it
    /// (docs/untitled-plan.md §7.1). Every save goes through these, so none can
    /// skip the re-arm.
    bool saveEditor(CodeEditor *editor, const QString &path);
    bool saveImage(ImageEditor *image, const QString &path);

    /// A document's backing file is gone: a watcher event, or the sidebar's own
    /// delete. `deliberate` says which — the sidebar gesture has already asked,
    /// so an unmodified tab closes rather than prompting
    /// (docs/untitled-plan.md §7.3).
    void documentVanished(const QString &path, bool deliberate);

    /// The prompt for one still-open document whose file is gone: Close tab,
    /// Save a copy…, or Cancel (docs/untitled-plan.md §7.3).
    void rescueDeletedDocument(QWidget *document, const QString &path);

    /// Everything keyed to a path follows the file to its new name
    /// (docs/untitled-plan.md §7.2).
    void followRenamedFile(const QString &oldPath, const QString &newPath);

    /// Connect one editor's signals. Runs for every editor tab created.
    void wireEditor(CodeEditor *editor);
    void wireImage(ImageEditor *editor);
    void wireImageReExport(ImageEditor *editor);

    /// Every open text editor, in tab order.
    QList<CodeEditor *> openEditors() const;
    QList<ImageEditor *> openImages() const;
    QList<BinaryView *> openBinaryViews() const;
    QList<AnalysisView *> openAnalysisViews() const;

    /// The open editor showing `path`, or null.
    CodeEditor *editorForPath(const QString &path) const;
    ImageEditor *imageForPath(const QString &path) const;
    BinaryView *binaryForKey(const QString &originKey) const;
    AnalysisView *analysisForPath(const QString &path) const;

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
    /// Bring the Find in Files dock forward with its query focused (Ctrl+Shift+F),
    /// re-reading the scope first: the project may have moved since the last search.
    void showFindInFiles();
    /// Bring the symbol palette up (Ctrl+Shift+O) with the build's symbols: a
    /// filter-as-you-type jump list over the labels and equates.
    void showSymbolPalette();

    void openRecentSource();

    void onBuildFinished(bool success, const QList<pist::Diagnostic> &diagnostics);
    void onStateUpdated(const pist::MachineState &state);
    void locationFromPc(quint32 pc);

    /// Hand the caret to the program counter, returning false when there is
    /// nothing to hand it to (no session, or a machine that is running rather
    /// than stopped). The one implementation behind all three engage
    /// affordances (cursor-plan §3.3).
    bool engagePcFollow();

    /// The one place m_pcFollow changes, so the menu item that shows the
    /// ownership cannot disagree with it.
    void setPcFollow(bool following);

    /// Show the PC at `line` in `target`, and in no other editor: a marker left
    /// behind on a file the machine has walked out of reads as "the program is
    /// here" when the user switches back to it. `target` may be null (the PC's
    /// file is not open) — then no editor holds a marker at all and the session
    /// chip carries the location (cursor-plan §4.3).
    void setExecutionMarker(CodeEditor *target, int line);

    /// Rebuild the program's source mapping from the listings this build
    /// produced, and load the linker's placement map when there was a link.
    void rebuildProgramMap();


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

    /// The directory a project-wide question is asked about: what the file
    /// browser was pointed at when the user chose it, else the focused document's
    /// own directory, else nothing — never the process's working directory,
    /// which is nobody's project (MIN-81). Git's panel and Find in Files both
    /// ask it here, so the two cannot disagree about what "the project" is.
    QString projectScopeDirectory() const;
    /// Point Find in Files at the project directory. Re-runs its query, because
    /// the results of the old scope are not answers about this one.
    void syncFindInFilesScope();

    /// Point the Git panel at the project directory, and show or hide the
    /// blame lane on every open editor. Blame stays off for a document with no
    /// path, an image tab, and a file that is not inside the repository.
    void syncGitDirectory();
    void applyGitBlame();
    void refreshBlame();

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
    void applyAudioOutputDevice();
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
    /// The central area: page 0 is the documents (the tabs and the two strips
    /// under them) and page 1 the welcome view. Zero documents is a real state
    /// now — a document is a file on disk — so the welcome view is a page
    /// rather than an empty buffer pretending to be one
    /// (docs/untitled-plan.md §5).
    QStackedWidget *m_center = nullptr;
    WelcomeView *m_welcome = nullptr;
    /// The open documents' backing files (docs/untitled-plan.md §7.1). There is
    /// always one: a document without a path does not exist any more, and an
    /// image that has none is not watched.
    DocumentWatcher *m_watcher = nullptr;
    /// This session's build count and its last outcome, held here and written to
    /// the settings once at teardown (docs/untitled-plan.md §6): a crash loses
    /// the summary, never the work.
    int m_sessionBuilds = 0;
    bool m_lastBuildOk = false;
    CodeEditor *m_editor = nullptr;
    ImageEditor *m_image = nullptr;
    BuildService *m_build = nullptr;

    /// The MAJ-41 seams. Qt parent-child owns them (each is constructed with
    /// `this` as its parent), and each was handed a Host — the operations it
    /// performs on this window (MIN-89), built in the constructor. These
    /// pointers are how MainWindow calls *into* a seam; the seams call back
    /// through their Hosts, never through these.
    DebugSessionController *m_session = nullptr;
    SessionLauncher *m_launcher = nullptr;
    BreakpointWatchpointModel *m_bpModel = nullptr;
    ProfilerController *m_profiling = nullptr;
    RemoteStateAdapter *m_remoteState = nullptr;
    /// The stop hook (docs/stop-hooks-plan.md): runs the project's script at
    /// every published stop, from a queued turn so its read-backs are outside
    /// the transport's emit stack.
    StopHookRunner *m_hooks = nullptr;

    DebugSessionController *sessionController() const { return m_session; }
    BreakpointWatchpointModel *breakpointsModel() const { return m_bpModel; }
    ProfilerController *profilerController() const { return m_profiling; }
    SessionLauncher *sessionLauncher() const { return m_launcher; }


    /// The most recent machine state, kept so the remote-control interface can
    /// answer `state` without touching the emulator.
    MachineState m_lastState;
    /// The dock and widget that host the emulator's display (the media panel).
    /// The dock is hidden until a media session engages it (docs/PLAN.md §12).
    QDockWidget *m_displayDock = nullptr;
    EmulatorDisplayWidget *m_display = nullptr;
    /// The media channel's audio output (docs/PLAN.md §12, phase 3).
    EmuAudio *m_audio = nullptr;

    /// The session runs in media mode (docs/PLAN.md §12): windowless emulator,
    /// frames over the media channel. Set when the launch engages it, cleared
    /// with the session.
    bool m_mediaDisplay = false;

    /// The most recent media-channel frame, for a screenshot from the
    /// framebuffer (the emulator has no window to capture).
    QImage m_lastMediaFrame;

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
    ///
    /// The row type is the state adapter's (RemoteStateAdapter::ProblemRow):
    /// the four fields *are* that verb's JSON, so the pane's list and the
    /// adapter's report are one shape rather than two that must agree.
    QList<RemoteStateAdapter::ProblemRow> m_problemEntries;

    /// Add one row to the Problems pane. The row carries its index into
    /// m_problemEntries as item data, so every reader gets the structured
    /// entry rather than parsing the display strings back out of the tree: the
    /// file column of a build-level note is a translated placeholder, and the
    /// line column is formatted for the eye.
    QTreeWidgetItem *addProblemRow(const RemoteStateAdapter::ProblemRow &entry);

    /// The entry a Problems row indexes, or null when the row has none.
    const RemoteStateAdapter::ProblemRow *problemEntryFor(const QTreeWidgetItem *item) const;

    DisassemblyView *m_disassembly = nullptr;
    RegistersView *m_registers = nullptr;
    MemoryView *m_memory = nullptr;
    class ConsoleInput *m_consoleInput = nullptr;

    /// Every open memory pane, keyed by the dump-routing tag each one carries, so
    /// a dump is routed back to the pane that asked for it. m_memory is the first
    /// pane (tag 0) and drives the auto-refresh / first-stop navigation.
    QHash<int, MemoryView *> m_memoryPanes;
    QDockWidget *m_memoryDock = nullptr;
    QString m_saveBytesPath;
    quint32 m_saveBytesAddress = 0;
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
    int m_iffSerial = 0;
    GitPanel *m_gitPanel = nullptr;
    QDockWidget *m_gitDock = nullptr;
    QAction *m_actGitBlame = nullptr;
    QVector<struct SymbolEntry> m_symbols;
    /// The candidates completion and the palette work from: the assembler's
    /// vocabulary plus the build's symbols, rebuilt whenever the listings are.
    QList<CompletionItem> m_completionItems;
    /// The palette, created on first use and kept: reopening it should not lose
    /// anything (it re-reads the candidates on every show).
    SymbolPaletteDialog *m_symbolPalette = nullptr;
    QStringList m_consoleVerbs;
    /// Where session events are published for remote-control watchers
    /// (control/RemoteControl); null when no one wired one up (tests).
    RemoteControl *m_eventSink = nullptr;
    class ProfilerView *m_profiler = nullptr;
    QDockWidget *m_profilerDock = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTreeWidget *m_problems = nullptr;
    QDockWidget *m_problemsDock = nullptr;
    /// Project-wide search: the dock itself, and the walk it drives.
    FindInFilesView *m_findInFiles = nullptr;
    QDockWidget *m_findInFilesDock = nullptr;

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

    /// Whether the caret's position is owned by the program counter: the user
    /// handed it over (Follow execution, the session chip, or the gutter's PC
    /// wedge), and every stop moves it until they take it back with a caret
    /// move of their own. Session-scoped — clearSessionViews resets it.
    bool m_pcFollow = false;
    /// Whether the PC has been *placed* in this session yet. The first stop is
    /// a placement — marker and chips only, the caret untouched — and every
    /// stop after it a movement. That asymmetry is what keeps the entry stop
    /// from yanking the caret while stepping still follows once the user asks
    /// it to (cursor-plan §3.2).
    bool m_pcPlaced = false;
    /// Set while the follow path moves the caret itself: the
    /// cursorPositionChanged that move produces must not be read as the user
    /// claiming the caret back, which would end following at the first step
    /// (cursor-plan §4.2).
    bool m_pcFollowInProgress = false;

    /// Source mapping for the whole program. Replaces a single LineMap because a
    /// linked program has one listing per module, each needing its own base.
    ProgramLineMap m_programMap;
    LineMap::SectionBases m_bases;

    /// The debugger command a remote `cmd` is waiting on, so its response is
    /// routed to debugCommandFinished.
    QString m_pendingDebugCommand;

    /// True while the build/run in flight was started by the quiet entry points
    /// (build()/run(), the slots RemoteControl invokes by name): a refusal then
    /// reports itself on the console and the status bar instead of a modal
    /// dialog, whose own event loop would hold the remote reply until the
    /// operation timeout. Set by buildImpl()/runImpl(), so it is still set when
    /// launchEmulator() runs after the build's async hop.
    bool m_quietDialogs = false;

    /// Session directory of the current (or most recent) run, so it can be
    /// removed when the session ends instead of accumulating in the temp
    /// location. See paths::removeSessionDir.
    QString m_currentSessionDir;

    /// Project configuration. Persisted to a `.pistproject` file beside the
    /// source, so settings travel with the project (docs/PLAN.md §5 rule 7 —
    /// never via a user's hatari.cfg).
    ProjectSettings m_settings;
    /// A trust refusal suppresses this project's hook for the current session
    /// without changing the enabled setting persisted in the user's file.
    bool m_stopHookRefused = false;

    QAction *m_actOpen = nullptr;
    QAction *m_actOpenBinary = nullptr;
    QAction *m_actOpenAnalysis = nullptr;
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
    QAction *m_actFindInFiles = nullptr;
    QAction *m_actOpenProject = nullptr;
    QAction *m_actOpenDemo = nullptr;
    QAction *m_actProfileStart = nullptr;
    QAction *m_actProfileStop = nullptr;
    QAction *m_actProfileToCursor = nullptr;
    QAction *m_actSaveProject = nullptr;
    QAction *m_actSettings = nullptr;
    QAction *m_actSave = nullptr;
    QAction *m_actCloseFile = nullptr;
    QAction *m_actCloseAll = nullptr;
    QAction *m_actBuild = nullptr;
    QAction *m_actBuildRelease = nullptr;
    QAction *m_actCompareBuild = nullptr;
    QAction *m_actRun = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actCaptureScreen = nullptr;
    QAction *m_actCaptureDma = nullptr;
    QAction *m_actNextDiagnostic = nullptr;
    QAction *m_actPrevDiagnostic = nullptr;
    QAction *m_actStep = nullptr;
    QAction *m_actStepOver = nullptr;
    QAction *m_actStepOut = nullptr;
    QAction *m_actRunToCursor = nullptr;
    /// Run ▸ Follow execution. Checkable, and the state it shows *is*
    /// m_pcFollow: the menu is where the caret's ownership is visible and can be
    /// flipped in one gesture (cursor-plan §3.3, §6.4).
    QAction *m_actFollow = nullptr;
    QAction *m_actResume = nullptr;
    QAction *m_actClearBreakpoints = nullptr;
    QAction *m_actToggleBreakpoint = nullptr;
    QAction *m_actGotoLine = nullptr;
    QAction *m_actSymbolPalette = nullptr;
    QAction *m_actPause = nullptr;
    QAction *m_actReleaseInput = nullptr;
    QAction *m_actAddWatchpoint = nullptr;
    QAction *m_actStopHooks = nullptr;
};

} // namespace pist
