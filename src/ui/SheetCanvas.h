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
/// drag phases to move their origin. Unplaced phases wait in a staging
/// gutter to the left of the sheet and can be dragged straight onto it. A
/// raw imported sheet can sit underneath while it is being sliced.
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
    /// Show/hide the imported sheet's pixels (the import reference) without
    /// losing them; hidden, the canvas shows only the composed phases.
    void setUnderlayVisible(bool on);
    void setSelectedPhase(int index);
    /// Cells about to be sliced into a new phase, drawn as yellow boxes so
    /// the dialog's numbers can be checked against the art. Image-space.
    void setSlicePreview(const QVector<QRect> &cells);
    void clearSlicePreview();
    void setScale(int scale);
    int scale() const { return m_scale; }
    int sheetIndex() const { return m_sheetIndex; }

    QSize sizeHint() const override;

signals:
    /// A strip was pressed. -1 when the press landed on empty space.
    void phaseSelected(int index);
    /// A drag finished: the phase's new origin, in sheet pixels.
    void phaseMoved(int index, int x, int y);
    /// An unplaced strip was dropped on the sheet: place it there.
    void phasePlaced(int index, int x, int y);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QPoint sheetOrigin() const;
    QRect sheetRect() const;
    QRect stripRect(const ImagePhase &phase, int ordinal, int dx, int dy) const;
    int stagingOrdinal(const ImagePhase &phase) const;
    void endDrag();

    ImageDocument *m_doc = nullptr;
    int m_sheetIndex = 0;
    int m_scale = 1;
    int m_selected = -1;
    QImage m_underlay;
    bool m_underlayVisible = true;
    QVector<QRect> m_slicePreview;
    bool m_dragging = false;
    bool m_dragFromStaging = false;
    int m_dragPhase = -1;
    QPoint m_dragOrigin;   // strip origin at press, in scale space
    QPoint m_pressPos;     // mouse position at press, in scale space
    QPoint m_dragOffset;   // live translation of the ghost, in scale space
};

} // namespace pist
