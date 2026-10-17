// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "hooks/StopHookRunner.h"

#include "emu/DebugBackend.h"
#include "emu/HexFormat.h"
#include "hooks/StopHookEngine.h"

#include <QEventLoop>
#include <QMetaObject>
#include <QScopeGuard>
#include <QTimer>

namespace pist {
namespace {

/// The default backstop for one round-trip. The transport times its own commands
/// out at 10 s and completes every request either way (`EmulatorHost.cpp:27`), so
/// this only catches an answer that never comes at all; the Host may override it
/// (`operationTimeoutMs`) so the give-up path is reachable in a test.
constexpr int kOperationTimeoutMs = 12000;

/// The tag a hook's memory dump carries. Panes use their own non-negative tags
/// and the remote `readmem` verb uses -1 (`MainWindow.h:94`): a dump must reach
/// the caller that asked for it, so a hook's is its own.
constexpr int kHookDumpTag = -2;

/// The register names a script may read, for the refusal to be specific.
const char *const kReadableRegisters = "d0-d7, a0-a7, pc, sr, usp, isp";
/// …and the ones it may write: Hatari cannot set usp/isp by name, which is why
/// the registers view leaves those two cells read-only (`ui/RegistersView.cpp`).
const char *const kWritableRegisters = "d0-d7, a0-a7, pc, sr";

/// `d3` → 3; -1 when the name is not a d0-d7/a0-a7 shape.
int indexedRegister(const QString &name)
{
    if (name.size() != 2)
        return -1;
    const QChar kind = name.at(0);
    if (kind != QLatin1Char('d') && kind != QLatin1Char('a'))
        return -1;
    const QChar digit = name.at(1);
    if (digit < QLatin1Char('0') || digit > QLatin1Char('7'))
        return -1;
    return digit.digitValue();
}

} // namespace

/// The script's view of the debugger. A nested class of the runner, so it reaches
/// the private operations directly without widening the seam's surface.
class StopHookRunner::ScriptApi : public hooks::HookApi
{
public:
    explicit ScriptApi(StopHookRunner *runner) : m_runner(runner) {}

    bool readMemory(quint32 address, int length, QByteArray *bytes, QString *error) override
    {
        return m_runner->readMemory(address, length, bytes, error);
    }
    bool writeMemory(quint32 address, quint8 value, QString *error) override
    {
        return m_runner->writeMemory(address, value, error);
    }
    bool readRegister(const QString &name, quint32 *value, QString *error) override
    {
        return m_runner->readRegister(name, value, error);
    }
    bool writeRegister(const QString &name, quint32 value, QString *error) override
    {
        return m_runner->writeRegister(name, value, error);
    }
    void log(const QString &message) override { m_runner->scriptLog(message); }
    void requestContinue() override { m_runner->requestContinue(); }

private:
    StopHookRunner *m_runner;
};

StopHookRunner::StopHookRunner(Host host, QObject *parent)
    : QObject(parent)
    , m_host(std::move(host))
{
}

void StopHookRunner::onStopPublished(const MachineState &state)
{
    if (m_host.enabled && !m_host.enabled())
        return;
    const QString path = m_host.scriptPath ? m_host.scriptPath() : QString();
    if (path.isEmpty())
        return;

    if (m_running) {
        // Only reachable if the user continued from inside the nested wait of a
        // run. The running script owns the state copy below; refuse rather than
        // let a second stop overwrite what it is reading.
        scriptLog(tr("skipped: the previous hook is still running"));
        return;
    }

    // Just what the API reads — the disassembly is none of a hook's business.
    m_regs = state.regs;
    m_pc = state.pc;
    m_stateDirty = false;

    if (m_queued)
        return;
    m_queued = true;
    // Outside the transport's emit stack: the drain is single-entry, so a nested
    // read-back loop started inside it would wait forever for an answer that
    // cannot be parsed until the emitting frame unwinds (docs/stop-hooks-plan.md
    // D2). The machine is held stopped, so there is no prompt to lose.
    QMetaObject::invokeMethod(this, [this] { runQueued(); }, Qt::QueuedConnection);
}

void StopHookRunner::runQueued()
{
    m_queued = false;
    if (m_running)
        return; // the refusal was already logged where the stop arrived
    doRun();
}

void StopHookRunner::doRun()
{
    IDebugBackend *backend = m_host.backend ? m_host.backend() : nullptr;
    if (!backend || !backend->isRunning()) {
        // The session ended between the stop and this turn.
        scriptLog(tr("skipped: no emulator session is running"));
        return;
    }
    if (!backend->isStopped()) {
        // A manual Continue won the race with this queued run.
        scriptLog(tr("skipped: the machine is running again"));
        return;
    }

    m_running = true;
    m_writes = 0;
    m_loggedLines = 0;
    m_continueRequested = false;

    // Hold the console for the run, so the completion of one of our commands can
    // never be confused with one the user typed — and therefore a resume can
    // never prune a write the script asked for (docs/stop-hooks-plan.md R5).
    const QScopeGuard release([this] {
        if (m_host.holdConsole)
            m_host.holdConsole(false);
    });
    if (m_host.holdConsole)
        m_host.holdConsole(true);

    const QString path = m_host.scriptPath ? m_host.scriptPath() : QString();
    const int budget = m_host.budgetMs ? m_host.budgetMs() : hooks::StopHookEngine::kDefaultBudgetMs;

    ScriptApi api(this);
    const hooks::StopHookResult result = hooks::StopHookEngine::run(path, api, budget);

    // One refresh for the run, not one per byte: the panes show the machine as
    // the hook left it.
    if (m_writes > 0 && m_host.refresh)
        m_host.refresh();

    if (!result.ran) {
        m_lastSummary = result.error;
        scriptLog(result.error);
    } else if (!result.ok) {
        m_lastSummary = result.line >= 0 ? tr("line %1: %2").arg(result.line).arg(result.error)
                                        : result.error;
        if (result.timedOut)
            scriptLog(tr("%1: stopped after %2 ms — the script did not finish")
                          .arg(result.file)
                          .arg(budget));
        else
            scriptLog(tr("%1:%2: %3")
                          .arg(result.file)
                          .arg(result.line)
                          .arg(result.error));
    } else {
        m_lastSummary = tr("ran at pc $%1 — %2 line(s), %3 write(s)")
                            .arg(hex::hex32(m_pc, hex::Case::Lower))
                            .arg(m_loggedLines)
                            .arg(m_writes);
    }

    m_running = false;

    if (m_continueRequested) {
        if (m_host.autoContinue && m_host.autoContinue()) {
            scriptLog(tr("the script asked to continue"));
            if (m_host.resume)
                m_host.resume();
        } else {
            scriptLog(tr("the script asked to continue; hooks.autoContinue is off, "
                         "so the machine stays stopped"));
        }
    }
}

bool StopHookRunner::readMemory(quint32 address, int length, QByteArray *bytes, QString *error)
{
    if (length < 1 || length > hooks::HookApi::kMaxReadBytes) {
        *error = tr("readmem: length %1 is outside 1..%2")
                     .arg(length)
                     .arg(hooks::HookApi::kMaxReadBytes);
        return false;
    }
    if (quint64(address) + quint64(length) > quint64(1) << 32) {
        *error = tr("readmem: $%1 + %2 bytes leaves the address space")
                     .arg(hex::hex32(address, hex::Case::Lower))
                     .arg(length);
        return false;
    }
    IDebugBackend *backend = m_host.backend ? m_host.backend() : nullptr;
    if (!backend || !backend->isRunning()) {
        *error = tr("readmem: no emulator session is running");
        return false;
    }
    if (!backend->isStopped()) {
        *error = tr("readmem: the machine is running, not stopped");
        return false;
    }

    QEventLoop loop;
    QList<MemoryRow> rows;
    bool answered = false;
    const auto dumped = connect(backend, &IDebugBackend::memoryDumpReady, this,
                                [&](quint32, const QList<MemoryRow> &dump, int tag) {
                                    if (tag != kHookDumpTag)
                                        return;
                                    rows = dump;
                                    answered = true;
                                    loop.quit();
                                });
    // A read can fail as well as answer: a timed-out command completes with no
    // dump at all, and waiting for a signal that will never come would hang the
    // hook — and the IDE with it.
    const auto finished = connect(backend, &IDebugBackend::commandFinished, this,
                                  [&](const QString &, const QString &) { loop.quit(); });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    backend->requestMemoryDump(address, length, kHookDumpTag);
    timeout.start(operationTimeout());
    loop.exec();
    disconnect(dumped);
    disconnect(finished);

    if (!answered) {
        *error = tr("readmem: the debugger did not answer with a dump");
        return false;
    }

    // Rows carry the addresses they actually cover, so the asked-for range is
    // assembled from them rather than assumed to start at row zero.
    QByteArray out(length, '\0');
    for (const MemoryRow &row : rows) {
        for (int i = 0; i < row.bytes.size(); ++i) {
            const quint64 at = quint64(row.address) + quint32(i);
            if (at < address || at >= quint64(address) + quint32(length))
                continue;
            out[int(at - quint64(address))] = char(row.bytes.at(i));
        }
    }
    *bytes = out;
    return true;
}

bool StopHookRunner::writeMemory(quint32 address, quint8 value, QString *error)
{
    IDebugBackend *backend = m_host.backend ? m_host.backend() : nullptr;
    if (!backend || !backend->isRunning()) {
        *error = tr("writemem: no emulator session is running");
        return false;
    }
    if (!backend->isStopped()) {
        *error = tr("writemem: the machine is running, not stopped");
        return false;
    }
    if (!issueAndWait([&] { backend->writeMemoryByte(address, value); }, QStringLiteral("writemem"), error))
        return false;
    m_stateDirty = true;
    ++m_writes;
    return true;
}

bool StopHookRunner::readRegister(const QString &name, quint32 *value, QString *error)
{
    const QString wanted = name.trimmed().toLower();
    if (!registerNameValid(wanted, error))
        return false;

    // The copy the stop published answers for free — unless a scripted write has
    // happened since, in which case it is no longer what the machine says.
    if (m_stateDirty || !m_regs.valid) {
        MachineState fresh;
        if (!fetchRegisters(&fresh, error))
            return false;
        m_regs = fresh.regs;
        m_pc = fresh.pc;
        m_stateDirty = false;
    }

    *value = registerValue(wanted);
    return true;
}

bool StopHookRunner::writeRegister(const QString &name, quint32 value, QString *error)
{
    const QString wanted = name.trimmed().toLower();
    if (!writableRegisterName(wanted, error))
        return false;
    if (wanted == QLatin1String("sr") && value > 0xFFFF) {
        *error = tr("writereg: sr is 16 bits, got $%1").arg(hex::hex32(value, hex::Case::Lower));
        return false;
    }

    IDebugBackend *backend = m_host.backend ? m_host.backend() : nullptr;
    if (!backend || !backend->isRunning()) {
        *error = tr("writereg: no emulator session is running");
        return false;
    }
    if (!backend->isStopped()) {
        *error = tr("writereg: the machine is running, not stopped");
        return false;
    }
    if (!issueAndWait([&] { backend->writeRegister(wanted, value); }, QStringLiteral("writereg"), error))
        return false;
    // Not patched into the copy: marking it dirty makes the next read a real
    // round-trip, which is the only answer that cannot be wrong.
    m_stateDirty = true;
    ++m_writes;
    return true;
}

void StopHookRunner::scriptLog(const QString &message)
{
    ++m_loggedLines;
    if (m_host.log)
        m_host.log(QStringLiteral("[hook] ") + message);
}

void StopHookRunner::requestContinue()
{
    m_continueRequested = true;
}

bool StopHookRunner::fetchRegisters(MachineState *out, QString *error)
{
    IDebugBackend *backend = m_host.backend ? m_host.backend() : nullptr;
    if (!backend || !backend->isRunning()) {
        *error = tr("readreg: no emulator session is running");
        return false;
    }
    if (!backend->isStopped()) {
        *error = tr("readreg: the machine is running, not stopped");
        return false;
    }

    QEventLoop loop;
    bool got = false;
    const auto updated = connect(backend, &IDebugBackend::stateUpdated, this,
                                 [&](const MachineState &state) {
                                     *out = state;
                                     got = true;
                                     loop.quit();
                                 });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    backend->dumpRegisters();
    timeout.start(operationTimeout());
    loop.exec();
    disconnect(updated);

    if (!got || !out->regs.valid) {
        *error = tr("readreg: the debugger did not report the registers");
        return false;
    }
    return true;
}

bool StopHookRunner::issueAndWait(const std::function<void()> &issue, const QString &what,
                                  QString *error)
{
    IDebugBackend *backend = m_host.backend ? m_host.backend() : nullptr;
    if (!backend)
        return false;

    QEventLoop loop;
    bool done = false;
    // Connected before the command is issued: the completion cannot be missed,
    // and the console is held for the run, so the next completion is this one's.
    const auto finished = connect(backend, &IDebugBackend::commandFinished, this,
                                  [&](const QString &, const QString &) {
                                      done = true;
                                      loop.quit();
                                  });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

    issue();
    timeout.start(operationTimeout());
    loop.exec();
    disconnect(finished);

    if (!done) {
        *error = tr("%1: the debugger did not acknowledge the command").arg(what);
        return false;
    }
    return true;
}

int StopHookRunner::operationTimeout() const
{
    if (m_host.operationTimeoutMs) {
        const int injected = m_host.operationTimeoutMs();
        if (injected > 0)
            return injected;
    }
    return kOperationTimeoutMs;
}

bool StopHookRunner::registerNameValid(const QString &name, QString *error) const
{
    if (indexedRegister(name) >= 0 || name == QLatin1String("pc") || name == QLatin1String("sr")
        || name == QLatin1String("usp") || name == QLatin1String("isp"))
        return true;
    *error = tr("readreg: '%1' is not a register (%2)").arg(name, QLatin1String(kReadableRegisters));
    return false;
}

bool StopHookRunner::writableRegisterName(const QString &name, QString *error) const
{
    if (indexedRegister(name) >= 0 || name == QLatin1String("pc") || name == QLatin1String("sr"))
        return true;
    if (name == QLatin1String("usp") || name == QLatin1String("isp")) {
        *error = tr("writereg: Hatari cannot set %1 by name").arg(name);
        return false;
    }
    *error = tr("writereg: '%1' is not a register (%2)").arg(name, QLatin1String(kWritableRegisters));
    return false;
}

quint32 StopHookRunner::registerValue(const QString &name) const
{
    const int i = indexedRegister(name);
    if (i >= 0)
        return name.at(0) == QLatin1Char('d') ? m_regs.d[i] : m_regs.a[i];
    if (name == QLatin1String("pc"))
        return m_pc;
    if (name == QLatin1String("sr"))
        return m_regs.sr;
    if (name == QLatin1String("usp"))
        return m_regs.usp;
    return m_regs.isp;
}

} // namespace pist
