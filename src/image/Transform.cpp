// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "image/Transform.h"

#include <QtMath>

#include <algorithm>
#include <cmath>
#include <utility>

namespace pist {

QVector<int> flipData(const QVector<int> &data, int width, int height, FlipDirection direction)
{
    QVector<int> out(data.size(), kTransparent);
    if (width <= 0 || height <= 0 || data.size() < width * height)
        return out;
    for (int row = 0; row < height; ++row) {
        const int srcRow = direction == FlipDirection::Vertical ? height - 1 - row : row;
        for (int col = 0; col < width; ++col) {
            const int srcCol = direction == FlipDirection::Horizontal ? width - 1 - col : col;
            out[row * width + col] = data[srcRow * width + srcCol];
        }
    }
    return out;
}

QVector<int> shiftData(const QVector<int> &data, int width, int height, ShiftDirection direction)
{
    QVector<int> out(data.size(), kTransparent);
    if (width <= 0 || height <= 0 || data.size() < width * height)
        return out;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            int srcRow = row;
            int srcCol = col;
            switch (direction) {
            case ShiftDirection::Left:
                srcCol = (col + 1) % width;
                break;
            case ShiftDirection::Right:
                srcCol = (col - 1 + width) % width;
                break;
            case ShiftDirection::Up:
                srcRow = (row + 1) % height;
                break;
            case ShiftDirection::Down:
                srcRow = (row - 1 + height) % height;
                break;
            }
            out[row * width + col] = data[srcRow * width + srcCol];
        }
    }
    return out;
}

QVector<int> rotate90Cw(const QVector<int> &data, int width, int height, int *outWidth,
                        int *outHeight)
{
    const int newWidth = height;
    const int newHeight = width;
    if (outWidth)
        *outWidth = newWidth;
    if (outHeight)
        *outHeight = newHeight;
    QVector<int> out(newWidth * newHeight, kTransparent);
    if (width <= 0 || height <= 0 || data.size() < width * height)
        return out;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int newX = height - 1 - y;
            const int newY = x;
            out[newY * newWidth + newX] = data[y * width + x];
        }
    }
    return out;
}

QVector<int> rotateIndexed(const QVector<int> &data, int size, double angleDeg)
{
    const int n = size * size;
    QVector<int> out(n, kTransparent);
    if (size <= 0 || data.size() < n)
        return out;

    const double theta = qDegreesToRadians(angleDeg);
    const double ccos = std::cos(theta);
    const double ssin = std::sin(theta);
    const double centre = size / 2.0;
    constexpr int kUnset = -2;
    out.fill(kUnset);

    auto rot = [&](int x, int y) {
        const double dx = x + 0.5 - centre;
        const double dy = y + 0.5 - centre;
        return std::pair<double, double>{ccos * dx - ssin * dy + centre - 0.5,
                                         ssin * dx + ccos * dy + centre - 0.5};
    };
    auto stamp = [&](double fx, double fy, int value) {
        const int x = int(std::lround(fx));
        const int y = int(std::lround(fy));
        if (x >= 0 && x < size && y >= 0 && y < size)
            out[y * size + x] = value;
    };
    auto bridge = [&](std::pair<double, double> p, std::pair<double, double> q, int value) {
        const int px = int(std::lround(p.first));
        const int py = int(std::lround(p.second));
        const int qx = int(std::lround(q.first));
        const int qy = int(std::lround(q.second));
        if (std::max(std::abs(qx - px), std::abs(qy - py)) <= 1)
            return;
        const int steps = int(std::ceil(std::max(std::abs(q.first - p.first),
                                                 std::abs(q.second - p.second))
                                        * 2.0));
        for (int i = 0; i <= steps; ++i) {
            const double t = steps == 0 ? 0.0 : double(i) / steps;
            stamp(p.first + (q.first - p.first) * t, p.second + (q.second - p.second) * t, value);
        }
    };

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int value = data[y * size + x];
            if (value == kTransparent)
                continue;
            const auto p = rot(x, y);
            stamp(p.first, p.second, value);
            if (x + 1 < size && data[y * size + x + 1] == value)
                bridge(p, rot(x + 1, y), value);
            if (y + 1 < size && data[(y + 1) * size + x] == value)
                bridge(p, rot(x, y + 1), value);
            if (x + 1 < size && y + 1 < size && data[(y + 1) * size + x + 1] == value)
                bridge(p, rot(x + 1, y + 1), value);
            if (x + 1 < size && y > 0 && data[(y - 1) * size + x + 1] == value)
                bridge(p, rot(x + 1, y - 1), value);
        }
    }

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (out[y * size + x] != kUnset)
                continue;
            const double dx = x + 0.5 - centre;
            const double dy = y + 0.5 - centre;
            const int sx = int(std::lround(ccos * dx + ssin * dy + centre - 0.5));
            const int sy = int(std::lround(-ssin * dx + ccos * dy + centre - 0.5));
            out[y * size + x] = (sx >= 0 && sx < size && sy >= 0 && sy < size)
                                    ? data[sy * size + sx]
                                    : kTransparent;
        }
    }
    return out;
}

QVector<QVector<int>> generateRotationFrames(const QVector<int> &data, int size, int count)
{
    QVector<QVector<int>> frames;
    if (size <= 0 || count < 2)
        return frames;
    for (int i = 1; i < count; ++i)
        frames.append(rotateIndexed(data, size, 360.0 / count * i));
    return frames;
}

QVector<OnionGhost> neighbourFrames(int current, int count, int distance)
{
    if (distance == 0)
        return {};
    const int n = std::abs(distance);
    const int index = distance < 0 ? current - n : current + n;
    if (index < 0 || index >= count)
        return {};
    OnionGhost ghost;
    ghost.index = index;
    ghost.prev = distance < 0;
    ghost.distance = n;
    return {ghost};
}

int nextPreviewFrame(int current, int count, int start, int end)
{
    if (count <= 0)
        return 0;
    const int lo = std::max(0, std::min({start, end, count - 1}));
    const int hi = std::max(0, std::min(std::max(start, end), count - 1));
    if (hi <= lo)
        return lo;
    if (current < lo || current > hi)
        return lo;
    return current >= hi ? lo : current + 1;
}

} // namespace pist
