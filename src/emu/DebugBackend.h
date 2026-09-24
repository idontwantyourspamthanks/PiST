// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/HatariTextParse.h"
#include "emu/MachineState.h"
#include "emu/MemoryDump.h"
#include "emu/SessionConfig.h"

#include <QObject>
#include <QProcessEnvironment>
#include <QString>

namespace pist {

// Must be `struct` to match emu/HatariProbe.h — MSVC mangles the mismatch
// (see EmulatorHost.h for the long form of this warning).
struct HatariCapabilities;

/// Which debug transport drives the session.
///
///   Native  stock Hatari, driven over stdin/stderr with prompt framing
///           (EmulatorHost; docs/PLAN.md §3.3).
///   Hrdb    the tattlemuss/hatari hrdb-main fork, driven over its typed TCP
///           protocol on 56001 (HrdbBackend; docs/PLAN.md §9 spike). The fork
///           is a user-supplied emulator, selected explicitly in the project
///           settings — it is version-identical to upstream, so no capability
///           probe can tell the two apart.
enum class BackendKind { Native, Hrdb };

/// What a request's response *is*, carried on the request itself rather than
/// inferred when it completes.
///
/// The command's first character is not a usable substitute: Hatari has many
/// non-disassembly `d*` commands (`db` = dspbreak, `dm`, `ds`, `dspbreak`) and
/// `parseDisassembly()` starts by clearing the cached disassembly, so a `db`
/// command once wiped the snapshot the Disassembly pane and HRDB's step-over
/// fall-through read — and published an empty state update (finding MAJ-12).
/// Text that merely resembles a dump was misrouted the same way: a console
/// `memdump $100 16` was published as memoryDumpReady with address 0, to a pane
/// that had not asked for a dump.
///
/// Every value but None is set by the typed intent that asked for it, so no
/// response is classified from text anywhere: the kinds below belong to
/// dumpRegisters/readDisassembly/readBasepage, and `MemoryDump` only ever to
/// requestMemoryDump()/requestStackDump(), which carry the pane tag with them.
/// A free-text command (the user's console, the remote `cmd` verb) is None —
/// its response is text for the log, never a state read.
enum class ResponseKind { None, Registers, Basepage, Disassembly, MemoryDump };

/// Whether a queued *free-text* command must survive a resume(). The keep
/// decision lives on the request itself (Pending::keepOnResume, which the typed
/// intents state when they queue); this predicate is how the free-text entry
/// points fill it in, from the only thing there is to go on — the user's own
/// text.
///
/// What can be read out of that text is the debugger's language: `b …` arms a
/// breakpoint (Hatari's `b all`, `b pc = $addr`, and the watchpoint
/// self-inequality `b ($addr).w ! …`), `profile …` controls collection.
///
/// The arm case matters because callers queue an arm and continue in one stack
/// frame (MainWindow::profileToCursor arms its breakpoint, turns profiling on,
/// then resumes synchronously), so a command written before the resume lands in
/// the queue rather than at the debugger. `profile` control commands are kept
/// for the same reason as the arms: they take effect at the continue itself
/// (Profile_CpuStart runs in DebugCpu_SetDebugging), so a pruned `profile on`
/// silently collects nothing while the UI reports "Collecting", and a pruned
/// `profile save`/`profile off` leaves the Profiler stuck on "Saving…".
inline bool survivesResume(const QString &text)
{
    return text.startsWith(QLatin1Char('b')) || text.startsWith(QLatin1String("profile "));
}

/// The environment both debug backends hand the Hatari process. Config
/// isolation (HOME / XDG_CONFIG_HOME pointed at the session dir, so Hatari never
/// loads the user's real `hatari.cfg` — docs/PLAN.md §5 rule 7) plus the X11
/// reparenting variables for an embedded display. Shared so the two transports
/// cannot drift apart.
inline QProcessEnvironment makeSessionEnvironment(const SessionConfig &config)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("HOME"), config.sessionDir);
    env.insert(QStringLiteral("XDG_CONFIG_HOME"), config.sessionDir);
    if (!config.parentWindowId.isEmpty()) {
        // Embedded display: Hatari reparents its SDL window into the container
        // window named here (src/control.c, under HAVE_X11 && SDL_VIDEO_DRIVER_X11).
        // Both processes must be X11 clients of the same display, which is why the
        // child is pinned to the X11 driver and why PiST runs on the xcb platform.
        env.insert(QStringLiteral("PARENT_WIN_ID"), config.parentWindowId);
        env.insert(QStringLiteral("SDL_VIDEODRIVER"), QStringLiteral("x11"));
    }
    return env;
}

/// The debug-transport contract MainWindow drives, independent of how the
/// emulator is talked to.
///
/// The interface is exactly the surface MainWindow used when EmulatorHost was
/// the only backend — nothing speculative. Both implementations emit the same
/// signals with the same meanings; the native backend's quirks (prompt
/// framing, the starved control socket) stay inside EmulatorHost.
///
/// What a caller wants is expressed by a *typed intent*, never by native
/// debugger text it built itself (finding MAJ-21). Before this, `src/ui/` and
/// `src/control/` assembled `m $%1 %2`, `info <subject>`, `profile on`,
/// `setopt --disasm ext`, `symbols prg`, `b pc = $X :once`, `w b $a $v` and
/// `r <reg>=$v` by hand, and each backend had to reverse-engineer them: the
/// same request had five spellings, and HRDB's translation of them guessed at
/// each command's gating from its prefix — which is how CRIT-6 (a profile
/// command pruned out of the queue), MAJ-12 (a `db` read as a disassembly) and
/// the hand-patched needsStop in setFloppyImage all happened. Each intent below
/// now states what is wanted; a backend renders its own wire syntax and its own
/// gating, and the caller knows neither.
class IDebugBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    ~IDebugBackend() override = default;

    /// Version-gating for the native bootstrap script. Only the native
    /// transport needs it; the HRDB fork's protocol is fixed, so the default
    /// ignores it.
    virtual void setCapabilities(const HatariCapabilities &caps) { Q_UNUSED(caps); }

    virtual bool start(const SessionConfig &config, QString *error) = 0;
    virtual void stop() = 0;

    virtual bool isRunning() const = 0;
    virtual bool isStopped() const = 0;

    /// The emulator process's id, or -1 when no session is running. Windows
    /// embedding has no window id to hand the emulator — Hatari's
    /// `PARENT_WIN_ID` reparenting is compiled in only under X11 upstream — so
    /// it finds the emulator's window by process instead (ui/EmbedWin32.h).
    /// Nothing else needs the id.
    virtual qint64 emulatorProcessId() const { return -1; }

    virtual void step() = 0;
    virtual void stepOver() = 0;
    virtual void resume() = 0;

    /// Break into a *running* emulation. Native arms a one-shot breakpoint
    /// `hatari-debug b pc ! 0 :once` over the control socket — a condition that is
    /// always true, so it traps at the next instruction. NOT `hatari-stop`: that
    /// only clears the VBL loop's active flag and never enters the debugger (see
    /// EmulatorHost::pause). HRDB: `break`, serviced at the next VBL. A stop
    /// follows via stoppedChanged(true), as for any breakpoint.
    virtual void pause() = 0;

    /// Queue the commands needed to render a full state snapshot; one
    /// stateUpdated follows when the batch completes.
    virtual void refresh() = 0;

    /// Queue a *free text* debugger command — the user's console, and the
    /// remote `cmd` verb. Nothing else belongs here: every internal request has
    /// a typed intent below, so no command PiST itself issues is ever
    /// classified or rewritten from its text. A `commandFinished` signal
    /// follows.
    virtual void command(const QString &commandText) = 0;

    /// Remove every breakpoint, then arm the given one — the caller clears
    /// first so a rebuilt program cannot leave a stale breakpoint behind.
    virtual void clearBreakpoints() = 0;

    /// Arm one conditional breakpoint. `condition` is the planner's Hatari
    /// breakpoint command (`b pc = $addr`, `&& <cond>` appended, or a
    /// watchpoint's `b ($addr).w ! ($addr).w` — see debug/Breakpoint.h and
    /// debug/Watchpoint.h): the debugger's own condition language, which both
    /// transports carry.
    virtual void armBreakpoint(const QString &condition) = 0;

    virtual void requestMemoryDump(quint32 address, int length, int tag = 0) = 0;
    virtual void requestStackDump(quint32 address, int length) = 0;
    virtual void dumpRegisters() = 0;

    /// Load the program's symbols into the debugger, so addresses resolve to
    /// names (`symbols prg`).
    virtual void loadSymbols() = 0;

    /// Read one of the debugger's `info <subject>` reports — the hardware
    /// state no memory dump can show (see ui/HardwareView's subject list). The
    /// report is not part of MachineState: it is parsed into the summary a pane
    /// displays and comes back on hardwareInfoReady() with the subject it was
    /// asked for.
    virtual void infoSubject(const QString &subject) = 0;

    /// Read the program's section bases into MachineState (`textBase`,
    /// `dataBase`, `bssBase`): the live addresses a source line is resolved
    /// against. Fills the snapshot, so one stateUpdated follows.
    virtual void readBasepage() = 0;

    /// Which disassembler a profile save should be rendered with. Ext is the
    /// external (Capstone) renderer, Uae the debugger's built-in one: the UAE
    /// core writes profile disassembly to the trace file instead of the save
    /// file, so a save needs Ext (`setopt --disasm ext|uae`).
    enum class DisasmEngine { Ext, Uae };
    virtual void setDisasmEngine(DisasmEngine engine) = 0;

    /// CPU profile collection. It takes effect on the next continue
    /// (Profile_CpuStart runs in DebugCpu_SetDebugging), so a queued control
    /// command must survive a resume().
    virtual void profileOn() = 0;
    virtual void profileOff() = 0;
    /// Save the collected profile to `path`. Completion is reported on
    /// profileSaveFinished(path) — the caller then parses the file, because the
    /// debugger's acknowledgement says nothing about its contents.
    virtual void profileSave(const QString &path) = 0;

    /// Arm a breakpoint that fires once at `address` — the debugger's
    /// `b pc = $<addr> :once` — the form every "stop there once" gesture uses:
    /// run to cursor, step-out's return address, and the guided profile run.
    virtual void breakAtAddressOnce(quint32 address) = 0;

    /// Write one byte of memory. Native: `w b $<addr> $<value>`. HRDB: the
    /// fork's `memset <addr> 1 <value>`, whose middle argument is a byte count
    /// and whose value is two hex digits.
    virtual void writeMemoryByte(quint32 address, quint8 value) = 0;

    /// Write a register: `r <name>=$<value>`, where the `=` is mandatory and a
    /// successful write prints nothing.
    virtual void writeRegister(const QString &name, quint32 value) = 0;

    /// Read the disassembly into MachineState, at the program counter or at
    /// `address`. Both fill the snapshot (one stateUpdated follows);
    /// readDisassemblyAt() also reports the text it read on
    /// disassemblyReady(), which is what a caller that asked about one address
    /// wants back (the remote `disasm` verb) rather than the snapshot.
    virtual void readDisassembly() = 0;
    virtual void readDisassemblyAt(quint32 address) = 0;

    /// Read the last `count` program-counter values, which the debugger records
    /// from the `history cpu` in the bootstrap script. Not part of
    /// MachineState: the execution-path pane is the only consumer, and the text
    /// arrives on historyReady().
    virtual void readHistory(int count) = 0;

    /// A debugger command typed by the user into the console input. Backends
    /// may surface its ack/response in the log (HRDB does; native streams via
    /// stderr already). Internal control commands (step/resume/bp arming) go
    /// through the typed intents instead and stay silent.
    ///
    /// A console command's response is text the user asked to see, never a
    /// state read: it is enqueued with ResponseKind::None, so no response text
    /// can rewrite MachineState or be published to a pane.
    virtual void consoleCommand(const QString &commandText) { command(commandText); }

    /// Insert or eject a floppy in a live session. Drive 0 is A:. An empty
    /// path ejects (`none`). While the debugger is stopped this is Hatari's
    /// `setopt --disk-a|--disk-b <path>` — over stdin (native) or through the
    /// fork's console (HRDB). A *running* native session has no readable stdin,
    /// so it sends the same command as a `hatari-debug` line on the control
    /// socket (the path pause uses); HRDB is serviced while running either way,
    /// so its console `setopt` goes out immediately. No session: a no-op — the
    /// path is already in project settings for the next Run. Default is a
    /// no-op.
    virtual void setFloppyImage(int drive, const QString &path)
    {
        Q_UNUSED(drive);
        Q_UNUSED(path);
    }

    /// The backend this is, for the log and for diagnostics.
    virtual BackendKind kind() const = 0;

signals:
    /// A memory-dump response: the address it covers, the rows the debugger
    /// printed, and the tag of the pane that asked for it.
    ///
    /// The rows are parsed in the transport that read the text, once (MAJ-45).
    /// A dump used to cross this boundary as a transcript and be parsed again
    /// by every consumer — the memory pane, the remote `readmem` verb, and, for
    /// a stack dump, both the step-out path and the stack view. The text is not
    /// lost with it: every response, this one included, still arrives verbatim
    /// on commandFinished().
    void memoryDumpReady(quint32 address, const QList<MemoryRow> &rows, int tag);
    /// A stack dump response, routed separately from the memory view's. Its
    /// rows are what the stack view renders and what the step-out path reads
    /// the return address from — one parse, two consumers.
    void stackDumpReady(quint32 sp, const QList<MemoryRow> &rows);

    /// The report an infoSubject() call answered with: the subject it asked
    /// about, so the pane that asked can recognise it, and the summary parsed
    /// from the report (MAJ-45), which is what the pane displays.
    void hardwareInfoReady(const QString &subject, const pist::HardwareSummary &summary);

    /// The text a readDisassemblyAt() answered with, for a caller that wants the
    /// disassembly itself rather than the snapshot the read also fills.
    /// readDisassembly() — the snapshot's own read at the PC — does not emit
    /// this: nothing is waiting on its text.
    void disassemblyReady(quint32 address, const QString &response);

    /// A profileSave() completed; `path` is the file it wrote.
    void profileSaveFinished(const QString &path);

    /// The text a readHistory() answered with.
    void historyReady(const QString &response);

    void runningChanged(bool running);
    void stoppedChanged(bool stopped);
    void commandFinished(const QString &command, const QString &response);

    /// One complete snapshot per state batch, not one per response.
    void stateUpdated(const pist::MachineState &state);
    void logLine(const QString &line);
    void errorOccurred(const QString &message);

    /// The emulator reported a new video size. It arrives over the control
    /// socket, which both backends keep open when the session has one (HRDB
    /// uses the same EmbedSocket, upstream's socket being part of its argv), so
    /// either transport emits it — a build without the socket never does.
    void embeddedSizeChanged(int width, int height);
};

/// Create the backend for a kind. Defined where both implementations are
/// linked.
IDebugBackend *createBackend(BackendKind kind, QObject *parent);

} // namespace pist
