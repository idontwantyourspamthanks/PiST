// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"

#include <QImage>
#include <QWidget>

namespace pist {

/// The composed sprite sheet: draws every phase placed on the current sheet
/// at its strip position, outlines each strip, and lets the user select and
/// drag phases to move their origin. A raw imported sheet can sit underneath
/// while it is being sliced into phases.
class SheetCanvas : public QWidget
{
    Q_OBJECT

public:
    explicit SheetCanvas(QWidget *parent = nullptr);

    void setDocument(ImageDocument *document);
    void setSheetIndex(int index);
    /// Raw pixels of a freshly imported sheet, drawn under the layout while
    /// it is being sliced; empty when there is none (reopened documents).
    void setUnderlay(const QImage &image);
    void setSelectedPhase(int index);
    void setScale(int scale);
    int scale() const { return m_scale; }

    QSize sizeHint() const override;

signals:
    /// A strip was pressed. -1 when the press landed on empty sheet.
    void phaseSelected(int index);
    /// A drag finished: the phase's new origin, in sheet pixels.
    void phaseMoved(int index, int x, int y);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QRect stripRect(const ImagePhase &phase, int dx, int dy) const;
    QPoint sheetCellAt(const QPoint &pos) const;
    void endDrag();

    ImageDocument *m_doc = nullptr;
    int m_sheetIndex = 0;
    int m_scale = 1;
    int m_selected = -1;
    QImage m_underlay;
    bool m_dragging = false;
    int m_dragPhase = -1;
    QPoint m_dragOrigin;
    QPoint m_pressCell;
    QPoint m_dragOffset;
};

} // namespace pist
