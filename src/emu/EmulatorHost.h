// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"
#include "emu/SessionConfig.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

class QLocalServer;
class QLocalSocket;
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
class EmulatorHost : public QObject
{
    Q_OBJECT

public:
    explicit EmulatorHost(QObject *parent = nullptr);
    ~EmulatorHost() override;

    /// Record what the emulator build supports. Consulted when writing the
    /// bootstrap script, which is version-gated (§5 rule 9).
    void setCapabilities(const HatariCapabilities &caps) { m_caps = &caps; }

    bool start(const SessionConfig &config, QString *error);
    void stop();

    bool isRunning() const;
    bool isStopped() const { return m_stopped; }

    /// Queue a debugger command. Delivered over stdin, so it works while the
    /// debugger is stopped. A `commandFinished` signal follows.
    ///
    /// The dump routing is per-command, not global, so a memory dump and a stack
    /// dump queued back-to-back each reach the right listener: dumpAddress and
    /// stackDump identify a `memdump` for routing to memoryDumpReady or
    /// stackDumpReady rather than commandFinished alone.
    void command(const QString &command, quint32 dumpAddress = 0, bool stackDump = false,
                 int dumpTag = 0);

    void step();     // `s`
    void stepOver(); // `n`
    void resume();   // `c`

    /// Queue the commands needed to render a full state snapshot.
    void refresh();

    /// Remove every breakpoint, then arm the given ones. Order matters: clearing
    /// first means a rebuilt program cannot leave a stale breakpoint behind at an
    /// address that has been reused by different code.
    ///
    /// Must be called after the entry stop, because the program's load address is
    /// only known once it has been executed (docs/PLAN.md §5 rule 6).
    void clearBreakpoints();

    /// Arm one breakpoint by emitting its Hatari `b` command. The caller resolves
    /// source lines to addresses first (see debug/Breakpoint.h).
    void armBreakpoint(const QString &condition);

    /// Request a memory dump of `length` bytes at `address`. The response arrives
    /// via commandFinished; the raw text is also parsed into memoryDumpReady.
    ///
    /// `tag` identifies which memory pane asked, so several panes can be open at
    /// once and each dump is routed back to the pane that requested it rather
    /// than broadcast to all of them.
    void requestMemoryDump(quint32 address, int length, int tag = 0);

    /// Request a memory dump of the stack (at the stack pointer). Routed to
    /// stackDumpReady instead of memoryDumpReady, so the stack view and the
    /// memory view do not clobber each other when both refresh on a stop.
    void requestStackDump(quint32 address, int length);

    /// Read all registers into the host's cached state.
    void dumpRegisters();

    static QString writeBootstrapScript(const QString &directory,
                                        const HatariCapabilities &caps,
                                        QString *error);

signals:
    /// A `memdump` response, with the command it answered and the tag of the
    /// pane that asked for it.
    void memoryDumpReady(quint32 address, const QString &response, int tag);

    /// A stack `memdump` response, routed separately from the memory view's.
    void stackDumpReady(quint32 address, const QString &response);

    void runningChanged(bool running);
    void stoppedChanged(bool stopped);
    void commandFinished(const QString &command, const QString &response);

    /// One complete snapshot per state batch, not one per response: a batch
    /// queued by refresh() is only complete once its last command has been
    /// parsed, so a listener reacts (and re-queues its own follow-up commands)
    /// once per stop instead of once per debugger response.
    void stateUpdated(const pist::MachineState &state);
    void logLine(const QString &line);
    void errorOccurred(const QString &message);

    /// The emulator reported a new video size over the control socket, in
    /// response to `hatari-embed-info`. Only emitted when the display is
    /// embedded; the container resizes itself to match.
    void embeddedSizeChanged(int width, int height);

private:
    struct Pending
    {
        QString text;
        QString response;
        QByteArray raw;
        /// For a `memdump` command: the address it dumps, and whether it is a
        /// stack dump (routed to stackDumpReady) or a memory-view dump. Carried
        /// per command so a queue of mixed dumps routes each correctly.
        quint32 dumpAddress = 0;
        bool stackDump = false;
        /// Which memory pane requested a `memdump`, so it can be routed back.
        int dumpTag = 0;

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
    void completeCurrent();

    /// Core of command(): report a missing session, then queue and dispatch.
    /// Split out so a refresh batch shares the same guard and reporting rather
    /// than duplicating it.
    void enqueue(Pending pending);

    /// Queue one command of a state snapshot. See Pending::batchMember.
    void enqueueSnapshotCommand(const QString &command, bool last);

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
    void handleSocketData();
    void parseRegisters(const QString &response);
    void parseBasepage(const QString &response);
    void parseDisassembly(const QString &response);
    void openSocketServer(QString *error);
    void closeSocketServer();

    SessionConfig m_config;
    const HatariCapabilities *m_caps = nullptr;

    QProcess *m_process = nullptr;
    QLocalServer *m_server = nullptr;
    QLocalSocket *m_socket = nullptr;

    QByteArray m_stderrBuffer;
    QByteArray m_socketBuffer;

    QQueue<Pending> m_queue;
    bool m_haveCurrent = false;
    Pending m_current;
    QTimer *m_commandTimeout = nullptr;
    QTimer *m_settleTimer = nullptr;

    /// Prompt detection. The debugger prompt is `> ` written before each
    /// blocking read; which stream carries it depends on the build (stdout with
    /// readline, stderr without), so both are counted.
    QString m_stdoutText;
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

    MachineState m_state;
    bool m_stopped = false;
};

} // namespace pist
