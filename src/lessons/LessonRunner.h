// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>

#include "control/ControlHost.h"
#include "lessons/Lesson.h"

class QTimer;

namespace pist {
namespace lessons {

/// Runs a lesson against the IDE.
///
/// It drives the same control::ControlHost interface the remote-control
/// protocol uses (docs/tutorial-plan.md §5 decision 2), so the panel, the
/// headless test and — later — an external agent share one implementation of
/// every step. The runner is deliberately QtCore-only: no widget, no IDE type.
///
/// A step's actions run in order and then its `expect` lines are evaluated
/// against one snapshot of the machine; a step that fails is reported, never
/// enforced, so a lesson cannot trap a user mid-step. The test treats the same
/// verdict as fatal (§7.2).
class LessonRunner : public QObject
{
    Q_OBJECT

public:
    /// Resolves `{key:step}` and friends to the key the *active* shortcut
    /// scheme has bound, so a lesson never names a key that is not there
    /// (§5 decision 9). An empty answer falls back to the action's name.
    using KeyResolver = std::function<QString(const QString &actionId)>;

    explicit LessonRunner(control::ControlHost *host, QObject *parent = nullptr);

    void setKeyResolver(KeyResolver resolver);

    void setLesson(const Lesson &lesson);
    const Lesson &lesson() const { return m_lesson; }

    int count() const { return int(m_lesson.steps.size()); }
    int index() const { return m_index; }
    const Step *currentStep() const;

    /// The step's teach prose, with `{key:...}` resolved for the active scheme.
    QString teachText(int index) const;

    bool busy() const { return m_busy; }
    bool verdictKnown() const { return m_verdictKnown; }
    bool verdictPassed() const { return m_verdictPassed; }
    QString verdictText() const { return m_verdict; }

    /// The file the lesson's program lives in, once start() has prepared it.
    QString sourcePath() const { return m_sourcePath; }

    /// Resolve the source, open it, and show the first step without running it.
    /// False when the lesson could not be prepared (a missing tool, a missing
    /// source); `failed` has already carried the reason.
    bool start();
    void restart();

public slots:
    /// Move to a step. Moving does not perform it.
    void goToStep(int index);

    /// Perform the current step's actions, then evaluate its checkpoints.
    void runStep();

signals:
    /// The lesson, the step or the verdict changed — the panel re-renders.
    void changed();
    /// The step was performed and checked. `detail` is empty when it passed.
    void stepChecked(int index, bool passed, const QString &detail);
    /// The lesson could not run at all (no source, a missing tool).
    void failed(const QString &why);
    /// The last step was reached and checked.
    void finished(bool allPassed);

private:
    struct Pending
    {
        int stepIndex = -1;
        int actionIndex = 0;
        QVector<Checkpoint> expects;
        QByteArray memory;      ///< a read an earlier `readmem` already made
        bool memoryValid = false;
        int failures = 0;
        QString firstFailure;
    };

    void performNextAction();
    void actionDone();
    /// Run `fn` again every `intervalMs` until it returns true, then call
    /// `onDone`; give up after `timeoutMs` and call `onTimeout`. Polling rather
    /// than events: a single step from a stopped machine emits no
    /// running/stopped transition, so there is no event to wait for (§7.2, and
    /// §15's open question 6).
    void pollUntil(std::function<bool()> fn, int timeoutMs, std::function<void()> onTimeout,
                   std::function<void()> onDone, int intervalMs = 200);
    void finishStep();
    /// Wait for the machine to stop when a checkpoint needs it stopped, then
    /// evaluate. The entry stop follows `run` by however long TOS takes to
    /// boot, so a Stopped checkpoint cannot be evaluated on the command's
    /// return.
    void awaitStopThenEvaluate();
    void evaluateExpects();
    Snapshot snapshot() const;
    QString resolveTokens(const QString &text) const;
    void fail(const QString &why);

    control::ControlHost *m_host = nullptr;
    KeyResolver m_keys;
    Lesson m_lesson;
    int m_index = 0;
    bool m_busy = false;
    bool m_verdictKnown = false;
    bool m_verdictPassed = false;
    QString m_verdict;
    QString m_sourcePath;
    Pending m_pending;
    QTimer *m_pollTimer = nullptr;
    std::function<bool()> m_pollTest;
    std::function<void()> m_pollTimeout;
    std::function<void()> m_pollDone;
    int m_pollElapsed = 0;
    int m_pollLimit = 0;
    int m_pollInterval = 200;
};

} // namespace lessons
} // namespace pist
