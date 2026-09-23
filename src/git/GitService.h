// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "git/GitTypes.h"

#include <QObject>
#include <QStringList>

class QProcess;

namespace pist {

/// Drives `git` as a subprocess, the same way the IDE drives vasm and Hatari:
/// argument lists only. Nothing here writes git config, and nothing is passed
/// through a shell.
///
/// One process at a time, so a status refresh cannot race a commit for the
/// index lock. Pull and push are plain `git pull` / `git push` — no refspec
/// and no `--force` — and `GIT_TERMINAL_PROMPT=0` makes a missing credential
/// fail instead of waiting for a password that nobody can type.
class GitService : public QObject
{
    Q_OBJECT

public:
    explicit GitService(QObject *parent = nullptr);
    ~GitService() override;

    static QString gitProgram();

    /// Any directory inside the project. Empty clears the repository. The
    /// root is discovered with `rev-parse --show-toplevel` rather than assumed.
    void setDirectory(const QString &directory);
    QString repositoryRoot() const { return m_root; }
    bool inRepository() const { return m_inRepo; }

    void refreshStatus();
    /// Blame `file` over the inclusive 1-based range, pretending the file has
    /// `contents` (the editor buffer). A line that differs from HEAD comes
    /// back uncommitted, instead of being attributed to a neighbour.
    void blame(const QString &file, int firstLine, int lastLine, const QByteArray &contents);
    /// Stage the ticked `stage` rows, then `git commit -F -` with no pathspec:
    /// the index — the checked `paths` — is exactly the commit, so a path the
    /// user staged elsewhere can never ride along and the index is never
    /// rewritten. `unstage` names the staged rows the panel left unchecked;
    /// the index is read before committing too, and a staged path outside
    /// `paths`, or a conflicted path, refuses the commit with an error and
    /// leaves the index alone. Hooks run. An empty message or an empty path
    /// list does not invoke git.
    void commit(const QStringList &stage, const QStringList &unstage,
                const QStringList &paths, const QString &message);
    void pull();
    void push();
    /// `git switch -- name`. No `--discard-changes`: if the worktree would be
    /// overwritten, git refuses and the panel shows that error.
    void switchBranch(const QString &name);
    /// `git switch -c name`, from the current HEAD, keeping local edits.
    void createBranch(const QString &name);
    /// `git diff` for one status row. Staged uses `--cached`; untracked uses
    /// `--no-index` against `/dev/null`. A difference is exit code 1, which
    /// is a result, not a failure.
    void diff(const QString &path, GitChange group);
    /// The latest commits, newest first. Not a graph.
    void refreshLog();
    /// `git show` for one commit: message and patch.
    void showCommit(const QString &hash);

    bool busy() const { return m_busy; }

signals:
    void statusReady(const pist::GitStatus &status);
    void blameReady(const QString &file, const pist::GitBlameMap &lines);
    /// `action` is "commit", "pull", "push", "switch" or "branch". `output` is
    /// git's own text.
    void operationFinished(const QString &action, bool ok, const QString &output);
    void branchesReady(const QStringList &branches);
    /// `group` is `GitChange`. `text` is the patch, or git's error.
    void diffReady(const QString &path, int group, bool ok, const QString &text);
    /// `ok` is false when `git log` failed for a reason other than an empty
    /// branch. An empty history is `ok` with no entries.
    void logReady(const pist::GitLog &entries, bool ok, const QString &error);
    void showReady(const QString &hash, bool ok, const QString &text);
    void repositoryChanged(bool inRepo, const QString &root);
    void busyChanged(bool busy);

private:
    struct Step {
        QStringList args;
        QByteArray input;
    };
    struct Task {
        QString name;
        QString workDir;
        int dirGen = 0;
        QList<Step> steps;
        int step = 0;
        QByteArray stdoutBuf;
        QByteArray stderrBuf;
        QString blameFile;
        int blameGen = 0;
        int blameFirst = 0;
        int blameLast = 0;
        QString diffPath;
        int diffGroup = 0;
        QString showHash;
        /// The checked paths a commit is checked against, once the index has
        /// been read.
        QStringList commitSelection;
    };

    void refreshBranches();
    void enqueue(const Task &task);
    void dropQueued(const QString &name);
    void startNext();
    void startStep();
    void settle(bool failedToStart);
    void finishTask(bool ok);
    void setBusy(bool busy);
    QString combinedOutput() const;

    QProcess *m_proc = nullptr;
    QByteArray m_pendingInput;
    QString m_requested;
    QString m_root;
    bool m_inRepo = false;
    bool m_running = false;
    bool m_discard = false;
    bool m_busy = false;
    int m_dirGen = 0;
    int m_blameGen = 0;
    QList<Task> m_queue;
    Task m_current;
};

} // namespace pist
