// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"
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

/// Hatari `b all`, `b pc = $addr`, and watchpoint self-inequality (`b ($addr).w ! …`).
/// Resume must keep these in the queue: Continue is enabled at the entry stop,
/// which is before `armBreakpoints()` has been flushed to the debugger.
inline bool isBreakpointCommand(const QString &text)
{
    return text.startsWith(QLatin1Char('b'));
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

    virtual void step() = 0;
    virtual void stepOver() = 0;
    virtual void resume() = 0;

    /// Break into a *running* emulation. Native: `hatari-stop` over the
    /// control socket (unavailable where the socket is not compiled in).
    /// HRDB: `break`, serviced at the next VBL. A stop follows via
    /// stoppedChanged(true), as for any breakpoint.
    virtual void pause() = 0;

    /// Queue the commands needed to render a full state snapshot; one
    /// stateUpdated follows when the batch completes.
    virtual void refresh() = 0;

    /// Queue a debugger command. A `commandFinished` signal follows. The dump
    /// routing arguments identify a memory dump so it reaches memoryDumpReady
    /// or stackDumpReady rather than commandFinished alone.
    virtual void command(const QString &command, quint32 dumpAddress = 0,
                         bool stackDump = false, int dumpTag = 0) = 0;

    /// Remove every breakpoint, then arm the given one — the caller clears
    /// first so a rebuilt program cannot leave a stale breakpoint behind.
    virtual void clearBreakpoints() = 0;
    virtual void armBreakpoint(const QString &condition) = 0;

    virtual void requestMemoryDump(quint32 address, int length, int tag = 0) = 0;
    virtual void requestStackDump(quint32 address, int length) = 0;
    virtual void dumpRegisters() = 0;

    /// A debugger command typed by the user into the console input. Backends
    /// may surface its ack/response in the log (HRDB does; native streams via
    /// stderr already). Internal control commands (step/resume/bp arming) go
    /// through command() instead and stay silent.
    virtual void consoleCommand(const QString &commandText) { command(commandText); }

    /// Insert or eject a floppy in a live session. Drive 0 is A:. An empty
    /// path ejects (`none`). While the debugger is stopped this is Hatari's
    /// `setopt --disk-a|--disk-b` (stdin / HRDB `console`); while native
    /// emulation is running it is `hatari-option` on the control socket,
    /// which is otherwise unread. No session: a no-op — the path is already
    /// in project settings for the next Run. Default is a no-op.
    virtual void setFloppyImage(int drive, const QString &path)
    {
        Q_UNUSED(drive);
        Q_UNUSED(path);
    }

    /// The backend this is, for the log and for diagnostics.
    virtual BackendKind kind() const = 0;

signals:
    /// A memory-dump response, with the command it answered and the tag of the
    /// pane that asked for it.
    void memoryDumpReady(quint32 address, const QString &response, int tag);
    /// A stack dump response, routed separately from the memory view's.
    void stackDumpReady(quint32 address, const QString &response);

    void runningChanged(bool running);
    void stoppedChanged(bool stopped);
    void commandFinished(const QString &command, const QString &response);

    /// One complete snapshot per state batch, not one per response.
    void stateUpdated(const pist::MachineState &state);
    void logLine(const QString &line);
    void errorOccurred(const QString &message);

    /// The emulator reported a new video size. Only the native backend emits
    /// this (it arrives over the control socket, which HRDB does not have).
    void embeddedSizeChanged(int width, int height);
};

/// Create the backend for a kind. Defined where both implementations are
/// linked.
IDebugBackend *createBackend(BackendKind kind, QObject *parent);

} // namespace pist
