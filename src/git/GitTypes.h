// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QMetaType>
#include <QString>
#include <QVector>

namespace pist {

/// One row of `git status --porcelain=v1 -z`. A path that is both staged and
/// modified in the worktree is two rows, so a checkbox can include one half
/// without the other.
enum class GitChange {
    Staged,
    Unstaged,
    Untracked,
};

struct GitChangeEntry {
    GitChange group = GitChange::Unstaged;
    QString path;
    /// Previous path when this row is a rename or a copy. Empty otherwise.
    QString from;
};

struct GitStatus {
    bool ok = false;
    QString error;
    QString branch;
    QString upstream;
    int ahead = 0;
    int behind = 0;
    bool detached = false;
    QVector<GitChangeEntry> entries;
};

/// One blamed line. `uncommitted` is a line git could not attribute — a buffer
/// that differs from HEAD, including a line typed since the file was saved.
struct GitBlameLine {
    QString hash;
    QString author;
    QString summary;
    QString date;
    bool uncommitted = false;
};

/// `git status --porcelain=v1 -z -b`. NUL-separated, so paths keep their
/// spaces. Rename records are `to\0from\0` (the `-z` order, not the `->` order).
GitStatus parsePorcelain(const QByteArray &bytes);

using GitBlameMap = QHash<int, GitBlameLine>;

/// `git blame -p` for a line range. A zero hash is an uncommitted line.
GitBlameMap parseBlame(const QByteArray &bytes);

/// One `git log` record. `refs` is `%D` (empty when the commit has no decoration).
struct GitLogEntry {
    QString hash;
    QString abbrev;
    QString author;
    QString date;
    QString refs;
    QString subject;
};

using GitLog = QVector<GitLogEntry>;

/// `git log --format=%H%x00%h%x00%an%x00%ad%x00%D%x00%s%x00`. Git writes a
/// newline after each record's trailing NUL; fields are trimmed.
GitLog parseLog(const QByteArray &bytes);

} // namespace pist

Q_DECLARE_METATYPE(pist::GitStatus)
Q_DECLARE_METATYPE(pist::GitBlameLine)
Q_DECLARE_METATYPE(pist::GitBlameMap)
Q_DECLARE_METATYPE(pist::GitLogEntry)
Q_DECLARE_METATYPE(pist::GitLog)
