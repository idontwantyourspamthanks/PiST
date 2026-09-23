// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QByteArray>
#include <QString>

namespace pist {

/// The one rule for writing a file the user already has.
///
/// Every save path in the IDE used to open the destination with
/// `QIODevice::Truncate` and check the byte count afterwards, which reports the
/// failure honestly and still leaves the file empty — the truncate already
/// happened, and on a full disk or a quota that is the only copy of the user's
/// source, sprite document or floppy image gone. The project file grew a
/// `QSaveFile` instead, and then found that Qt 6.8.1's `commit()` does not
/// notice a flush failing inside it: it renames the truncated temporary file
/// over the destination and returns true. Qt 6.10 checks the device error
/// there, so a development machine on a newer Qt never sees it — and 6.8.1 is
/// what every release archive bundles (docs/PLAN.md §9).
///
/// So the guarantee lives here rather than at each call site, and leans on
/// neither behaviour: write to a temporary file, flush it, check the device
/// error, and only then let it take the destination's place.
///
/// One trade is deliberate. `QSaveFile::setDirectWriteFallback(true)` — what Qt's
/// own docs recommend for user-edited documents — would let a save proceed when
/// the temporary file cannot be created, the case Qt names being a writable file
/// in a read-only directory. It stays off: that fallback *is* the
/// truncate-then-hope write this rule replaces. Such a save now fails with a
/// reason and the user keeps every byte, instead of the old behaviour gambling
/// the file on a write that has already destroyed what was there.
namespace files {

/// Replace `path` with `bytes`. On any failure — the directory, the open, a
/// short write, a flush that cannot reach the disk, the rename — `path` still
/// holds exactly what it held before, and `*error` says why.
bool write(const QString &path, const QByteArray &bytes, QString *error);

} // namespace files
} // namespace pist
