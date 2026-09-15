// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QVector>

namespace pist {

struct Grid {
    int width = 0;
    int height = 0;
};

enum class DrawTool {
    Brush,
    Line,
    Rect,
    RoundRect,
    Ellipse,
    Fill,
    Eyedropper,
};

inline bool isShapeTool(DrawTool tool)
{
    return tool == DrawTool::Line || tool == DrawTool::Rect || tool == DrawTool::RoundRect
        || tool == DrawTool::Ellipse;
}

QVector<int> brushIndices(int centerIndex, int size, const Grid &grid);
QVector<int> lineIndices(int startIndex, int endIndex, int size, const Grid &grid);
QVector<int> shapeIndices(DrawTool tool, int startIndex, int endIndex, int size, const Grid &grid);
QVector<int> fillIndices(int startIndex, const QVector<int> &data, const Grid &grid);

} // namespace pist
