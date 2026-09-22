// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "git/GitService.h"

#include <QHash>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QPushButton;
class QSplitter;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace pist {

/// Status, the commit set, pull/push, the branch, a diff of the selected row,
/// and a flat history. The checked rows are the commit: nothing else is staged
/// into it, and a file left unchecked is taken back out of the index first.
/// The branch selector switches with `git switch`; New… creates one. A switch
/// that would overwrite local edits is refused by git, and that error is
/// shown. The diff is read-only and only present while a file or a commit is
/// selected: staged rows are `git diff --cached`, other rows are `git diff`.
/// History lists commits; selecting one shows `git show` in that same view.
class GitPanel : public QWidget
{
    Q_OBJECT

public:
    explicit GitPanel(QWidget *parent = nullptr);

    /// Project directory. Empty, or a folder that is not a repository, leaves
    /// the actions disabled and says so.
    void setDirectory(const QString &directory);
    void refresh();

    bool inRepository() const;
    QString repositoryRoot() const;

    void blame(const QString &file, int firstLine, int lastLine, const QByteArray &contents);

signals:
    void repositoryChanged(bool inRepo, const QString &root);
    void blameReady(const QString &file, const pist::GitBlameMap &lines);

private:
    void showStatus(const GitStatus &status);
    void showBranches(const QStringList &names);
    void requestDiff(QTreeWidgetItem *item);
    void showDiff(const QString &path, int group, bool ok, const QString &text);
    void clearDiff();
    void setHistoryMode(bool on);
    void showLog(const GitLog &entries, bool ok, const QString &error);
    void showCommit(const QString &hash, bool ok, const QString &text);
    void applyWorktreeVisibility(bool clean);
    void selectCurrentBranch();
    void updateCommitEnabled();
    QString askBranchName();
    QString branchText(const GitStatus &status) const;

    GitService *m_git = nullptr;
    QString m_directory;
    QLabel *m_branch = nullptr;
    QLabel *m_empty = nullptr;
    QPlainTextEdit *m_output = nullptr;
    QTreeWidget *m_files = nullptr;
    QListWidget *m_log = nullptr;
    QStackedWidget *m_stack = nullptr;
    QSplitter *m_split = nullptr;
    QPlainTextEdit *m_diff = nullptr;
    QPushButton *m_changes = nullptr;
    QPushButton *m_history = nullptr;
    QPlainTextEdit *m_message = nullptr;
    QPushButton *m_commit = nullptr;
    QPushButton *m_pull = nullptr;
    QPushButton *m_push = nullptr;
    QComboBox *m_branches = nullptr;
    QPushButton *m_newBranch = nullptr;
    QString m_currentBranch;
    bool m_detached = false;
    QString m_diffPath;
    GitChange m_diffGroup = GitChange::Unstaged;
    QString m_showHash;
    bool m_historyMode = false;
    /// Remembered checkboxes, so a refresh does not undo a choice. Keyed by
    /// group and path — one file can have a staged row and an unstaged row.
    QHash<QString, bool> m_checked;
    bool m_inRepo = false;
};

} // namespace pist
