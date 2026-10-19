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

/// Why the machine stopped, as far as the IDE can honestly tell. The transport
/// reports a prompt, not a reason: only a breakpoint (whose program counter we
/// armed) and a step (which we asked for) can be named.
enum class StopReason { Unknown, Breakpoint, Step };

/// What a stop is, beyond the machine's state: why it happened, which breakpoint
/// it was, and whether that breakpoint carries a script of its own.
struct StopTrigger
{
    StopReason reason = StopReason::Unknown;
    /// The breakpoint that fired, `"file:line"`, when `reason` is Breakpoint.
    QString at;
    /// A script attached to that breakpoint, run *instead of* the project's hook.
    /// Empty means the project's, which is the usual case.
    QString script;
};

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
        /// Resolve a label to its address in this run (`sym()` in a script).
        /// Unset, or refusing, makes `sym()` raise — never return 0.
        std::function<bool(const QString &name, quint32 *address, QString *error)> symbol;
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

    /// What a run did, in fields rather than in a sentence: the `hook` verb, its
    /// MCP tool and the event stream must not parse translated prose.
    /// `lastRunSummary()` renders this, so the two cannot disagree.
    struct LastRun
    {
        /// True once a script has actually been executed. A skip — no script
        /// configured, the hook off, the session gone — is not a run.
        bool ran = false;
        bool ok = false;
        /// The budget cut the script off. It did whatever it did up to there.
        bool timedOut = false;
        /// The stop the script ran at.
        quint32 pc = 0;
        /// The script's own base name (it is what the console and a breakpoint
        /// label call it), and the line an error came from, -1 when the engine
        /// reported none.
        QString file;
        int line = -1;
        /// The script's own words when it failed; empty when it worked.
        QString message;
        /// What it did: console lines logged and bytes written.
        int logs = 0;
        int writes = 0;
    };

    /// A stop was published with a validated state (from `MainWindow`'s stop
    /// publication): run the hook, if the project — or the breakpoint that fired
    /// — has one.
    ///
    /// The run happens one event-loop turn later, outside the transport's emit
    /// stack — a nested read-back loop started *inside* it could never see the
    /// answer it waits for, because the drain is single-entry
    /// (docs/stop-hooks-plan.md D2).
    void onStopPublished(const MachineState &state, const StopTrigger &trigger = StopTrigger());

    /// How the last run ended, for the dialog's "last run" line. Empty before the
    /// first run.
    QString lastRunSummary() const;
    /// The same run, as fields. `ran` is false before the first run.
    const LastRun &lastRun() const { return m_lastRun; }

signals:
    /// A script ran, and this is what it did — emitted before the `continue()`
    /// policy runs, so a watcher's `stopped` → `hook` → `running` order holds.
    /// Not emitted for a skip: there was no run to report.
    void runFinished(const pist::StopHookRunner::LastRun &last);

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
    /// The API's `reason`/`at`: what this stop is, from the trigger it carried.
    QString stopReason() const;
    QString stopAt() const { return m_trigger.at; }
    bool resolveSymbol(const QString &name, quint32 *address, QString *error);

    void runQueued();
    void doRun();
    /// Issue one debugger command and wait for the transport to complete it.
    /// `expected` is the text the backend will report in `commandFinished` — the
    /// only completion this waits for. Holding the console keeps the user's own
    /// commands out, but not the toolbar's or the remote channel's, which complete
    /// while a hook is waiting.
    bool issueAndWait(const QString &expected, const std::function<void()> &issue,
                      const QString &what, QString *error);
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
    /// Why this stop happened, and which script to run — the API's `reason`,
    /// `at`, and the per-breakpoint override.
    StopTrigger m_trigger;
    bool m_continueRequested = false;
    int m_writes = 0;
    int m_loggedLines = 0;
    bool m_queued = false;
    bool m_running = false;
    /// The text of the command `issueAndWait` is waiting on, so a completion that
    /// belongs to someone else is ignored rather than taken as its answer.
    QString m_commandInFlight;
    LastRun m_lastRun;
};

/// A run as the single line the `hook` event carries to a watcher, and the one
/// rendering of a run that is not prose: `lastRunSummary()` is written for a
/// person and is translated, and an agent must not parse a sentence for a line
/// number.
///
/// The shape is `<status> file=<name> line=<n> writes=<n> logs=<n> pc=0x<n>
/// [message=<the script's own words>]`, with `status` one of `ok`, `timeout`
/// (the budget cut the script off), `error`, or `norun` (the file could not be
/// read). `message` is last because it is the only free-text field: everything
/// after `message=` belongs to it.
QString hookEventDetail(const StopHookRunner::LastRun &run);

} // namespace pist
