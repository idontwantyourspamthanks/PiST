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
    /// Overlay a neighbouring frame's composite at `opacity` (empty = off).
    void setOnion(const QVector<int> &pixels, qreal opacity = 0.5);
    void setCellSize(int size);
    int cellSize() const { return m_cellSize; }
    /// Cell-space rectangle of the selection (empty = none). Stored clamped
    /// to the grid.
    void setSelection(const QRect &rect);
    QRect selection() const { return m_selection; }
    bool hasSelection() const;

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
    void shiftRequested(ShiftDirection direction);
    /// Marquee drag finished; `rect` is empty when the drag was a click.
    void selectionMade(const QRect &rect);
    /// A drag inside the selection moved its pixels: `source` is the rect the
    /// patch was cut from, `delta` the offset it was dropped at.
    void selectionMoved(const QRect &source, const QPoint &delta);
    /// Arrow keys with the select tool: move the selection's pixels one cell.
    void selectionNudged(const QPoint &step);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    Grid grid() const;
    int indexAt(const QPoint &pos) const;
    QRect cellRect(int index) const;
    QPoint clampedCell(const QPoint &pos) const;
    QRect gridRect() const;
    void rebuildImage();
    void applyAt(int index, bool erase);
    void updateCursor();
    QImage movePatch() const;
    void drawMarchingAnts(QPainter &p, const QRect &cells) const;

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
    QVector<int> m_onion;
    qreal m_onionOpacity = 0.5;
    QImage m_logical;
    QRect m_selection;
    bool m_selecting = false;
    QPoint m_selectStart;
    QRect m_marquee;
    bool m_moving = false;
    QPoint m_moveStartCell;
    QPoint m_moveDelta;
    QImage m_movePatch;
};

} // namespace pist
