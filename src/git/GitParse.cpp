// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "git/GitTypes.h"

#include <QDateTime>

namespace pist {

namespace {

bool isRename(QChar index, QChar worktree)
{
    return index == QLatin1Char('R') || index == QLatin1Char('C')
        || worktree == QLatin1Char('R') || worktree == QLatin1Char('C');
}

void parseBranch(const QString &header, GitStatus &status)
{
    QString line = header;
    QString tracking;
    const int bracket = line.lastIndexOf(QStringLiteral(" ["));
    if (bracket > 0 && line.endsWith(QLatin1Char(']'))) {
        tracking = line.mid(bracket + 2, line.size() - bracket - 3);
        line.truncate(bracket);
    }

    if (line.startsWith(QStringLiteral("HEAD (no branch)"))) {
        status.detached = true;
        status.branch = QStringLiteral("HEAD");
    } else if (line.startsWith(QStringLiteral("No commits yet on "))) {
        status.branch = line.mid(QStringLiteral("No commits yet on ").size());
    } else {
        const int dots = line.indexOf(QStringLiteral("..."));
        if (dots >= 0) {
            status.branch = line.left(dots);
            status.upstream = line.mid(dots + 3);
        } else {
            status.branch = line;
        }
    }

    const int aheadAt = tracking.indexOf(QStringLiteral("ahead "));
    if (aheadAt >= 0)
        status.ahead = tracking.mid(aheadAt + 6).section(QLatin1Char(','), 0, 0).toInt();
    const int behindAt = tracking.indexOf(QStringLiteral("behind "));
    if (behindAt >= 0)
        status.behind = tracking.mid(behindAt + 7).section(QLatin1Char(','), 0, 0).toInt();
}

bool isCommitHash(const QByteArray &token)
{
    if (token.size() != 40)
        return false;
    for (char c : token) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
            || (c >= 'A' && c <= 'F');
        if (!hex)
            return false;
    }
    return true;
}

} // namespace

GitStatus parsePorcelain(const QByteArray &bytes)
{
    GitStatus status;
    if (bytes.isEmpty()) {
        status.error = QStringLiteral("git status returned nothing");
        return status;
    }

    int i = 0;
    bool sawHeader = false;
    while (i < bytes.size()) {
        int end = bytes.indexOf('\0', i);
        if (end < 0)
            end = bytes.size();
        const QByteArray record = bytes.mid(i, end - i);
        i = end + 1;
        if (record.isEmpty())
            continue;

        if (record.startsWith("##")) {
            parseBranch(QString::fromUtf8(record.mid(3)), status);
            sawHeader = true;
            continue;
        }
        // "XY path" — the two status letters, a space, then the path.
        if (record.size() < 4 || record.at(2) != ' ')
            continue;

        const QChar index = QChar(record.at(0));
        const QChar worktree = QChar(record.at(1));
        // The record's path is the current name. `-z` puts a rename's old
        // name in the following field (`to\0from\0`), the reverse of `from -> to`.
        QString path = QString::fromUtf8(record.mid(3));
        QString from;
        if (isRename(index, worktree) && i < bytes.size()) {
            int fromEnd = bytes.indexOf('\0', i);
            if (fromEnd < 0)
                fromEnd = bytes.size();
            from = QString::fromUtf8(bytes.mid(i, fromEnd - i));
            i = fromEnd + 1;
        }

        if (index == QLatin1Char('?') && worktree == QLatin1Char('?')) {
            status.entries.append({GitChange::Untracked, path, {}});
            continue;
        }
        if (index != QLatin1Char(' ') && index != QLatin1Char('?'))
            status.entries.append({GitChange::Staged, path, from});
        if (worktree != QLatin1Char(' ') && worktree != QLatin1Char('?'))
            status.entries.append({GitChange::Unstaged, path, {}});
    }

    status.ok = sawHeader;
    if (!sawHeader)
        status.error = QStringLiteral("git status returned nothing");
    return status;
}

GitBlameMap parseBlame(const QByteArray &bytes)
{
    QHash<QString, GitBlameLine> commits;
    GitBlameMap lines;
    QString hash;
    int finalLine = 0;

    const QList<QByteArray> rows = bytes.split('\n');
    for (QByteArray row : rows) {
        if (row.endsWith('\r'))
            row.chop(1);
        if (row.isEmpty())
            continue;
        if (row.startsWith('\t')) {
            if (finalLine <= 0 || hash.isEmpty())
                continue;
            GitBlameLine info = commits.value(hash);
            info.hash = hash;
            info.uncommitted = hash == QString(40, QLatin1Char('0'));
            lines.insert(finalLine, info);
            continue;
        }

        const int space = row.indexOf(' ');
        if (space == 40 && isCommitHash(row.left(40))) {
            const QList<QByteArray> parts = row.split(' ');
            if (parts.size() >= 3) {
                hash = QString::fromLatin1(parts.at(0));
                finalLine = parts.at(2).toInt();
                if (!commits.contains(hash))
                    commits.insert(hash, GitBlameLine{});
            }
            continue;
        }

        if (hash.isEmpty())
            continue;
        auto &info = commits[hash];
        if (row.startsWith("author "))
            info.author = QString::fromUtf8(row.mid(7));
        else if (row.startsWith("author-time ")) {
            const qint64 secs = row.mid(12).toLongLong();
            info.date = QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        } else if (row.startsWith("summary "))
            info.summary = QString::fromUtf8(row.mid(8));
    }
    return lines;
}

GitLog parseLog(const QByteArray &bytes)
{
    GitLog log;
    const QList<QByteArray> fields = bytes.split('\0');
    for (int i = 0; i + 5 < fields.size(); i += 6) {
        GitLogEntry entry;
        entry.hash = QString::fromUtf8(fields.at(i)).trimmed();
        if (entry.hash.isEmpty())
            continue;
        entry.abbrev = QString::fromUtf8(fields.at(i + 1)).trimmed();
        entry.author = QString::fromUtf8(fields.at(i + 2)).trimmed();
        entry.date = QString::fromUtf8(fields.at(i + 3)).trimmed();
        entry.refs = QString::fromUtf8(fields.at(i + 4)).trimmed();
        entry.subject = QString::fromUtf8(fields.at(i + 5)).trimmed();
        log.append(entry);
    }
    return log;
}

} // namespace pist
