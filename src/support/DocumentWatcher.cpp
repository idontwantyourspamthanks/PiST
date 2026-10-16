// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "support/DocumentWatcher.h"

#include <QFileInfo>
#include <QFileSystemWatcher>

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
        if (QFileInfo::exists(path)) {
            // Still there: this is a replace — our own save's staged rename, or
            // another writer's. Qt may have dropped the watch with the inode it
            // pointed at, so put it back and stay quiet: nothing happened that a
            // document has to be told about.
            if (!m_watcher->files().contains(path))
                m_watcher->addPath(path);
            return;
        }
        // Gone. Qt has already dropped its own watch; report it, and let the
        // owner decide whether the path is watched again (a recreated file
        // after a save does, a closed document does not).
        m_watcher->removePath(path);
        emit deleted(path);
    });
}

void DocumentWatcher::addPath(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString absolute = normalised(path);
    if (!m_watcher->files().contains(absolute))
        m_watcher->addPath(absolute);
}

void DocumentWatcher::forget(const QString &path)
{
    if (path.isEmpty())
        return;
    m_watcher->removePath(normalised(path));
}

void DocumentWatcher::rearm(const QString &path)
{
    if (path.isEmpty())
        return;
    const QString absolute = normalised(path);
    // removePaths first is not a formality: a watch that survived the rename
    // still points at the old inode, and addPath() on a path Qt already lists
    // would leave it exactly as it is.
    m_watcher->removePath(absolute);
    m_watcher->addPath(absolute);
}

void DocumentWatcher::setPaths(const QStringList &paths)
{
    QStringList wanted;
    for (const QString &path : paths) {
        if (path.isEmpty())
            continue;
        wanted.append(normalised(path));
    }

    const QStringList watched = m_watcher->files();
    for (const QString &path : watched) {
        if (!wanted.contains(path))
            m_watcher->removePath(path);
    }
    for (const QString &path : wanted) {
        if (!watched.contains(path))
            m_watcher->addPath(path);
    }
}

QStringList DocumentWatcher::paths() const
{
    QStringList paths = m_watcher->files();
    paths.sort();
    return paths;
}

} // namespace pist
