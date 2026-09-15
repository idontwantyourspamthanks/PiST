// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ImageCanvas.h"

#include "ui/Appearance.h"

#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

namespace pist {

ImageCanvas::ImageCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    updateCursor();
}

void ImageCanvas::setDocument(ImageDocument *document)
{
    m_doc = document;
    rebuildImage();
    updateGeometry();
    update();
}

void ImageCanvas::setTool(DrawTool tool)
{
    m_tool = tool;
    updateCursor();
}

void ImageCanvas::setBrushSize(int size)
{
    m_brushSize = qBound(1, size, 8);
}

void ImageCanvas::setCurrentColour(int cubeIndex)
{
    m_colour = cubeIndex;
    updateCursor();
}

void ImageCanvas::setShowGrid(bool on)
{
    m_showGrid = on;
    update();
}

void ImageCanvas::setPreview(const QVector<int> &indices, int colour)
{
    m_preview = indices;
    m_previewColour = colour;
    update();
}

void ImageCanvas::clearPreview()
{
    m_preview.clear();
    update();
}

void ImageCanvas::setCellSize(int size)
{
    const int next = qBound(kMinCellSize, size, kMaxCellSize);
    if (next == m_cellSize)
        return;
    m_cellSize = next;
    updateGeometry();
    adjustSize();
    update();
    emit cellSizeChanged(m_cellSize);
}

QSize ImageCanvas::sizeHint() const
{
    if (!m_doc)
        return {256, 256};
    return {m_doc->width() * m_cellSize, m_doc->height() * m_cellSize};
}

Grid ImageCanvas::grid() const
{
    if (!m_doc)
        return {};
    return Grid{m_doc->width(), m_doc->height()};
}

int ImageCanvas::indexAt(const QPoint &pos) const
{
    if (!m_doc || m_cellSize <= 0)
        return -1;
    const int col = pos.x() / m_cellSize;
    const int row = pos.y() / m_cellSize;
    if (col < 0 || col >= m_doc->width() || row < 0 || row >= m_doc->height())
        return -1;
    return row * m_doc->width() + col;
}

QRect ImageCanvas::cellRect(int index) const
{
    if (!m_doc || m_doc->width() <= 0)
        return {};
    const int col = index % m_doc->width();
    const int row = index / m_doc->width();
    return {col * m_cellSize, row * m_cellSize, m_cellSize, m_cellSize};
}

void ImageCanvas::rebuildImage()
{
    if (!m_doc) {
        m_logical = QImage();
        return;
    }
    m_logical = QImage(m_doc->width(), m_doc->height(), QImage::Format_ARGB32);
    const QVector<int> &pixels = m_doc->pixels();
    for (int y = 0; y < m_doc->height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(m_logical.scanLine(y));
        for (int x = 0; x < m_doc->width(); ++x) {
            const int cube = pixels.at(y * m_doc->width() + x);
            if (cube < 0) {
                const bool checker = ((x + y) & 1) == 0;
                line[x] = checker ? qRgb(40, 40, 40) : qRgb(70, 70, 70);
            } else {
                const Rgb rgb = cubeRgb(m_doc->paletteKind(), cube);
                line[x] = qRgb(rgb.r, rgb.g, rgb.b);
            }
        }
    }
}

void ImageCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), appearance::colors().gutter);
    if (!m_doc || m_logical.isNull())
        return;

    rebuildImage();
    if (!m_preview.isEmpty()) {
        const Rgb rgb = m_previewColour < 0 ? Rgb{70, 70, 70}
                                            : cubeRgb(m_doc->paletteKind(), m_previewColour);
        const QRgb preview = qRgb(rgb.r, rgb.g, rgb.b);
        for (int index : m_preview) {
            if (index < 0 || index >= m_doc->pixelCount())
                continue;
            const int x = index % m_doc->width();
            const int y = index / m_doc->width();
            m_logical.setPixel(x, y, preview);
        }
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(QRect(0, 0, m_doc->width() * m_cellSize, m_doc->height() * m_cellSize), m_logical);

    if (m_showGrid && m_cellSize >= 4) {
        p.setPen(QColor(0, 0, 0, 80));
        for (int x = 0; x <= m_doc->width(); ++x)
            p.drawLine(x * m_cellSize, 0, x * m_cellSize, m_doc->height() * m_cellSize);
        for (int y = 0; y <= m_doc->height(); ++y)
            p.drawLine(0, y * m_cellSize, m_doc->width() * m_cellSize, y * m_cellSize);
    }
}

void ImageCanvas::applyAt(int index, bool erase)
{
    if (!m_doc || index < 0)
        return;
    const int colour = erase ? kTransparent : m_colour;
    QVector<int> indices;
    if (m_tool == DrawTool::Fill)
        indices = fillIndices(index, m_doc->pixels(), grid());
    else if (m_tool == DrawTool::Eyedropper) {
        const int cube = m_doc->pixels().at(index);
        emit colourPicked(cube);
        return;
    } else {
        indices = brushIndices(index, m_brushSize, grid());
        if (m_lastIndex >= 0 && m_lastIndex != index && m_tool == DrawTool::Brush) {
            for (int i : lineIndices(m_lastIndex, index, m_brushSize, grid()))
                if (!indices.contains(i))
                    indices.append(i);
        }
    }
    m_lastIndex = index;
    emit indicesPainted(indices, colour);
}

void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    const int index = indexAt(event->pos());
    emit cursorIndexChanged(index);
    if (index < 0)
        return;

    if (m_tool == DrawTool::Eyedropper || event->button() == Qt::MiddleButton) {
        emit colourPicked(m_doc->pixels().at(index));
        return;
    }

    m_erase = event->button() == Qt::RightButton;
    m_painting = true;
    m_lastIndex = -1;
    m_shapeStart = index;

    if (isShapeTool(m_tool)) {
        setPreview(shapeIndices(m_tool, m_shapeStart, index, m_brushSize, grid()),
                   m_erase ? kTransparent : m_colour);
        return;
    }
    applyAt(index, m_erase);
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const int index = indexAt(event->pos());
    emit cursorIndexChanged(index);
    if (!m_painting)
        return;
    if (isShapeTool(m_tool)) {
        if (index < 0)
            return;
        setPreview(shapeIndices(m_tool, m_shapeStart, index, m_brushSize, grid()),
                   m_erase ? kTransparent : m_colour);
        return;
    }
    if (index >= 0 && m_tool != DrawTool::Fill)
        applyAt(index, m_erase);
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_painting)
        return;
    m_painting = false;
    if (isShapeTool(m_tool)) {
        const int index = indexAt(event->pos());
        const int end = index >= 0 ? index : m_shapeStart;
        const int colour = m_erase ? kTransparent : m_colour;
        emit indicesPainted(shapeIndices(m_tool, m_shapeStart, end, m_brushSize, grid()), colour);
        clearPreview();
    }
    m_lastIndex = -1;
    emit strokeEnded();
}

void ImageCanvas::wheelEvent(QWheelEvent *event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QWidget::wheelEvent(event);
        return;
    }
    emit zoomStepsRequested(event->angleDelta().y() > 0 ? 1 : -1);
    event->accept();
}

void ImageCanvas::updateCursor()
{
    appearance::CanvasCursor kind = appearance::CanvasCursor::Crosshair;
    switch (m_tool) {
    case DrawTool::Brush:
        kind = appearance::CanvasCursor::Brush;
        break;
    case DrawTool::Fill:
        kind = appearance::CanvasCursor::Fill;
        break;
    case DrawTool::Eyedropper:
        kind = appearance::CanvasCursor::Eyedropper;
        break;
    case DrawTool::Line:
    case DrawTool::Rect:
    case DrawTool::RoundRect:
    case DrawTool::Ellipse:
        kind = appearance::CanvasCursor::Crosshair;
        break;
    }
    QColor paint;
    if (kind == appearance::CanvasCursor::Brush && m_doc && m_colour >= 0)
        paint = m_doc->displayColor(m_colour);
    setCursor(appearance::canvasCursor(kind, paint));
}

} // namespace pist
