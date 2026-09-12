// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

#pragma once

#include "build/Diagnostic.h"
#include "build/LineMap.h"
#include "emu/HatariProbe.h"
#include "emu/MachineState.h"
#include "emu/SessionConfig.h"

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
class DisassemblyView;
class EmulatorHost;
class RegistersView;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void openFile();
    void saveFile();
    void build();
    void run();
    void stopSession();
    void step();
    void stepOver();
    void resume();

private:
    void createActions();
    void createDocks();
    void createToolBar();
    void createStatusBar();

    void onBuildFinished(bool success, const QList<pist::Diagnostic> &diagnostics);
    void onStateUpdated(const pist::MachineState &state);
    void locationFromPc(quint32 pc);

    /// Per-session working directory, kept short so the control socket path fits
    /// in sockaddr_un::sun_path.
    QString makeSessionDir();

    CodeEditor *m_editor = nullptr;
    BuildService *m_build = nullptr;
    EmulatorHost *m_host = nullptr;
    HatariCapabilities m_caps;

    DisassemblyView *m_disassembly = nullptr;
    RegistersView *m_registers = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTreeWidget *m_problems = nullptr;
    QTabWidget *m_bottomTabs = nullptr;

    QLabel *m_statusToolchain = nullptr;
    QLabel *m_statusEmulator = nullptr;

    LineMap m_lineMap;
    LineMap::SectionBases m_bases;

    QAction *m_actOpen = nullptr;
    QAction *m_actSave = nullptr;
    QAction *m_actBuild = nullptr;
    QAction *m_actRun = nullptr;
    QAction *m_actStop = nullptr;
    QAction *m_actStep = nullptr;
    QAction *m_actStepOver = nullptr;
    QAction *m_actResume = nullptr;
};

} // namespace pist
