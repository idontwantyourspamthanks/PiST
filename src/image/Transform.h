// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"

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

/// Clockwise 90° of a (possibly rectangular) buffer. `outWidth`/`outHeight`
/// receive the swapped dimensions.
QVector<int> rotate90Cw(const QVector<int> &data, int width, int height, int *outWidth,
                        int *outHeight);

/// Indexed-pixel rotation about the grid centre. Square grids only. Right
/// angles are pixel-exact; other angles resample without blending colours.
QVector<int> rotateIndexed(const QVector<int> &data, int size, double angleDeg);

/// `count - 1` clockwise copies at 360/count° steps (the 0° original is skipped).
QVector<QVector<int>> generateRotationFrames(const QVector<int> &data, int size, int count);

/// The single neighbouring frame `distance` away from `current`. Negative
/// distance is behind, positive is ahead, 0 turns onion off.
QVector<OnionGhost> neighbourFrames(int current, int count, int distance = -1);

int nextPreviewFrame(int current, int count, int start, int end);

} // namespace pist
