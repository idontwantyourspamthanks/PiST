// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/DebugBackend.h"

#include <QString>
#include <QStringList>

class QLibrary;

namespace pist {

/// Where a macOS app keeps hatari_libretro. From Contents/MacOS the core is
/// ../Frameworks/<name>, which is where the disk image copies it; beside the
/// executable is the second place, for a build tree that has not been bundled.
/// Only paths are returned. Existence is the loader's concern.
QString libretroCoreFileName();
QStringList libretroCoreCandidates(const QString &applicationDir);

/// In-process Hatari. The dylib's ABI is emu/LibretroAbi.h. Until a session
/// launch selects this backend, a missing dylib fails start() and the
/// subprocess backends are what Run uses. docs/agents/mac.md.
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

private:
    /// The typed intents the first slice does not call yet. Named, so a caller
    /// that reaches one hears which one, instead of a silent no-op.
    void notInThisSlice(const QString &what);

    QLibrary *m_library = nullptr;
    bool m_running = false;
    bool m_stopped = false;
};

} // namespace pist
