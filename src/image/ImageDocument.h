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

/// A named rectangle on the canvas: one sprite of a sprite sheet. Optional
/// document metadata — a .pim without regions is a plain sprite document,
/// and v1 files load unchanged.
struct ImageRegion {
    QString name;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

/// In-memory sprite: size, ST palette cube, up to 16 active colours, and one
/// or more frames. Each frame is a bottom-first layer stack plus a maintained
/// composite (`kTransparent` = empty). Paint lands on the active layer.
class ImageDocument
{
public:
    static constexpr int kMinSize = 1;
    static constexpr int kMaxWidth = kStScreenWidth;
    static constexpr int kMaxHeight = kStScreenHeight;

    ImageDocument();

    /// Blank document of `width`×`height` with one transparent frame.
    static ImageDocument create(int width, int height, PaletteKind kind);

    int width() const { return m_width; }
    int height() const { return m_height; }
    int pixelCount() const { return m_width * m_height; }
    PaletteKind paletteKind() const { return m_kind; }
    const QVector<int> &active() const { return m_active; }
    int background() const { return m_background; }
    int frameCount() const { return m_frames.size(); }
    int currentFrame() const { return m_current; }
    /// Composite of the current frame (what the viewer sees).
    const QVector<int> &pixels() const { return m_frames.at(m_current).composite; }
    /// Composite of frame `index`.
    const QVector<int> &frame(int index) const { return m_frames.at(index).composite; }
    /// Active layer's pixels (fill / stroke source).
    const QVector<int> &activeLayerPixels() const;

    int layerCount() const;
    int activeLayer() const { return m_activeLayer; }
    const QVector<ImageLayer> &layers() const { return m_frames.at(m_current).layers; }
    const QVector<ImagePhase> &phases() const { return m_phases; }
    const QVector<ImageRegion> &regions() const { return m_regions; }
    void setRegions(const QVector<ImageRegion> &regions);

    /// The current frame (or `frame`) cropped to `region`, clipped to the
    /// canvas and sharing the palette: the basis of per-region export. A
    /// region that misses the canvas yields a 1×1 transparent document.
    ImageDocument cropped(const ImageRegion &region, int frame = -1) const;

    bool isModified() const { return m_modified; }
    void setModified(bool on) { m_modified = on; }

    QString lastError() const { return m_lastError; }

    bool load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;
    QByteArray toJson() const;
    bool fromJson(const QByteArray &json, QString *error = nullptr);

    /// Replace the whole document (used by import-as-replace).
    void replaceWith(const ImageDocument &other);

    bool setCurrentFrame(int index);
    /// Insert a blank frame after the current one (matching its layer names)
    /// and select it.
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

    int addPhase();
    bool removePhase(int index);
    bool renamePhase(int index, const QString &name);
    bool setPhaseRange(int index, int start, int end);

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
    struct Frame {
        QVector<ImageLayer> layers;
        QVector<int> composite;
    };

    bool setGeometry(int width, int height, QString *error);
    QVector<int> blankPixels() const;
    ImageLayer makeLayer(const QString &name) const;
    Frame blankFrame() const;
    Frame blankFrameFrom(const Frame &templateFrame) const;
    void remesh(Frame &frame) const;
    void remeshCurrent();
    void clampActiveLayer();
    int resolvedLayer(int layer) const;
    ImageLayer *layerAt(int layer);
    const ImageLayer *layerAt(int layer) const;
    static int mergedPixel(const Frame &frame, int index);
    void touch() { m_modified = true; }

    int m_width = 32;
    int m_height = 32;
    PaletteKind m_kind = PaletteKind::Ste;
    QVector<int> m_active;
    int m_background = 0;
    QVector<Frame> m_frames;
    QVector<ImagePhase> m_phases;
    QVector<ImageRegion> m_regions;
    int m_current = 0;
    int m_activeLayer = 0;
    bool m_modified = false;
    mutable QString m_lastError;
};

} // namespace pist
