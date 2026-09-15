// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"
#include "image/Transform.h"

#include <QString>
#include <QVector>

namespace pist {

struct ImageLayer {
    QString name;
    bool visible = true;
    QVector<int> pixels;
};

/// One animation frame of a phase: a bottom-first layer stack plus a
/// maintained composite (`kTransparent` = empty). Paint lands on the active
/// layer.
struct ImageFrame {
    QVector<ImageLayer> layers;
    QVector<int> composite;
};

/// A sprite-sheet file the document's phases are laid out on. The pixels
/// live in the file; the document keeps the target path and geometry.
struct ImageSheet {
    QString path;
    int width = 320;
    int height = 200;
};

/// One animation of the sprite set: `frames.size()` cells of cellW×cellH,
/// owned outright by the phase — each phase has its own cell size, so a
/// document can mix 32×32 characters with a 64×64 boss. `sheet`/`x`/`y`
/// place the strip on a sheet: frame k is painted at [x + k*cellW, y].
/// `sheet` indexes the document's sheets, or is -1 while the phase is
/// unplaced. A phase always holds at least one frame.
struct ImagePhase {
    QString name;
    int cellW = 32;
    int cellH = 32;
    int sheet = -1;
    int x = 0;
    int y = 0;
    QVector<ImageFrame> frames;
};

/// In-memory sprite set: an ST palette cube, up to 16 active colours, and
/// phases that each own their frames. The editor edits one phase at a time;
/// the "canvas" is the current phase's cell size. Width/height/frame
/// accessors delegate to the current phase and frame.
class ImageDocument
{
public:
    static constexpr int kMinSize = 1;
    static constexpr int kMaxWidth = kStScreenWidth;
    static constexpr int kMaxHeight = kStScreenHeight;

    ImageDocument();

    /// Blank document of one phase with `width`×`height` cells and one
    /// transparent frame.
    static ImageDocument create(int width, int height, PaletteKind kind);

    /// The current phase's cell size: the canvas the editor edits.
    int width() const { return phase().cellW; }
    int height() const { return phase().cellH; }
    int pixelCount() const { return width() * height(); }
    PaletteKind paletteKind() const { return m_kind; }
    const QVector<int> &active() const { return m_active; }
    int background() const { return m_background; }

    int phaseCount() const { return m_phases.size(); }
    int currentPhase() const { return m_currentPhase; }
    const QVector<ImagePhase> &phases() const { return m_phases; }
    const QVector<ImageSheet> &sheets() const { return m_sheets; }

    /// Frames of the current phase.
    int frameCount() const { return phase().frames.size(); }
    int currentFrame() const { return m_currentFrame; }
    /// Composite of the current phase's frame `index` (what the viewer sees).
    const QVector<int> &frame(int index) const;
    /// Composite of the current frame.
    const QVector<int> &pixels() const { return phase().frames.at(m_currentFrame).composite; }
    /// Active layer's pixels (fill / stroke source).
    const QVector<int> &activeLayerPixels() const;

    int layerCount() const;
    int activeLayer() const { return m_activeLayer; }
    const QVector<ImageLayer> &layers() const { return phase().frames.at(m_currentFrame).layers; }

    bool isModified() const { return m_modified; }
    void setModified(bool on) { m_modified = on; }

    QString lastError() const { return m_lastError; }

    bool load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;
    QByteArray toJson() const;
    bool fromJson(const QByteArray &json, QString *error = nullptr);

    /// Replace the whole document (used by import-as-replace).
    void replaceWith(const ImageDocument &other);

    bool setCurrentPhase(int index);
    /// Add an empty phase named `name` with `cellW`×`cellH` cells and one
    /// transparent frame; returns its index and makes it current.
    int addPhase(const QString &name, int cellW, int cellH);
    /// Change a phase's cell size. Frames keep their content anchored at the
    /// top-left: pixels that no longer fit are cropped, newly exposed areas
    /// are transparent.
    bool setPhaseCellSize(int phaseIndex, int width, int height, QString *error = nullptr);
    bool removePhase(int index);
    bool renamePhase(int index, const QString &name);
    /// Place a phase's strip on a sheet; `sheet` -1 marks it unplaced.
    bool setPhasePlacement(int index, int sheet, int x, int y);
    /// Register a sheet target; returns its index.
    int addSheet(const QString &path, int width, int height);

    bool setCurrentFrame(int index);
    /// Insert a blank frame after the current one (matching its layer names).
    int addFrame();
    /// Insert a copy of `source` after the current frame and select it.
    int duplicateFrame(int source);
    /// Remove `index` when more than one frame remains.
    bool removeFrame(int index);
    /// Move the frame at `from` to `to`. Current-frame selection follows.
    bool moveFrame(int from, int to);

    bool setActiveLayer(int index);
    int addLayer();
    bool removeLayer(int index);
    bool moveLayer(int from, int to);
    bool renameLayer(int index, const QString &name);
    bool setLayerVisible(int index, bool visible);

    void setActive(const QVector<int> &indices);
    bool toggleActive(int cubeIndex);
    void setBackground(int cubeIndex);
    void setPaletteKind(PaletteKind kind);

    /// Paint on the active layer of the current frame (or `layer` if >= 0).
    void setPixel(int index, int cubeIndex);
    void fillIndices(const QVector<int> &indices, int cubeIndex, int layer = -1);
    void restoreIndices(const QVector<int> &indices, const QVector<int> &values, int layer = -1);
    /// Replace the active layer's buffer, then remesh the composite.
    bool replaceActiveLayer(const QVector<int> &pixels);

    bool flipActiveLayer(FlipDirection direction);
    bool shiftActiveLayer(ShiftDirection direction);
    /// Insert `count-1` rotated copies of the current frame after it. Square only.
    int generateRotations(int count);

    /// Cube indices painted in the current composite that are not in `active()`.
    QVector<int> overspill() const;

    QColor displayColor(int cubeIndex) const;

private:
    const ImagePhase &phase() const { return m_phases.at(m_currentPhase); }
    ImagePhase &phase();
    ImageFrame *frameAt(int index);
    const ImageFrame *frameAt(int index) const;

    static QVector<int> resizedPixels(const QVector<int> &pixels, int oldW, int oldH,
                                      int newW, int newH);
    QVector<int> blankPixels() const;
    ImageLayer makeLayer(const QString &name) const;
    ImageFrame blankFrame() const;
    ImageFrame blankFrameFrom(const ImageFrame &templateFrame) const;
    void remesh(ImageFrame &frame) const;
    void remeshCurrent();
    void clampActiveLayer();
    int resolvedLayer(int layer) const;
    ImageLayer *layerAt(int layer);
    const ImageLayer *layerAt(int layer) const;
    static int mergedPixel(const ImageFrame &frame, int index);
    void touch() { m_modified = true; }

    PaletteKind m_kind = PaletteKind::Ste;
    QVector<int> m_active;
    int m_background = 0;
    QVector<ImageSheet> m_sheets;
    QVector<ImagePhase> m_phases;
    int m_currentPhase = 0;
    int m_currentFrame = 0;
    int m_activeLayer = 0;
    bool m_modified = false;
    mutable QString m_lastError;
};

} // namespace pist
