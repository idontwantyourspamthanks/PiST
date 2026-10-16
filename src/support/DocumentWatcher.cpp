// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "support/DocumentWatcher.h"

#include <QFileInfo>
#include <QFileSystemWatcher>

#include <utility>

namespace pist {

namespace {

/// The form every path is stored and compared in: absolute, so a document
/// opened through a relative path and the same file named absolutely are one
/// watch, not two.
QString normalised(const QString &path)
{
    return QFileInfo(path).absoluteFilePath();
}

} // namespace

DocumentWatcher::DocumentWatcher(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) {
        // Qt can report a path the owner has stopped caring about — see
        // m_watched — so those are dropped here, which is what makes forget()
        // and setPaths() mean what they say on every platform.
        if (!m_watched.contains(path))
            return;

        if (QFileInfo::exists(path)) {
            // Still there: a replace — our own save's staged rename, or another
            // writer's. Qt may have dropped the watch with the inode it pointed
            // at, so put it back and stay quiet: nothing happened that a document
            // has to be told about. A file that came back is live again, so a
            // later deletion is news.
            m_reportedGone.remove(path);
            if (!m_watcher->files().contains(path))
                m_watcher->addPath(path);
            return;
        }
        // Gone. Qt has already dropped its own watch; report it once — Windows
        // raises the same deletion twice — and let the owner decide whether the
        // path is watched again (a recreated file after a save does, a closed
        // document does not).
        m_watcher->removePath(path);
        if (m_reportedGone.contains(path))
            return;
        m_reportedGone.insert(path);
        emit deleted(path);
    });
}

void DocumentWatcher::addPath(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString absolute = normalised(path);
    m_watched.insert(absolute);
    m_reportedGone.remove(absolute);
    if (!m_watcher->files().contains(absolute))
        m_watcher->addPath(absolute);
}

void DocumentWatcher::forget(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString absolute = normalised(path);
    m_watched.remove(absolute);
    m_reportedGone.remove(absolute);
    m_watcher->removePath(absolute);
}

void DocumentWatcher::rearm(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString absolute = normalised(path);
    // The owner is saying this path is live: watch it again, by remove-then-add,
    // because a watch that survived the rename still points at the old inode and
    // addPath() alone would leave it exactly as it is.
    m_watched.insert(absolute);
    m_reportedGone.remove(absolute);
    m_watcher->removePath(absolute);
    m_watcher->addPath(absolute);
}

void DocumentWatcher::setPaths(const QStringList &paths)
{
    QSet<QString> wanted;
    for (const QString &path : paths) {
        if (!path.isEmpty())
            wanted.insert(normalised(path));
    }

    const QSet<QString> previous = std::move(m_watched);
    m_watched = wanted;
    for (const QString &path : previous) {
        if (!wanted.contains(path))
            m_watcher->removePath(path);
    }
    const QStringList watched = m_watcher->files();
    for (const QString &path : wanted) {
        if (!watched.contains(path))
            m_watcher->addPath(path);
    }
    // A path the owner has dropped is not reported on again, whether or not it
    // was already reported gone.
    m_reportedGone.intersect(wanted);
}

QStringList DocumentWatcher::paths() const
{
    QStringList paths(m_watched.cbegin(), m_watched.cend());
    paths.sort();
    return paths;
}

} // namespace pist
