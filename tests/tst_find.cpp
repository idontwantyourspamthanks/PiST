// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// Find in Files' engine and its walk, with no window and no emulator: the
// counting rules the editor's find bar shares with it, what the walk reads and
// what it refuses to, and what it reports about both.

#include "editor/FindScan.h"
#include "ui/FindInFiles.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>

using namespace pist;

namespace {

bool writeFixture(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(bytes) == bytes.size();
}

/// A walk driven to completion, with what it found and what it said.
struct WalkResult
{
    /// One entry per file the walk reported hits for, in the order it reported
    /// them: the path and that file's rows.
    QList<QPair<QString, QList<FindInFilesHit>>> files;
    FindInFilesSummary summary;
    int finishes = 0;

    QList<FindInFilesHit> hits() const
    {
        QList<FindInFilesHit> all;
        for (const auto &file : files)
            all += file.second;
        return all;
    }
};

/// Run one query to the end, waiting for the event loop the walk runs on.
bool runWalk(FindInFiles *walk, const QString &root, const QString &needle,
             const FindOptions &options, WalkResult *out, int timeoutMs = 15000)
{
    QObject::connect(walk, &FindInFiles::hitsFound, walk,
                     [out](const QString &path, const QList<FindInFilesHit> &hits) {
                         out->files.append({path, hits});
                     });
    QObject::connect(walk, &FindInFiles::finished, walk,
                     [out](const FindInFilesSummary &summary) {
                         out->summary = summary;
                         ++out->finishes;
                     });
    walk->start(root, needle, options);
    return QTest::qWaitFor([out] { return out->finishes > 0; }, timeoutMs);
}

} // namespace

class TstFind : public QObject
{
    Q_OBJECT

private slots:
    // --- the scan the editor's find bar also uses -------------------------

    /// The two options, and the default that a find bar starts from.
    void theScanCountsWhatTheFindBarCounts();
    /// A capped count is a floor, and says so — the "more than N" rule.
    void theCapIsAFloorRatherThanATotal();

    // --- the walk ---------------------------------------------------------

    /// What a hit is: the file, the line, the match's own span, and the line's
    /// text, in the order the paths read in.
    void theWalkReportsHitsWithTheirSpans();
    /// What it refuses to read, and counts as skipped rather than silently.
    void theWalkSkipsWhatIsNotTextToSearch();
    /// The cap stops the walk and the summary admits it.
    void theWalkStopsWhenTheCapIsReached();
    /// An empty query is answered at once; a cancellation is answered with
    /// nothing at all.
    void anEmptyQueryFinishesAtOnceAndACancelIsSilent();
    /// A walk replaced by a newer query never reports: its summary would describe
    /// results that are already off the screen.
    void aReplacedWalkReportsNothing();
};

void TstFind::theScanCountsWhatTheFindBarCounts()
{
    QTextDocument document;
    document.setPlainText(
        QStringLiteral("move.w d0,d1\nmovem.l d2-d7,-(sp)\nmoveq #0,d0\nMOVE d0,d1\n"));

    // Case-insensitive by default, which is where a find bar starts.
    QCOMPARE(findScan(&document, QStringLiteral("move"), FindOptions{}, 0, nullptr).size(), 4);

    // Whole words only: not the "move" inside "movem" or "moveq", but "move.w"
    // and a shouted, bare "MOVE" are words.
    QCOMPARE(findScan(&document, QStringLiteral("move"), FindOptions{false, true}, 0, nullptr)
                 .size(),
             2);

    // Case sensitivity, on the other hand, drops the shouted one.
    QCOMPARE(findScan(&document, QStringLiteral("move"), FindOptions{true, true}, 0, nullptr)
                 .size(),
             1);

    // Hits come back in document order, which is the order the tour walks.
    const QList<QTextCursor> hits = findScan(&document, QStringLiteral("d0"), FindOptions{}, 0,
                                             nullptr);
    QCOMPARE(hits.size(), 3);
    QVERIFY(hits.first().selectionStart() < hits.last().selectionStart());

    // No needle is no hits, and no document is not a crash.
    QVERIFY(findScan(&document, QString(), FindOptions{}, 0, nullptr).isEmpty());
    QVERIFY(findScan(nullptr, QStringLiteral("move"), FindOptions{}, 0, nullptr).isEmpty());
}

void TstFind::theCapIsAFloorRatherThanATotal()
{
    QString text;
    for (int i = 0; i < 12; ++i)
        text += QStringLiteral("needle %1\n").arg(i);
    QTextDocument document;
    document.setPlainText(text);

    bool capped = false;
    QCOMPARE(findScan(&document, QStringLiteral("needle"), FindOptions{}, 5, &capped).size(), 5);
    QVERIFY2(capped, "a hit past the limit is what makes the count a floor");

    // Exactly the lot: nothing was left out, so the count is the total.
    capped = false;
    QCOMPARE(findScan(&document, QStringLiteral("needle"), FindOptions{}, 12, &capped).size(), 12);
    QVERIFY(!capped);

    // No limit at all keeps everything, which is what Replace All relies on.
    QCOMPARE(findScan(&document, QStringLiteral("needle"), FindOptions{}, 0, nullptr).size(), 12);
}

void TstFind::theWalkReportsHitsWithTheirSpans()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("main.s")),
                         "move.w d0,d1\n    moveq #0,d0\n"));
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("sub/util.s")), "  move.w  d2,d3\n"));

    FindInFiles walk;
    WalkResult result;
    QVERIFY2(runWalk(&walk, dir.path(), QStringLiteral("move"), FindOptions{}, &result),
             "the walk did not finish");

    QCOMPARE(result.finishes, 1);
    QCOMPARE(result.files.size(), 2);
    // The paths read in order, so the first row a user sees is the first path.
    QCOMPARE(result.files.at(0).first, dir.filePath(QStringLiteral("main.s")));
    QCOMPARE(result.files.at(1).first, dir.filePath(QStringLiteral("sub/util.s")));

    const QList<FindInFilesHit> first = result.files.at(0).second;
    QCOMPARE(first.size(), 2);
    QCOMPARE(first.at(0).line, 1);
    QCOMPARE(first.at(0).column, 0);
    QCOMPARE(first.at(0).length, 4); // "move", not the whole word "move.w"
    QCOMPARE(first.at(0).preview, QStringLiteral("move.w d0,d1"));
    QCOMPARE(first.at(1).line, 2);
    QCOMPARE(first.at(1).column, 4); // after the indentation, which the preview keeps
    QCOMPARE(first.at(1).preview, QStringLiteral("    moveq #0,d0"));

    QCOMPARE(result.summary.hits, 3);
    QCOMPARE(result.summary.files, 2);
    QCOMPARE(result.summary.skipped, 0);
    QVERIFY(!result.summary.capped);
}

void TstFind::theWalkSkipsWhatIsNotTextToSearch()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("code.s")), "needle\n"));
    // Binaries and images by suffix, whether or not their bytes happen to be text.
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("sprite.pim")), "needle\n"));
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("build.lst")), "needle\n"));
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("roms/tos.img")), "needle\n"));
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("program.prg")), "needle\n"));
    // An unknown suffix is read, but a NUL byte settles it whatever its name says.
    QByteArray binary("a");
    binary.append('\0');
    binary.append("needle");
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("odd.dat")), binary));
    // A file too large to be source anyone is reading inside.
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("huge.s")), QByteArray(5 * 1024 * 1024, 'x')));
    // Hidden directories are not the project's own text.
    QVERIFY(writeFixture(dir.filePath(QStringLiteral(".git/config")), "needle\n"));

    FindInFiles walk;
    WalkResult result;
    QVERIFY(runWalk(&walk, dir.path(), QStringLiteral("needle"), FindOptions{}, &result));

    QCOMPARE(result.files.size(), 1);
    QCOMPARE(result.files.at(0).first, dir.filePath(QStringLiteral("code.s")));
    QCOMPARE(result.summary.files, 1);
    QCOMPARE(result.summary.hits, 1);
    // Every file the walk refused, counted: the header has to be able to say so.
    QCOMPARE(result.summary.skipped, 6);
}

void TstFind::theWalkStopsWhenTheCapIsReached()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Six files of nine hundred hits each: well past the cap, which the walk
    // must stop at and admit, rather than walk the rest of the tree and report a
    // total it never reached.
    QByteArray body;
    for (int i = 0; i < 900; ++i)
        body += "needle\n";
    for (int file = 0; file < 6; ++file)
        QVERIFY(writeFixture(dir.filePath(QStringLiteral("file%1.s").arg(file)), body));

    FindInFiles walk;
    WalkResult result;
    QVERIFY(runWalk(&walk, dir.path(), QStringLiteral("needle"), FindOptions{}, &result));

    QVERIFY2(result.summary.capped, "the summary must admit the cap stopped the walk");
    QCOMPARE(result.summary.hits, kMaxMatches);
    QCOMPARE(result.hits().size(), kMaxMatches);
    QVERIFY2(result.summary.files < 6, "the walk stopped rather than reading the rest");
}

void TstFind::anEmptyQueryFinishesAtOnceAndACancelIsSilent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir.filePath(QStringLiteral("code.s")), "needle\n"));

    FindInFiles walk;
    WalkResult result;
    QObject::connect(&walk, &FindInFiles::finished, &walk,
                     [&result](const FindInFilesSummary &) { ++result.finishes; });

    // Nothing to search: one answer, immediately, and it is empty.
    walk.start(dir.path(), QString(), FindOptions{});
    QCOMPARE(result.finishes, 1);
    QCOMPARE(result.summary.hits, 0);
    QVERIFY(!walk.isRunning());

    // A walk that is cancelled says nothing — the caller that cancelled has
    // already replaced what its summary would have been for.
    walk.start(dir.path(), QStringLiteral("needle"), FindOptions{});
    QVERIFY(walk.isRunning());
    walk.cancel();
    QVERIFY(!walk.isRunning());
    QTest::qWait(50);
    QCOMPARE(result.finishes, 1);
}

void TstFind::aReplacedWalkReportsNothing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    for (int i = 0; i < 40; ++i)
        QVERIFY(writeFixture(dir.filePath(QStringLiteral("file%1.s").arg(i)),
                             QByteArray(4000, 'x') + "needle\n"));

    FindInFiles walk;
    WalkResult result;
    QObject::connect(&walk, &FindInFiles::hitsFound, &walk,
                     [&result](const QString &path, const QList<FindInFilesHit> &hits) {
                         result.files.append({path, hits});
                     });
    QObject::connect(&walk, &FindInFiles::finished, &walk,
                     [&result](const FindInFilesSummary &summary) {
                         result.summary = summary;
                         ++result.finishes;
                     });

    // A second query immediately: the first is replaced before it has scanned
    // anything, so neither its rows nor its summary may ever arrive.
    walk.start(dir.path(), QStringLiteral("needle"), FindOptions{});
    walk.start(dir.path(), QStringLiteral("nothing-is-here"), FindOptions{});
    QVERIFY(QTest::qWaitFor([&result] { return result.finishes > 0; }, 15000));
    QCOMPARE(result.finishes, 1);
    QCOMPARE(result.summary.hits, 0);
    QCOMPARE(result.summary.files, 40); // the second query's own walk
    QVERIFY2(result.files.isEmpty(), "the replaced query's hits were never reported");
}

QTEST_MAIN(TstFind)
#include "tst_find.moc"
