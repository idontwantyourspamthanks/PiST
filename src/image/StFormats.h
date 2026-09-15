// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"

#include <QByteArray>
#include <QString>

namespace pist {

enum class StImageFormat {
    Unknown,
    Pim,
    Pi1,
    Neo,
    Iff,
    Png,
    Mbk,
    Assembler,
    BitplaneBin,
};

StImageFormat stFormatFromPath(const QString &path);
bool isPimPath(const QString &path);
bool isImportableImagePath(const QString &path);

struct ImportedSheet {
    int width = 0;
    int height = 0;
    PaletteKind kind = PaletteKind::Ste;
    QVector<int> active;
    QVector<int> pixels;
};

/// Decode a ST still image into cube-index pixels and a 16-colour active set.
bool importStImage(const QByteArray &bytes, StImageFormat format, PaletteKind kind,
                   ImportedSheet *out, QString *error);

bool importPi1(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error);
bool importNeo(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error);
bool importIff(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error);
bool importPng(const QByteArray &bytes, PaletteKind kind, ImportedSheet *out, QString *error);

/// Apply an imported sheet as the document's only frame (replace) or as an
/// extra frame when the size matches.
bool applyImport(ImageDocument *doc, const ImportedSheet &sheet, bool append, QString *error);

QByteArray exportPi1(const ImageDocument &doc, int frame, QString *error);
QByteArray exportNeo(const ImageDocument &doc, int frame, const QString &name, QString *error);
QByteArray exportIff(const ImageDocument &doc, int frame, QString *error);
QByteArray exportPng(const ImageDocument &doc, int frame, QString *error);
QByteArray exportStosMbk(const ImageDocument &doc, int maskColour, int bankNumber, QString *error);
QByteArray exportAssembler(const ImageDocument &doc, int frame, QString *error);
QByteArray exportBitplanes(const ImageDocument &doc, int frame, QString *error);

/// Export one region of a sprite sheet: crop the frame to `region` (clipped
/// to the canvas) and produce exactly the bytes the whole-document exporter
/// produces for that crop. Formats: Assembler (a `dc.w` include) and
/// BitplaneBin (raw word-padded bitplanes). Empty array on failure.
QByteArray exportRegion(const ImageDocument &doc, int frame, const ImageRegion &region,
                        StImageFormat format, QString *error);

} // namespace pist
