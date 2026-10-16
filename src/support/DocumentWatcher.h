// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class QFileSystemWatcher;

namespace pist {

/// Watches the files the open documents are bound to and reports the one event
/// that matters: the backing file going away (docs/untitled-plan.md §7.1).
///
/// Deliberately small and widget-free, because the rules around Qt's watcher
/// are the kind that are easy to get wrong and impossible to see. The one the
/// design turns on: `files::write()` replaces a file by staging a temporary and
/// renaming it over the target, which leaves a watch pointing at an inode nobody
/// holds any more — and whether Qt notices, drops the path, or keeps watching it
/// is not something Qt's documentation promises either way. Measured on Qt 6.10
/// over Linux (`tests/tst_watcher.cpp`, which pins the behaviour this class
/// needs): the watch *survives* the staged rename and no change signal is
/// emitted, so `rearm()` is defensive rather than load-bearing on that pair. It
/// is called after our own writes all the same, because that is Qt's business
/// and not this class's to assume.
///
/// Both halves of the strength live here: rearm() after our own writes, and a
/// path that does report a change put back on the list immediately, so one event
/// cannot cost the watch.
class DocumentWatcher : public QObject
{
    Q_OBJECT

public:
    explicit DocumentWatcher(QObject *parent = nullptr);

    /// Watch `path`. Empty paths are ignored: every document has a path, and a
    /// path-less one is not a document (docs/untitled-plan.md §2).
    void addPath(const QString &path);

    /// Stop watching `path` — the document closed, or its file was deleted
    /// deliberately and a watcher event would only repeat what the gesture
    /// already said.
    void forget(const QString &path);

    /// Re-arm after our own write to `path`. The staged rename in
    /// `files::write()` may have dropped the watch, so the path is re-added
    /// rather than assumed good.
    void rearm(const QString &path);

    /// Watch exactly `paths` — the open documents' paths — dropping the rest.
    /// Idempotent, so the owner can call it whenever the set of open documents
    /// changes instead of tracking the difference itself.
    void setPaths(const QStringList &paths);

    /// The paths currently watched, sorted.
    QStringList paths() const;

signals:
    /// `path` does not exist any more: a real deletion, or a rename seen from
    /// this side (which for the document bound to it is the same event).
    void deleted(const QString &path);

private:
    QFileSystemWatcher *m_watcher = nullptr;
};

} // namespace pist
