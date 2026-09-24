// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/DebugBackend.h"
#include "emu/LibretroAbi.h"

#include <QByteArray>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QWaitCondition>

#include <atomic>
#include <cstdint>

class QLibrary;
class QThread;

namespace pist {

/// Where a macOS app keeps hatari_libretro. From Contents/MacOS the core is
/// ../Frameworks/<name>, which is where the disk image copies it; beside the
/// executable is the second place, for a build tree that has not been bundled.
/// Only paths are returned. Existence is the loader's concern.
QString libretroCoreFileName();
QStringList libretroCoreCandidates(const QString &applicationDir);
/// The first candidate that exists, or empty. `--diagnose` prints this.
QString findLibretroCore(const QString &applicationDir);

/// Whether a session boots the in-process core. True only on macOS, when the
/// project has not named its own Hatari, and the dylib is present. A named
/// emulator path keeps the subprocess even on a Mac that has the core. Linux
/// and Windows stay subprocesses: `onMacOS` is false there. The flag is an
/// argument so the decision can be tested off a Mac.
bool sessionUsesInProcessCore(bool onMacOS, const QString &hatariPath,
                              const QString &applicationDir);

/// In-process Hatari. The dylib's ABI is emu/LibretroAbi.h. Session launch
/// selects it on macOS when `sessionUsesInProcessCore` is true. A missing
/// dylib fails start(). The owner thread is the only caller of the core:
/// the UI posts steps, resumes, and breakpoint changes onto it.
/// docs/agents/mac.md.
class LibretroBackend : public IDebugBackend
{
    Q_OBJECT

public:
    explicit LibretroBackend(QObject *parent = nullptr);
    ~LibretroBackend() override;

    bool start(const SessionConfig &config, QString *error) override;
    void stop() override;

    bool isRunning() const override;
    bool isStopped() const override;

    void step() override;
    void stepOver() override;
    void resume() override;
    void pause() override;
    void refresh() override;
    void command(const QString &commandText) override;
    void clearBreakpoints() override;
    void armBreakpoint(const QString &condition) override;
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
    void breakAtAddressOnce(quint32 address) override;
    void writeMemoryByte(quint32 address, quint8 value) override;
    void writeRegister(const QString &name, quint32 value) override;
    void readDisassembly() override;
    void readDisassemblyAt(quint32 address) override;
    void readHistory(int count) override;

    BackendKind kind() const override { return BackendKind::Libretro; }

    /// Bumped by `stop()`. A `frameReady` whose epoch does not match arrived
    /// for a session that has already ended.
    int frameEpoch() const { return m_epoch.load(); }

signals:
    /// One copied frame from the core's thread, `Format_RGB32` words.
    /// `epoch` is `frameEpoch()` at the moment the core returned it. The
    /// bytes are owned; the core's pointer is not.
    void frameReady(const QByteArray &pixels, int width, int height, int pitch, int epoch);

private:
    /// A request the UI thread hands to the owner. `keepOnResume` is the
    /// subprocess rule: Continue is live at the entry stop, before the
    /// breakpoint arms have been posted, and those arms have to be applied
    /// before the CPU runs again.
    enum class CoreJob {
        Arm,
        Clear,
        BreakOnce,
        Registers,
        Basepage,
        Refresh,
        Step,
        StepOver,
        Pause,
        Memory,
        Stack,
    };
    struct CoreRequest {
        CoreJob job = CoreJob::Refresh;
        bool keepOnResume = false;
        QString text;
        quint32 address = 0;
        int length = 0;
        int tag = 0;
    };

    /// The typed intents this backend does not serve. Named, so a caller
    /// that reaches one hears which one, instead of a silent no-op.
    void notInThisSlice(const QString &what);
    /// The core's owner. Runs frames, and the debugger calls, until `stop()`.
    void pump();
    void post(const CoreRequest &request);
    void dispatch(const CoreRequest &request);
    /// Registers and the basepage, one snapshot. Called on the owner thread.
    bool readCoreState();
    /// Blocks until the UI has handled the snapshot, so breakpoint arms posted
    /// from that handler are in the queue before the owner resumes.
    void publishState();
    void publishFrame();
    QList<MemoryRow> rowsFromRam(quint32 address, int length) const;

    using RunFn = int (*)(PistHatariFrame *, int *);
    using HaltFn = void (*)();
    using StepFn = int (*)();
    using ArmFn = int (*)(const char *);
    using RegsFn = int (*)(const char **, uint32_t *, int, int *);
    using BaseFn = int (*)(uint32_t *, uint32_t *, uint32_t *);
    using RamFn = void *(*)(size_t *);

    QLibrary *m_library = nullptr;
    QThread *m_thread = nullptr;
    RunFn m_runFn = nullptr;
    HaltFn m_haltFn = nullptr;
    StepFn m_stepFn = nullptr;
    StepFn m_stepOverFn = nullptr;
    StepFn m_resumeFn = nullptr;
    StepFn m_pauseFn = nullptr;
    StepFn m_clearFn = nullptr;
    ArmFn m_armFn = nullptr;
    RegsFn m_regsFn = nullptr;
    BaseFn m_baseFn = nullptr;
    RamFn m_ramFn = nullptr;
    QMutex m_gate;
    QWaitCondition m_wake;
    QQueue<CoreRequest> m_jobs;
    bool m_continueWhenIdle = false;
    MachineState m_state;
    std::atomic<bool> m_quit{false};
    std::atomic<bool> m_hold{false};
    std::atomic<int> m_epoch{0};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopped{false};
};

} // namespace pist
