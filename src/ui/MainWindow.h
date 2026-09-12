// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/Diagnostic.h"
#include "debug/Breakpoint.h"
#include "build/LineMap.h"
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
class MemoryView;
class RegistersView;

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

private:
    void createActions();
    void createMenus();
    void createDocks();
    void createToolBar();
    void createStatusBar();

    /// Load the project settings that sit beside a source file, if any.
    void loadProjectForSource(const QString &sourcePath);

    void onBuildFinished(bool success, const QList<pist::Diagnostic> &diagnostics);
    void onStateUpdated(const pist::MachineState &state);
    void locationFromPc(quint32 pc);

    /// Called once the debugger stops. This is the point at which the program's
    /// load address is known, so breakpoints can finally be resolved and armed.
    void onDebuggerStopped();
    void armBreakpoints();

    /// Fold the addresses the ArmPlan resolved back into the stored breakpoint
    /// list, so the panel shows where each one actually landed.
    QList<Breakpoint> mergeResolved(const ArmPlan &plan) const;
    void refreshBreakpointMarkers();

    /// Per-session working directory, kept short so the control socket path fits
    /// in sockaddr_un::sun_path.
    QString makeSessionDir();

    CodeEditor *m_editor = nullptr;
    BuildService *m_build = nullptr;
    EmulatorHost *m_host = nullptr;
    HatariCapabilities m_caps;

    DisassemblyView *m_disassembly = nullptr;
    RegistersView *m_registers = nullptr;
    MemoryView *m_memory = nullptr;
    BreakpointPanel *m_breakpointPanel = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTreeWidget *m_problems = nullptr;
    QTabWidget *m_bottomTabs = nullptr;

    QLabel *m_statusToolchain = nullptr;
    QLabel *m_statusEmulator = nullptr;

    LineMap m_lineMap;
    LineMap::SectionBases m_bases;

    /// Breakpoints are stored per file:line, never per address: the program is
    /// relocated by GEMDOS on every run (docs/PLAN.md §5 rule 6).
    QList<Breakpoint> m_breakpoints;

    /// Set once the entry stop has been handled for the current session, so the
    /// arming sequence runs exactly once.
    bool m_sessionArmed = false;

    /// Set by Run, consumed by onBuildFinished. Needed because the build is
    /// asynchronous: the launch has to wait for it, not run alongside it.
    bool m_launchAfterBuild = false;

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
    QAction *m_actStep = nullptr;
    QAction *m_actStepOver = nullptr;
    QAction *m_actResume = nullptr;
    QAction *m_actClearBreakpoints = nullptr;
};

} // namespace pist
