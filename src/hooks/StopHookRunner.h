// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>

namespace pist {

class IDebugBackend;

/// Runs the project's stop-hook script at every debugger stop, with the machine
/// held stopped at its prompt (docs/stop-hooks-plan.md).
///
/// A seam in the MAJ-41 sense — it owns the script's execution (the API it may
/// call, the wait for each debugger answer, the budget, the continue it may ask
/// for) and reaches the window only through the `Host` operations below — but it
/// lives in the hook module rather than in `ui/` because it needs no widgets at
/// all. That is not tidiness: it is what lets the runner be driven against a real
/// emulator session in a test with no display, which a `ui/` home would forbid.
class StopHookRunner : public QObject
{
    Q_OBJECT

public:
    struct Host
    {
        /// The debug transport in force. Read per use: a launch selects it.
        std::function<IDebugBackend *()> backend;
        /// One line on the console.
        std::function<void(const QString &)> log;
        /// The script to run — the project's stop hook. Empty means none.
        std::function<QString()> scriptPath;
        /// Whether the project has the hook switched on.
        std::function<bool()> enabled;
        /// Whether the script's `continue()` is honoured without asking.
        std::function<bool()> autoContinue;
        /// Resume, through the same path the Continue action uses, so the arms
        /// queued at the entry stop survive it (invariant 5).
        std::function<void()> resume;
        /// Refresh the views after a run that wrote something — once per run,
        /// not once per byte.
        std::function<void()> refresh;
        /// Hold the console input while a hook runs. A command the user typed
        /// could otherwise complete first and be mistaken for one of ours, and
        /// ours would then be pruned by a resume (docs/stop-hooks-plan.md R5).
        std::function<void(bool)> holdConsole;
        /// The run budget, in milliseconds.
        std::function<int()> budgetMs;
        /// How long one scripted read or write waits for its answer before the
        /// runner gives up. The transport's own command timeout is the default
        /// backstop; a test injects a short one to reach the give-up path.
        std::function<int()> operationTimeoutMs;
    };

    explicit StopHookRunner(Host host, QObject *parent = nullptr);

    /// A stop was published with a validated state (from `MainWindow`'s stop
    /// publication): run the hook, if the project has one.
    ///
    /// The run happens one event-loop turn later, outside the transport's emit
    /// stack — a nested read-back loop started *inside* it could never see the
    /// answer it waits for, because the drain is single-entry
    /// (docs/stop-hooks-plan.md D2).
    void onStopPublished(const MachineState &state);

    /// How the last run ended, for the dialog's "last run" line. Empty before the
    /// first run.
    QString lastRunSummary() const { return m_lastSummary; }

private:
    /// The script's view of the debugger: `hooks::HookApi` implemented over the
    /// operations below (defined in the .cpp, where nothing else needs it).
    class ScriptApi;

    /// The script's operations. Each waits for its own debugger answer with a
    /// timeout, and reports a sentence naming itself when it cannot.
    bool readMemory(quint32 address, int length, QByteArray *bytes, QString *error);
    bool writeMemory(quint32 address, quint8 value, QString *error);
    bool readRegister(const QString &name, quint32 *value, QString *error);
    bool writeRegister(const QString &name, quint32 value, QString *error);
    void scriptLog(const QString &message);
    void requestContinue();

    void runQueued();
    void doRun();
    /// Issue one debugger command and wait for the transport to complete it.
    /// Safe to call only while a run holds the console, so the completion that
    /// arrives is provably this command's.
    bool issueAndWait(const std::function<void()> &issue, const QString &what, QString *error);
    /// A fresh register batch, for when the published copy is no longer the
    /// machine's truth (a scripted write came first).
    bool fetchRegisters(MachineState *out, QString *error);
    bool registerNameValid(const QString &name, QString *error) const;
    bool writableRegisterName(const QString &name, QString *error) const;
    quint32 registerValue(const QString &name) const;
    /// The per-operation backstop, from the Host or the transport's own timeout.
    int operationTimeout() const;

    Host m_host;
    /// The stop's register file and PC, as the batch that published the stop
    /// left them. Just those two: the API reads nothing else, so a stop does not
    /// copy the disassembly to answer `readreg("d0")`.
    Registers m_regs;
    quint32 m_pc = 0;
    /// A scripted write happened, so the copy above is no longer what the machine
    /// says: the next register read makes a round-trip instead of answering from it.
    bool m_stateDirty = false;
    bool m_continueRequested = false;
    int m_writes = 0;
    int m_loggedLines = 0;
    bool m_queued = false;
    bool m_running = false;
    QString m_lastSummary;
};

} // namespace pist
