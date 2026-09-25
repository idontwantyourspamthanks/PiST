// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/LibretroBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QMetaObject>

#include <cstring>
#include <QMutexLocker>
#include <QThread>

namespace pist {

QString libretroCoreFileName()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("hatari_libretro.dylib");
#elif defined(Q_OS_WIN)
    return QStringLiteral("hatari_libretro.dll");
#else
    return QStringLiteral("hatari_libretro.so");
#endif
}

QStringList libretroCoreCandidates(const QString &applicationDir)
{
    QStringList paths;
    if (applicationDir.isEmpty())
        return paths;
    const QString name = libretroCoreFileName();
    const QDir dir(applicationDir);
    // Contents/MacOS -> Contents/Frameworks, the place the disk image copies
    // the core. Beside the executable covers a build that has not been sealed
    // into an app bundle.
    paths << QDir::cleanPath(dir.absoluteFilePath(QStringLiteral("../Frameworks/") + name));
    paths << QDir::cleanPath(dir.absoluteFilePath(name));
    return paths;
}

QString findLibretroCore(const QString &applicationDir)
{
    // A built core that is not inside an app bundle. The release app does not
    // set this; the Frameworks candidate below is what it loads.
    const QString overridePath = qEnvironmentVariable("PIST_LIBRETRO_CORE");
    if (!overridePath.isEmpty() && QFileInfo::exists(overridePath))
        return overridePath;
    for (const QString &candidate : libretroCoreCandidates(applicationDir)) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

bool sessionUsesInProcessCore(bool onMacOS, const QString &hatariPath, const QString &applicationDir)
{
    return onMacOS && hatariPath.isEmpty() && !findLibretroCore(applicationDir).isEmpty();
}

LibretroBackend::LibretroBackend(QObject *parent)
    : IDebugBackend(parent)
    , m_library(new QLibrary(this))
{
    // Dumps are emitted from the owner thread. The panes live on the UI thread.
    qRegisterMetaType<QList<MemoryRow>>("QList<MemoryRow>");
}

LibretroBackend::~LibretroBackend()
{
    stop();
}

bool LibretroBackend::start(const SessionConfig &config, QString *error)
{
    Q_UNUSED(config);
    stop();

    const QString found = findLibretroCore(QCoreApplication::applicationDirPath());
    if (found.isEmpty()) {
        if (error) {
            *error = tr("The libretro Hatari core was not found. Looked in:\n%1")
                         .arg(libretroCoreCandidates(QCoreApplication::applicationDirPath())
                                  .join(QLatin1Char('\n')));
        }
        return false;
    }

    m_library->setFileName(found);
    if (!m_library->load()) {
        if (error)
            *error = tr("Could not load %1: %2").arg(found, m_library->errorString());
        return false;
    }

    auto abi = reinterpret_cast<int (*)()>(m_library->resolve("pist_hatari_abi"));
    if (!abi || abi() != PIST_HATARI_ABI) {
        const int got = abi ? abi() : 0;
        m_library->unload();
        if (error) {
            *error = tr("%1 speaks ABI %2; this PiST expects %3.")
                         .arg(found)
                         .arg(got)
                         .arg(PIST_HATARI_ABI);
        }
        return false;
    }

    auto startCore = reinterpret_cast<int (*)(const PistHatariSession *, char *, int)>(
        m_library->resolve("pist_hatari_start"));
    if (!startCore) {
        m_library->unload();
        if (error)
            *error = tr("%1 does not export pist_hatari_start.").arg(found);
        return false;
    }

    // The image is whichever ROM the session resolved: a TOS file the user
    // configured, or EmuTOS when that is what discovery selected. The core
    // does not carry a ROM of its own.
    if (config.tosPath.isEmpty()) {
        m_library->unload();
        if (error) {
            *error = tr("No TOS ROM was given. Choose a TOS image in the project settings, "
                        "or leave the choice empty and let PiST pick EmuTOS.");
        }
        return false;
    }

    // QByteArray storage has to outlive the call: the core reads the pointers
    // during start and must not be handed a temporary's buffer.
    const QByteArray tos = config.tosPath.toUtf8();
    const QByteArray gemdos = config.gemdosDir.toUtf8();
    const QByteArray program = config.programPath.toUtf8();
    const QString diskAPath = !config.bootFloppyPath.isEmpty() ? config.bootFloppyPath
                                                               : config.floppyImages.value(0);
    const QByteArray diskA = diskAPath.toUtf8();
    const QByteArray diskB = config.floppyImages.value(1).toUtf8();
    const QByteArray machine = config.machine.toUtf8();
    const auto cstr = [](const QString &text, const QByteArray &bytes) {
        return text.isEmpty() ? nullptr : bytes.constData();
    };

    PistHatariSession session{};
    session.abi = PIST_HATARI_ABI;
    session.tosPath = cstr(config.tosPath, tos);
    session.gemdosDir = cstr(config.gemdosDir, gemdos);
    session.programPath = cstr(config.programPath, program);
    session.diskA = cstr(diskAPath, diskA);
    session.diskB = config.floppyImages.value(1).isEmpty() ? nullptr : diskB.constData();
    session.machine = cstr(config.machine, machine);
    session.memSizeMiB = config.memSizeMiB;

    char coreError[512] = {};
    if (startCore(&session, coreError, static_cast<int>(sizeof coreError)) != 0) {
        const QString detail = QString::fromUtf8(coreError);
        m_library->unload();
        if (error) {
            *error = detail.isEmpty()
                         ? tr("%1 refused to start the session.").arg(found)
                         : tr("%1 refused to start the session: %2").arg(found, detail);
        }
        return false;
    }

    m_runFn = reinterpret_cast<RunFn>(m_library->resolve("pist_hatari_run"));
    m_haltFn = reinterpret_cast<HaltFn>(m_library->resolve("pist_hatari_stop"));
    m_stepFn = reinterpret_cast<StepFn>(m_library->resolve("pist_hatari_step"));
    m_stepOverFn = reinterpret_cast<StepFn>(m_library->resolve("pist_hatari_step_over"));
    m_resumeFn = reinterpret_cast<StepFn>(m_library->resolve("pist_hatari_resume"));
    m_pauseFn = reinterpret_cast<StepFn>(m_library->resolve("pist_hatari_pause"));
    m_clearFn = reinterpret_cast<StepFn>(m_library->resolve("pist_hatari_clear_breakpoints"));
    m_armFn = reinterpret_cast<ArmFn>(m_library->resolve("pist_hatari_arm_breakpoint"));
    m_regsFn = reinterpret_cast<RegsFn>(m_library->resolve("pist_hatari_registers"));
    m_baseFn = reinterpret_cast<BaseFn>(m_library->resolve("pist_hatari_basepage"));
    m_ramFn = reinterpret_cast<RamFn>(m_library->resolve("pist_hatari_ram"));
    if (!m_runFn || !m_haltFn || !m_stepFn || !m_stepOverFn || !m_resumeFn || !m_pauseFn
        || !m_clearFn || !m_armFn || !m_regsFn || !m_baseFn || !m_ramFn) {
        if (m_haltFn)
            m_haltFn();
        m_runFn = nullptr;
        m_haltFn = nullptr;
        m_stepFn = nullptr;
        m_stepOverFn = nullptr;
        m_resumeFn = nullptr;
        m_pauseFn = nullptr;
        m_clearFn = nullptr;
        m_armFn = nullptr;
        m_regsFn = nullptr;
        m_baseFn = nullptr;
        m_ramFn = nullptr;
        m_library->unload();
        if (error)
            *error = tr("%1 does not export the debugger calls this PiST needs.").arg(found);
        return false;
    }

    // The core is up and the entry breakpoint is armed. The owner thread is
    // what advances it; this thread only draws what that one copies out.
    m_quit = false;
    m_hold = false;
    m_running = true;
    m_stopped = false;
    m_thread = QThread::create([this] { pump(); });
    m_thread->start();
    emit runningChanged(true);
    return true;
}

void LibretroBackend::pump()
{
    while (!m_quit.load()) {
        CoreRequest request;
        bool have = false;
        bool resumeNow = false;
        {
            QMutexLocker lock(&m_gate);
            // Held means the debugger has the CPU. Wait for a command, or for
            // Continue, which is only taken once the queue is empty so an arm
            // posted from the stop's snapshot still lands first.
            while (!m_quit.load() && m_jobs.isEmpty() && m_hold.load() && !m_continueWhenIdle)
                m_wake.wait(&m_gate);
            if (m_quit.load())
                break;
            if (!m_jobs.isEmpty()) {
                request = m_jobs.dequeue();
                have = true;
            } else if (m_hold.load() && m_continueWhenIdle) {
                m_continueWhenIdle = false;
                m_hold = false;
                resumeNow = true;
            }
        }
        if (have) {
            dispatch(request);
            continue;
        }
        if (resumeNow) {
            if (!m_resumeFn || m_resumeFn() != 0) {
                emit errorOccurred(tr("The libretro core failed to resume."));
                QMutexLocker lock(&m_gate);
                m_hold = true;
                continue;
            }
            m_stopped = false;
            emit stoppedChanged(false);
            continue;
        }
        if (m_quit.load() || !m_runFn)
            break;

        const int epoch = m_epoch.load();
        PistHatariFrame frame{};
        int stopped = 0;
        if (m_runFn(&frame, &stopped) != 0) {
            emit errorOccurred(tr("The libretro core failed while running a frame."));
            break;
        }
        if (m_quit.load() || epoch != m_epoch.load())
            break;
        if (frame.pixels && frame.width > 0 && frame.height > 0 && frame.pitch > 0) {
            // The ABI pointer dies at the next run. Copy before leaving this thread.
            const int bytes = frame.pitch * frame.height;
            QByteArray pixels(bytes, Qt::Uninitialized);
            memcpy(pixels.data(), frame.pixels, static_cast<size_t>(bytes));
            emit frameReady(pixels, frame.width, frame.height, frame.pitch, epoch);
        }
        if (stopped) {
            m_stopped = true;
            {
                QMutexLocker lock(&m_gate);
                if (!m_quit.load())
                    m_hold = true;
            }
            if (!m_quit.load())
                emit stoppedChanged(true);
        }
    }
    if (m_haltFn)
        m_haltFn();
    m_haltFn = nullptr;
    m_runFn = nullptr;
    m_stepFn = nullptr;
    m_stepOverFn = nullptr;
    m_resumeFn = nullptr;
    m_pauseFn = nullptr;
    m_clearFn = nullptr;
    m_armFn = nullptr;
    m_regsFn = nullptr;
    m_baseFn = nullptr;
    m_ramFn = nullptr;
}

void LibretroBackend::post(const CoreRequest &request)
{
    if (!m_running) {
        emit errorOccurred(tr("No emulator session is running."));
        return;
    }
    QMutexLocker lock(&m_gate);
    m_jobs.enqueue(request);
    m_wake.wakeAll();
}

void LibretroBackend::dispatch(const CoreRequest &request)
{
    if (m_quit.load())
        return;
    switch (request.job) {
    case CoreJob::Arm:
        if (!m_armFn || m_armFn(request.text.toUtf8().constData()) != 0)
            emit errorOccurred(tr("Could not arm breakpoint: %1").arg(request.text));
        break;
    case CoreJob::Clear:
        if (m_clearFn)
            m_clearFn();
        break;
    case CoreJob::BreakOnce: {
        const QString condition = QStringLiteral("b pc = $%1 :once").arg(request.address, 0, 16);
        if (!m_armFn || m_armFn(condition.toUtf8().constData()) != 0)
            emit errorOccurred(tr("Could not arm breakpoint: %1").arg(condition));
        break;
    }
    case CoreJob::Registers:
    case CoreJob::Basepage:
    case CoreJob::Refresh:
        if (readCoreState())
            publishState();
        else
            emit errorOccurred(tr("The libretro core did not return a register snapshot."));
        break;
    case CoreJob::Step:
    case CoreJob::StepOver: {
        const StepFn fn = request.job == CoreJob::Step ? m_stepFn : m_stepOverFn;
        if (!fn || fn() != 0) {
            emit errorOccurred(tr("The libretro core failed to step."));
            break;
        }
        m_stopped = true;
        publishFrame();
        if (!m_quit.load())
            emit stoppedChanged(true);
        break;
    }
    case CoreJob::Pause:
        if (m_stopped)
            break;
        if (!m_pauseFn || m_pauseFn() != 0)
            emit errorOccurred(tr("The libretro core failed to pause."));
        break;
    case CoreJob::Memory:
        emit memoryDumpReady(request.address, rowsFromRam(request.address, request.length), request.tag);
        break;
    case CoreJob::Stack:
        emit stackDumpReady(request.address, rowsFromRam(request.address, request.length));
        break;
    }
}

bool LibretroBackend::readCoreState()
{
    if (!m_regsFn || !m_baseFn)
        return false;
    const char *names[32] = {};
    uint32_t values[32] = {};
    int needed = 0;
    const int count = m_regsFn(names, values, 32, &needed);
    if (count < 0)
        return false;

    Registers regs;
    quint32 pc = 0;
    for (int i = 0; i < count; ++i) {
        if (!names[i])
            continue;
        const QLatin1String name(names[i]);
        const quint32 value = values[i];
        if (name.size() == 2 && names[i][1] >= '0' && names[i][1] <= '7') {
            const int index = names[i][1] - '0';
            if (name.at(0) == QLatin1Char('D'))
                regs.d[index] = value;
            else if (name.at(0) == QLatin1Char('A'))
                regs.a[index] = value;
        } else if (name == QLatin1String("PC"))
            pc = value;
        else if (name == QLatin1String("USP"))
            regs.usp = value;
        else if (name == QLatin1String("ISP"))
            regs.isp = value;
        else if (name == QLatin1String("SR")) {
            regs.sr = static_cast<quint16>(value);
            regs.flagC = value & 0x1;
            regs.flagV = value & 0x2;
            regs.flagZ = value & 0x4;
            regs.flagN = value & 0x8;
            regs.flagX = value & 0x10;
        }
    }
    regs.valid = true;
    m_state.regs = regs;
    m_state.pc = pc;

    uint32_t text = 0;
    uint32_t data = 0;
    uint32_t bss = 0;
    if (m_baseFn(&text, &data, &bss) == 0) {
        m_state.textBase = text;
        m_state.dataBase = data;
        m_state.bssBase = bss;
    }
    return true;
}

void LibretroBackend::publishState()
{
    const MachineState state = m_state;
    // The slot arms breakpoints. Running it before this returns is what keeps
    // those arms ahead of Continue. The owner does not hold m_gate here.
    QMetaObject::invokeMethod(this, [this, state] {
        emit stateUpdated(state);
    }, Qt::BlockingQueuedConnection);
}

void LibretroBackend::publishFrame()
{
    if (!m_runFn || m_quit.load())
        return;
    const int epoch = m_epoch.load();
    PistHatariFrame frame{};
    int stopped = 0;
    // While the core is stopped this copies the frame and does not resume.
    if (m_runFn(&frame, &stopped) != 0)
        return;
    if (m_quit.load() || epoch != m_epoch.load())
        return;
    if (!frame.pixels || frame.width <= 0 || frame.height <= 0 || frame.pitch <= 0)
        return;
    const int bytes = frame.pitch * frame.height;
    QByteArray pixels(bytes, Qt::Uninitialized);
    memcpy(pixels.data(), frame.pixels, static_cast<size_t>(bytes));
    emit frameReady(pixels, frame.width, frame.height, frame.pitch, epoch);
}

QList<MemoryRow> LibretroBackend::rowsFromRam(quint32 address, int length) const
{
    QList<MemoryRow> rows;
    if (!m_ramFn || length <= 0)
        return rows;
    size_t ramSize = 0;
    const auto *ram = static_cast<const uint8_t *>(m_ramFn(&ramSize));
    if (!ram || ramSize == 0)
        return rows;
    int offset = 0;
    while (offset < length) {
        const quint64 at = quint64(address) + quint64(offset);
        if (at >= ramSize)
            break;
        const int want = qMin(16, length - offset);
        const int have = int(qMin(quint64(want), quint64(ramSize) - at));
        MemoryRow row;
        row.address = quint32(at);
        row.bytes.resize(have);
        memcpy(row.bytes.data(), ram + at, size_t(have));
        rows.append(row);
        offset += have;
        if (have < want)
            break;
    }
    return rows;
}

void LibretroBackend::stop()
{
    // Frames already queued carry the old epoch and the panel drops them.
    m_epoch.fetch_add(1);
    {
        QMutexLocker lock(&m_gate);
        m_quit = true;
        m_hold = false;
        m_continueWhenIdle = false;
        m_jobs.clear();
        m_wake.wakeAll();
    }
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    } else if (m_haltFn) {
        m_haltFn();
        m_haltFn = nullptr;
        m_runFn = nullptr;
        m_stepFn = nullptr;
        m_stepOverFn = nullptr;
        m_resumeFn = nullptr;
        m_pauseFn = nullptr;
        m_clearFn = nullptr;
        m_armFn = nullptr;
        m_regsFn = nullptr;
        m_baseFn = nullptr;
        m_ramFn = nullptr;
    }
    if (m_library->isLoaded())
        m_library->unload();
    const bool wasLive = m_running.load() || m_stopped.load();
    m_running = false;
    m_stopped = false;
    m_state = {};
    if (wasLive) {
        emit runningChanged(false);
        emit stoppedChanged(false);
    }
}

bool LibretroBackend::isRunning() const
{
    return m_running;
}

bool LibretroBackend::isStopped() const
{
    return m_stopped;
}

void LibretroBackend::notInThisSlice(const QString &what)
{
    emit errorOccurred(tr("%1 is not implemented in the libretro backend yet.").arg(what));
}

void LibretroBackend::step()
{
    CoreRequest request;
    request.job = CoreJob::Step;
    {
        QMutexLocker lock(&m_gate);
        m_continueWhenIdle = false;
    }
    post(request);
}

void LibretroBackend::stepOver()
{
    CoreRequest request;
    request.job = CoreJob::StepOver;
    {
        QMutexLocker lock(&m_gate);
        m_continueWhenIdle = false;
    }
    post(request);
}

void LibretroBackend::resume()
{
    if (!m_stopped)
        return;
    QMutexLocker lock(&m_gate);
    QQueue<CoreRequest> kept;
    while (!m_jobs.isEmpty()) {
        const CoreRequest request = m_jobs.dequeue();
        if (request.keepOnResume)
            kept.enqueue(request);
    }
    m_jobs = kept;
    m_continueWhenIdle = true;
    m_wake.wakeAll();
}

void LibretroBackend::pause()
{
    CoreRequest request;
    request.job = CoreJob::Pause;
    post(request);
}

void LibretroBackend::refresh()
{
    CoreRequest request;
    request.job = CoreJob::Refresh;
    request.keepOnResume = true;
    post(request);
}

void LibretroBackend::command(const QString &commandText)
{
    Q_UNUSED(commandText);
    notInThisSlice(QStringLiteral("console command"));
}

void LibretroBackend::clearBreakpoints()
{
    CoreRequest request;
    request.job = CoreJob::Clear;
    request.keepOnResume = true;
    post(request);
}

void LibretroBackend::armBreakpoint(const QString &condition)
{
    CoreRequest request;
    request.job = CoreJob::Arm;
    request.keepOnResume = true;
    request.text = condition;
    post(request);
}

void LibretroBackend::requestMemoryDump(quint32 address, int length, int tag)
{
    CoreRequest request;
    request.job = CoreJob::Memory;
    request.address = address;
    request.length = length;
    request.tag = tag;
    post(request);
}

void LibretroBackend::requestStackDump(quint32 address, int length)
{
    CoreRequest request;
    request.job = CoreJob::Stack;
    request.address = address;
    request.length = length;
    post(request);
}

void LibretroBackend::dumpRegisters()
{
    CoreRequest request;
    request.job = CoreJob::Registers;
    request.keepOnResume = true;
    post(request);
}

void LibretroBackend::loadSymbols()
{
    // The core loads the program's symbols when the debugger is entered
    // (DebugUI calls Symbols_LoadCurrentProgram before it returns the stop).
    // There is no second load to do from here.
}

void LibretroBackend::infoSubject(const QString &subject)
{
    // Hardware registers are not in this slice. The stop path asks anyway;
    // an error dialog on every stop would bury the register snapshot.
    Q_UNUSED(subject);
}

void LibretroBackend::readBasepage()
{
    CoreRequest request;
    request.job = CoreJob::Basepage;
    request.keepOnResume = true;
    post(request);
}
void LibretroBackend::setDisasmEngine(DisasmEngine engine)
{
    Q_UNUSED(engine);
    notInThisSlice(QStringLiteral("disassembler choice"));
}
void LibretroBackend::profileOn() { notInThisSlice(QStringLiteral("profile on")); }
void LibretroBackend::profileOff() { notInThisSlice(QStringLiteral("profile off")); }
void LibretroBackend::profileSave(const QString &path)
{
    Q_UNUSED(path);
    notInThisSlice(QStringLiteral("profile save"));
}
void LibretroBackend::breakAtAddressOnce(quint32 address)
{
    CoreRequest request;
    request.job = CoreJob::BreakOnce;
    request.keepOnResume = true;
    request.address = address;
    post(request);
}
void LibretroBackend::writeMemoryByte(quint32 address, quint8 value)
{
    Q_UNUSED(address);
    Q_UNUSED(value);
    notInThisSlice(QStringLiteral("write memory"));
}
void LibretroBackend::writeRegister(const QString &name, quint32 value)
{
    Q_UNUSED(name);
    Q_UNUSED(value);
    notInThisSlice(QStringLiteral("write register"));
}
void LibretroBackend::readDisassembly()
{
    // Disassembly text is not in this slice. The entry attach asks for it
    // after the registers; an error there would land on every stop.
}
void LibretroBackend::readDisassemblyAt(quint32 address)
{
    Q_UNUSED(address);
}
void LibretroBackend::readHistory(int count)
{
    // The history pane asks on every stop. Same as disassembly: quiet until
    // the core can answer it.
    Q_UNUSED(count);
}

} // namespace pist
