// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <utility>

namespace pist {

/// The QProcess idioms every subprocess owner in the tree shares.
///
/// Header-only on purpose: it is the one part of the build module that the
/// other modules' subprocess users (git, emu, toolchain) include without
/// taking a link dependency on build/ — there is nothing here to link.
///
/// Findings MIN-49 (the lifecycle idiom) and MIN-50 (the synchronous run) both
/// name this duplication; the helpers live here so the next caller inherits
/// the guard instead of a fifth copy of it.

/// How long a killed process is given to be reaped before its owner moves on.
/// A kill that has not been collected in two seconds is not going to be, and
/// the owner is usually in a destructor where waiting forever is worse.
constexpr int kProcessReapTimeoutMs = 2000;

/// Stop `process` — if it is still running — and reap it.
///
/// The handlers go first: the owner is going away, or is about to drop this
/// process, and must not be called back by a half-torn-down object. An empty
/// or already-finished process is left alone, so this is safe to call on every
/// teardown path. The owner still owns what happens next (a `deleteLater()` on
/// a cancel path, nothing at all in a destructor that Qt's parent-child
/// ownership will clean up).
inline void killAndRelease(QProcess *process, QObject *owner)
{
    if (!process)
        return;
    process->disconnect(owner);
    if (process->state() != QProcess::NotRunning) {
        process->kill();
        process->waitForFinished(kProcessReapTimeoutMs);
    }
}

/// Call `handler` when `process` could not be launched at all.
///
/// Qt emits `finished` only when a child dies, so a program that never started
/// — an uninstalled toolchain is the ordinary case, not an exotic one — would
/// otherwise leave the owner's queue stuck forever: nothing advances it, the
/// pending operation never answers, and the QProcess leaks. `context` is the
/// owner, so the handler cannot outlive it.
template <typename Handler>
inline void connectFailedToStart(QProcess *process, QObject *context, Handler handler)
{
    QObject::connect(process, &QProcess::errorOccurred, context,
                     [handler = std::move(handler)](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart)
                             handler();
                     });
}

/// How a synchronous run is driven. Every field's default is the ordinary
/// case; a caller that needs a different policy states it here rather than
/// copying the loop.
struct RunOptions
{
    /// Empty leaves the run in the caller's own working directory.
    QString workingDirectory;
    /// When set, the child's stdout is written here instead of being captured
    /// (QProcess truncates the file before the child starts, so a caller that
    /// needs the old bytes kept must stage under a temporary name — see
    /// ToolFetch::extractZipMember).
    QString stdoutFile;
    /// Launching a binary is either immediate or never: this only has to be
    /// long enough for a loaded machine to fork.
    int startTimeoutMs = 10000;
    /// A whole run's budget. A tool probe answers in milliseconds; a build
    /// driven from a fetched source archive is minutes. A run that overruns is
    /// killed and reported, never waited on indefinitely.
    int finishTimeoutMs = 60000;
};

/// The outcome of one runSync.
struct SyncRun
{
    bool started = false;      ///< the process was launched at all
    bool finished = false;     ///< it exited on its own within the timeout
    bool normalExit = false;   ///< it exited, rather than crashing
    int exitCode = -1;         ///< meaningful once `finished`
    /// stdout followed by stderr, as text. Captured whether the run succeeded
    /// or not, so a failure message can quote the tool.
    QString output;
    /// Why the process never ran to completion, or empty. Set only for a
    /// failure to start and for a timeout.
    QString error;

    /// True when the process ran and reported success.
    bool ok() const { return finished && normalExit && exitCode == 0; }

    /// The text to show a user when `ok()` is false: the process's own output
    /// when it ran and failed, and the reason it never ran when it did not.
    QString failureText() const { return error.isEmpty() ? output.trimmed() : error; }
};

/// Run `program` to completion and capture its output.
///
/// The synchronous counterpart of the async owners elsewhere in the tree: for
/// a probe or a short tool invocation, where a queued task and a signal would
/// be more machinery than the answer is worth. It does not spin a nested event
/// loop, so it must not be called from a context that has work to service
/// while it waits.
inline SyncRun runSync(const QString &program, const QStringList &args,
                       const RunOptions &options = RunOptions())
{
    SyncRun run;
    QProcess process;
    if (!options.workingDirectory.isEmpty())
        process.setWorkingDirectory(options.workingDirectory);
    if (!options.stdoutFile.isEmpty())
        process.setStandardOutputFile(options.stdoutFile);

    process.start(program, args);
    if (!process.waitForStarted(options.startTimeoutMs)) {
        run.error = QObject::tr("could not start %1").arg(program);
        return run;
    }
    run.started = true;

    if (!process.waitForFinished(options.finishTimeoutMs)) {
        process.kill();
        process.waitForFinished(kProcessReapTimeoutMs);
        run.output = QString::fromUtf8(process.readAllStandardOutput())
                   + QString::fromUtf8(process.readAllStandardError());
        run.error = QObject::tr("%1 did not finish").arg(program);
        return run;
    }

    run.finished = true;
    run.normalExit = process.exitStatus() == QProcess::NormalExit;
    run.exitCode = process.exitCode();
    run.output = QString::fromUtf8(process.readAllStandardOutput())
               + QString::fromUtf8(process.readAllStandardError());
    return run;
}

} // namespace pist
