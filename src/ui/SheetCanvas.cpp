// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SheetCanvas.h"

#include <QMouseEvent>
#include <QPainter>

namespace pist {

namespace {
constexpr int kMargin = 12;   // around the sheet, in scale units
constexpr int kStagingX = 4;  // staging gutter strips start here
constexpr int kStagingGap = 20;
} // namespace

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

void SheetCanvas::setUnderlayVisible(bool on)
{
    m_underlayVisible = on;
    update();
}

void SheetCanvas::setSelectedPhase(int index)
{
    m_selected = index;
    update();
}

void SheetCanvas::setSlicePreview(const QVector<QRect> &cells)
{
    m_slicePreview = cells;
    update();
}

void SheetCanvas::clearSlicePreview()
{
    m_slicePreview.clear();
    update();
}

void SheetCanvas::setScale(int scale)
{
    m_scale = qBound(1, scale, 8);
    updateGeometry();
    adjustSize();
    update();
}

QPoint SheetCanvas::sheetOrigin() const
{
    // The staging gutter sits left of the sheet; its width follows the
    // widest unplaced strip so nothing is clipped.
    int widest = 0;
    bool any = false;
    for (const ImagePhase &phase : m_doc->phases()) {
        if (phase.sheet != -1)
            continue;
        any = true;
        widest = qMax(widest, phase.frames.size() * phase.cellW);
    }
    const int gutter = any ? kStagingX + widest + 16 : kMargin;
    return {gutter, kMargin};
}

QRect SheetCanvas::sheetRect() const
{
    const ImageSheet sheet = m_doc->sheets().value(m_sheetIndex);
    const QPoint origin = sheetOrigin();
    return QRect(origin, QSize(sheet.width, sheet.height));
}

int SheetCanvas::stagingOrdinal(const ImagePhase &phase) const
{
    int ordinal = 0;
    for (const ImagePhase &candidate : m_doc->phases()) {
        if (&candidate == &phase)
            break;
        if (candidate.sheet == -1)
            ++ordinal;
    }
    return ordinal;
}

QRect SheetCanvas::stripRect(const ImagePhase &phase, int ordinal, int dx, int dy) const
{
    const int w = phase.frames.size() * phase.cellW;
    if (phase.sheet == -1) {
        // Staging: unplaced strips stack top-down in the gutter.
        const int y = kMargin + ordinal * (phase.cellH + kStagingGap);
        return QRect(kStagingX + dx, y + dy, w, phase.cellH);
    }
    const QPoint origin = sheetOrigin();
    return QRect(origin.x() + phase.x + dx, origin.y() + phase.y + dy, w, phase.cellH);
}

QSize SheetCanvas::sizeHint() const
{
    if (!m_doc || m_sheetIndex < 0 || m_sheetIndex >= m_doc->sheets().size())
        return {340, 224};
    const ImageSheet sheet = m_doc->sheets().at(m_sheetIndex);
    const QPoint origin = sheetOrigin();
    return {(origin.x() + sheet.width + kMargin) * m_scale,
            (origin.y() * 2 + sheet.height) * m_scale};
}

void SheetCanvas::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(30, 30, 30));
    if (!m_doc || m_sheetIndex < 0 || m_sheetIndex >= m_doc->sheets().size()) {
        p.setPen(QColor(255, 255, 255, 140));
        p.drawText(rect(), Qt::AlignCenter,
                   tr("No sprite sheet yet — New sheet creates a 320×200 target "
                      "for the phases."));
        return;
    }
    const ImageSheet sheet = m_doc->sheets().at(m_sheetIndex);
    const QRect sheetRectPx(sheetRect().x() * m_scale, sheetRect().y() * m_scale,
                            sheet.width * m_scale, sheet.height * m_scale);
    p.fillRect(sheetRectPx, QColor(50, 50, 50));

    if (m_underlayVisible && !m_underlay.isNull()) {
        // The freshly imported file, dimmed so placed strips stand out.
        p.setOpacity(0.45);
        p.drawImage(sheetRectPx, m_underlay);
        p.setOpacity(1.0);
    }

    for (int i = 0; i < m_doc->phases().size(); ++i) {
        const ImagePhase &phase = m_doc->phases().at(i);
        if (phase.sheet != m_sheetIndex && phase.sheet != -1)
            continue;
        const int dx = m_dragging && m_dragPhase == i ? m_dragOffset.x() : 0;
        const int dy = m_dragging && m_dragPhase == i ? m_dragOffset.y() : 0;
        const QRect stripScale = stripRect(phase, stagingOrdinal(phase), dx, dy);
        const QRect stripPx(stripScale.x() * m_scale, stripScale.y() * m_scale,
                            stripScale.width() * m_scale, stripScale.height() * m_scale);

        for (int k = 0; k < phase.frames.size(); ++k) {
            const QVector<int> &composite = phase.frames.at(k).composite;
            const int ox = stripPx.x() + k * phase.cellW * m_scale;
            for (int row = 0; row < phase.cellH; ++row) {
                for (int col = 0; col < phase.cellW; ++col) {
                    const int value = composite.at(row * phase.cellW + col);
                    if (value < 0)
                        continue;
                    const Rgb rgb = cubeRgb(m_doc->paletteKind(), value);
                    p.fillRect(ox + col * m_scale, stripPx.y() + row * m_scale, m_scale,
                               m_scale, qRgb(rgb.r, rgb.g, rgb.b));
                }
            }
        }

        const bool selected = i == m_selected;
        QPen halo(QColor(0, 0, 0, 160));
        halo.setWidth(3);
        p.setPen(halo);
        p.setBrush(Qt::NoBrush);
        p.drawRect(stripPx);
        QPen outline(selected ? QColor(255, 220, 0) : QColor(255, 255, 255, 190));
        outline.setStyle(selected ? Qt::SolidLine : Qt::DashLine);
        p.setPen(outline);
        p.drawRect(stripPx);
        p.setPen(phase.sheet == -1 ? QColor(255, 255, 255, 130) : QColor(255, 255, 255, 220));
        p.drawText(stripPx.adjusted(0, -14, 0, 0), phase.name);

        // A placed strip that runs off the sheet would be clipped by the
        // export.
        if (phase.sheet >= 0 && !sheetRectPx.contains(stripPx)) {
            QPen overflow(QColor(255, 80, 80), 2, Qt::DashLine);
            p.setPen(overflow);
            p.drawRect(stripPx);
        }
    }

    // Pending slice: one yellow box per cell the dialog will cut.
    for (const QRect &cell : m_slicePreview) {
        QPen pen(QColor(255, 220, 0), 2, Qt::DashLine);
        p.setPen(pen);
        p.setBrush(QColor(255, 220, 0, 40));
        p.drawRect(cell.x() * m_scale, cell.y() * m_scale, cell.width() * m_scale,
                   cell.height() * m_scale);
    }

    if (m_doc->sheets().size() > 0) {
        bool anyUnplaced = false;
        for (const ImagePhase &phase : m_doc->phases())
            anyUnplaced |= phase.sheet == -1;
        if (anyUnplaced) {
            p.setPen(QColor(255, 255, 255, 120));
            p.drawText(QRect(0, 0, sheetRectPx.x(), 14), Qt::AlignCenter, tr("unplaced"));
        }
    }
}

void SheetCanvas::mousePressEvent(QMouseEvent *event)
{
    if (!m_doc || event->button() != Qt::LeftButton)
        return;
    const QPoint pos = event->pos() / m_scale;
    int hit = -1;
    bool fromStaging = false;
    for (int i = m_doc->phases().size() - 1; i >= 0; --i) {
        const ImagePhase &phase = m_doc->phases().at(i);
        if (phase.sheet != m_sheetIndex && phase.sheet != -1)
            continue;
        if (stripRect(phase, stagingOrdinal(phase), 0, 0).contains(pos)) {
            hit = i;
            fromStaging = phase.sheet == -1;
            break;
        }
    }
    m_selected = hit;
    update();
    emit phaseSelected(hit);
    if (hit < 0)
        return;
    const ImagePhase &phase = m_doc->phases().at(hit);
    const QRect strip = stripRect(phase, stagingOrdinal(phase), 0, 0);
    m_dragging = true;
    m_dragPhase = hit;
    m_dragFromStaging = fromStaging;
    m_dragOrigin = strip.topLeft();
    m_pressPos = pos;
    m_dragOffset = QPoint();
}

void SheetCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging)
        return;
    m_dragOffset = event->pos() / m_scale - m_pressPos;
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
    const bool fromStaging = m_dragFromStaging;
    const QPoint origin = m_dragOrigin + m_dragOffset;
    m_dragPhase = -1;
    m_dragOffset = QPoint();
    update();
    if (phase < 0)
        return;

    if (fromStaging) {
        // Dropping a staged strip on the sheet places it with its origin at
        // the drop point; anywhere else leaves it unplaced.
        if (sheetRect().contains(origin)
            && sheetRect().contains(origin + QPoint(1, 1))) {
            const QPoint sheetPos = origin - sheetOrigin();
            emit phasePlaced(phase, sheetPos.x(), sheetPos.y());
        }
        return;
    }
    const ImagePhase &moved = m_doc->phases().at(phase);
    const QPoint sheetPos = origin - sheetOrigin();
    if (sheetPos.x() != moved.x || sheetPos.y() != moved.y)
        emit phaseMoved(phase, sheetPos.x(), sheetPos.y());
}

} // namespace pist
