// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"
#include "image/Tools.h"

#include <QImage>
#include <QWidget>

namespace pist {

/// Pixel grid: paints the current frame at integer cell size, maps pointer
/// events to pixel indices, and previews shape tools while dragging.
class ImageCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit ImageCanvas(QWidget *parent = nullptr);

    void setDocument(ImageDocument *document);
    void setTool(DrawTool tool);
    void setBrushSize(int size);
    void setCurrentColour(int cubeIndex);
    void setShowGrid(bool on);
    void setPreview(const QVector<int> &indices, int colour);
    void clearPreview();
    void setCellSize(int size);
    int cellSize() const { return m_cellSize; }

    static constexpr int kMinCellSize = 1;
    static constexpr int kMaxCellSize = 128;

    QSize sizeHint() const override;

signals:
    void indicesPainted(const QVector<int> &indices, int colour);
    void strokeEnded();
    void colourPicked(int cubeIndex);
    void cursorIndexChanged(int index);
    void cellSizeChanged(int size);
    void zoomStepsRequested(int steps);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    Grid grid() const;
    int indexAt(const QPoint &pos) const;
    QRect cellRect(int index) const;
    void rebuildImage();
    void applyAt(int index, bool erase);
    void updateCursor();

    ImageDocument *m_doc = nullptr;
    DrawTool m_tool = DrawTool::Brush;
    int m_brushSize = 1;
    int m_colour = kTransparent;
    int m_cellSize = 16;
    bool m_showGrid = true;
    bool m_painting = false;
    bool m_erase = false;
    int m_shapeStart = -1;
    int m_lastIndex = -1;
    QVector<int> m_preview;
    int m_previewColour = kTransparent;
    QImage m_logical;
};

} // namespace pist
