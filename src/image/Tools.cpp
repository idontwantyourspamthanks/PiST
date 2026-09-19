// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "image/Tools.h"

#include <QPoint>
#include <QQueue>
#include <QSet>

#include <algorithm>

namespace pist {

namespace {

void idxToXY(int idx, const Grid &grid, int *x, int *y)
{
    *x = grid.width == 0 ? 0 : idx % grid.width;
    *y = grid.width == 0 ? 0 : idx / grid.width;
}

bool inBounds(int x, int y, const Grid &grid)
{
    return x >= 0 && x < grid.width && y >= 0 && y < grid.height;
}

QVector<int> uniqueSorted(const QSet<int> &set)
{
    QVector<int> out(set.begin(), set.end());
    std::sort(out.begin(), out.end());
    return out;
}

QVector<int> stampPath(const QVector<QPoint> &coords, int size, const Grid &grid)
{
    QSet<int> result;
    for (const QPoint &pt : coords) {
        if (!inBounds(pt.x(), pt.y(), grid))
            continue;
        for (int idx : brushIndices(pt.y() * grid.width + pt.x(), size, grid))
            result.insert(idx);
    }
    return uniqueSorted(result);
}

QVector<QPoint> rectCoords(int x0, int y0, int x1, int y1)
{
    const int xmin = qMin(x0, x1), xmax = qMax(x0, x1);
    const int ymin = qMin(y0, y1), ymax = qMax(y0, y1);
    QVector<QPoint> pts;
    for (int x = xmin; x <= xmax; ++x) {
        pts.append(QPoint(x, ymin));
        pts.append(QPoint(x, ymax));
    }
    for (int y = ymin; y <= ymax; ++y) {
        pts.append(QPoint(xmin, y));
        pts.append(QPoint(xmax, y));
    }
    return pts;
}

QVector<QPoint> circleOffsets(int r)
{
    QVector<QPoint> pts;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        pts.append(QPoint(x, y));
        pts.append(QPoint(y, x));
        pts.append(QPoint(-x, y));
        pts.append(QPoint(-y, x));
        pts.append(QPoint(x, -y));
        pts.append(QPoint(y, -x));
        pts.append(QPoint(-x, -y));
        pts.append(QPoint(-y, -x));
        ++y;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            --x;
            err += 2 * (y - x) + 1;
        }
    }
    return pts;
}

QVector<QPoint> roundedRectCoords(int x0, int y0, int x1, int y1)
{
    const int xmin = qMin(x0, x1), xmax = qMax(x0, x1);
    const int ymin = qMin(y0, y1), ymax = qMax(y0, y1);
    const int boxW = xmax - xmin, boxH = ymax - ymin;
    const int r = qMax(0, qMin(qRound(qMin(boxW, boxH) / 4.0),
                              qMin(boxW / 2, boxH / 2)));
    if (r == 0)
        return rectCoords(x0, y0, x1, y1);

    QVector<QPoint> pts;
    for (int x = xmin + r; x <= xmax - r; ++x) {
        pts.append(QPoint(x, ymin));
        pts.append(QPoint(x, ymax));
    }
    for (int y = ymin + r; y <= ymax - r; ++y) {
        pts.append(QPoint(xmin, y));
        pts.append(QPoint(xmax, y));
    }
    for (const QPoint &off : circleOffsets(r)) {
        if (off.x() <= 0 && off.y() <= 0)
            pts.append(QPoint(xmin + r + off.x(), ymin + r + off.y()));
        if (off.x() >= 0 && off.y() <= 0)
            pts.append(QPoint(xmax - r + off.x(), ymin + r + off.y()));
        if (off.x() <= 0 && off.y() >= 0)
            pts.append(QPoint(xmin + r + off.x(), ymax - r + off.y()));
        if (off.x() >= 0 && off.y() >= 0)
            pts.append(QPoint(xmax - r + off.x(), ymax - r + off.y()));
    }
    return pts;
}

QVector<QPoint> ellipseCoords(int ax0, int ay0, int ax1, int ay1)
{
    int x0 = qMin(ax0, ax1), x1 = qMax(ax0, ax1);
    int y0 = qMin(ay0, ay1), y1 = qMax(ay0, ay1);
    QVector<QPoint> pts;
    int a = x1 - x0, b = y1 - y0;
    int b1 = b & 1;
    int dx = 4 * (1 - a) * b * b;
    int dy = 4 * (b1 + 1) * a * a;
    int err = dx + dy + b1 * a * a;

    y0 += (b + 1) / 2;
    y1 = y0 - b1;
    a = 8 * a * a;
    b1 = 8 * b * b;

    do {
        pts.append(QPoint(x1, y0));
        pts.append(QPoint(x0, y0));
        pts.append(QPoint(x0, y1));
        pts.append(QPoint(x1, y1));
        const int e2 = 2 * err;
        if (e2 <= dy) {
            ++y0;
            --y1;
            err += dy += a;
        }
        if (e2 >= dx || 2 * err > dy) {
            ++x0;
            --x1;
            err += dx += b1;
        }
    } while (x0 <= x1);

    while (y0 - y1 < b) {
        pts.append(QPoint(x0 - 1, y0));
        pts.append(QPoint(x1 + 1, y0));
        pts.append(QPoint(x0 - 1, y1));
        pts.append(QPoint(x1 + 1, y1));
        ++y0;
        --y1;
    }
    return pts;
}

} // namespace

QVector<int> brushIndices(int centerIndex, int size, const Grid &grid)
{
    int cx = 0, cy = 0;
    idxToXY(centerIndex, grid, &cx, &cy);
    const int half = (size - 1) / 2;
    QSet<int> result;
    for (int dy = -half; dy < size - half; ++dy) {
        for (int dx = -half; dx < size - half; ++dx) {
            const int nx = cx + dx, ny = cy + dy;
            if (inBounds(nx, ny, grid))
                result.insert(ny * grid.width + nx);
        }
    }
    return uniqueSorted(result);
}

QVector<int> lineIndices(int startIndex, int endIndex, int size, const Grid &grid)
{
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    idxToXY(startIndex, grid, &x0, &y0);
    idxToXY(endIndex, grid, &x1, &y1);
    QSet<int> result;
    int x = x0, y = y0;
    const int dx = qAbs(x1 - x0), dy = qAbs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        for (int idx : brushIndices(y * grid.width + x, size, grid))
            result.insert(idx);
        if (x == x1 && y == y1)
            break;
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
    return uniqueSorted(result);
}

QVector<int> shapeIndices(DrawTool tool, int startIndex, int endIndex, int size, const Grid &grid)
{
    if (tool == DrawTool::Line)
        return lineIndices(startIndex, endIndex, size, grid);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    idxToXY(startIndex, grid, &x0, &y0);
    idxToXY(endIndex, grid, &x1, &y1);
    if (x0 == x1 && y0 == y1)
        return stampPath({QPoint(x0, y0)}, size, grid);

    QVector<QPoint> coords;
    if (tool == DrawTool::Rect)
        coords = rectCoords(x0, y0, x1, y1);
    else if (tool == DrawTool::RoundRect)
        coords = roundedRectCoords(x0, y0, x1, y1);
    else
        coords = ellipseCoords(x0, y0, x1, y1);
    return stampPath(coords, size, grid);
}

QVector<int> fillIndices(int startIndex, const QVector<int> &data, const Grid &grid)
{
    if (startIndex < 0 || startIndex >= data.size() || grid.width <= 0)
        return {};
    const int target = data.at(startIndex);
    QVector<char> visited(data.size(), 0);
    QQueue<int> queue;
    QVector<int> result;
    queue.enqueue(startIndex);
    visited[startIndex] = 1;
    while (!queue.isEmpty()) {
        const int idx = queue.dequeue();
        result.append(idx);
        int x = 0, y = 0;
        idxToXY(idx, grid, &x, &y);
        const int nx[4] = {x - 1, x + 1, x, x};
        const int ny[4] = {y, y, y - 1, y + 1};
        for (int i = 0; i < 4; ++i) {
            if (!inBounds(nx[i], ny[i], grid))
                continue;
            const int ni = ny[i] * grid.width + nx[i];
            // inBounds() checks the grid, but `visited`/`data` are sized to the
            // data; a grid larger than the data would index past them.
            if (ni >= data.size() || visited[ni] || data[ni] != target)
                continue;
            visited[ni] = 1;
            queue.enqueue(ni);
        }
    }
    return result;
}

} // namespace pist
