// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// The document watcher's rules around Qt's own file watcher: a real deletion, a
// rename seen from the document's side, forget()/setPaths() dropping a path, and
// the re-arm after our own staged write (`files::write()` replaces the target by
// renaming a temporary over it). Real temporary files; no display, no emulator.
//
// Measured on Qt 6.10 over Linux (2026-10-16): the staged rename does not by
// itself drop the watch — no signal fires and the path stays watched — so the
// re-arm case is defensive rather than demonstrably load-bearing on that pair.
// What it pins is the behaviour the document model needs: a real deletion *after*
// our own save is still reported (docs/untitled-plan.md §7.1, §11).

#include "support/DocumentWatcher.h"
#include "support/FileWrite.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

namespace {

/// A bounded wait for a watcher signal. File-watcher delivery is asynchronous:
/// an inotify event reaches a socket and is dispatched on a later event-loop
/// pass, so asserting immediately races the kernel, and waiting forever would
/// hang the suite instead of failing it. QTRY_* pumps the event loop until the
/// condition holds or this bound expires — a missing signal fails the test, it
/// does not stall it.
constexpr int kWatchTimeoutMs = 5000;

/// Write `path` with `bytes`, the plain way: the setup for the cases that are
/// not themselves about the write. Case 3 uses files::write() on purpose.
void writeFile(const QString &path, const QByteArray &bytes = QByteArrayLiteral("x"))
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();
}

} // namespace

class TstWatcher : public QObject
{
    Q_OBJECT

private slots:
    void reportsADeletedFile();
    void reportsARenameAsADeletion();
    void rearmAfterOurOwnWriteKeepsTheWatchAlive();
    void forgetStopsTheReports();
    void setPathsWatchesExactlyTheGivenDocuments();
    void ignoresEmptyPaths();
};

// A watched file removed from disk is the event the whole class exists for.
void TstWatcher::reportsADeletedFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.s"));
    writeFile(path);

    DocumentWatcher watcher;
    QSignalSpy spy(&watcher, &DocumentWatcher::deleted);

    watcher.addPath(path);
    // Stored absolute: the temp dir already hands us an absolute path, which is
    // the form the signal is compared in (and why the test uses real files).
    QCOMPARE(watcher.paths(), QStringList{path});

    QVERIFY(QFile::remove(path));

    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, kWatchTimeoutMs);
    QCOMPARE(spy.first().first().toString(), path);
}

// A rename on disk is, from the bound document's point of view, the same event
// as a deletion: the path it holds no longer exists.
void TstWatcher::reportsARenameAsADeletion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.s"));
    const QString renamed = dir.filePath(QStringLiteral("renamed.s"));
    writeFile(path);

    DocumentWatcher watcher;
    QSignalSpy spy(&watcher, &DocumentWatcher::deleted);
    watcher.addPath(path);

    // The external move the IDE never saw.
    QVERIFY(QFile::rename(path, renamed));

    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, kWatchTimeoutMs);
    QCOMPARE(spy.first().first().toString(), path);
}

// Our own save runs through files::write(), which replaces the target by renaming
// a staged temporary over it — the operation a file watcher may or may not survive
// (measured: it survives here). The re-arm is what the save path does; what must
// hold is that a real deletion after it is still reported
// (docs/untitled-plan.md §7.1, §11).
void TstWatcher::rearmAfterOurOwnWriteKeepsTheWatchAlive()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.s"));
    writeFile(path, QByteArrayLiteral("before"));

    DocumentWatcher watcher;
    QSignalSpy spy(&watcher, &DocumentWatcher::deleted);
    watcher.addPath(path);

    // Our own save, exactly as the editor performs it.
    QString error;
    QVERIFY2(files::write(path, QByteArrayLiteral("after"), &error), qPrintable(error));

    // Re-arm after the write, as the save path would.
    watcher.rearm(path);
    QCOMPARE(watcher.paths(), QStringList{path});

    // The watch must have followed onto the replacement: the deletion is the
    // event the document needs, and losing it is the bug rearm() exists for.
    QVERIFY(QFile::remove(path));

    QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), kWatchTimeoutMs);
    QCOMPARE(spy.first().first().toString(), path);
}

// forget() is the closed-document / deliberate-deletion case: the watch goes
// and the report must not come.
void TstWatcher::forgetStopsTheReports()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString watched = dir.filePath(QStringLiteral("kept.s"));
    const QString forgotten = dir.filePath(QStringLiteral("forgotten.s"));
    writeFile(watched);
    writeFile(forgotten);

    DocumentWatcher watcher;
    QSignalSpy spy(&watcher, &DocumentWatcher::deleted);
    watcher.addPath(forgotten);
    watcher.addPath(watched);
    watcher.forget(forgotten);
    QCOMPARE(watcher.paths(), QStringList{watched});

    // Remove both. `watched` is a control: its report proves the watcher and the
    // event loop delivered everything that was going to arrive, so asserting
    // "exactly one" is deterministic rather than a guess at a sleep length. The
    // forgotten path is dropped from the watcher's own set as well as from Qt's
    // list, because on Windows Qt still raises its directory event for a removed
    // path while another file in that directory stays watched — filtering the
    // report is what makes "nothing is heard for it" hold on every platform.
    QVERIFY(QFile::remove(forgotten));
    QVERIFY(QFile::remove(watched));

    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, kWatchTimeoutMs);
    QCOMPARE(spy.first().first().toString(), watched);
}

// setPaths() is called whenever the set of open documents changes; it must leave
// exactly that set watched, dropping a closed document's path.
void TstWatcher::setPathsWatchesExactlyTheGivenDocuments()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString kept = dir.filePath(QStringLiteral("kept.s"));
    const QString dropped = dir.filePath(QStringLiteral("dropped.s"));
    writeFile(kept);
    writeFile(dropped);

    DocumentWatcher watcher;
    watcher.addPath(kept);
    watcher.addPath(dropped);
    // paths() is sorted, not insertion-ordered.
    QCOMPARE(watcher.paths(), QStringList({dropped, kept}));

    // One document closed: only the remaining one is watched.
    watcher.setPaths({kept});
    QCOMPARE(watcher.paths(), QStringList{kept});

    QSignalSpy spy(&watcher, &DocumentWatcher::deleted);

    // The dropped path is no longer watched — removing it must stay silent — and
    // the kept one is the control that fixes the bound (see forgetStopsTheReports).
    QVERIFY(QFile::remove(dropped));
    QVERIFY(QFile::remove(kept));

    QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, kWatchTimeoutMs);
    QCOMPARE(spy.first().first().toString(), kept);
}

// A path-less document does not exist (docs/untitled-plan.md §2), so an empty
// path is not a watch and changes nothing.
void TstWatcher::ignoresEmptyPaths()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("doc.s"));
    writeFile(path);

    DocumentWatcher watcher;
    watcher.addPath(QString());
    QVERIFY(watcher.paths().isEmpty());

    watcher.addPath(path);
    QCOMPARE(watcher.paths(), QStringList{path});

    watcher.addPath(QString());
    QCOMPARE(watcher.paths(), QStringList{path});
}

QTEST_MAIN(TstWatcher)
#include "tst_watcher.moc"
