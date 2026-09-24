// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "emu/LibretroBackend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>

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
    for (const QString &candidate : libretroCoreCandidates(applicationDir)) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

LibretroBackend::LibretroBackend(QObject *parent)
    : IDebugBackend(parent)
    , m_library(new QLibrary(this))
{
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
    if (!m_runFn || !m_haltFn) {
        if (m_haltFn)
            m_haltFn();
        m_runFn = nullptr;
        m_haltFn = nullptr;
        m_library->unload();
        if (error)
            *error = tr("%1 does not export pist_hatari_run.").arg(found);
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
        {
            QMutexLocker lock(&m_gate);
            while (m_hold.load() && !m_quit.load())
                m_wake.wait(&m_gate);
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
            emit stoppedChanged(true);
            m_hold = true;
        }
    }
    if (m_haltFn)
        m_haltFn();
    m_haltFn = nullptr;
    m_runFn = nullptr;
}

void LibretroBackend::stop()
{
    // Frames already queued carry the old epoch and the panel drops them.
    m_epoch.fetch_add(1);
    {
        QMutexLocker lock(&m_gate);
        m_quit = true;
        m_hold = false;
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
    }
    if (m_library->isLoaded())
        m_library->unload();
    const bool wasLive = m_running.load() || m_stopped.load();
    m_running = false;
    m_stopped = false;
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

void LibretroBackend::step() { notInThisSlice(QStringLiteral("step")); }
void LibretroBackend::stepOver() { notInThisSlice(QStringLiteral("step over")); }
void LibretroBackend::resume() { notInThisSlice(QStringLiteral("resume")); }
void LibretroBackend::pause() { notInThisSlice(QStringLiteral("pause")); }
void LibretroBackend::refresh() { notInThisSlice(QStringLiteral("refresh")); }
void LibretroBackend::command(const QString &commandText)
{
    Q_UNUSED(commandText);
    notInThisSlice(QStringLiteral("console command"));
}
void LibretroBackend::clearBreakpoints() { notInThisSlice(QStringLiteral("clear breakpoints")); }
void LibretroBackend::armBreakpoint(const QString &condition)
{
    Q_UNUSED(condition);
    notInThisSlice(QStringLiteral("arm breakpoint"));
}
void LibretroBackend::requestMemoryDump(quint32 address, int length, int tag)
{
    Q_UNUSED(address);
    Q_UNUSED(length);
    Q_UNUSED(tag);
    notInThisSlice(QStringLiteral("memory dump"));
}
void LibretroBackend::requestStackDump(quint32 address, int length)
{
    Q_UNUSED(address);
    Q_UNUSED(length);
    notInThisSlice(QStringLiteral("stack dump"));
}
void LibretroBackend::dumpRegisters() { notInThisSlice(QStringLiteral("registers")); }
void LibretroBackend::loadSymbols() { notInThisSlice(QStringLiteral("symbols")); }
void LibretroBackend::infoSubject(const QString &subject)
{
    Q_UNUSED(subject);
    notInThisSlice(QStringLiteral("hardware info"));
}
void LibretroBackend::readBasepage() { notInThisSlice(QStringLiteral("basepage")); }
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
    Q_UNUSED(address);
    notInThisSlice(QStringLiteral("break once"));
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
void LibretroBackend::readDisassembly() { notInThisSlice(QStringLiteral("disassembly")); }
void LibretroBackend::readDisassemblyAt(quint32 address)
{
    Q_UNUSED(address);
    notInThisSlice(QStringLiteral("disassembly"));
}
void LibretroBackend::readHistory(int count)
{
    Q_UNUSED(count);
    notInThisSlice(QStringLiteral("history"));
}

} // namespace pist
