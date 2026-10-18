// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/FindInFiles.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringDecoder>
#include <QTextDocument>
#include <QTimer>

namespace pist {
namespace {

/// How long one tick may spend scanning before the event loop gets a turn. The
/// walk is chunked by *time* rather than by file count so that a directory of
/// small files finishes promptly and a directory of large ones still leaves the
/// window able to paint and to hear the next keystroke.
constexpr int kTickMs = 10;

/// A file bigger than this is not scanned: it is not a source file anyone is
/// looking inside, and reading it would stall the tick.
constexpr qint64 kMaxFileBytes = 4 * 1024 * 1024;

/// How much of a file is sniffed for a NUL byte before giving up on it being
/// text — the same evidence the open path uses, and enough for any real source.
constexpr int kBinarySniffBytes = 4096;

/// Every file under `dir`, in a stable order (name-sorted, directories walked
/// where their name falls), skipping symbolic links and hidden directories.
///
/// Sorted because the results list and the tests both want a walk that is
/// reproducible — and because the order a user expects is the order the paths
/// read in. Hidden directories because `.git` is not the project's own text
/// however much of it is plain.
void collectFiles(const QString &dir, QStringList *out)
{
    const QFileInfoList entries = QDir(dir).entryInfoList(QDir::Files | QDir::Dirs
                                                              | QDir::NoDotAndDotDot,
                                                          QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &entry : entries) {
        // Never follow a link: a symlink back up the tree is a walk that does
        // not end.
        if (entry.isSymLink())
            continue;
        if (entry.isDir()) {
            if (!entry.fileName().startsWith(QLatin1Char('.')))
                collectFiles(entry.absoluteFilePath(), out);
            continue;
        }
        out->append(entry.absoluteFilePath());
    }
}

/// The bytes of a source file as text: UTF-8 when they are UTF-8, Latin-1
/// otherwise — the two encodings the editor itself writes back unchanged. The
/// choice only has to be consistent with the columns reported here, which are
/// offsets in the text this produced.
QString decodeText(const QByteArray &data)
{
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(data);
    return utf8.hasError() ? QString::fromLatin1(data) : text;
}

/// The line containing `position`, without its newline and without trailing
/// whitespace (leading whitespace is kept: it is where a match inside it would
/// be, and it is what an indented source line looks like).
QString lineTextAt(const QString &text, int position)
{
    // A negative `from` would mean "search backwards from the end", which would
    // make the first line of a file read as its last.
    int start = 0;
    if (position > 0)
        start = text.lastIndexOf(QLatin1Char('\n'), position - 1) + 1;
    int end = text.indexOf(QLatin1Char('\n'), start);
    if (end < 0)
        end = text.size();
    while (end > start && text.at(end - 1).isSpace())
        --end;
    return text.mid(start, end - start);
}

/// The hits of one file, as rows: 1-based line, the column the match starts at
/// within its line, its length, and the line's text.
QList<FindInFilesHit> describeHits(const QString &text, const QList<QTextCursor> &hits)
{
    QList<FindInFilesHit> rows;
    rows.reserve(hits.size());
    int line = 1;
    int lineStart = 0;
    for (const QTextCursor &hit : hits) {
        const int position = hit.selectionStart();
        // Hits arrive in order, so the line count is a walk from the last one
        // rather than a search from the top for each.
        while (true) {
            const int next = text.indexOf(QLatin1Char('\n'), lineStart);
            if (next < 0 || next >= position)
                break;
            lineStart = next + 1;
            ++line;
        }
        FindInFilesHit row;
        row.line = line;
        row.column = position - lineStart;
        row.length = hit.selectionEnd() - position;
        row.preview = lineTextAt(text, position);
        rows.append(row);
    }
    return rows;
}

} // namespace

bool isSearchableFile(const QString &path)
{
    static const QSet<QString> unsearchable = {
        // Executables and object code.
        QStringLiteral("prg"), QStringLiteral("tos"), QStringLiteral("ttp"),
        QStringLiteral("bin"), QStringLiteral("sys"), QStringLiteral("o"),
        // Disk and still-image formats, and the ROM the emulator runs.
        QStringLiteral("st"),  QStringLiteral("msa"), QStringLiteral("img"),
        QStringLiteral("dim"), QStringLiteral("ipf"), QStringLiteral("pi1"),
        QStringLiteral("pi2"), QStringLiteral("pi3"), QStringLiteral("neo"),
        QStringLiteral("iff"), QStringLiteral("ilbm"), QStringLiteral("png"),
        QStringLiteral("mbk"), QStringLiteral("pim"), QStringLiteral("rom"),
        // Generated inventory: a vasm listing is the build's output, not source.
        QStringLiteral("lst"),
    };
    return !unsearchable.contains(QFileInfo(path).suffix().toLower());
}

FindInFiles::FindInFiles(QObject *parent)
    : QObject(parent)
    , m_tick(new QTimer(this))
{
    m_tick->setSingleShot(true);
    connect(m_tick, &QTimer::timeout, this, &FindInFiles::step);
}

FindInFiles::~FindInFiles() = default;

void FindInFiles::start(const QString &root, const QString &needle, const FindOptions &options)
{
    cancel();

    m_needle = needle;
    m_options = options;
    m_summary = FindInFilesSummary{};
    m_files.clear();
    m_next = 0;

    if (root.isEmpty() || needle.isEmpty() || !QFileInfo(root).isDir()) {
        // Nothing to walk. Saying so at once is the answer the dock wants for an
        // empty query: the results clear and the header stops describing a search
        // that is no longer the one on screen.
        emit finished(m_summary);
        return;
    }

    collectFiles(root, &m_files);
    m_running = true;
    m_tick->start(0);
}

void FindInFiles::cancel()
{
    if (!m_running)
        return;
    m_tick->stop();
    m_running = false;
    m_files.clear();
    m_next = 0;
}

void FindInFiles::step()
{
    if (!m_running)
        return;

    QElapsedTimer budget;
    budget.start();
    while (m_next < m_files.size() && budget.elapsed() < kTickMs) {
        const QString path = m_files.at(m_next++);

        QFile file(path);
        if (!isSearchableFile(path) || !file.open(QIODevice::ReadOnly)
            || file.size() > kMaxFileBytes) {
            ++m_summary.skipped;
            continue;
        }
        const QByteArray data = file.readAll();
        // A NUL byte settles it whatever the name says — the same evidence the
        // open path uses to decide a file is not text.
        if (data.left(kBinarySniffBytes).contains('\0')) {
            ++m_summary.skipped;
            continue;
        }
        ++m_summary.files;

        const QString text = decodeText(data);
        QTextDocument document;
        document.setPlainText(text);

        bool capped = false;
        const QList<QTextCursor> hits = findScan(&document, m_needle, m_options,
                                                 kMaxMatches - m_summary.hits, &capped);
        if (!hits.isEmpty())
            emit hitsFound(path, describeHits(text, hits));
        m_summary.hits += hits.size();

        // One hit inside this file past what is left of the cap, or the cap
        // reached exactly: either way the walk stops here, and `capped` says the
        // count is a floor rather than a total.
        if (capped || m_summary.hits >= kMaxMatches) {
            m_summary.capped = true;
            end();
            return;
        }
    }

    if (m_next >= m_files.size()) {
        end();
        return;
    }
    m_tick->start(0);
}

void FindInFiles::end()
{
    m_tick->stop();
    m_running = false;
    m_files.clear();
    m_next = 0;
    emit finished(m_summary);
}

} // namespace pist
