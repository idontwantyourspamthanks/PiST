// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ImageCanvas.h"

#include "ui/Appearance.h"

#include <QKeyEvent>
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
    // Not rebuilt here: paintEvent owns the rebuild, and doing it in both
    // places was the same 320x200 ARGB32 image built twice per event.
    m_imageDirty = true;
    updateGeometry();
    update();
}

void ImageCanvas::invalidateImage()
{
    if (m_imageDirty)
        return;
    m_imageDirty = true;
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

void ImageCanvas::setOnion(const QVector<int> &pixels, qreal opacity)
{
    m_onion = pixels;
    m_onionOpacity = qBound(0.0, opacity, 1.0);
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

void ImageCanvas::setSelection(const QRect &rect)
{
    const QRect clipped = rect.isNull() ? QRect() : rect.normalized().intersected(gridRect());
    m_selection = clipped.isEmpty() ? QRect() : clipped;
    update();
}

bool ImageCanvas::hasSelection() const
{
    return !m_selection.isEmpty();
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

QPoint ImageCanvas::clampedCell(const QPoint &pos) const
{
    if (!m_doc || m_cellSize <= 0)
        return {};
    return {qBound(0, pos.x() / m_cellSize, m_doc->width() - 1),
            qBound(0, pos.y() / m_cellSize, m_doc->height() - 1)};
}

QRect ImageCanvas::gridRect() const
{
    if (!m_doc)
        return {};
    return {0, 0, m_doc->width(), m_doc->height()};
}

QImage ImageCanvas::movePatch() const
{
    if (!m_doc || !hasSelection())
        return QImage();
    // The patch is the selection drawn as the canvas draws it: empty cells
    // transparent, so the grid shows through the moving block.
    return indicesToImage(m_doc->activeLayerPixels(), m_doc->width(), m_doc->height(),
                          m_doc->paletteKind(), EmptyStyle::Transparent, m_selection);
}

void ImageCanvas::drawMarchingAnts(QPainter &p, const QRect &cells) const
{
    if (cells.isEmpty())
        return;
    const QRectF r(cells.left() * m_cellSize + 0.5, cells.top() * m_cellSize + 0.5,
                   cells.width() * m_cellSize - 1.0, cells.height() * m_cellSize - 1.0);
    QPen white(Qt::white, 1, Qt::CustomDashLine, Qt::SquareCap, Qt::MiterJoin);
    white.setDashPattern({3, 3});
    QPen black(Qt::black, 1, Qt::CustomDashLine, Qt::SquareCap, Qt::MiterJoin);
    black.setDashPattern({3, 3});
    black.setDashOffset(3);
    p.setBrush(Qt::NoBrush);
    p.setPen(white);
    p.drawRect(r);
    p.setPen(black);
    p.drawRect(r);
}

void ImageCanvas::rebuildImage()
{
    m_imageDirty = false;
    if (!m_doc) {
        m_logical = QImage();
        return;
    }
    m_logical = indicesToImage(m_doc->pixels(), m_doc->width(), m_doc->height(),
                               m_doc->paletteKind(), EmptyStyle::Checkerboard);
}

void ImageCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), appearance::colors().gutter);
    if (!m_doc)
        return;

    // One rebuild per paint, and only when the pixels actually changed: a
    // repaint for an expose, a scroll or a tool change reuses m_logical.
    if (m_imageDirty)
        rebuildImage();
    if (m_logical.isNull())
        return;
    // The drag overlays below are painted on top of the frame, never into the
    // cache: the cached image is the document's pixels, or the next repaint
    // would show the last overlay baked in. (Rebuilding on every paint used to
    // hide that; the copy is far cheaper than the rebuild it replaces.)
    const bool overlaid = (m_moving && hasSelection()) || !m_preview.isEmpty();
    QImage frame = overlaid ? m_logical.copy() : m_logical;
    // While a move drag is live the source area reads as cut out already.
    if (m_moving && hasSelection()) {
        for (int y = m_selection.top(); y <= m_selection.bottom(); ++y) {
            for (int x = m_selection.left(); x <= m_selection.right(); ++x) {
                const bool checker = ((x + y) & 1) == 0;
                frame.setPixel(x, y, checker ? qRgb(40, 40, 40) : qRgb(70, 70, 70));
            }
        }
    }
    if (!m_preview.isEmpty()) {
        const Rgb rgb = m_previewColour < 0 ? Rgb{70, 70, 70}
                                            : cubeRgb(m_doc->paletteKind(), m_previewColour);
        const QRgb preview = qRgb(rgb.r, rgb.g, rgb.b);
        for (int index : m_preview) {
            if (index < 0 || index >= m_doc->pixelCount())
                continue;
            const int x = index % m_doc->width();
            const int y = index / m_doc->width();
            frame.setPixel(x, y, preview);
        }
    }

    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    const QRect dest(0, 0, m_doc->width() * m_cellSize, m_doc->height() * m_cellSize);
    p.drawImage(dest, frame);

    if (m_moving && !m_movePatch.isNull()) {
        const QRectF drop(m_selection.translated(m_moveDelta));
        p.drawImage(QRectF(drop.left() * m_cellSize, drop.top() * m_cellSize,
                           drop.width() * m_cellSize, drop.height() * m_cellSize),
                    m_movePatch);
    }

    if (!m_onion.isEmpty() && m_onionOpacity > 0 && m_onion.size() >= m_doc->pixelCount()) {
        const QImage ghost = indicesToImage(m_onion, m_doc->width(), m_doc->height(),
                                            m_doc->paletteKind(), EmptyStyle::Transparent);
        p.setOpacity(m_onionOpacity);
        p.drawImage(dest, ghost);
        p.setOpacity(1.0);
    }

    if (m_showGrid && m_cellSize >= 4) {
        p.setPen(QColor(0, 0, 0, 80));
        for (int x = 0; x <= m_doc->width(); ++x)
            p.drawLine(x * m_cellSize, 0, x * m_cellSize, m_doc->height() * m_cellSize);
        for (int y = 0; y <= m_doc->height(); ++y)
            p.drawLine(0, y * m_cellSize, m_doc->width() * m_cellSize, y * m_cellSize);
    }

    drawMarchingAnts(p, m_moving ? m_selection.translated(m_moveDelta)
                                 : (m_marquee.isEmpty() ? m_selection : m_marquee));
}

void ImageCanvas::applyAt(int index, bool erase)
{
    if (!m_doc || index < 0)
        return;
    const int colour = erase ? kTransparent : m_colour;
    QVector<int> indices;
    if (m_tool == DrawTool::Fill)
        indices = fillIndices(index, m_doc->activeLayerPixels(), grid());
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

    if (m_tool == DrawTool::Select) {
        if (event->button() != Qt::LeftButton)
            return;
        const QPoint cell(index % m_doc->width(), index / m_doc->width());
        if (hasSelection() && m_selection.contains(cell)) {
            m_moving = true;
            m_moveStartCell = cell;
            m_moveDelta = QPoint(0, 0);
            m_movePatch = movePatch();
        } else {
            m_selecting = true;
            m_selectStart = cell;
            m_marquee = QRect();
        }
        update();
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
    if (m_selecting) {
        m_marquee = QRect(m_selectStart, clampedCell(event->pos())).normalized();
        update();
        return;
    }
    if (m_moving) {
        m_moveDelta = clampedCell(event->pos()) - m_moveStartCell;
        update();
        return;
    }
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
    if (m_selecting) {
        m_selecting = false;
        // Recompute from the release position: move events can be compressed
        // (or missed), so the release point is authoritative. A drag that
        // never left the start cell is a click, which collapses the selection.
        const QPoint end = clampedCell(event->pos());
        m_marquee = m_selectStart == end ? QRect() : QRect(m_selectStart, end).normalized();
        m_selection = m_marquee;
        m_marquee = QRect();
        update();
        emit selectionMade(m_selection);
        return;
    }
    if (m_moving) {
        m_moving = false;
        const QRect source = m_selection;
        const QPoint delta = clampedCell(event->pos()) - m_moveStartCell;
        m_moveDelta = QPoint(0, 0);
        m_movePatch = QImage();
        update();
        if (!delta.isNull() && !source.isEmpty())
            emit selectionMoved(source, delta);
        return;
    }
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

void ImageCanvas::keyPressEvent(QKeyEvent *event)
{
    // With the select tool and an active selection the arrows move the
    // selection's pixels instead of shifting the whole layer.
    const bool moveSelection = m_tool == DrawTool::Select && hasSelection();
    switch (event->key()) {
    case Qt::Key_Left:
        if (moveSelection) {
            emit selectionNudged(QPoint(-1, 0));
            return;
        }
        emit shiftRequested(ShiftDirection::Left);
        return;
    case Qt::Key_Right:
        if (moveSelection) {
            emit selectionNudged(QPoint(1, 0));
            return;
        }
        emit shiftRequested(ShiftDirection::Right);
        return;
    case Qt::Key_Up:
        if (moveSelection) {
            emit selectionNudged(QPoint(0, -1));
            return;
        }
        emit shiftRequested(ShiftDirection::Up);
        return;
    case Qt::Key_Down:
        if (moveSelection) {
            emit selectionNudged(QPoint(0, 1));
            return;
        }
        emit shiftRequested(ShiftDirection::Down);
        return;
    default:
        QWidget::keyPressEvent(event);
        break;
    }
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
    case DrawTool::Select:
        kind = appearance::CanvasCursor::Selection;
        break;
    }
    QColor paint;
    if (kind == appearance::CanvasCursor::Brush && m_doc && m_colour >= 0)
        paint = m_doc->displayColor(m_colour);
    setCursor(appearance::canvasCursor(kind, paint));
}

} // namespace pist
