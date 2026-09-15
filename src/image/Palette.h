// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QColor>
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

} // namespace pist
