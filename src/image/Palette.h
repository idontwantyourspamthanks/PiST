// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

namespace pist {

/// The two ST colour cubes. STfm is 3 bits per channel (512); STe is 4 (4096).
enum class PaletteKind {
    Stfm,
    Ste,
};

/// An RGB triple in 0..255, matching the cube entries LemonAndLime seeds.
struct Rgb {
    quint8 r = 0;
    quint8 g = 0;
    quint8 b = 0;

    bool operator==(const Rgb &other) const
    {
        return r == other.r && g == other.g && b == other.b;
    }

    QColor toColor() const { return QColor(r, g, b); }
};

constexpr int kTransparent = -1;
constexpr int kMaxActive = 16;
constexpr int kStScreenWidth = 320;
constexpr int kStScreenHeight = 200;

/// "stfm" / "ste" as stored in a `.pim`.
QString paletteKindName(PaletteKind kind);
bool paletteKindFromName(const QString &name, PaletteKind *kind);

int cubeSize(PaletteKind kind);
int channelCount(PaletteKind kind);

/// Channel levels the cube actually uses (8 for STfm, 16 for STe).
QVector<int> channelLevels(PaletteKind kind);

/// The hardware cube in r-major order: index = r*n*n + g*n + b.
const QVector<Rgb> &cubeColours(PaletteKind kind);

Rgb cubeRgb(PaletteKind kind, int index);
QColor cubeColor(PaletteKind kind, int index);

/// How `indicesToImage()` fills a cell that has no colour (kTransparent).
/// One helper renders every cube-index grid the sprite editor shows, so its
/// callers cannot drift apart on what "nothing here" looks like — a frame
/// thumbnail used to paint a different empty texture than the canvas of the
/// very same frame.
enum class EmptyStyle {
    /// Two-tone: the grid canvas' own "nothing here" texture, and what the
    /// frame thumbnails and the animation preview show for the same frame.
    Checkerboard,
    /// One flat grey panel, dimmed under the imported-sheet reference so the
    /// composed phases drawn over it stand out.
    Flat,
    /// Fully transparent: an underlay or the widget's own background shows
    /// through (the sheet view's cells, the selection patch, the onion ghost).
    Transparent,
};

/// Render cube-index pixels — `width`×`height`, row-major, kTransparent empty
/// — as an ARGB32 image. An entry the buffer does not hold reads as empty, so
/// an undersized buffer renders shorter rather than indexing past its end.
/// `region` is the part of the grid to render, in source pixels; the default
/// renders all of it, and the checkerboard phase follows the source
/// coordinates so a crop shows the same texture as the whole. A null image
/// for an empty region or an empty grid.
QImage indicesToImage(const QVector<int> &pixels, int width, int height, PaletteKind kind,
                      EmptyStyle empty, const QRect &region = QRect());

/// Snap an 8-bit channel onto the cube's levels.
int snapChannel(PaletteKind kind, int value);

/// Cube index of the nearest hardware colour.
int nearestCubeIndex(PaletteKind kind, Rgb rgb);
int nearestCubeIndex(PaletteKind kind, const QColor &color);

/// Default working set of 16 colours: bright primaries then black, as in
/// LemonAndLime's `defaultActiveIndices`.
QVector<int> defaultActiveIndices(PaletteKind kind);

/// Map a painted cube index onto an ST colour register (0–15). Transparent
/// and unknown colours become 0.
int stColourIndex(int cubeIndex, const QVector<int> &active);

/// ST colour words for the 16-entry hardware table.
quint16 stfmColourWord(Rgb rgb);
quint16 steColourWord(Rgb rgb);
quint16 colourWord(PaletteKind kind, Rgb rgb);

Rgb rgbFromSteWord(quint16 word);
Rgb rgbFromStfmWord(quint16 word);

QVector<quint16> stColourTable(PaletteKind kind, const QVector<int> &active);

/// The register table an STfm-era still-image container holds (Degas PI1,
/// Neochrome NEO, STOS MBK). Those formats predate the STe and their 16 entries
/// are always 3-bit STfm words, whatever cube the document paints from, so an
/// STe document's 16-level colours are quantised here rather than written with
/// the STe layout the file cannot hold.
QVector<quint16> stfmColourTable(PaletteKind kind, const QVector<int> &active);

} // namespace pist
