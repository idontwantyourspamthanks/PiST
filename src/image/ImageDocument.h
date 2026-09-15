// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"

#include <QString>
#include <QVector>

namespace pist {

/// In-memory sprite: size, ST palette cube, up to 16 active colours, and one
/// or more frames of cube indices (`kTransparent` = empty).
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
    const QVector<int> &pixels() const { return m_frames.at(m_current); }
    const QVector<int> &frame(int index) const { return m_frames.at(index); }

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
    /// Append a blank (transparent) frame and select it.
    int addFrame();
    /// Append a copy of `source` and select it. Returns the new index, or -1.
    int duplicateFrame(int source);
    /// Remove `index` when more than one frame remains.
    bool removeFrame(int index);

    void setActive(const QVector<int> &indices);
    bool toggleActive(int cubeIndex);
    void setBackground(int cubeIndex);
    void setPaletteKind(PaletteKind kind);

    void setPixel(int index, int cubeIndex);
    void fillIndices(const QVector<int> &indices, int cubeIndex);
    void restoreIndices(const QVector<int> &indices, const QVector<int> &values);

    /// Cube indices painted in the current frame that are not in `active()`.
    QVector<int> overspill() const;

    QColor displayColor(int cubeIndex) const;

private:
    bool setGeometry(int width, int height, QString *error);
    QVector<int> blankFrame() const;
    void touch() { m_modified = true; }

    int m_width = 32;
    int m_height = 32;
    PaletteKind m_kind = PaletteKind::Ste;
    QVector<int> m_active;
    int m_background = 0;
    QVector<QVector<int>> m_frames;
    int m_current = 0;
    bool m_modified = false;
    mutable QString m_lastError;
};

} // namespace pist
