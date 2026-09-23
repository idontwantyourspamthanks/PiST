// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"
#include "image/Palette.h"
#include "image/StFormats.h"

#include <QDialog>
#include <QHash>
#include <QImage>
#include <QString>
#include <QVector>

#include <functional>

class QLineEdit;
class QSpinBox;

namespace pist {

/// The "slice a phase out of the imported sheet" dialog. Every field change
/// repaints the preview boxes on the sheet canvas, so the numbers can be
/// checked against the art before anything is cut.
class SlicePhaseDialog : public QDialog
{
public:
    SlicePhaseDialog(QWidget *parent, const QString &suggestedName, int defaultW,
                     int defaultH, int sheetWidth);

    struct Values {
        QString name;
        int x = 0;
        int y = 0;
        int cellW = 32;
        int cellH = 32;
        int count = 1;
    };

    Values values() const;

    std::function<void(const Values &)> onChanged;

private:
    QSpinBox *makeSpin(const QString &objectName, int value, int min, int max);

    QLineEdit *m_name = nullptr;
    QSpinBox *m_w = nullptr;
    QSpinBox *m_h = nullptr;
    QSpinBox *m_x = nullptr;
    QSpinBox *m_y = nullptr;
    QSpinBox *m_count = nullptr;
};

/// The sheet pixels a document's phases are cut from, and the cuts themselves:
/// the imported-sheet cache behind spritesheet mode (per-sheet pixels plus the
/// underlay the sheet canvas draws beneath the layout), and the slice/re-slice
/// operations that fill a phase's frames from the sheet art under them. The
/// document is passed in per call — the editor owns it — so the cache follows
/// whatever document the editor currently holds.
///
/// The operations mutate pixels only; the caller owns the undo entry (a
/// `pushSnapshot` around the call) and the refresh/`notifyModified` choreography
/// that follows, which is why the slice operations report whether they wrote
/// anything instead of announcing it themselves.
class SheetSlicing
{
public:
    /// (Re)load the pixel data of every sheet target that has a readable file,
    /// so slicing works after a document is reopened. Sheets without a path or
    /// with a missing file stay pixel-less until re-imported.
    void load(const ImageDocument &doc);
    /// Drop every cached sheet — the document they described is gone.
    void clear();
    bool hasSheet(int sheetIndex) const { return m_pixels.contains(sheetIndex); }
    /// Whether any sheet's pixels are cached at all.
    bool hasAnyPixels() const { return !m_pixels.isEmpty(); }
    /// Register `sheet`'s pixels for `sheetIndex`, with the underlay image the
    /// sheet canvas draws under the layout.
    void insert(int sheetIndex, const ImportedSheet &sheet, PaletteKind kind);
    /// The underlay for `sheetIndex`; an empty image when there is none.
    QImage underlay(int sheetIndex) const { return m_underlays.value(sheetIndex); }

    /// Fill a placed phase's still-empty frames from the sheet pixels under
    /// them (placement and frame adds pull the art; drawn frames survive).
    /// Returns whether anything was written.
    bool slicePlacedPhaseFrames(ImageDocument &doc, int phaseIndex);
    /// The frame just added to a placed phase extends the strip: pull the next
    /// cell from the sheet's pixels when they are available.
    void sliceAddedFrame(ImageDocument &doc);
    /// After a phase moved, re-slice the frames that still match the cells they
    /// were cut from (or are empty) — the strip follows the sheet until a frame
    /// is edited; edited frames keep their pixels. Returns whether anything was
    /// written.
    bool reSliceUntouchedFrames(ImageDocument &doc, int phaseIndex, int oldSheet, int oldX,
                                int oldY);
    /// Slice `count` cells out of an imported sheet into a new phase — the
    /// spritesheet-mode "add a phase over the sheet" flow. Returns the new phase
    /// index, or -1 when the sheet has no imported pixels.
    int addPhaseFromSheet(ImageDocument &doc, int sheetIndex, const QString &name, int x, int y,
                          int cellW, int cellH, int count);

private:
    QHash<int, ImportedSheet> m_pixels;
    QHash<int, QImage> m_underlays;
};

} // namespace pist
