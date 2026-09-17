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

/// Compose the sheet `sheetIndex` from its placed phases: a single-frame
/// document of the sheet's size with each phase's frames painted at
/// [x + k*cellW, y]. Transparency stays transparent; unplaced phases and
/// phases on other sheets are skipped. Fails on an unknown sheet index.
ImageDocument composeSheet(const ImageDocument &doc, int sheetIndex, QString *error);

/// Slice `count` cells of cellW×cellH out of `sheet`, left to right from
/// [x, y], into frame pixel arrays (transparency preserved).
QVector<QVector<int>> sliceSheetCells(const ImportedSheet &sheet, int x, int y,
                                      int cellW, int cellH, int count);

/// Re-order the active colours so the sprite never occupies colour 0: the
/// reserved background slot takes index 0 and every painted colour moves up,
/// which is what makes an ST still image re-import losslessly when its
/// colour 0 is treated as transparent. Uses the current frame's pixels; a
/// sheet that already keeps colour 0 free is returned unchanged. When all
/// 16 colours are painted there is no slot to reserve: `*error` is set and
/// the returned document is unspecified.
ImageDocument spriteSafeDocument(const ImageDocument &doc, QString *error);

QByteArray exportPi1(const ImageDocument &doc, int frame, QString *error);
QByteArray exportNeo(const ImageDocument &doc, int frame, const QString &name, QString *error);
QByteArray exportIff(const ImageDocument &doc, int frame, QString *error);
QByteArray exportPng(const ImageDocument &doc, int frame, QString *error);
QByteArray exportStosMbk(const ImageDocument &doc, int maskColour, int bankNumber, QString *error);
QByteArray exportAssembler(const ImageDocument &doc, int frame, QString *error);
QByteArray exportBitplanes(const ImageDocument &doc, int frame, QString *error);

/// What a raw bitplane data file (the `.dat` a sprite export writes) holds.
/// The blocks are written in the order listed here; every one is optional.
struct BitplaneDataOptions {
    bool palette = true;
    bool sprite = true;
    bool masked = false;
    bool shifted = false;
    bool shiftedMasked = false;
    /// Pre-shifted copies, the unshifted one included: 2, 4 or 8, so a copy
    /// sits 16/count pixels to the right of the one before it. Ignored unless
    /// a shifted block is selected.
    int preShifts = 4;
    /// The ST colour register the mask leaves out, on top of the pixels that
    /// are not painted at all: a blit keeps the screen under both. 0 is the
    /// background register, which is what a sprite is normally cut against;
    /// -1 means every painted colour is opaque.
    int transparent = 0;
};

/// One block of an exported file: the label the assembler source uses for it,
/// and where the encoder writes it.
struct BitplaneBlock {
    enum class Kind {
        Palette,
        Sprite,
        Masked,
        Shifted,
        ShiftedMasked,
    };

    Kind kind = Kind::Palette;
    /// Which frame of the phase this is (-1 for the palette, which is written
    /// once for the whole file).
    int frame = -1;
    /// Which pre-shifted copy this is (0 = unshifted); -1 for the blocks that
    /// are not pre-shifted.
    int shift = -1;
    QString name;
    int offset = 0;
    int bytes = 0;
};

/// The blocks `options` selects for a `width`×`height` phase of `frameCount`
/// frames, in the order they are written, with the offsets and sizes the
/// encoder produces. The palette comes first and the frames follow, each
/// frame's blocks together, so a frame's data is one contiguous run and every
/// frame is the same size. The export dialog prints this map and the encoder
/// walks it, so the labels the user writes their `equ`s against cannot drift
/// from the data.
QVector<BitplaneBlock> bitplaneLayout(int width, int height, int frameCount,
                                      const BitplaneDataOptions &options);

/// Encode `phase` of `doc` — every frame of it — as a raw ST bitplane blob.
/// The blocks appear in `bitplaneLayout()` order: palette (16 colour words,
/// register 0–15, for $ff8240), then each frame, then the frames' pre-shifted
/// copies, masked ones last within their frame.
///
/// Plane data is screen format: `width` rounded up to whole 16-pixel groups,
/// each group plane 0..3 (4 words). A masked group is mask,0,1,2,3 (5 words).
/// A pre-shifted copy is one group wider than the frame, so the pixels a shift
/// pushes past its right edge still fit, and every copy has the same stride —
/// the source for shift k is `base + k * stride`, so a shift needs no branch.
/// Frames are laid out the same way, so frame f of a block is
/// `base + f * frameStride` with the same stride for every frame.
///
/// A mask bit is set where the pixel is *not* drawn — unpainted, or the palette
/// colour `options.transparent` nominates — so the blitter keeps the screen
/// there:
///
///     move.w (a0)+,d0 / and.w d0,(a1)     ; mask: a 1 bit keeps the screen
///     move.w (a0)+,d0 / or.w  d0,(a1)+    ; plane 0, then 1..3
///
/// Fails (with an empty result and `*error` set) when no block is selected, or
/// a shifted block is asked for with a pre-shift count other than 2, 4 or 8.
QByteArray exportBitplaneData(const ImageDocument &doc, int phase,
                              const BitplaneDataOptions &options, QString *error);

/// A ready-to-assemble `-Ftos` program that shows `phase` moving across an ST
/// low-resolution screen, animating through its frames as it goes: it asks
/// XBIOS Getrez (leaving a non-4-bitplane screen alone with a message), takes
/// over supervisor mode, keeps and replaces the palette, clears the screen and
/// blits one frame per VBL — erasing where it was, and using the pre-shifted
/// copies so the step is as fine as the data allows — until a key is pressed,
/// then puts the palette back and returns.
///
/// The data is *not* inlined: the source pulls the `.dat` in with `incbin`
/// `dataFile`, named as the export wrote it (vasm resolves an `incbin` beside
/// the source it is assembling, so the pair travels together), and addresses
/// each block with the `equ`s from `bitplaneLayout()`.
///
/// Fails, with `*error` set, when `options` holds no sprite block to draw.
QByteArray exportScrollDemo(const ImageDocument &doc, int phase,
                            const BitplaneDataOptions &options, const QString &dataFile,
                            QString *error);


} // namespace pist
