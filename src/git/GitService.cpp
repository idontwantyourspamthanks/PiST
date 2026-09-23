// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "git/GitService.h"

#include "build/ProcessUtil.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace pist {

namespace {

QStringList uniquePaths(const QStringList &paths)
{
    QStringList out;
    for (const QString &path : paths) {
        if (!path.isEmpty() && !out.contains(path))
            out.append(path);
    }
    return out;
}

/// The error for a staged path that the checked set leaves out. `paths` is
/// non-empty.
QString stagedOutsideMessage(const QStringList &paths)
{
    return GitService::tr("%1 is staged but not checked — include it or unstage it.")
        .arg(paths.join(QStringLiteral(", ")));
}

/// Why a commit must not run, or an empty string when the index is exactly the
/// checked set. `selection` is the checked paths. A path staged in the index
/// but left out of the selection would be swept into a pathspec-less commit,
/// and a conflicted path would commit markers — both are refused here, before
/// `git add` runs, so the user's index is never rewritten to hide them.
QString commitBlocker(const GitStatus &status, const QStringList &selection)
{
    QStringList conflicted;
    QStringList outside;
    for (const GitChangeEntry &entry : status.entries) {
        if (entry.conflicted) {
            if (selection.contains(entry.path) && !conflicted.contains(entry.path))
                conflicted.append(entry.path);
        } else if (entry.group == GitChange::Staged && !selection.contains(entry.path)) {
            if (!outside.contains(entry.path))
                outside.append(entry.path);
        }
    }
    if (!conflicted.isEmpty()) {
        return GitService::tr("Cannot commit a conflicted path: %1. Resolve it first.")
            .arg(conflicted.join(QStringLiteral(", ")));
    }
    if (!outside.isEmpty())
        return stagedOutsideMessage(outside);
    return {};
}

} // namespace

GitService::GitService(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<GitStatus>();
    qRegisterMetaType<GitBlameLine>();
    qRegisterMetaType<GitBlameMap>();
    qRegisterMetaType<GitLogEntry>();
    qRegisterMetaType<GitLog>();

    m_proc = new QProcess(this);
    connect(m_proc, &QProcess::started, this, [this] {
        // start() returns before the process exists, and write() before that
        // is discarded. The body (a commit message, or the buffer being
        // blamed) has to go out once git is actually reading.
        if (!m_pendingInput.isEmpty())
            m_proc->write(m_pendingInput);
        m_pendingInput.clear();
        m_proc->closeWriteChannel();
    });
    connect(m_proc, &QProcess::finished, this, [this](int, QProcess::ExitStatus) { settle(false); });
    connectFailedToStart(m_proc, this, [this] {
        // FailedToStart emits no finished, which would leave the queue stuck.
        settle(true);
    });
}

GitService::~GitService()
{
    // A window closing mid-status must not destroy a live QProcess. settle()
    // would then run against an object that is already going away.
    killAndRelease(m_proc, this);
}

QString GitService::gitProgram()
{
    return QStandardPaths::findExecutable(QStringLiteral("git"));
}

void GitService::setDirectory(const QString &directory)
{
    if (directory == m_requested)
        return;
    m_requested = directory;
    ++m_dirGen;
    m_root.clear();
    m_inRepo = false;
    emit repositoryChanged(false, {});

    dropQueued(QStringLiteral("discover"));
    dropQueued(QStringLiteral("status"));
    dropQueued(QStringLiteral("blame"));
    dropQueued(QStringLiteral("diff"));
    dropQueued(QStringLiteral("log"));
    dropQueued(QStringLiteral("show"));
    if (m_running && (m_current.name == QLatin1String("discover")
                      || m_current.name == QLatin1String("status")
                      || m_current.name == QLatin1String("blame")
                      || m_current.name == QLatin1String("diff")
                      || m_current.name == QLatin1String("log")
                      || m_current.name == QLatin1String("show"))) {
        m_discard = true;
        m_proc->kill();
    }

    GitStatus empty;
    if (directory.isEmpty()) {
        empty.error = tr("Open a folder to use git.");
        emit statusReady(empty);
        return;
    }
    if (gitProgram().isEmpty()) {
        empty.error = tr("git was not found on PATH.");
        emit statusReady(empty);
        return;
    }

    Task task;
    task.name = QStringLiteral("discover");
    task.workDir = directory;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel")}, {}});
    enqueue(task);
}

void GitService::refreshStatus()
{
    if (!m_inRepo || m_root.isEmpty())
        return;
    bool statusPending = m_running && m_current.name == QLatin1String("status");
    if (!statusPending) {
        for (const Task &task : m_queue) {
            if (task.name == QLatin1String("status")) {
                statusPending = true;
                break;
            }
        }
    }
    if (!statusPending) {
        Task task;
        task.name = QStringLiteral("status");
        task.workDir = m_root;
        task.dirGen = m_dirGen;
        task.steps.append({{QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                             QStringLiteral("-z"), QStringLiteral("-b")},
                            {}});
        enqueue(task);
    }
    refreshBranches();
}

void GitService::refreshBranches()
{
    if (!m_inRepo || m_root.isEmpty())
        return;
    // A switch just changed the list. Drop the listing already in flight so
    // the panel does not repaint the old names over the new branch.
    dropQueued(QStringLiteral("branches"));
    if (m_running && m_current.name == QLatin1String("branches")) {
        m_discard = true;
        m_proc->kill();
    }

    Task task;
    task.name = QStringLiteral("branches");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("branch"), QStringLiteral("--format=%(refname:short)")}, {}});
    enqueue(task);
}

void GitService::blame(const QString &file, int firstLine, int lastLine, const QByteArray &contents)
{
    if (!m_inRepo || file.isEmpty() || firstLine <= 0 || lastLine < firstLine)
        return;

    // A scroll settles into a new range. Drop the one already queued, and
    // abandon the one in flight: its line range is already stale.
    ++m_blameGen;
    dropQueued(QStringLiteral("blame"));
    if (m_running && m_current.name == QLatin1String("blame")) {
        m_discard = true;
        m_proc->kill();
    }

    Task task;
    task.name = QStringLiteral("blame");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.blameGen = m_blameGen;
    task.blameFile = file;
    task.blameFirst = firstLine;
    task.blameLast = lastLine;
    task.steps.append({{QStringLiteral("blame"), QStringLiteral("-p"), QStringLiteral("-L"),
                         QStringLiteral("%1,%2").arg(firstLine).arg(lastLine),
                         QStringLiteral("--contents"), QStringLiteral("-"),
                         QStringLiteral("--"), file},
                        contents});
    enqueue(task);
}

void GitService::commit(const QStringList &stage, const QStringList &unstage,
                        const QStringList &paths, const QString &message)
{
    const QStringList commitPaths = uniquePaths(paths);
    const QString text = message.trimmed();
    if (!m_inRepo || text.isEmpty() || commitPaths.isEmpty()) {
        emit operationFinished(QStringLiteral("commit"), false,
                               tr("Choose at least one file and write a message."));
        return;
    }

    // A staged row the panel left unchecked is still in the index, so a
    // pathspec-less commit would take it. Refuse rather than unstage it: the
    // index is the user's, partial `git add -p` selections included. The index
    // is read below as well, so a path staged outside PiST is caught too.
    QStringList unchecked;
    for (const QString &path : uniquePaths(unstage)) {
        if (!commitPaths.contains(path))
            unchecked.append(path);
    }
    if (!unchecked.isEmpty()) {
        emit operationFinished(QStringLiteral("commit"), false, stagedOutsideMessage(unchecked));
        return;
    }

    // Add the ticked working-tree rows, then commit the index with no
    // pathspec. The first step reads the index so a path staged outside the
    // selection, or a conflicted path, stops the commit before `git add` runs.
    // No `-a`, no `--no-verify`: hooks run, and the checkboxes are the set.
    Task task;
    task.name = QStringLiteral("commit");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.commitSelection = uniquePaths(paths + stage);
    task.steps.append({{QStringLiteral("status"), QStringLiteral("--porcelain=v1"),
                        QStringLiteral("-z"), QStringLiteral("-b")},
                       {}});
    const QStringList add = uniquePaths(stage);
    if (!add.isEmpty()) {
        QStringList args{QStringLiteral("add"), QStringLiteral("--")};
        args += add;
        task.steps.append({args, {}});
    }
    QByteArray body = text.toUtf8();
    body.append('\n');
    task.steps.append({{QStringLiteral("commit"), QStringLiteral("-F"), QStringLiteral("-")}, body});
    enqueue(task);
    setBusy(true);
}

void GitService::pull()
{
    if (!m_inRepo)
        return;
    Task task;
    task.name = QStringLiteral("pull");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("pull")}, {}});
    enqueue(task);
    setBusy(true);
}

void GitService::push()
{
    if (!m_inRepo)
        return;
    Task task;
    task.name = QStringLiteral("push");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("push")}, {}});
    enqueue(task);
    setBusy(true);
}

void GitService::switchBranch(const QString &name)
{
    if (!m_inRepo || name.isEmpty())
        return;
    // Plain switch. Git aborts when the worktree would be overwritten, which
    // is the refusal we want — there is no discard flag to opt into.
    Task task;
    task.name = QStringLiteral("switch");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("switch"), QStringLiteral("--"), name}, {}});
    enqueue(task);
    setBusy(true);
}

void GitService::createBranch(const QString &name)
{
    const QString branch = name.trimmed();
    if (!m_inRepo || branch.isEmpty()) {
        emit operationFinished(QStringLiteral("branch"), false, tr("Enter a branch name."));
        return;
    }
    Task task;
    task.name = QStringLiteral("branch");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("switch"), QStringLiteral("-c"), branch}, {}});
    enqueue(task);
    setBusy(true);
}

void GitService::diff(const QString &path, GitChange group)
{
    if (!m_inRepo || path.isEmpty())
        return;
    // Selecting another row replaces the diff already queued or in flight.
    dropQueued(QStringLiteral("diff"));
    if (m_running && m_current.name == QLatin1String("diff")) {
        m_discard = true;
        m_proc->kill();
    }

    QStringList args{QStringLiteral("diff"), QStringLiteral("--no-color"), QStringLiteral("--no-ext-diff")};
    if (group == GitChange::Staged) {
        args << QStringLiteral("--cached") << QStringLiteral("--") << path;
    } else if (group == GitChange::Untracked) {
        // Git's empty side is `/dev/null` on every platform, including Git for
        // Windows. Qt's null device is `NUL` there, which `--no-index` cannot
        // open as a file.
        args << QStringLiteral("--no-index") << QStringLiteral("--")
             << QStringLiteral("/dev/null") << path;
    } else {
        args << QStringLiteral("--") << path;
    }

    Task task;
    task.name = QStringLiteral("diff");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.diffPath = path;
    task.diffGroup = int(group);
    task.steps.append({args, {}});
    enqueue(task);
}

void GitService::refreshLog()
{
    if (!m_inRepo || m_root.isEmpty())
        return;
    dropQueued(QStringLiteral("log"));
    if (m_running && m_current.name == QLatin1String("log")) {
        m_discard = true;
        m_proc->kill();
    }

    // One past the number the panel shows, so it can tell that the history
    // was cut off. Field order matches parseLog.
    Task task;
    task.name = QStringLiteral("log");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.steps.append({{QStringLiteral("log"), QStringLiteral("-n"), QStringLiteral("201"),
                         QStringLiteral("--date=short"),
                         QStringLiteral("--format=%H%x00%h%x00%an%x00%ad%x00%D%x00%s%x00")},
                        {}});
    enqueue(task);
}

void GitService::showCommit(const QString &hash)
{
    if (!m_inRepo || hash.isEmpty())
        return;
    dropQueued(QStringLiteral("show"));
    if (m_running && m_current.name == QLatin1String("show")) {
        m_discard = true;
        m_proc->kill();
    }

    Task task;
    task.name = QStringLiteral("show");
    task.workDir = m_root;
    task.dirGen = m_dirGen;
    task.showHash = hash;
    task.steps.append({{QStringLiteral("show"), QStringLiteral("--no-color"),
                         QStringLiteral("--no-ext-diff"), QStringLiteral("--format=medium"), hash},
                        {}});
    enqueue(task);
}

void GitService::enqueue(const Task &task)
{
    m_queue.append(task);
    if (!m_running)
        startNext();
}

void GitService::dropQueued(const QString &name)
{
    QList<Task> kept;
    for (const Task &task : m_queue) {
        if (task.name != name)
            kept.append(task);
    }
    m_queue = kept;
}

void GitService::startNext()
{
    if (m_running || m_queue.isEmpty()) {
        if (!m_running)
            setBusy(false);
        return;
    }
    m_current = m_queue.takeFirst();
    m_current.step = 0;
    m_current.stdoutBuf.clear();
    m_current.stderrBuf.clear();
    m_running = true;
    m_discard = false;
    startStep();
}

void GitService::startStep()
{
    const Step &step = m_current.steps.at(m_current.step);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    m_proc->setProcessEnvironment(env);
    m_proc->setWorkingDirectory(m_current.workDir);

    QStringList args;
    args << QStringLiteral("--no-pager");
    args += step.args;
    m_pendingInput = step.input;
    m_proc->start(gitProgram(), args);
}

void GitService::settle(bool failedToStart)
{
    if (!m_running)
        return;

    m_current.stdoutBuf += m_proc->readAllStandardOutput();
    m_current.stderrBuf += m_proc->readAllStandardError();

    if (m_discard) {
        m_discard = false;
        m_running = false;
        startNext();
        return;
    }

    const bool normal = !failedToStart && m_proc->exitStatus() == QProcess::NormalExit;
    // `git diff` is 0 when the command works. `--no-index` (untracked files)
    // exits 1 when the two sides differ; that patch is still the result.
    const bool diffResult = m_current.name == QLatin1String("diff")
        && (m_proc->exitCode() == 0 || m_proc->exitCode() == 1);
    const bool ok = normal && (m_proc->exitCode() == 0 || diffResult);
    if (!ok) {
        m_running = false;
        finishTask(false);
        return;
    }

    // A commit reads the index first. Stop here, before `git add` runs, when a
    // staged path is outside the checked set or a checked path is conflicted:
    // the index is the user's, and a conflict is never committed as a
    // resolution the user was not shown.
    if (m_current.name == QLatin1String("commit") && m_current.step == 0) {
        const QString blocked = commitBlocker(parsePorcelain(m_current.stdoutBuf),
                                              m_current.commitSelection);
        if (!blocked.isEmpty()) {
            m_current.stdoutBuf.clear();
            m_current.stderrBuf = blocked.toUtf8();
            m_running = false;
            finishTask(false);
            return;
        }
    }

    ++m_current.step;
    if (m_current.step < m_current.steps.size()) {
        m_current.stdoutBuf.clear();
        m_current.stderrBuf.clear();
        startStep();
        return;
    }

    m_running = false;
    finishTask(true);
}

QString GitService::combinedOutput() const
{
    const QString out = QString::fromUtf8(m_current.stdoutBuf).trimmed();
    const QString err = QString::fromUtf8(m_current.stderrBuf).trimmed();
    if (out.isEmpty())
        return err;
    if (err.isEmpty())
        return out;
    return out + QLatin1Char('\n') + err;
}

void GitService::finishTask(bool ok)
{
    const Task done = m_current;
    const bool stale = done.dirGen != m_dirGen
        && (done.name == QLatin1String("discover") || done.name == QLatin1String("status")
            || done.name == QLatin1String("blame") || done.name == QLatin1String("branches")
            || done.name == QLatin1String("diff") || done.name == QLatin1String("log")
            || done.name == QLatin1String("show"));

    if (!stale && done.name == QLatin1String("discover")) {
        if (ok) {
            m_root = QString::fromUtf8(done.stdoutBuf).trimmed();
            m_inRepo = !m_root.isEmpty();
            emit repositoryChanged(m_inRepo, m_root);
            if (m_inRepo)
                refreshStatus();
        } else {
            GitStatus status;
            const QString err = combinedOutput();
            status.error = err.contains(QStringLiteral("not a git repository"))
                               ? tr("Not a git repository.")
                               : err;
            emit statusReady(status);
        }
    } else if (!stale && done.name == QLatin1String("status")) {
        if (ok) {
            emit statusReady(parsePorcelain(done.stdoutBuf));
        } else {
            GitStatus status;
            status.error = combinedOutput();
            emit statusReady(status);
        }
    } else if (!stale && done.name == QLatin1String("blame")) {
        if (done.blameGen == m_blameGen) {
            GitBlameMap lines = ok ? parseBlame(done.stdoutBuf) : GitBlameMap{};
            if (!ok) {
                // An untracked file has no history. Every visible line is ours.
                GitBlameLine uncommitted;
                uncommitted.uncommitted = true;
                for (int line = done.blameFirst; line <= done.blameLast; ++line)
                    lines.insert(line, uncommitted);
            }
            emit blameReady(done.blameFile, lines);
        }
    } else if (!stale && done.name == QLatin1String("branches")) {
        QStringList names;
        if (ok) {
            for (const QString &line : QString::fromUtf8(done.stdoutBuf).split(QLatin1Char('\n'))) {
                const QString name = line.trimmed();
                if (!name.isEmpty())
                    names.append(name);
            }
        }
        emit branchesReady(names);
    } else if (!stale && done.name == QLatin1String("diff")) {
        const QString text = ok ? QString::fromUtf8(done.stdoutBuf) : combinedOutput();
        emit diffReady(done.diffPath, done.diffGroup, ok, text);
    } else if (!stale && done.name == QLatin1String("log")) {
        const QString err = combinedOutput();
        if (!ok && err.contains(QStringLiteral("does not have any commits")))
            emit logReady({}, true, {});
        else if (!ok)
            emit logReady({}, false, err);
        else
            emit logReady(parseLog(done.stdoutBuf), true, {});
    } else if (!stale && done.name == QLatin1String("show")) {
        const QString text = ok ? QString::fromUtf8(done.stdoutBuf) : combinedOutput();
        emit showReady(done.showHash, ok, text);
    } else if (done.name == QLatin1String("commit") || done.name == QLatin1String("pull")
               || done.name == QLatin1String("push") || done.name == QLatin1String("switch")
               || done.name == QLatin1String("branch")) {
        emit operationFinished(done.name, ok, combinedOutput());
        if (m_inRepo && done.dirGen == m_dirGen)
            refreshStatus();
    }

    startNext();
}

void GitService::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged(busy);
}

} // namespace pist
