// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SheetCanvas.h"

#include <QMouseEvent>
#include <QPainter>

namespace pist {

SheetCanvas::SheetCanvas(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void SheetCanvas::setDocument(ImageDocument *document)
{
    m_doc = document;
    updateGeometry();
    update();
}

void SheetCanvas::setSheetIndex(int index)
{
    m_sheetIndex = index;
    update();
}

void SheetCanvas::setUnderlay(const QImage &image)
{
    m_underlay = image;
    update();
}

void SheetCanvas::setSelectedPhase(int index)
{
    m_selected = index;
    update();
}

void SheetCanvas::setScale(int scale)
{
    m_scale = qBound(1, scale, 8);
    updateGeometry();
    adjustSize();
    update();
}

QSize SheetCanvas::sizeHint() const
{
    if (!m_doc)
        return {320, 200};
    const ImageSheet sheet = m_doc->sheets().value(m_sheetIndex);
    return {sheet.width * m_scale, sheet.height * m_scale};
}

QRect SheetCanvas::stripRect(const ImagePhase &phase, int dx, int dy) const
{
    const int w = phase.frames.size() * phase.cellW;
    return QRect((phase.x + dx) * m_scale, (phase.y + dy) * m_scale, w * m_scale,
                 phase.cellH * m_scale);
}

QPoint SheetCanvas::sheetCellAt(const QPoint &pos) const
{
    return {pos.x() / m_scale, pos.y() / m_scale};
}

void SheetCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));
    if (!m_doc || m_sheetIndex < 0 || m_sheetIndex >= m_doc->sheets().size())
        return;
    const ImageSheet sheet = m_doc->sheets().at(m_sheetIndex);
    const QRect sheetRect(0, 0, sheet.width * m_scale, sheet.height * m_scale);
    p.fillRect(sheetRect, QColor(50, 50, 50));

    if (!m_underlay.isNull()) {
        // The freshly imported file, dimmed so placed strips stand out.
        p.setOpacity(0.45);
        p.drawImage(sheetRect, m_underlay);
        p.setOpacity(1.0);
    }

    for (int i = 0; i < m_doc->phases().size(); ++i) {
        const ImagePhase &phase = m_doc->phases().at(i);
        if (phase.sheet != m_sheetIndex)
            continue;
        const int dx = m_dragging && m_dragPhase == i ? m_dragOffset.x() : 0;
        const int dy = m_dragging && m_dragPhase == i ? m_dragOffset.y() : 0;
        for (int k = 0; k < phase.frames.size(); ++k) {
            const QVector<int> &composite = phase.frames.at(k).composite;
            const int ox = (phase.x + dx + k * phase.cellW) * m_scale;
            const int oy = (phase.y + dy) * m_scale;
            for (int row = 0; row < phase.cellH; ++row) {
                for (int col = 0; col < phase.cellW; ++col) {
                    const int value = composite.at(row * phase.cellW + col);
                    if (value < 0)
                        continue;
                    const Rgb rgb = cubeRgb(m_doc->paletteKind(), value);
                    p.fillRect(ox + col * m_scale, oy + row * m_scale, m_scale, m_scale,
                               qRgb(rgb.r, rgb.g, rgb.b));
                }
            }
        }

        const QRect strip = stripRect(phase, dx, dy);
        QPen halo(QColor(0, 0, 0, 160));
        halo.setWidth(3);
        p.setPen(halo);
        p.setBrush(Qt::NoBrush);
        p.drawRect(strip);
        QPen outline(i == m_selected ? QColor(255, 220, 0) : QColor(255, 255, 255, 190));
        outline.setStyle(i == m_selected ? Qt::SolidLine : Qt::DashLine);
        p.setPen(outline);
        p.drawRect(strip);
        p.setPen(QColor(255, 255, 255, 220));
        p.drawText(strip.adjusted(0, -14, 0, 0), phase.name);

        // A strip that runs off the sheet would be clipped by the export.
        const QRect sheetBounds(0, 0, sheet.width * m_scale, sheet.height * m_scale);
        if (!sheetBounds.contains(strip)) {
            QPen overflow(QColor(255, 80, 80), 2, Qt::DashLine);
            p.setPen(overflow);
            p.drawRect(strip);
        }
    }
}

void SheetCanvas::mousePressEvent(QMouseEvent *event)
{
    if (!m_doc || event->button() != Qt::LeftButton)
        return;
    const QPoint cell = sheetCellAt(event->pos());
    int hit = -1;
    for (int i = m_doc->phases().size() - 1; i >= 0; --i) {
        const ImagePhase &phase = m_doc->phases().at(i);
        if (phase.sheet != m_sheetIndex)
            continue;
        if (stripRect(phase, 0, 0).contains(event->pos())) {
            hit = i;
            break;
        }
    }
    m_selected = hit;
    update();
    emit phaseSelected(hit);
    if (hit < 0)
        return;
    m_dragging = true;
    m_dragPhase = hit;
    m_dragOrigin = QPoint(m_doc->phases().at(hit).x, m_doc->phases().at(hit).y);
    m_pressCell = cell;
    m_dragOffset = QPoint();
}

void SheetCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    const QPoint cell = sheetCellAt(event->pos());
    m_dragOffset = cell - m_pressCell;
    update();
}

void SheetCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    endDrag();
}

void SheetCanvas::endDrag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    const int phase = m_dragPhase;
    const int x = m_dragOrigin.x() + m_dragOffset.x();
    const int y = m_dragOrigin.y() + m_dragOffset.y();
    m_dragPhase = -1;
    m_dragOffset = QPoint();
    update();
    if (phase >= 0 && (x != m_dragOrigin.x() || y != m_dragOrigin.y()))
        emit phaseMoved(phase, x, y);
}

} // namespace pist
