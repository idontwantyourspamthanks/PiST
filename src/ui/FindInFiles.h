// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "editor/FindScan.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class QTimer;

namespace pist {

/// One hit, as the results tree needs it.
struct FindInFilesHit
{
    /// 1-based, counted the way the editor and the Problems pane count.
    int line = 0;
    /// Where the match starts within its line, and how long it is: what
    /// selecting it in the editor and painting it in the results both need.
    int column = 0;
    int length = 0;
    /// The line's own text, trimmed, for the results row.
    QString preview;
};

/// What a walk did, for the header above the results.
///
/// The sheet asks for the counts to be *stated* rather than implied — what was
/// searched, what was skipped — in the same honestly-capped style as the
/// editor's "N of more than 2000".
struct FindInFilesSummary
{
    /// Files whose text was read and scanned.
    int files = 0;
    /// Files deliberately not read: a binary, image, object or ROM suffix; a
    /// hidden directory; or one too large to scan for a substring.
    int skipped = 0;
    /// Hits found. A floor once `capped` is set.
    int hits = 0;
    /// The hit cap stopped the walk, so `hits` is a floor, `files` is not the
    /// whole tree, and more hits may exist.
    bool capped = false;
};

/// Whether Find in Files reads `path` as text to search.
///
/// This is the feature's own question — "would a user search inside this?" — and
/// it is deliberately *not* `MainWindow`'s `suffixIsKnownBinary`, which answers a
/// different one ("is this a text document to open out of a disk image?" and so
/// counts `.s` and `.asm` as refusals, since assembly builds from the hard
/// drive). A source file is exactly what this searches for, and `.rom` and `.lst`
/// are here because a ROM image and a vasm listing are not source.
bool isSearchableFile(const QString &path);

/// Walks a directory tree looking for a needle, a chunk at a time.
///
/// The walk is chunked on this thread rather than run on a worker: nothing else
/// in the tree walks a directory off the GUI thread (the one `QThread` is a modal
/// setup dialog, and the docks that do slow work drive a subprocess), a budgeted
/// tick keeps the window responsive without a new module edge or a second way for
/// results to arrive, and cancelling therefore cannot race with the results —
/// which is the property that makes "a re-run while typing a new query" simple
/// enough to be obviously right.
class FindInFiles : public QObject
{
    Q_OBJECT

public:
    explicit FindInFiles(QObject *parent = nullptr);
    ~FindInFiles() override;

    /// Start a walk over `root`, cancelling whatever was running — a summary for
    /// the cancelled walk is deliberately not emitted, since only the caller that
    /// replaced it could act on one. Every start() is answered by exactly one
    /// `finished`, immediately when there is nothing to walk (an empty query is
    /// how the dock clears its results).
    void start(const QString &root, const QString &needle, const FindOptions &options);
    /// Stop the walk; no `finished` follows.
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    /// One file's hits, as soon as that file has been scanned.
    void hitsFound(const QString &path, const QList<FindInFilesHit> &hits);
    /// The walk ended, with what it managed.
    void finished(const FindInFilesSummary &summary);

private:
    void step();
    void end();

    QTimer *m_tick = nullptr;
    QString m_needle;
    FindOptions m_options;
    /// The files to scan, in the order the tree gave them (directories sorted,
    /// hidden ones left out), and how far the walk has got.
    QStringList m_files;
    int m_next = 0;
    FindInFilesSummary m_summary;
    bool m_running = false;
};

} // namespace pist
