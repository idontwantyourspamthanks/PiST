// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/FindScan.h"

#include <QChar>
#include <QTextDocument>

namespace pist {
namespace {

/// A "word" for whole-word search: letters, digits and the underscore, which is
/// what the editor's find bar has always meant by it.
bool isWordCharacter(const QChar &character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_');
}

/// Whether `hit` is delimited by non-word characters rather than sitting inside
/// a longer one — `move` in `move.w` but not in `movem`.
bool isWholeWord(QTextDocument *document, const QTextCursor &hit)
{
    const int start = hit.selectionStart();
    const int end = hit.selectionEnd();
    if (start > 0 && isWordCharacter(document->characterAt(start - 1)))
        return false;
    return !isWordCharacter(document->characterAt(end));
}

} // namespace

QList<QTextCursor> findScan(QTextDocument *document, const QString &needle,
                            const FindOptions &options, int limit, bool *capped)
{
    QList<QTextCursor> hits;
    if (capped)
        *capped = false;
    if (!document || needle.isEmpty())
        return hits;

    QTextDocument::FindFlags flags;
    if (options.caseSensitive)
        flags |= QTextDocument::FindCaseSensitively;

    QTextCursor at(document);
    while (true) {
        const QTextCursor hit = document->find(needle, at, flags);
        if (hit.isNull())
            break;
        at = hit;
        if (options.wholeWord && !isWholeWord(document, hit))
            continue;
        if (limit > 0 && hits.size() >= limit) {
            if (capped)
                *capped = true;
            break;
        }
        hits.append(hit);
    }
    return hits;
}

} // namespace pist
