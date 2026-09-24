// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/DebugBackend.h"
#include "emu/EmbedSocket.h"

#include "emu/MachineState.h"

#include <QByteArray>
#include <QQueue>
#include <QString>

class QProcess;
class QTcpSocket;
class QTimer;

namespace pist {

/// Debug backend over the HRDB protocol: the tattlemuss/hatari hrdb-main
/// fork's typed TCP channel on 127.0.0.1:56001, protocol 0x100a.
///
/// Unlike the native transport there is no channel split and no prompt
/// framing: the socket is serviced while running *and* while stopped (the
/// fork's RemoteDebug_BreakLoop replaces the stdin debugger loop for the
/// session — the fork's stdin is never read), commands are NUL-terminated
/// ASCII, replies are 0x01-separated and NUL-terminated, and async
/// `!`-prefixed notifications share the stream. Verified live against the
/// fork; docs/PLAN.md §9 has the spike record.
///
/// Text-shaped debugger features (disassembly, `info`, history) go through
/// the fork's `console` command, which returns only OK on the socket: the
/// output lands on the process's stderr (debugOutput and the disassembler's
/// TraceFile both default there), flushed before the OK is sent, so the
/// response is the stderr tail since dispatch, drained when the OK arrives.
class HrdbBackend : public IDebugBackend
{
    Q_OBJECT

public:
    explicit HrdbBackend(QObject *parent = nullptr);
    ~HrdbBackend() override;

    bool start(const SessionConfig &config, QString *error) override;
    void stop() override;

    bool isRunning() const override;
    bool isStopped() const override { return m_stopped; }
    qint64 emulatorProcessId() const override;

    BackendKind kind() const override { return BackendKind::Hrdb; }

    void step() override;
    void stepOver() override;
    void resume() override;
    /// `break`, serviced at the next VBL — works while running, which is what
    /// HRDB exists for here (docs/FUTURE.md §1).
    void pause() override;
    void setFloppyImage(int drive, const QString &path) override;
    void refresh() override;

    void consoleCommand(const QString &command) override;
    void command(const QString &commandText) override;

    void clearBreakpoints() override;
    void armBreakpoint(const QString &condition) override;
    void breakAtAddressOnce(quint32 address) override;

    void requestMemoryDump(quint32 address, int length, int tag = 0) override;
    void requestStackDump(quint32 address, int length) override;
    void dumpRegisters() override;

    void loadSymbols() override;
    void infoSubject(const QString &subject) override;
    void readBasepage() override;
    void setDisasmEngine(DisasmEngine engine) override;
    void profileOn() override;
    void profileOff() override;
    void profileSave(const QString &path) override;
    void writeRegister(const QString &name, quint32 value) override;
    void writeMemoryByte(quint32 address, quint8 value) override;
    void readDisassembly() override;
    void readDisassemblyAt(quint32 address) override;
    void readHistory(int count) override;

private:
    struct Pending
    {
        QString text;      ///< the request as the caller phrased it, for the log
        QString wire;      ///< what goes over the socket
        /// What this request's response *is*, set by the typed intent that
        /// queued it, None for a free-text command. completeCurrent switches on
        /// it instead of the text prefixes that read a `db` (dspbreak) as a
        /// disassembly and untagged text as a pane dump (MAJ-12).
        ResponseKind kind = ResponseKind::None;
        quint32 memBytes = 0;   ///< for `mem`: requested length (payload is padded)
        /// A free-text command (the user's console, the remote `cmd` verb):
        /// someone is waiting on its answer, so its deferral is announced (the
        /// sender hears why nothing is coming, instead of the silent hold the
        /// old per-text fallback did).
        bool freeText = false;
        /// Set once the deferral above has been reported, so a command held
        /// over many dispatch attempts is announced exactly once.
        bool deferAnnounced = false;
        quint32 dumpAddress = 0;
        bool stackDump = false;
        int dumpTag = 0;
        /// Set by infoSubject(): the subject the report was asked about, which
        /// names it on hardwareInfoReady() when it comes back. Empty means this request
        /// is not an info report.
        QString infoSubject;
        /// Set by profileSave(): the file to name on profileSaveFinished().
        QString profilePath;
        /// Set by readDisassemblyAt(): report the text it read on
        /// disassemblyReady(), for the caller that asked about that address.
        bool reportDisassembly = false;
        quint32 disassemblyAddress = 0;
        /// Set by readHistory(): report the text on historyReady().
        bool reportHistory = false;
        /// The response is the process's stderr tail since dispatch (console
        /// passthroughs), drained when the socket OK arrives.
        bool captureStderr = false;
        /// The command runs from the fork's remote break loop, so it cannot be
        /// dispatched against a running machine: the queue holds it until the
        /// next stop. Stated per request, never inferred from the command text
        /// (the fallback that used to do the inferring changed a running
        /// floppy insert's semantics — see setFloppyImage).
        bool needsStop = false;
        /// Whether this request must survive a resume(): the caller queued it
        /// in the same stack frame as the continue (see survivesResume).
        bool keepOnResume = false;
        /// State-snapshot batch membership, same contract as the native
        /// backend: stateUpdated fires once, when the batch's last completes.
        bool batchMember = false;
        bool batchEnd = false;
    };

    /// The Pending for a request the fork's `console` handler runs: `wireBody`
    /// is the debugger command passed to it, the response is the process's
    /// stderr tail (the fork flushes before its OK), and it needs the remote
    /// break loop — a debugger command cannot run against a moving machine.
    static Pending consoleRequest(const QString &text, const QString &wireBody,
                                  ResponseKind kind = ResponseKind::None,
                                  bool keepOnResume = false);

    /// The Pending for a *free-text* command — the user's console, the remote
    /// `cmd` verb, and nothing else. The text is passed to the fork's console
    /// unchanged: PiST rewrites nothing, so a command means here what it means
    /// in the debugger (the table that used to rewrite `r`/`s`/`c`/`b`/`w b`
    /// by prefix is gone with the callers that needed it — CRIT-6, MAJ-12).
    static Pending freeTextRequest(const QString &text);

    void enqueue(Pending pending);
    void dispatchNext();
    void completeCurrent(const QByteArray &message);
    void handleNotification(const QByteArray &message);

    void onSocketData();

    /// Fill m_state from a typed `regs` reply's key/value fields, including
    /// the live TEXT/DATA/BSS bases (the fork reports them as register keys,
    /// so no `info basepage` text parse is needed).
    void parseRegPairs(const QList<QByteArray> &fields);

    /// The stderr tail since the current console command's dispatch, draining
    /// the pipe first (the fork flushes before it sends the OK).
    QString readStderrTail();

    /// uudecode the fork's `mem` payload (4 chars per 3 bytes, 32-based,
    /// zero-padded at the tail) into the rows both backends publish. memBytes
    /// caps the decode: the payload can hold up to 2 padding bytes beyond the
    /// request.
    static QList<MemoryRow> memoryRows(quint32 address, quint32 memBytes, const QByteArray &uu);

    QProcess *m_process = nullptr;
    QTcpSocket *m_socket = nullptr;
    QTimer *m_connectRetry = nullptr;
    QTimer *m_handshakeWatchdog = nullptr;
    /// Fails a command the fork never answered — the native backend has a
    /// per-command timeout; HRDB had only the handshake watchdog, so a dead link
    /// wedged the queue forever (finding 11).
    QTimer *m_commandWatchdog = nullptr;

    QByteArray m_buffer;
    QQueue<Pending> m_queue;
    bool m_haveCurrent = false;
    /// After the command watchdog fails a command, one reply is still owed to it
    /// (a slow — not dead — fork may answer late). Swallow the next reply so it
    /// can't be mis-attributed to the following command, mirroring the native
    /// backend's owed-prompts guard (finding 11).
    bool m_owedReply = false;
    Pending m_current;

    /// The control-socket server, used for the embedded display's video-size
    /// reports. The fork keeps the upstream `--control-socket`; HRDB replaces
    /// only the debugger channels.
    EmbedSocket m_embedSocket;

    QString m_stderrText;
    qint64 m_stderrConsumed = 0;
    QString m_sessionDir;

    MachineState m_state;
    bool m_stopped = false;
    /// Between process start and the socket handshake: commands queue up and
    /// dispatch once the listener accepts us.
    bool m_ready = false;
};

} // namespace pist
