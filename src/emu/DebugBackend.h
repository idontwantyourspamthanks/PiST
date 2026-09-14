// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/MachineState.h"
#include "emu/SessionConfig.h"

#include <QObject>
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
