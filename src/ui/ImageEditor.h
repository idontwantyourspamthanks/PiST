// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"
#include "image/Tools.h"

#include <QWidget>

class QAction;
class QButtonGroup;
class QEvent;
class QLabel;
class QListWidget;
class QScrollArea;
class QToolButton;
class QUndoStack;

namespace pist {

class ImageCanvas;

/// Document tab for a `.pim` sprite: grid canvas, ST palette, frames, and
/// import/export of Atari ST still-image formats.
class ImageEditor : public QWidget
{
    Q_OBJECT

public:
    explicit ImageEditor(QWidget *parent = nullptr);

    bool loadFile(const QString &path);
    bool saveFile(const QString &path);
    bool importFile(const QString &path, bool append);
    bool exportFile(const QString &path);

    QString lastError() const { return m_lastError; }
    QString filePath() const { return m_filePath; }
    void setFilePath(const QString &path) { m_filePath = path; }
    bool isModifiedSinceLoad() const;
    QString displayName() const;

    ImageDocument &document() { return m_doc; }
    const ImageDocument &document() const { return m_doc; }

    void newDocument(int width, int height, PaletteKind kind);
    void undo();
    void redo();
    /// Rebuild toolbar icons after a theme change.
    void applyAppearance();

signals:
    void modificationChanged(bool modified);

private slots:
    void setTool();
    void selectSwatch();
    void openPalettePicker();
    void addFrame();
    void removeFrame();
    void duplicateFrame();
    void selectFrame(int row);
    void toggleGrid(bool on);
    void fitToView();
    void zoomIn();
    void zoomOut();
    void zoomBy(int steps);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildSwatches();
    void refreshBrushIcon();
    void refreshFrames();
    void refreshCanvas();
    void updateOverspill();
    void paintIndices(const QVector<int> &indices, int colour);
    void finishStroke();
    void pickColour(int cubeIndex);
    void notifyModified();
    void updateStatus();

    ImageDocument m_doc;
    QString m_filePath;
    QString m_lastError;
    DrawTool m_tool = DrawTool::Brush;
    int m_brushSize = 1;
    int m_colour = 0;
    QVector<int> m_strokeIndices;
    QVector<int> m_strokeBefore;
    int m_strokeColour = kTransparent;

    ImageCanvas *m_canvas = nullptr;
    QScrollArea *m_scroll = nullptr;
    QButtonGroup *m_tools = nullptr;
    QButtonGroup *m_swatches = nullptr;
    QWidget *m_swatchBar = nullptr;
    QLabel *m_overspill = nullptr;
    QListWidget *m_frames = nullptr;
    QUndoStack *m_undo = nullptr;
    QLabel *m_status = nullptr;
    QAction *m_actUndo = nullptr;
    QAction *m_actRedo = nullptr;
    QAction *m_actGrid = nullptr;
    QAction *m_actFit = nullptr;
    QAction *m_actZoomIn = nullptr;
    QAction *m_actZoomOut = nullptr;
    QAction *m_actPalette = nullptr;
    QToolButton *m_addFrame = nullptr;
    QToolButton *m_dupFrame = nullptr;
    QToolButton *m_removeFrame = nullptr;
};

} // namespace pist
