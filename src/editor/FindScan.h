// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development
//
// One search, two callers: the editor's find bar, and Find in Files.
//
// The sheet's rule for Find in Files is that its counting must match the
// editor's exactly — "a tool that counts differently in two places looks like it
// is lying in one of them" — so the semantics are not copied, they are shared:
// both call `findScan()` with their own options, and there is no second
// implementation to drift away from this one.

#pragma once

#include <QList>
#include <QString>
#include <QTextCursor>

class QTextDocument;

namespace pist {

/// The options a search runs with: the editor's two toggles, and nothing else.
/// Case-insensitive by default, which is what a find bar starts as, and no regex
/// — a plain substring is what both callers are matching.
struct FindOptions
{
    bool caseSensitive = false;
    bool wholeWord = false;
};

/// How many hits one scan keeps.
///
/// A common token in a large file has thousands of them, and a search re-runs on
/// every keystroke and every cursor move; both callers therefore admit the cap
/// ("more than N hits") rather than report a total the walk never reached. A
/// caller that must act on *every* hit — the editor's Replace All — re-scans
/// without it.
constexpr int kMaxMatches = 2000;

/// Every hit of `needle` in `document`, in document order, at most `limit` of
/// them (`limit <= 0` keeps all of them).
///
/// `capped`, when given, is set only if the walk stopped at the limit with
/// another hit still to come: one hit past the limit is what proves "more than",
/// where the limit alone only says the walk stopped.
///
/// Whole-word is not one of `QTextDocument::FindFlags` — Qt matches the needle
/// and this filters it by the characters on either side — so the rule lives
/// here, in the one place both callers go through.
QList<QTextCursor> findScan(QTextDocument *document, const QString &needle,
                            const FindOptions &options, int limit = 0, bool *capped = nullptr);

} // namespace pist
