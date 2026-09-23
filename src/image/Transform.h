// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"

#include <QPoint>
#include <QRect>
#include <QString>
#include <QVector>

namespace pist {

enum class FlipDirection {
    Horizontal,
    Vertical,
};

enum class ShiftDirection {
    Left,
    Right,
    Up,
    Down,
};

struct OnionGhost {
    int index = -1;
    bool prev = true;
    int distance = 0;
};

QVector<int> flipData(const QVector<int> &data, int width, int height, FlipDirection direction);
QVector<int> shiftData(const QVector<int> &data, int width, int height, ShiftDirection direction);

/// Values of `rect` (clipped to the grid), row by row. An empty result means
/// the rectangle lies outside the grid.
QVector<int> regionData(const QVector<int> &data, int width, int height, const QRect &rect);

/// Copy of `data` with `rect` (clipped to the grid) set to `value`.
QVector<int> clearRegion(const QVector<int> &data, int width, int height, const QRect &rect,
                         int value);

/// Copy of `data` with `patch` (`patchWidth` wide, height implied) stamped at
/// `pos`, clipped to the grid. Transparent patch pixels leave the destination
/// underneath untouched, so a sprite can be dropped onto existing art.
QVector<int> stampRegion(const QVector<int> &data, int width, int height,
                         const QVector<int> &patch, int patchWidth, const QPoint &pos);

/// Move `rect` by `delta`: the source rectangle is cleared and its previous
/// contents stamped at the offset position, both clipped to the grid (pixels
/// pushed off the canvas are dropped).
QVector<int> moveRegion(const QVector<int> &data, int width, int height, const QRect &rect,
                        const QPoint &delta);

/// Clockwise 90° of a (possibly rectangular) buffer. `outWidth`/`outHeight`
/// receive the swapped dimensions.
QVector<int> rotate90Cw(const QVector<int> &data, int width, int height, int *outWidth,
                        int *outHeight);

/// Indexed-pixel rotation about the grid centre. Square grids only. Right
/// angles are pixel-exact; other angles resample without blending colours.
QVector<int> rotateIndexed(const QVector<int> &data, int size, double angleDeg);

/// The single neighbouring frame `distance` away from `current`. Negative
/// distance is behind, positive is ahead, 0 turns onion off.
QVector<OnionGhost> neighbourFrames(int current, int count, int distance = -1);

int nextPreviewFrame(int current, int count, int start, int end);

} // namespace pist
