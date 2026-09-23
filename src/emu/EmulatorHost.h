// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/DebugBackend.h"
#include "emu/MachineState.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

#include "emu/EmbedSocket.h"

class QProcess;
class QTimer;

namespace pist {

// Must be `struct` here to match its definition (emu/HatariProbe.h). MSVC
// mangles a class-typed reference differently from a struct-typed one, so a
// mismatch produces an unresolved symbol on Windows while the Itanium ABI used
// elsewhere links it happily.
struct HatariCapabilities;

/// Owns one Hatari process and the debug channels attached to it.
///
/// Channel model (verified against Hatari 2.6.1; docs/PLAN.md §3.3). The two channels
/// are not interchangeable:
///
///   stdin   the *only* channel that works while the debugger is stopped. The
///           debugger blocks in DebugUI_GetCommand reading stdin, and nothing
///           under src/debug/ polls the control socket, so `hatari-debug`
///           commands sent while stopped are never read.
///   socket  serviced only from the SDL event pump (src/sdl/gui_event.c:134),
///           so it works only while emulation is running.
///
/// Completion framing: readline echoes the command and then writes the next
/// prompt to **stdout**, which is a true end-of-command signal because the loop
/// cannot print a prompt until it has finished executing. Command output itself
/// arrives on **stderr**.
class EmulatorHost : public IDebugBackend
{
    Q_OBJECT

public:
    explicit EmulatorHost(QObject *parent = nullptr);
    ~EmulatorHost() override;

    bool start(const SessionConfig &config, QString *error) override;
    void stop() override;

    bool isRunning() const override;
    bool isStopped() const override { return m_stopped; }

    BackendKind kind() const override { return BackendKind::Native; }

    /// Queue a *free text* debugger command — the user's console and the remote
    /// `cmd` verb — delivered over stdin, so it works while the debugger is
    /// stopped. A `commandFinished` signal follows.
    ///
    /// The response is ResponseKind::None: text for the log, never a state read.
    /// Text that looks like a dump (`m $100 16`, `memdump`) or like a read
    /// (`r`, `d`) answers through commandFinished alone rather than being
    /// parsed into the snapshot or published to a pane (MAJ-12). Internal
    /// requests use the typed intents below instead.
    void command(const QString &commandText) override;

    void step() override;     // `s`
    void stepOver() override; // `n`
    void resume() override;   // `c`
    /// Break into a running emulation by arming an always-true one-shot over
    /// the control socket — the only channel serviced while emulation runs
    /// (docs/PLAN.md §3.3); see IDebugBackend::pause.
    void pause() override;
    void setFloppyImage(int drive, const QString &path) override;

    /// Queue the commands needed to render a full state snapshot.
    void refresh() override;

    /// Remove every breakpoint, then arm the given ones. Order matters: clearing
    /// first means a rebuilt program cannot leave a stale breakpoint behind at an
    /// address that has been reused by different code.
    ///
    /// Must be called after the entry stop, because the program's load address is
    /// only known once it has been executed (docs/PLAN.md §5 rule 6).
    void clearBreakpoints() override;

    /// Arm one breakpoint by emitting its Hatari `b` command. The caller resolves
    /// source lines to addresses first (see debug/Breakpoint.h).
    void armBreakpoint(const QString &condition) override;

    /// Arm a one-shot at `address` (`b pc = $<addr> :once`).
    void breakAtAddressOnce(quint32 address) override;

    /// Request a memory dump of `length` bytes at `address`. The response
    /// arrives via commandFinished, and the rows parsed from it on
    /// memoryDumpReady (MAJ-45) — the panes and the remote verb read those, not
    /// the transcript.
    ///
    /// `tag` identifies which memory pane asked, so several panes can be open at
    /// once and each dump is routed back to the pane that requested it rather
    /// than broadcast to all of them.
    void requestMemoryDump(quint32 address, int length, int tag = 0) override;

    /// Request a memory dump of the stack (at the stack pointer). Routed to
    /// stackDumpReady instead of memoryDumpReady, so the stack view and the
    /// memory view do not clobber each other when both refresh on a stop.
    void requestStackDump(quint32 address, int length) override;

    /// Read all registers into the host's cached state.
    void dumpRegisters() override;

    void loadSymbols() override;                            // `symbols prg`
    void infoSubject(const QString &subject) override;      // `info <subject>`
    void readBasepage() override;                           // `info basepage`
    void setDisasmEngine(DisasmEngine engine) override;     // `setopt --disasm ext|uae`
    void profileOn() override;                              // `profile on`
    void profileOff() override;                             // `profile off`
    void profileSave(const QString &path) override;         // `profile save <path>`
    void writeRegister(const QString &name, quint32 value) override;      // `r <reg>=$<val>`
    void writeMemoryByte(quint32 address, quint8 value) override;         // `w b $<addr> $<val>`
    void readDisassembly() override;                        // `d`
    void readDisassemblyAt(quint32 address) override;       // `d $<addr>`
    void readHistory(int count) override;                   // `history <count>`

    static QString writeBootstrapScript(const QString &directory,
                                        const HatariCapabilities &caps,
                                        QString *error);


private:
    struct Pending
    {
        QString text;
        QString response;
        QByteArray raw;
        /// What this request's response *is*, set by the typed intent that
        /// queued it; a free-text command is None. completeCurrent switches on
        /// it instead of guessing from the command text (MAJ-12).
        ResponseKind kind = ResponseKind::None;
        /// For a dump request: the address it covers, and whether it is a stack
        /// dump (routed to stackDumpReady) or a memory-view dump. Carried per
        /// command so a queue of mixed dumps routes each correctly.
        quint32 dumpAddress = 0;
        bool stackDump = false;
        /// Which memory pane requested the dump, so it can be routed back.
        int dumpTag = 0;

        /// Set by infoSubject(): the subject the report was asked about, which
        /// names it on hardwareInfoReady() when the report comes back. Empty means this
        /// request is not an info report.
        QString infoSubject;

        /// Set by profileSave(): the file to name on profileSaveFinished() when
        /// this command completes.
        QString profilePath;

        /// Set by readDisassemblyAt(): report the text it read on
        /// disassemblyReady(), for the caller that asked about that one address.
        bool reportDisassembly = false;
        quint32 disassemblyAddress = 0;

        /// Set by readHistory(): report the text on historyReady().
        bool reportHistory = false;

        /// Whether this command must survive a resume(): the caller queued it
        /// in the same stack frame as the continue, so pruning it would silently
        /// lose it (see survivesResume). Stated by the typed intent for internal
        /// callers, and read out of the command's own language for free text.
        bool keepOnResume = false;

        /// Part of a state snapshot queued by refresh(). The responses of a
        /// batch each fill one part of MachineState, so only its last command
        /// emits stateUpdated (with the complete snapshot); the earlier members
        /// must not. A standalone state command emits its own update.
        bool batchMember = false;
        /// The last command of a state snapshot batch: its parsed response
        /// emits the single stateUpdated for the whole batch.
        bool batchEnd = false;
    };

    void dispatchNext();
    /// One dispatch attempt. Called only from dispatchNext()'s single-entry
    /// wrapper; see the re-entrancy note there.
    void dispatchNextOnce();
    void completeCurrent();
    void finishContinue();

    /// Re-entrancy guards for the transport state machine. The stderr line
    /// loop, the prompt handlers and the dispatch path call into each other: a
    /// stoppedChanged consumer queues commands from inside the banner line, and
    /// the pre-dispatch drain runs the line loop again. Re-entering the body
    /// mid-run interleaves the command queue and the stderr buffer (lost entry
    /// framing, heap corruption). Each entry point defers a nested request to
    /// the outermost frame instead. Frame-scoped: always cleared on unwind, so
    /// they cannot leak across sessions.
    bool m_inStderr = false;
    bool m_inDispatch = false;
    bool m_dispatchPending = false;

    /// Core of command(): report a missing session, then queue and dispatch.
    /// Split out so a refresh batch shares the same guard and reporting rather
    /// than duplicating it.
    void enqueue(Pending pending);

    /// The one place a typed intent becomes a queue entry: the debugger text for
    /// it, what its response is, and whether it must survive a resume(). Keeps
    /// the wire spelling inside this class — and off the callers (MAJ-21).
    void enqueueIntent(const QString &commandText, ResponseKind kind = ResponseKind::None,
                       bool keepOnResume = false);

    /// Queue one command of a state snapshot. See Pending::batchMember.
    void enqueueSnapshotCommand(const QString &command, ResponseKind kind, bool last);

    /// Reset every per-session framing field. Called from start(), so a second
    /// session on the same host cannot inherit the first one's queue, buffers,
    /// owed prompts or pending timers (docs/code-review-glm-001.md §P2).
    void resetTransport();
    void onPrompt();
    void onCommandTimeout();
    void handleStderrLine(const QString &line);

    /// Read and dispatch whatever stderr data the process has buffered.
    void processStderrData();

    /// Read pending stderr synchronously, before deciding a response is done.
    void drainStderr();
    void handleStdoutData(const QByteArray &data);
    void parseRegisters(const QString &response);
    void parseBasepage(const QString &response);
    void parseDisassembly(const QString &response);
    /// Open the control-socket server (shared EmbedSocket). Returns false when
    /// a requested socket could not be listened on; a build without the option
    /// is a success with no server (the session runs over stdin/stderr only).
    bool openSocketServer(QString *error);
    SessionConfig m_config;

    QProcess *m_process = nullptr;
    /// The control-socket server (embed size reports + `hatari-debug` lines
    /// for pause). Shared with the HRDB backend.
    EmbedSocket m_embedSocket;

    QByteArray m_stderrBuffer;

    QQueue<Pending> m_queue;
    bool m_haveCurrent = false;
    Pending m_current;
    QTimer *m_commandTimeout = nullptr;
    QTimer *m_settleTimer = nullptr;

    /// Prompt detection. The debugger prompt is `> ` written before each
    /// blocking read; which stream carries it depends on the build (stdout with
    /// readline, stderr without), so both are counted.
    ///
    /// m_promptCount is tallied incrementally as chunks arrive (m_promptCarry is
    /// the previous chunk's last two characters, so a prompt split across two
    /// chunks is still seen), never by re-counting the whole buffer: the buffer
    /// is truncated above 64 KiB, and a count that spans the truncation silently
    /// lost the prompts it dropped (MAJ-15).
    QString m_stdoutText;
    QString m_promptCarry;
    int m_stdoutLoggedChars = 0;
    quint64 m_promptCount = 0;
    quint64 m_promptTarget = 0;

    /// Set when the debugger has announced entry but its session dump is still
    /// being written; cleared once it reaches its prompt.
    bool m_awaitingEntryPrompt = false;

    /// Prompts still owed by commands that timed out. Their arrival must be
    /// swallowed so they cannot complete a later command.
    int m_owedPrompts = 0;

    /// Size of the stderr buffer when the current command was dispatched. Used to
    /// tell a fresh prompt from the stale one left over from before it.
    int m_stderrAtDispatch = 0;

    /// Address of the most recent memdump request, so its response can be
    /// reported back with the address it came from.

    /// Continue was requested while breakpoint-arming commands were still
    /// queued. Keep the debugger stopped until those flush, then write `c`.
    bool m_continueWhenIdle = false;

    MachineState m_state;
    bool m_stopped = false;
};

} // namespace pist
