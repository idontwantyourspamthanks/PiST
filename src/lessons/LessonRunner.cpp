// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "lessons/LessonRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace pist {
namespace lessons {

namespace {

/// The key a fallback answer uses when the scheme does not bind the action at
/// all: the action's name, so the prose still reads ("press Step").
QString fallbackKeyName(const QString &actionId)
{
    static const QHash<QString, QString> names = {
        { QStringLiteral("build"), QStringLiteral("Build") },
        { QStringLiteral("run"), QStringLiteral("Run") },
        { QStringLiteral("step"), QStringLiteral("Step") },
        { QStringLiteral("step-over"), QStringLiteral("Step over") },
        { QStringLiteral("continue"), QStringLiteral("Continue") },
        { QStringLiteral("toggle-breakpoint"), QStringLiteral("Toggle breakpoint") },
        { QStringLiteral("clear-breakpoints"), QStringLiteral("Clear breakpoints") },
        { QStringLiteral("step-out"), QStringLiteral("Step out") },
        { QStringLiteral("run-to-cursor"), QStringLiteral("Run to cursor") },
    };
    return names.value(actionId, actionId);
}

quint32 hexOrDecimal(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.startsWith(QLatin1Char('$')))
        return trimmed.mid(1).toUInt(nullptr, 16);
    if (trimmed.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        return trimmed.mid(2).toUInt(nullptr, 16);
    bool ok = false;
    const quint32 decimal = trimmed.toUInt(&ok, 10);
    if (ok)
        return decimal;
    return trimmed.toUInt(nullptr, 16);
}

} // namespace

LessonRunner::LessonRunner(control::ControlHost *host, QObject *parent)
    : QObject(parent), m_host(host)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(200);
    connect(m_pollTimer, &QTimer::timeout, this, [this]() {
        m_pollElapsed += m_pollInterval;
        if (m_pollTest && m_pollTest()) {
            m_pollTimer->stop();
            m_pollTest = nullptr;
            m_pollTimeout = nullptr;
            auto onDone = m_pollDone;
            m_pollDone = nullptr;
            if (onDone)
                onDone();
            return;
        }
        if (m_pollElapsed >= m_pollLimit) {
            m_pollTimer->stop();
            auto onTimeout = m_pollTimeout;
            m_pollTest = nullptr;
            m_pollTimeout = nullptr;
            m_pollDone = nullptr;
            if (onTimeout)
                onTimeout();
        }
    });
}

void LessonRunner::setKeyResolver(KeyResolver resolver)
{
    m_keys = std::move(resolver);
    emit changed();
}

void LessonRunner::setLesson(const Lesson &lesson)
{
    m_lesson = lesson;
    m_index = 0;
    m_sourcePath.clear();
    m_verdict.clear();
    m_verdictKnown = false;
    m_verdictPassed = false;
    m_busy = false;
    emit changed();
}

const Step *LessonRunner::currentStep() const
{
    if (m_index < 0 || m_index >= m_lesson.steps.size())
        return nullptr;
    return &m_lesson.steps.at(m_index);
}

QString LessonRunner::resolveTokens(const QString &text) const
{
    if (!text.contains(QLatin1String("{key:")))
        return text;
    static const QRegularExpression re(QStringLiteral("\\{key:([a-z-]+)\\}"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += text.mid(last, match.capturedStart() - last);
        const QString id = match.captured(1);
        const QString key = m_keys ? m_keys(id) : QString();
        out += key.isEmpty() ? fallbackKeyName(id) : key;
        last = match.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

QString LessonRunner::teachText(int index) const
{
    if (index < 0 || index >= m_lesson.steps.size())
        return {};
    return resolveTokens(m_lesson.steps.at(index).teach);
}

void LessonRunner::goToStep(int index)
{
    if (index < 0 || index >= m_lesson.steps.size() || index == m_index)
        return;
    m_index = index;
    m_verdict.clear();
    m_verdictKnown = false;
    m_verdictPassed = false;
    emit changed();
}

bool LessonRunner::start()
{
    if (!m_host) {
        fail(tr("No IDE to run the lesson against."));
        return false;
    }
    if (!m_lesson.usable()) {
        fail(m_lesson.error.isEmpty() ? tr("This lesson could not be loaded.") : m_lesson.error);
        return false;
    }

    // Where a lesson's program is assembled. A template lesson writes its
    // starter file outside the source tree — in the user's own data directory —
    // and a lesson that ships a source runs a *copy* of it there, because the
    // build writes a .prg and a listing beside whatever it assembles: a lesson
    // must not write into the source tree, and an installed bundle's lessons
    // directory may not be writable at all.
    const auto scratchDir = [this]() -> QString {
        QString base = qEnvironmentVariable("PIST_LESSON_DIR");
        if (base.isEmpty()) {
            base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
                   + QStringLiteral("/PiST/lessons");
        }
        base += QLatin1Char('/') + m_lesson.id;
        QDir().mkpath(base);
        return base;
    };
    const auto copyBeside = [this, &scratchDir](const QString &original, QString *target) -> bool {
        const QDir from(QFileInfo(original).absolutePath());
        const QString base = scratchDir();
        if (base.isEmpty())
            return false;
        for (const QString &name : from.entryList(QDir::Files)) {
            const QString to = base + QLatin1Char('/') + name;
            QFile::remove(to);
            if (!QFile::copy(from.filePath(name), to)) {
                fail(tr("Could not copy %1 into %2.").arg(name, base));
                return false;
            }
        }
        *target = base + QLatin1Char('/') + QFileInfo(original).fileName();
        return true;
    };

    QString path;
    switch (m_lesson.source.kind) {
    case SourceSpec::Template: {
        const QString base = scratchDir();
        if (base.isEmpty()) {
            fail(tr("Could not make a working directory for the lesson."));
            return false;
        }
        path = base + QLatin1Char('/') + m_lesson.source.as;
        if (!QFileInfo::exists(path)) {
            QString error;
            if (!m_host->createFileFromTemplate(m_lesson.source.value, path, &error)) {
                fail(tr("Could not create %1 from the \"%2\" template: %3")
                         .arg(m_lesson.source.as, m_lesson.source.value, error));
                return false;
            }
        }
        break;
    }
    case SourceSpec::LessonFile: {
        const QString original = QDir(m_lesson.dir).filePath(m_lesson.source.value);
        if (!QFileInfo::exists(original)) {
            fail(tr("The lesson's source %1 is missing.").arg(m_lesson.source.value));
            return false;
        }
        if (!copyBeside(original, &path))
            return false;
        break;
    }
    case SourceSpec::RepoFile: {
        // Relative to the repository root that holds lessons/ — the demo
        // projects a lesson reuses rather than copies (§9).
        const QString root = QFileInfo(m_lesson.dir).dir().absolutePath();
        const QString original = QDir(root).filePath(m_lesson.source.value);
        if (!QFileInfo::exists(original)) {
            fail(tr("This lesson uses %1, which ships with the source tree.")
                     .arg(m_lesson.source.value));
            return false;
        }
        if (!copyBeside(original, &path))
            return false;
        break;
    }
    case SourceSpec::None:
        break;
    }

    if (!path.isEmpty()) {
        if (!m_host->openPathQuiet(path)) {
            fail(tr("Could not open %1.").arg(path));
            return false;
        }
        m_sourcePath = path;
    }
    m_index = 0;
    m_verdict.clear();
    m_verdictKnown = false;
    m_verdictPassed = false;
    emit changed();
    return true;
}

void LessonRunner::restart()
{
    const QString path = m_sourcePath;
    m_index = 0;
    m_verdict.clear();
    m_verdictKnown = false;
    m_verdictPassed = false;
    m_busy = false;
    if (!path.isEmpty())
        m_host->openPathQuiet(path);
    emit changed();
}

void LessonRunner::fail(const QString &why)
{
    m_busy = false;
    m_verdict = why;
    m_verdictKnown = true;
    m_verdictPassed = false;
    emit failed(why);
    emit changed();
}

void LessonRunner::runStep()
{
    const Step *step = currentStep();
    if (!step || m_busy)
        return;
    m_pending = Pending();
    m_pending.stepIndex = m_index;
    m_pending.expects = step->expects;

    // A goto or an explain before the lesson has opened anything is a lesson
    // authoring bug, not a user-facing surprise: start() is idempotent and the
    // panel always calls it first.
    if (m_sourcePath.isEmpty() && m_lesson.source.kind != SourceSpec::None) {
        if (!start())
            return;
    }

    m_busy = true;
    m_verdict.clear();
    m_verdictKnown = false;
    emit changed();
    performNextAction();
}

void LessonRunner::performNextAction()
{
    const Step *step = currentStep();
    if (!step || m_pending.actionIndex >= step->actions.size()) {
        finishStep();
        return;
    }

    const Action action = step->actions.at(m_pending.actionIndex);
    const QString verb = action.verb;

    if (verb == QLatin1String("open")) {
        QString path = action.args.value(0);
        if (QFileInfo(path).isRelative() && !m_lesson.dir.isEmpty())
            path = QDir(m_lesson.dir).filePath(path);
        m_host->openPathQuiet(path);
        actionDone();
        return;
    }
    if (verb == QLatin1String("goto")) {
        bool ok = false;
        const int line = action.args.value(0).toInt(&ok);
        if (!ok) {
            fail(tr("`goto` takes a line number; a label is not mapped to a line yet."));
            return;
        }
        m_host->navigateToLine(m_sourcePath, line);
        actionDone();
        return;
    }
    if (verb == QLatin1String("breakpoint") || verb == QLatin1String("clearbreakpoint")) {
        // Set or clear, never toggle: a lesson that re-runs a step must not
        // remove the breakpoint it just added, and one that moves its breakpoint
        // on (a loop's `dbf` line, then the line the loop exits to) must be able
        // to take the first away — continuing from a breakpoint the loop
        // revisits would never reach the second.
        const bool on = verb == QLatin1String("breakpoint");
        const QString what = action.args.value(0);
        bool numeric = false;
        const int line = what.toInt(&numeric);
        if (numeric) {
            m_host->setBreakpointAtLine(line, on);
        } else {
            QString detail;
            m_host->setBreakpointAtLabel(what, on, &detail);
        }
        actionDone();
        return;
    }
    if (verb == QLatin1String("build") || verb == QLatin1String("buildrelease")) {
        QMetaObject::Connection connection;
        connection = m_host->onBuildCompleted(this, [this, connection](bool) {
            QObject::disconnect(connection);
            actionDone();
        });
        if (verb == QLatin1String("build"))
            m_host->build();
        else
            m_host->buildRelease();
        return;
    }
    if (verb == QLatin1String("run")) {
        QMetaObject::Connection connection;
        connection = m_host->onSessionRunningChanged(this, [this, connection](bool running) {
            if (!running)
                return;
            QObject::disconnect(connection);
            actionDone();
        });
        m_host->run();
        return;
    }
    if (verb == QLatin1String("stop")) {
        m_host->stopSession();
        actionDone();
        return;
    }
    if (verb == QLatin1String("step") || verb == QLatin1String("stepover")
        || verb == QLatin1String("continue")) {
        // A step from a stopped machine emits no running/stopped transition, and
        // `stateJson()` changes the moment the machine resumes (`running` is the
        // emulator process, `stopped` the debugger's stop flag — they move
        // independently), so "some field changed" would return immediately and
        // the checkpoints would read the *previous* stop's registers. What says
        // "the machine has stopped somewhere new" is a stopped machine holding a
        // register batch whose PC is not the one it was on.
        const QString pcBefore = m_host->stateJson().value(QStringLiteral("pc")).toString();
        if (verb == QLatin1String("step"))
            m_host->step();
        else if (verb == QLatin1String("stepover"))
            m_host->stepOver();
        else
            m_host->resume();
        pollUntil(
            [this, pcBefore]() {
                const QJsonObject state = m_host->stateJson();
                return state.value(QStringLiteral("stopped")).toBool()
                       && state.contains(QStringLiteral("pc"))
                       && state.value(QStringLiteral("pc")).toString() != pcBefore;
            },
            verb == QLatin1String("continue") ? 60000 : 20000,
            [this]() { fail(tr("The machine did not stop anywhere new after the step.")); },
            [this]() { actionDone(); });
        return;
    }
    if (verb == QLatin1String("readmem")) {
        const quint32 address = hexOrDecimal(action.args.value(0));
        const int length = action.args.value(1).toInt();
        QMetaObject::Connection connection;
        connection = m_host->onDebugReadMemoryFinished(
            this, [this, connection](quint32, const QList<MemoryRow> &rows) {
                QObject::disconnect(connection);
                m_pending.memory.clear();
                for (const MemoryRow &row : rows)
                    for (quint8 byte : row.bytes)
                        m_pending.memory.append(char(byte));
                m_pending.memoryValid = true;
                actionDone();
            });
        m_host->debugReadMemory(address, length);
        return;
    }
    if (verb == QLatin1String("disasm")) {
        QMetaObject::Connection connection;
        connection = m_host->onDebugReadFinished(this, [this, connection](quint32, const QString &) {
            QObject::disconnect(connection);
            actionDone();
        });
        m_host->debugReadDisassembly(action.args.isEmpty() ? 0 : hexOrDecimal(action.args.value(0)));
        return;
    }
    if (verb == QLatin1String("explain")) {
        m_host->raiseReference(action.args.value(0));
        actionDone();
        return;
    }

    fail(tr("`%1` is not a lesson verb.").arg(verb));
}

void LessonRunner::actionDone()
{
    ++m_pending.actionIndex;
    performNextAction();
}

void LessonRunner::pollUntil(std::function<bool()> fn, int timeoutMs, std::function<void()> onTimeout,
                             std::function<void()> onDone, int intervalMs)
{
    m_pollTest = std::move(fn);
    m_pollTimeout = std::move(onTimeout);
    m_pollDone = std::move(onDone);
    m_pollLimit = timeoutMs;
    m_pollElapsed = 0;
    m_pollInterval = intervalMs;
    m_pollTimer->setInterval(intervalMs);
    m_pollTimer->start();
}

Snapshot LessonRunner::snapshot() const
{
    Snapshot snap;
    const QJsonObject state = m_host->stateJson();
    snap.running = state.value(QStringLiteral("running")).toBool();
    snap.stopped = state.value(QStringLiteral("stopped")).toBool();

    // stateJson writes every register as a hex *string* ("0x00012596"), and
    // omits them entirely when no register batch has arrived yet — so a string
    // is parsed as hex, and a missing key stays missing rather than becoming
    // zero, which a `>= 1` checkpoint would have passed vacuously.
    const auto valueOf = [](const QJsonValue &value) -> quint32 {
        return value.isString() ? hexOrDecimal(value.toString()) : quint32(value.toDouble());
    };
    const auto insert = [&snap, &valueOf](const QJsonObject &source, const QString &key) {
        if (source.contains(key))
            snap.registers.insert(key, valueOf(source.value(key)));
    };
    insert(state, QStringLiteral("pc"));
    insert(state, QStringLiteral("sr"));
    const QJsonObject d = state.value(QStringLiteral("d")).toObject();
    const QJsonObject a = state.value(QStringLiteral("a")).toObject();
    for (auto it = d.begin(); it != d.end(); ++it)
        snap.registers.insert(it.key().toLower(), valueOf(it.value()));
    for (auto it = a.begin(); it != a.end(); ++it)
        snap.registers.insert(it.key().toLower(), valueOf(it.value()));

    snap.problems = m_host->problemsJson().size();
    snap.memory = m_pending.memory;
    snap.memoryValid = m_pending.memoryValid;
    QString file;
    int line = 0;
    if (m_host->pcSourcePosition(&file, &line)) {
        snap.sourceFile = file;
        snap.sourceLine = line;
    }
    return snap;
}

void LessonRunner::finishStep()
{
    awaitStopThenEvaluate();
}

void LessonRunner::awaitStopThenEvaluate()
{
    const Step *step = currentStep();
    if (!step) {
        evaluateExpects();
        return;
    }

    // A checkpoint that says something about the machine needs it stopped, and
    // *stopped* alone is not enough: `stopped` is the debugger's stop flag while
    // the emulator process is alive, and a session that has only just come up
    // reports it before any register batch has arrived. A batch carrying a PC is
    // what says a real stop has happened — and after `run` that is the entry
    // stop, however long TOS takes to boot.
    const bool needsStop = std::any_of(step->expects.cbegin(), step->expects.cend(),
                                       [](const Checkpoint &checkpoint) {
                                           return checkpoint.kind != Checkpoint::Running;
                                       });
    const auto stoppedWithRegisters = [this]() {
        const QJsonObject state = m_host->stateJson();
        return state.value(QStringLiteral("stopped")).toBool()
               && state.contains(QStringLiteral("pc"));
    };
    if (!needsStop || stoppedWithRegisters()) {
        evaluateExpects();
        return;
    }

    pollUntil(
        stoppedWithRegisters,
        // The boots in CI are slow and a lesson is not a benchmark.
        180000,
        // Timing out still evaluates: the checkpoint reports what it actually
        // found ("expected stopped, found the machine running"), which is a
        // better failure than a hang.
        [this]() { evaluateExpects(); },
        [this]() { evaluateExpects(); });
}

void LessonRunner::evaluateExpects()
{
    const Step *step = currentStep();
    m_busy = false;
    m_verdictKnown = true;

    if (!step) {
        m_verdict.clear();
        m_verdictPassed = true;
        emit changed();
        return;
    }

    if (!step->nominal.isEmpty()) {
        // Nothing to check, and the lesson says so.
        m_verdictPassed = true;
        m_verdict = resolveTokens(step->nominal);
        emit stepChecked(m_pending.stepIndex, true, QString());
        emit changed();
        if (m_index == m_lesson.steps.size() - 1)
            emit finished(true);
        return;
    }

    // A `mem` checkpoint needs the bytes; read them once if no `readmem` did.
    const bool needsMemory = std::any_of(step->expects.cbegin(), step->expects.cend(),
                                         [](const Checkpoint &c) {
                                             return c.kind == Checkpoint::Memory;
                                         });
    if (needsMemory && !m_pending.memoryValid) {
        const Checkpoint &memory = *std::find_if(step->expects.cbegin(), step->expects.cend(),
                                                 [](const Checkpoint &c) {
                                                     return c.kind == Checkpoint::Memory;
                                                 });
        QMetaObject::Connection connection;
        connection = m_host->onDebugReadMemoryFinished(
            this, [this, connection](quint32, const QList<MemoryRow> &rows) {
                QObject::disconnect(connection);
                m_pending.memory.clear();
                for (const MemoryRow &row : rows)
                    for (quint8 byte : row.bytes)
                        m_pending.memory.append(char(byte));
                m_pending.memoryValid = true;
                evaluateExpects();
            });
        m_host->debugReadMemory(memory.address, memory.length);
        return;
    }

    const Snapshot snap = snapshot();
    QStringList failures;
    QVector<Checkpoint> expects = step->expects;
    for (const Checkpoint &checkpoint : expects) {
        QString detail;
        if (!evaluateCheckpoint(checkpoint, snap, &detail))
            failures << detail;
    }

    m_verdictPassed = failures.isEmpty();
    if (m_verdictPassed) {
        m_verdict = expects.size() == 1
                        ? tr("%1 holds").arg(checkpointText(expects.first()))
                        : tr("all %1 checks hold").arg(expects.size());
        emit stepChecked(m_pending.stepIndex, true, QString());
    } else {
        m_verdict = failures.join(QStringLiteral("; "));
        emit stepChecked(m_pending.stepIndex, false, m_verdict);
    }
    emit changed();
    if (m_index == m_lesson.steps.size() - 1)
        emit finished(m_verdictPassed);
}

} // namespace lessons
} // namespace pist
