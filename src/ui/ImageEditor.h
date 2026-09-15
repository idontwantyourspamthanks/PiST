// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"
#include "image/Tools.h"

#include <QWidget>

class QAction;
class QButtonGroup;
class QComboBox;
class QEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTimer;
class QToolButton;
class QUndoStack;

namespace pist {

class ImageCanvas;

/// Document tab for a `.pim` sprite: grid canvas, ST palette, frames, layers,
/// onion-skin, animated preview, and import/export of Atari ST still-image formats.
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
    /// Take `doc` as the edited sheet, untitled (used by region extraction).
    void replaceDocument(const ImageDocument &doc);
    void undo();
    void redo();
    /// Rebuild toolbar icons after a theme change.
    void applyAppearance();

signals:
    void modificationChanged(bool modified);
    /// The user asked to open a region's crop as its own sprite.
    void regionExtractRequested(const pist::ImageDocument &doc);

private slots:
    void setTool();
    void selectSwatch();
    void openPalettePicker();
    void addFrame();
    void removeFrame();
    void duplicateFrame();
    void moveFrameUp();
    void moveFrameDown();
    void selectFrame(int row);
    void toggleGrid(bool on);
    void fitToView();
    void zoomIn();
    void zoomOut();
    void zoomBy(int steps);
    void flipHorizontal();
    void flipVertical();
    void rotate90();
    void generateEightWay();
    void shiftLeft();
    void shiftRight();
    void shiftUp();
    void shiftDown();
    void onionChanged();
    void togglePlay(bool on);
    void previewTick();
    void fpsChanged(int fps);
    void previewPhaseChanged(int index);
    void addLayer();
    void removeLayer();
    void moveLayerUp();
    void moveLayerDown();
    void selectLayer(int row);
    void layerVisibilityChanged(QListWidgetItem *item);
    void renameLayer();
    void addPhase();
    void removePhase();
    void selectPhase(int row);
    void renamePhase();
    void phaseRangeChanged();
    void addRegion();
    void removeRegion();
    void selectRegion(int row);
    void regionFieldChanged();
    void extractRegion();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildSwatches();
    void refreshBrushIcon();
    void refreshFrames();
    void refreshLayers();
    void refreshPhases();
    void refreshRegions();
    void refreshOnion();
    void refreshPreview();
    void refreshCanvas();
    void refreshChrome();
    void updateOverspill();
    void paintIndices(const QVector<int> &indices, int colour);
    void finishStroke();
    void pickColour(int cubeIndex);
    void notifyModified();
    void updateStatus();
    void pushSnapshot(const ImageDocument &before, const QString &text);
    void applyLayerBuffer(const QVector<int> &before, const QString &text);
    void shiftBy(ShiftDirection direction);
    int previewRangeStart() const;
    int previewRangeEnd() const;
    int stackIndexFromDisplay(int displayRow) const;

    ImageDocument m_doc;
    QString m_filePath;
    QString m_lastError;
    DrawTool m_tool = DrawTool::Brush;
    int m_brushSize = 1;
    int m_colour = 0;
    QVector<int> m_strokeIndices;
    QVector<int> m_strokeBefore;
    int m_strokeColour = kTransparent;
    int m_strokeLayer = 0;
    int m_strokeFrame = 0;
    int m_onionDistance = 0;
    int m_previewFrame = 0;
    int m_previewPhase = -1;
    int m_fps = 8;
    bool m_playing = false;

    ImageCanvas *m_canvas = nullptr;
    QScrollArea *m_scroll = nullptr;
    QButtonGroup *m_tools = nullptr;
    QButtonGroup *m_swatches = nullptr;
    QWidget *m_swatchBar = nullptr;
    QLabel *m_overspill = nullptr;
    QListWidget *m_frames = nullptr;
    QListWidget *m_layers = nullptr;
    QListWidget *m_phases = nullptr;
    QListWidget *m_regions = nullptr;
    QLineEdit *m_regionName = nullptr;
    QSpinBox *m_regionX = nullptr;
    QSpinBox *m_regionY = nullptr;
    QSpinBox *m_regionW = nullptr;
    QSpinBox *m_regionH = nullptr;
    QLabel *m_preview = nullptr;
    QComboBox *m_onion = nullptr;
    QComboBox *m_previewPhaseBox = nullptr;
    QSpinBox *m_fpsBox = nullptr;
    QSpinBox *m_phaseStart = nullptr;
    QSpinBox *m_phaseEnd = nullptr;
    QTimer *m_previewTimer = nullptr;
    QUndoStack *m_undo = nullptr;
    QLabel *m_status = nullptr;
    QAction *m_actUndo = nullptr;
    QAction *m_actRedo = nullptr;
    QAction *m_actGrid = nullptr;
    QAction *m_actFit = nullptr;
    QAction *m_actZoomIn = nullptr;
    QAction *m_actZoomOut = nullptr;
    QAction *m_actPalette = nullptr;
    QAction *m_actFlipH = nullptr;
    QAction *m_actFlipV = nullptr;
    QAction *m_actRotate = nullptr;
    QAction *m_actPlay = nullptr;
    QAction *m_actShiftLeft = nullptr;
    QAction *m_actShiftRight = nullptr;
    QAction *m_actShiftUp = nullptr;
    QAction *m_actShiftDown = nullptr;
    QToolButton *m_addFrame = nullptr;
    QToolButton *m_dupFrame = nullptr;
    QToolButton *m_removeFrame = nullptr;
    QToolButton *m_frameUp = nullptr;
    QToolButton *m_frameDown = nullptr;
    QToolButton *m_addLayer = nullptr;
    QToolButton *m_removeLayer = nullptr;
    QToolButton *m_layerUp = nullptr;
    QToolButton *m_layerDown = nullptr;
    QToolButton *m_addPhase = nullptr;
    QToolButton *m_removePhase = nullptr;
    QToolButton *m_addRegion = nullptr;
    QToolButton *m_removeRegion = nullptr;
    QPushButton *m_extractRegion = nullptr;
};

} // namespace pist
