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
#include "emu/MachineState.h"
#include "emu/SessionConfig.h"
#include "project/ProjectSettings.h"

#include <QMainWindow>
#include <QString>

class QAction;
class QDockWidget;
class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
class QTreeWidget;

namespace pist {

class BuildService;
class CodeEditor;
class BreakpointPanel;
class DisassemblyView;
class EmulatorHost;
class FileBrowser;
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
    void openPath(const QString &path);

    /// The build & debug console contents, for scripted verification.
    QString debugConsoleText() const;

    /// A plain-text snapshot of the machine state (registers, PC, running
    /// state), for the remote-control interface. Read-only.
    QString stateSummary() const;

signals:
    /// The asynchronous build finished, for the remote-control interface to
    /// answer a `build` command. Success carries no diagnostics; a failure is
    /// accompanied by the console text.
    void buildCompleted(bool success);

    /// The emulator session started or stopped, so the remote-control interface
    /// can answer `run` when the session is actually up rather than merely
    /// requested.
    void sessionRunningChanged(bool running);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openFile();
    void saveFile();
    void openProject();
    void saveProject();
    void editSettings();
    void build();
    void run();
    void launchEmulator();
    void stopSession();
    void step();
    void stepOver();
    void resume();

    void removeBreakpoint(const QString &file, int line);
    void goToBreakpoint(const QString &file, int line);
    void toggleBreakpointAtLine(int line);
    void editBreakpointCondition(int line);
    void clearAllBreakpoints();
    void addWatchpoint();
    void removeWatchpoint(int index);

public:
    /// Parse and add a watchpoint by address text (e.g. "$12345" or "$12345.l").
    /// Separated from the dialog so the remote-control interface and tests can use
    /// it without a prompt. Returns false and sets error on a bad address.
    bool addWatchpointAddress(const QString &text, QString *error);

private:
    void createActions();
    void createMenus();
    void createDocks();
    void createToolBar();
    void createStatusBar();

    /// Load the project settings that sit beside a source file, if any.
    void loadProjectForSource(const QString &sourcePath);

    /// Update the title and the Save action to reflect the modified state.
    void updateModifiedState();

    /// Ask to save when there is unsaved work. Returns false if the user
    /// cancelled, in which case the caller must abandon its action.
    bool maybeSave();

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
    bool canEmbedDisplay() const;

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

    /// Restore the factory dock layout (the default tab groupings), discarding
    /// the user's current arrangement. Invoked from the View menu.
    void resetToDefaultLayout();

    CodeEditor *m_editor = nullptr;
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

    /// The factory dock arrangement, captured after the default tab groupings are
    /// applied, so "Reset layout" can restore it. Saved/restored via QSettings.
    QByteArray m_defaultLayoutState;

    /// The View menu, kept so the per-dock show/hide actions can be appended once
    /// the docks exist (menus are created before docks).
    class QMenu *m_viewMenu = nullptr;
    EmulatorHost *m_host = nullptr;
    HatariCapabilities m_caps;

    DisassemblyView *m_disassembly = nullptr;
    RegistersView *m_registers = nullptr;
    MemoryView *m_memory = nullptr;
    StackView *m_stack = nullptr;
    HardwareView *m_hardware = nullptr;
    BreakpointPanel *m_breakpointPanel = nullptr;
    FileBrowser *m_fileBrowser = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTreeWidget *m_problems = nullptr;
    QTabWidget *m_bottomTabs = nullptr;

    QLabel *m_statusToolchain = nullptr;
    QLabel *m_statusEmulator = nullptr;

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

    /// Set once breakpoints have been armed against the live bases this session.
    /// Arming has to wait for the basepage response (not a timer, which always
    /// loses that race), so this guards doing it exactly once, when they arrive.
    bool m_breakpointsArmedThisSession = false;

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
    QAction *m_actOpenProject = nullptr;
    QAction *m_actSaveProject = nullptr;
    QAction *m_actSettings = nullptr;
    QAction *m_actSave = nullptr;
    QAction *m_actBuild = nullptr;
    QAction *m_actRun = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actEmbedDisplay = nullptr;
    QAction *m_actStep = nullptr;
    QAction *m_actStepOver = nullptr;
    QAction *m_actResume = nullptr;
    QAction *m_actClearBreakpoints = nullptr;
    QAction *m_actAddWatchpoint = nullptr;
};

} // namespace pist
