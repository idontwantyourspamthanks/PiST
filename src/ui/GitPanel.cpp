// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/GitPanel.h"

#include "ui/Appearance.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

namespace pist {

namespace {

const int kPathRole = Qt::UserRole;
const int kGroupRole = Qt::UserRole + 1;
const int kHashRole = Qt::UserRole;
const int kLogShown = 200;

QString checkKey(GitChange group, const QString &path)
{
    return QString::number(int(group)) + QLatin1Char(':') + path;
}

} // namespace

GitPanel::GitPanel(QWidget *parent)
    : QWidget(parent)
{
    m_git = new GitService(this);

    m_branch = new QLabel(tr("Git"), this);
    m_branch->setObjectName(QStringLiteral("gitBranch"));
    m_branch->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_pull = new QPushButton(tr("Pull"), this);
    m_pull->setObjectName(QStringLiteral("gitPull"));
    m_pull->setToolTip(tr("git pull"));
    m_push = new QPushButton(tr("Push"), this);
    m_push->setObjectName(QStringLiteral("gitPush"));
    m_push->setToolTip(tr("git push"));
    m_pull->setEnabled(false);
    m_push->setEnabled(false);

    auto *header = new QHBoxLayout;
    header->addWidget(m_branch, 1);
    header->addWidget(m_pull);
    header->addWidget(m_push);

    m_output = new QPlainTextEdit(this);
    m_output->setObjectName(QStringLiteral("gitOutput"));
    m_output->setReadOnly(true);
    m_output->setMaximumHeight(fontMetrics().height() * 6);
    m_output->setPlaceholderText(tr("Pull and push report here."));

    m_files = new QTreeWidget(this);
    m_files->setObjectName(QStringLiteral("gitFiles"));
    m_files->setHeaderHidden(true);
    m_files->setRootIsDecorated(false);

    m_log = new QListWidget(this);
    m_log->setObjectName(QStringLiteral("gitLog"));
    appearance::markMono(m_log);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_files);
    m_stack->addWidget(m_log);

    m_changes = new QPushButton(tr("Changes"), this);
    m_changes->setObjectName(QStringLiteral("gitChanges"));
    m_changes->setCheckable(true);
    m_changes->setChecked(true);
    m_history = new QPushButton(tr("History"), this);
    m_history->setObjectName(QStringLiteral("gitHistory"));
    m_history->setCheckable(true);
    m_history->setEnabled(false);
    auto *modes = new QButtonGroup(this);
    modes->setExclusive(true);
    modes->addButton(m_changes);
    modes->addButton(m_history);
    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(m_changes);
    modeRow->addWidget(m_history);
    modeRow->addStretch(1);

    m_diff = new QPlainTextEdit(this);
    m_diff->setObjectName(QStringLiteral("gitDiff"));
    m_diff->setReadOnly(true);
    m_diff->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_diff->setVisible(false);
    appearance::markMono(m_diff);
    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setObjectName(QStringLiteral("gitDiffSplit"));
    m_split->addWidget(m_stack);
    m_split->addWidget(m_diff);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);
    m_split->setVisible(false);

    m_empty = new QLabel(tr("Open a folder to use git."), this);
    m_empty->setObjectName(QStringLiteral("gitEmpty"));
    m_empty->setWordWrap(true);

    m_message = new QPlainTextEdit(this);
    m_message->setObjectName(QStringLiteral("gitMessage"));
    m_message->setPlaceholderText(tr("Commit message"));
    m_message->setMaximumHeight(fontMetrics().height() * 6);

    m_commit = new QPushButton(tr("Commit"), this);
    m_commit->setObjectName(QStringLiteral("gitCommit"));
    m_commit->setToolTip(tr("Commit the checked files. Hooks run."));
    m_commit->setEnabled(false);

    m_branches = new QComboBox(this);
    m_branches->setObjectName(QStringLiteral("gitBranches"));
    m_branches->setEnabled(false);
    m_newBranch = new QPushButton(tr("New…"), this);
    m_newBranch->setObjectName(QStringLiteral("gitNewBranch"));
    m_newBranch->setToolTip(tr("Create a branch from the current HEAD"));
    m_newBranch->setEnabled(false);
    auto *branchRow = new QHBoxLayout;
    branchRow->addWidget(m_branches, 1);
    branchRow->addWidget(m_newBranch);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->addLayout(header);
    layout->addWidget(m_output);
    layout->addLayout(modeRow);
    layout->addWidget(m_empty);
    layout->addWidget(m_split, 1);
    layout->addWidget(m_message);
    layout->addWidget(m_commit);
    layout->addLayout(branchRow);

    connect(m_git, &GitService::statusReady, this, &GitPanel::showStatus);
    connect(m_git, &GitService::branchesReady, this, &GitPanel::showBranches);
    connect(m_git, &GitService::diffReady, this, &GitPanel::showDiff);
    connect(m_git, &GitService::logReady, this, &GitPanel::showLog);
    connect(m_git, &GitService::showReady, this, &GitPanel::showCommit);
    connect(m_git, &GitService::repositoryChanged, this, [this](bool inRepo, const QString &root) {
        m_inRepo = inRepo;
        m_pull->setEnabled(inRepo && !m_git->busy());
        m_push->setEnabled(inRepo && !m_git->busy());
        m_history->setEnabled(inRepo);
        if (!inRepo && m_historyMode)
            m_changes->setChecked(true);
        updateCommitEnabled();
        emit repositoryChanged(inRepo, root);
    });
    connect(m_git, &GitService::blameReady, this, &GitPanel::blameReady);
    connect(m_git, &GitService::busyChanged, this, [this](bool busy) {
        m_pull->setEnabled(m_inRepo && !busy);
        m_push->setEnabled(m_inRepo && !busy);
        updateCommitEnabled();
    });
    connect(m_git, &GitService::operationFinished, this, [this](const QString &action, bool ok,
                                                                const QString &output) {
        const QString label = action == QLatin1String("commit") ? tr("Commit")
            : action == QLatin1String("pull")                   ? tr("Pull")
            : action == QLatin1String("push")                   ? tr("Push")
            : action == QLatin1String("switch")                 ? tr("Switch")
                                                                 : tr("New branch");
        m_output->setPlainText((ok ? tr("%1 finished.") : tr("%1 failed.")).arg(label)
                               + QLatin1Char('\n') + output);
        if (ok && action == QLatin1String("commit"))
            m_message->clear();
        // A refused commit finishes without a status refresh, so put the
        // button back rather than leaving it disabled.
        updateCommitEnabled();
    });

    connect(m_message, &QPlainTextEdit::textChanged, this, &GitPanel::updateCommitEnabled);
    connect(m_files, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item) {
        if (!item || item->parent() == nullptr)
            return;
        const QString path = item->data(0, kPathRole).toString();
        const auto group = static_cast<GitChange>(item->data(0, kGroupRole).toInt());
        m_checked.insert(checkKey(group, path), item->checkState(0) == Qt::Checked);
        updateCommitEnabled();
    });
    connect(m_commit, &QPushButton::clicked, this, [this] {
        QStringList stage;
        QStringList unstage;
        QStringList paths;
        for (int g = 0; g < m_files->topLevelItemCount(); ++g) {
            QTreeWidgetItem *group = m_files->topLevelItem(g);
            for (int i = 0; i < group->childCount(); ++i) {
                QTreeWidgetItem *item = group->child(i);
                const QString path = item->data(0, kPathRole).toString();
                const auto kind = static_cast<GitChange>(item->data(0, kGroupRole).toInt());
                const bool checked = item->checkState(0) == Qt::Checked;
                if (checked && (kind == GitChange::Unstaged || kind == GitChange::Untracked)) {
                    if (!stage.contains(path))
                        stage.append(path);
                }
                if (!checked && kind == GitChange::Staged) {
                    if (!unstage.contains(path))
                        unstage.append(path);
                }
                if (checked && !paths.contains(path))
                    paths.append(path);
            }
        }
        m_commit->setEnabled(false);
        m_git->commit(stage, unstage, paths, m_message->toPlainText());
    });
    connect(m_pull, &QPushButton::clicked, m_git, &GitService::pull);
    connect(m_push, &QPushButton::clicked, m_git, &GitService::push);
    connect(m_branches, &QComboBox::currentTextChanged, this, [this](const QString &name) {
        if (name.isEmpty() || name == m_currentBranch || name == tr("Detached HEAD"))
            return;
        m_git->switchBranch(name);
    });
    connect(m_newBranch, &QPushButton::clicked, this, [this] {
        const QString name = askBranchName();
        if (!name.isEmpty())
            m_git->createBranch(name);
    });
    connect(m_files, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (m_historyMode)
            return;
        if (!item || item->parent() == nullptr) {
            m_diffPath.clear();
            m_diff->clear();
            m_diff->setVisible(false);
            return;
        }
        requestDiff(item);
    });
    connect(m_changes, &QPushButton::toggled, this, [this](bool on) {
        if (on)
            setHistoryMode(false);
    });
    connect(m_history, &QPushButton::toggled, this, [this](bool on) {
        if (on)
            setHistoryMode(true);
    });
    connect(m_log, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item, QListWidgetItem *) {
        if (!m_historyMode || !item)
            return;
        const QString hash = item->data(kHashRole).toString();
        if (hash.isEmpty()) {
            m_diff->clear();
            m_diff->setVisible(false);
            return;
        }
        m_showHash = hash;
        m_diff->setVisible(true);
        m_git->showCommit(hash);
    });
}

void GitPanel::setDirectory(const QString &directory)
{
    if (directory == m_directory)
        return;
    m_directory = directory;
    m_checked.clear();
    clearDiff();
    m_git->setDirectory(directory);
}

void GitPanel::refresh()
{
    m_git->refreshStatus();
}

bool GitPanel::inRepository() const
{
    return m_git->inRepository();
}

QString GitPanel::repositoryRoot() const
{
    return m_git->repositoryRoot();
}

void GitPanel::blame(const QString &file, int firstLine, int lastLine, const QByteArray &contents)
{
    m_git->blame(file, firstLine, lastLine, contents);
}

QString GitPanel::branchText(const GitStatus &status) const
{
    QString text = status.detached ? tr("Detached HEAD") : status.branch;
    QStringList notes;
    if (!status.upstream.isEmpty())
        notes << status.upstream;
    if (status.ahead > 0)
        notes << tr("ahead %1").arg(status.ahead);
    if (status.behind > 0)
        notes << tr("behind %1").arg(status.behind);
    if (!notes.isEmpty())
        text += QStringLiteral("  ") + notes.join(QStringLiteral(", "));
    return text;
}

void GitPanel::showStatus(const GitStatus &status)
{
    m_inRepo = status.ok;
    m_pull->setEnabled(status.ok && !m_git->busy());
    m_push->setEnabled(status.ok && !m_git->busy());

    if (!status.ok) {
        m_branch->setText(tr("Git"));
        m_currentBranch.clear();
        m_detached = false;
        m_files->clear();
        m_log->clear();
        m_branches->blockSignals(true);
        m_branches->clear();
        m_branches->blockSignals(false);
        m_history->setEnabled(false);
        if (m_historyMode)
            m_changes->setChecked(true);
        clearDiff();
        m_empty->setText(status.error.isEmpty() ? tr("Not a git repository.") : status.error);
        m_empty->setVisible(true);
        m_split->setVisible(false);
        updateCommitEnabled();
        return;
    }

    m_branch->setText(branchText(status));
    m_currentBranch = status.detached ? QString() : status.branch;
    m_detached = status.detached;
    selectCurrentBranch();

    // Rebuild without itemChanged, which would treat the new checks as edits.
    m_files->blockSignals(true);
    m_files->clear();

    auto group = [this](const QString &title) {
        auto *item = new QTreeWidgetItem(m_files, {title});
        item->setFlags(Qt::ItemIsEnabled);
        return item;
    };
    QTreeWidgetItem *conflicts = nullptr;
    QTreeWidgetItem *staged = nullptr;
    QTreeWidgetItem *unstaged = nullptr;
    QTreeWidgetItem *untracked = nullptr;
    QTreeWidgetItem *keep = nullptr;

    for (const GitChangeEntry &entry : status.entries) {
        QTreeWidgetItem *parent = nullptr;
        if (entry.conflicted) {
            if (!conflicts)
                conflicts = group(tr("Conflicts"));
            parent = conflicts;
        } else if (entry.group == GitChange::Staged) {
            if (!staged)
                staged = group(tr("Staged"));
            parent = staged;
        } else if (entry.group == GitChange::Untracked) {
            if (!untracked)
                untracked = group(tr("Untracked"));
            parent = untracked;
        } else {
            if (!unstaged)
                unstaged = group(tr("Changes"));
            parent = unstaged;
        }

        QString label = entry.path;
        if (!entry.from.isEmpty())
            label = tr("%1  (from %2)").arg(entry.path, entry.from);
        auto *row = new QTreeWidgetItem(parent, {label});
        // A conflict has no half to stage and no half to commit. The row
        // reports the state and can only be selected, to open the file.
        Qt::ItemFlags flags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (!entry.conflicted)
            flags |= Qt::ItemIsUserCheckable;
        row->setFlags(flags);
        row->setData(0, kPathRole, entry.path);
        row->setData(0, kGroupRole, int(entry.group));
        if (!entry.conflicted) {
            const QString key = checkKey(entry.group, entry.path);
            // Staged rows start checked: they are already the next commit. A
            // refresh remembers whatever the user changed that to.
            const bool checked = m_checked.contains(key) ? m_checked.value(key)
                                                         : entry.group == GitChange::Staged;
            row->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
            m_checked.insert(key, checked);
        }
        if (entry.path == m_diffPath && entry.group == m_diffGroup)
            keep = row;
    }

    m_files->expandAll();
    if (keep)
        m_files->setCurrentItem(keep);
    m_files->blockSignals(false);
    m_history->setEnabled(true);
    if (m_historyMode) {
        // The file list is hidden. Restoring its row must not replace the commit.
        m_empty->setVisible(false);
        m_split->setVisible(true);
        m_git->refreshLog();
    } else {
        if (keep)
            requestDiff(keep);
        else
            clearDiff();
        applyWorktreeVisibility(status.entries.isEmpty());
    }
    updateCommitEnabled();
}

void GitPanel::requestDiff(QTreeWidgetItem *item)
{
    if (!item || item->parent() == nullptr)
        return;
    m_diffPath = item->data(0, kPathRole).toString();
    m_diffGroup = static_cast<GitChange>(item->data(0, kGroupRole).toInt());
    if (m_diffPath.isEmpty())
        return;
    m_diff->setVisible(true);
    m_git->diff(m_diffPath, m_diffGroup);
}

void GitPanel::showDiff(const QString &path, int group, bool ok, const QString &text)
{
    if (m_historyMode || path != m_diffPath || group != int(m_diffGroup))
        return;
    if (!ok) {
        m_diff->setPlainText(text.isEmpty() ? tr("Diff failed.") : text);
        return;
    }
    m_diff->setPlainText(text.trimmed().isEmpty() ? tr("No differences.") : text);
}

void GitPanel::clearDiff()
{
    m_diffPath.clear();
    m_showHash.clear();
    m_diff->clear();
    m_diff->setVisible(false);
}

void GitPanel::applyWorktreeVisibility(bool clean)
{
    m_empty->setVisible(clean);
    m_split->setVisible(!clean);
    if (clean)
        m_empty->setText(tr("Working tree clean."));
}

void GitPanel::setHistoryMode(bool on)
{
    if (m_historyMode == on)
        return;
    m_historyMode = on;
    m_stack->setCurrentWidget(on ? static_cast<QWidget *>(m_log) : static_cast<QWidget *>(m_files));
    m_message->setVisible(!on);
    m_commit->setVisible(!on);
    m_diff->clear();
    m_diff->setVisible(false);
    if (on) {
        m_empty->setVisible(false);
        m_split->setVisible(m_inRepo);
        if (m_inRepo)
            m_git->refreshLog();
        return;
    }

    m_showHash.clear();
    if (!m_inRepo) {
        m_split->setVisible(false);
        m_empty->setVisible(true);
        return;
    }
    if (QTreeWidgetItem *item = m_files->currentItem())
        requestDiff(item);
    applyWorktreeVisibility(m_files->topLevelItemCount() == 0);
}

void GitPanel::showLog(const GitLog &entries, bool ok, const QString &error)
{
    if (!m_historyMode)
        return;

    const bool truncated = ok && entries.size() > kLogShown;
    const int count = truncated ? kLogShown : entries.size();

    m_log->blockSignals(true);
    m_log->clear();
    QListWidgetItem *keep = nullptr;
    for (int i = 0; i < count; ++i) {
        const GitLogEntry &entry = entries.at(i);
        QString label = entry.abbrev + QStringLiteral("  ") + entry.subject;
        if (!entry.refs.isEmpty())
            label += QStringLiteral("  (") + entry.refs + QLatin1Char(')');
        auto *item = new QListWidgetItem(label, m_log);
        item->setData(kHashRole, entry.hash);
        item->setToolTip(tr("%1\n%2\n%3").arg(entry.author, entry.date, entry.hash));
        if (entry.hash == m_showHash)
            keep = item;
    }
    if (count == 0) {
        auto *note = new QListWidgetItem(ok ? tr("No commits yet.")
                                             : (error.isEmpty() ? tr("Could not read history.") : error),
                                          m_log);
        note->setFlags(Qt::NoItemFlags);
    } else if (truncated) {
        auto *note = new QListWidgetItem(tr("Showing the latest %1 commits.").arg(kLogShown), m_log);
        note->setFlags(Qt::NoItemFlags);
    }
    m_log->blockSignals(false);

    if (keep) {
        m_log->setCurrentItem(keep);
    } else if (count > 0) {
        m_showHash.clear();
        m_log->setCurrentItem(m_log->item(0));
    } else {
        m_showHash.clear();
        m_diff->clear();
        m_diff->setVisible(false);
    }
}

void GitPanel::showCommit(const QString &hash, bool ok, const QString &text)
{
    if (!m_historyMode || hash != m_showHash)
        return;
    if (!ok) {
        m_diff->setPlainText(text.isEmpty() ? tr("Could not show this commit.") : text);
        return;
    }
    m_diff->setPlainText(text.trimmed().isEmpty() ? tr("Empty commit.") : text);
}

void GitPanel::showBranches(const QStringList &names)
{
    QStringList sorted;
    for (const QString &name : names) {
        const QString trimmed = name.trimmed();
        if (!trimmed.isEmpty() && !sorted.contains(trimmed))
            sorted.append(trimmed);
    }
    sorted.sort(Qt::CaseInsensitive);

    m_branches->blockSignals(true);
    m_branches->clear();
    m_branches->addItems(sorted);
    m_branches->blockSignals(false);
    selectCurrentBranch();
}

void GitPanel::selectCurrentBranch()
{
    const QString detached = tr("Detached HEAD");
    m_branches->blockSignals(true);
    const int phantom = m_branches->findText(detached);
    if (m_detached) {
        if (phantom < 0)
            m_branches->insertItem(0, detached);
        m_branches->setCurrentIndex(m_branches->findText(detached));
    } else {
        if (phantom >= 0)
            m_branches->removeItem(phantom);
        int index = m_branches->findText(m_currentBranch);
        if (index < 0 && !m_currentBranch.isEmpty()) {
            m_branches->addItem(m_currentBranch);
            index = m_branches->findText(m_currentBranch);
        }
        if (index >= 0)
            m_branches->setCurrentIndex(index);
    }
    m_branches->blockSignals(false);
}

QString GitPanel::askBranchName()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("New branch"));
    dialog.setObjectName(QStringLiteral("gitNewBranchDialog"));
    auto *edit = new QLineEdit(&dialog);
    edit->setObjectName(QStringLiteral("gitBranchName"));
    edit->setPlaceholderText(tr("Branch name"));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(false);
    connect(edit, &QLineEdit::textChanged, ok, [ok](const QString &text) {
        const QString name = text.trimmed();
        bool space = false;
        for (const QChar ch : name)
            space = space || ch.isSpace();
        ok->setEnabled(!name.isEmpty() && !space);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Branch name"), &dialog));
    layout->addWidget(edit);
    layout->addWidget(buttons);
    edit->setFocus();

    if (dialog.exec() != QDialog::Accepted)
        return {};
    return edit->text().trimmed();
}

void GitPanel::updateCommitEnabled()
{
    bool any = false;
    for (int g = 0; g < m_files->topLevelItemCount() && !any; ++g) {
        QTreeWidgetItem *group = m_files->topLevelItem(g);
        for (int i = 0; i < group->childCount(); ++i) {
            if (group->child(i)->checkState(0) == Qt::Checked)
                any = true;
        }
    }
    const bool ready = m_inRepo && !m_git->busy();
    m_branches->setEnabled(ready);
    m_newBranch->setEnabled(ready);
    const bool message = !m_message->toPlainText().trimmed().isEmpty();
    m_commit->setEnabled(ready && any && message);
}

} // namespace pist
