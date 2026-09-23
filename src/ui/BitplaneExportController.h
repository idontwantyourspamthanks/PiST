// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"
#include "image/StFormats.h"

#include <QObject>
#include <QString>

namespace pist {

/// The sprite editor's bitplane exports: the `.dat` of raw bitplanes, the
/// ready-to-assemble scroller that goes beside it, and the recipe a re-export
/// repeats. A `.pim` does not record the export choices and neither does the
/// project — export stays explicit — so how to repeat one lives here, per
/// document, per session: the phase, the blocks and the file, plus the
/// scroller when that export wrote one. The editor owns one and wires its
/// re-export action to it; `reExport()` announces through `bitplaneReExported`
/// for the console to print the same block map as the explicit export.
///
/// The document is held by reference: the editor reassigns one document member
/// in place, so the reference stays valid across loads and new documents.
class BitplaneExportController : public QObject
{
    Q_OBJECT

public:
    explicit BitplaneExportController(const ImageDocument &doc, QObject *parent = nullptr);

    /// Whether a bitplane export has succeeded in this session, so there is a
    /// recipe to repeat.
    bool canReExport() const { return !m_recipe.path.isEmpty(); }
    /// The `.dat` the remembered export wrote; empty before the first one.
    QString lastExportPath() const { return m_recipe.path; }
    /// The phase the remembered export used, and the blocks it wrote, so the
    /// shell can print the same block map for a re-export as for the explicit
    /// one.
    int lastExportPhase() const { return m_recipe.phase; }
    BitplaneDataOptions lastExportOptions() const { return m_recipe.options; }
    /// The scroller written beside `lastExportPath()`, for the re-export
    /// action's tool tip; empty when that export wrote none.
    QString lastExportScroller() const { return m_recipe.scroller; }

    /// Remember that the export `exportBitplane()` just wrote also came with
    /// this scroller, so `reExport()` writes it too. The export flow calls this
    /// once the scroller is out; `exportBitplane()` clears it, so a new export
    /// replaces the pair rather than reusing it. Ignored when no bitplane
    /// export has succeeded yet.
    void setScroller(const QString &scroller);
    /// Drop the remembered export — the document it described is gone. Leaves
    /// the recipe empty, so a re-export is refused.
    void forget();

    /// Write `path` as the raw bitplanes of `phase` — all of its frames, with
    /// the blocks `options` picks — and remember it as the recipe. False leaves
    /// `lastError()` saying why.
    bool exportBitplane(const QString &path, int phase, const BitplaneDataOptions &options);
    /// Write the ready-to-assemble scroller for `phase`, a program that
    /// animates its frames across the screen, `incbin`-ing `dataFile` (the
    /// `.dat` written beside it). Named `writeScrollDemo` so it does not hide
    /// the encoder `pist::exportScrollDemo()` it wraps.
    bool writeScrollDemo(const QString &path, int phase, const BitplaneDataOptions &options,
                         const QString &dataFile);
    /// Write the remembered export again — the same phase, blocks and `.dat`,
    /// and the scroller beside it when that export wrote one — with no dialog
    /// and no overwrite prompt, encoding the document's current pixels. False
    /// leaves `lastError()` saying why. Nothing is encoded unless both blobs
    /// encode, so a phase that is gone cannot truncate a good `.dat`; the
    /// `.dat` goes out before its scroller, so a scroller that will not open is
    /// reported with the `.dat` already written.
    bool reExport();

    QString lastError() const { return m_lastError; }

signals:
    /// A re-export finished: `path` is the `.dat`, `scroller` what was written
    /// beside it (empty when none). `error` is empty on success. A re-export
    /// has no shell caller to report to — the explicit export is driven from
    /// the shell, which logs it there — so the controller announces this one
    /// for the console to print the same block map.
    void bitplaneReExported(const QString &path, const QString &scroller, const QString &error);
    /// The recipe changed — a new export seeded it, its scroller arrived, or it
    /// was forgotten — so the re-export action's enabled state and tool tip
    /// have to follow.
    void recipeChanged();

private:
    /// The bitplane export a re-export repeats, as `.pim` does not record it
    /// and the project does not either: export stays explicit, and this is per
    /// document, per session. `path` empty means no export has happened.
    struct Recipe {
        QString path;
        int phase = 0;
        BitplaneDataOptions options;
        /// The scroller written beside `path`, empty when that export wrote
        /// none or the user declined to overwrite one already there.
        QString scroller;
    };

    /// Encode and write the `.dat` (and the scroller, when one is named): the
    /// one tail both the explicit export and the re-export share.
    bool exportBytes(const QString &path, int phase, const BitplaneDataOptions &options,
                     const QString &scroller);
    bool writeBytes(const QString &path, const QByteArray &bytes);

    const ImageDocument &m_doc;
    Recipe m_recipe;
    QString m_lastError;
};

} // namespace pist
