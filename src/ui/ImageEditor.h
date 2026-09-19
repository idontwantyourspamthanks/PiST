// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/ImageDocument.h"
#include "image/StFormats.h"
#include "image/Tools.h"

#include <QHash>
#include <QBitArray>
#include <QImage>

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
class QStackedWidget;
class QTimer;
class QToolButton;
class QUndoStack;

namespace pist {

class ImageCanvas;
class SheetCanvas;

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
    bool exportFile(const QString &path, bool spriteSafe = false);
    /// Compose sheet `sheetIndex` from its placed phases and write it.
    bool exportSheetFile(const QString &path, int sheetIndex, bool spriteSafe);
    /// Write a `.dat` of raw bitplanes for `phase` — all of its frames;
    /// `options` picks the blocks. See `exportBitplaneData` for the layout.
    bool exportBitplaneFile(const QString &path, int phase, const BitplaneDataOptions &options);
    /// Write the ready-to-assemble scroller for `phase`: a program that
    /// animates its frames across the screen, `incbin`-ing `dataFile` (the
    /// `.dat` written beside it).
    bool exportScrollDemoFile(const QString &path, int phase, const BitplaneDataOptions &options,
                              const QString &dataFile);

    /// Remember that the export `exportBitplaneFile()` just wrote also came
    /// with this scroller, so `reExportBitplaneData()` writes it too. The
    /// export flow calls this once the scroller is out; `exportBitplaneFile()`
    /// clears it, so a new export replaces the pair rather than reusing it.
    /// Ignored when no bitplane export has succeeded yet.
    void setBitplaneExportScroller(const QString &scroller);

    /// Whether a bitplane export has succeeded in this session, so there is a
    /// recipe to repeat.
    bool canReExportBitplane() const { return !m_bitplaneRecipe.path.isEmpty(); }
    /// The `.dat` the remembered export wrote; empty before the first one.
    QString lastBitplaneExportPath() const { return m_bitplaneRecipe.path; }
    /// The phase the remembered export used, and the blocks it wrote, so the
    /// shell can print the same block map for a re-export as for the explicit
    /// one.
    int lastBitplaneExportPhase() const { return m_bitplaneRecipe.phase; }
    BitplaneDataOptions lastBitplaneExportOptions() const { return m_bitplaneRecipe.options; }
    /// Write the remembered bitplane export again — the same phase, blocks and
    /// `.dat`, and the scroller beside it when that export wrote one — with no
    /// dialog and no overwrite prompt, encoding the document's current pixels.
    /// False leaves `lastError()` saying why. Nothing is encoded unless both
    /// blobs encode, so a phase that is gone cannot truncate a good `.dat`;
    /// the `.dat` goes out before its scroller, so a scroller that will not
    /// open is reported with the `.dat` already written.
    bool reExportBitplaneData();
    /// The toolbar's re-export action, for a File menu entry to share: adding
    /// this action to a menu keeps one shortcut and one enabled state, where a
    /// second action would make Ctrl+Shift+E ambiguous.
    QAction *reExportAction() const { return m_actReExport; }
    /// The sheet the current phase is placed on (0 when unplaced); -1 when
    /// the document has no sheets at all.
    int currentSheetIndex() const;
    /// Slice `count` cells out of an imported sheet into a new phase — the
    /// spritesheet-mode "add a phase over the sheet" flow. Returns the new
    /// phase index, or -1 when the sheet has no imported pixels.
    int addPhaseFromSheet(int sheetIndex, const QString &name, int x, int y,
                          int cellW, int cellH, int count);
    /// Fill a placed phase's still-empty frames from the sheet pixels under
    /// them (placement and frame adds pull the art; drawn frames survive).
    void slicePlacedPhaseFrames(int phaseIndex);
    /// After a phase moved, re-slice the frames that still match the cells
    /// they were cut from (or are empty) — the strip follows the sheet until
    /// a frame is edited; edited frames keep their pixels.
    void reSliceUntouchedFrames(int phaseIndex, int oldSheet, int oldX, int oldY);

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
    /// A re-export finished: `path` is the `.dat`, `scroller` what was written
    /// beside it (empty when none). `error` is empty on success. A re-export
    /// has no shell caller to report to — the explicit export is driven from
    /// the shell, which logs it there — so the widget announces this one for
    /// the console to print the same block map.
    void bitplaneReExported(const QString &path, const QString &scroller, const QString &error);

private slots:
    void setSheetMode(bool on);
    void onNewSheet();
    void slicePhaseFromSheet();
    void onPhasePlacementChanged();
    void onPhaseCellSizeChanged();
    void setTool();
    void selectSwatch();
    void openPalettePicker();
    void copySelection();
    void cutSelection();
    void pasteClipboard();
    void deleteSelection();
    void moveSelection(const QRect &source, const QPoint &delta);
    void nudgeSelection(const QPoint &step);
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

    /// Repeat the last bitplane export, reporting through the status label
    /// the way an explicit export does.
    void reExportBitplane();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// The bitplane export a re-export repeats, as `.pim` does not record it
    /// and the project does not either: export stays explicit, and this is per
    /// document, per session. `path` empty means no export has happened.
    struct BitplaneExportRecipe {
        QString path;
        int phase = 0;
        BitplaneDataOptions options;
        /// The scroller written beside `path`, empty when that export wrote
        /// none or the user declined to overwrite one already there.
        QString scroller;
    };

    bool writeBytes(const QString &path, const QByteArray &bytes);
    bool exportBitplaneBytes(const QString &path, int phase, const BitplaneDataOptions &options,
                             const QString &scroller);
    /// Drop the remembered export — the document it described is gone. Leaves
    /// the action disabled.
    void forgetBitplaneExport();
    /// Match the action's enabled state and tool tip to the recipe.
    void refreshReExport();
    void rebuildSwatches();
    void refreshBrushIcon();
    void refreshFrames();
    void refreshLayers();
    void refreshPhases();
    void refreshOnion();
    void refreshPreview();
    void refreshCanvas();
    void refreshSheetView();
    void refreshPhasePlacement();
    void refreshChrome();
    void updateOverspill();
    void paintIndices(const QVector<int> &indices, int colour);
    void finishStroke();
    void pickColour(int cubeIndex);
    void notifyModified();
    void updateStatus();
    void pushSnapshot(const ImageDocument &before, const QString &text);
    void applyLayerBuffer(const QVector<int> &before, const QString &text);
    /// Replace the active layer's buffer as one undoable command; no-op when
    /// the buffers are equal. Returns whether anything changed.
    bool applyLayerEdit(const QVector<int> &before, const QVector<int> &after,
                        const QString &text);
    void clearSelection();
    void shiftBy(ShiftDirection direction);
    int stackIndexFromDisplay(int displayRow) const;

    ImageDocument m_doc;
    QString m_filePath;
    QString m_lastError;
    DrawTool m_tool = DrawTool::Brush;
    int m_brushSize = 1;
    int m_colour = 0;
    QVector<int> m_strokeIndices;
    QVector<int> m_strokeBefore;
    // O(1) membership for the stroke dedup below; sized to the frame's pixel
    // count at stroke start. m_strokeIndices.contains() was O(n) per visit, so a
    // full-canvas stroke scanned ~n^2/2 (measured ~530 ms for a 320x200 phase).
    QBitArray m_strokeSeen;
    int m_strokeColour = kTransparent;
    int m_strokePhase = 0;
    int m_strokeLayer = 0;
    int m_strokeFrame = 0;
    int m_onionDistance = 0;
    int m_previewFrame = 0;
    int m_previewPhase = -1;
    int m_fps = 8;
    bool m_playing = false;
    /// Internal clipboard: cube indices of the last copy/cut, `m_clipWidth` wide.
    QVector<int> m_clip;
    int m_clipWidth = 0;

    ImageCanvas *m_canvas = nullptr;
    QScrollArea *m_scroll = nullptr;
    SheetCanvas *m_sheetCanvas = nullptr;
    QStackedWidget *m_modeStack = nullptr;
    QComboBox *m_phaseSheet = nullptr;
    QSpinBox *m_phaseX = nullptr;
    QSpinBox *m_phaseY = nullptr;
    QSpinBox *m_phaseCellW = nullptr;
    QSpinBox *m_phaseCellH = nullptr;
    QAction *m_actSheetMode = nullptr;
    QAction *m_actNewSheet = nullptr;
    QAction *m_actSheetSource = nullptr;
    /// Imported sheet pixels (for slicing) and their display form, per sheet.
    QHash<int, ImportedSheet> m_importedSheets;
    QHash<int, QImage> m_sheetUnderlays;
    QButtonGroup *m_tools = nullptr;
    QButtonGroup *m_swatches = nullptr;
    QWidget *m_swatchBar = nullptr;
    QLabel *m_overspill = nullptr;
    QListWidget *m_frames = nullptr;
    QListWidget *m_layers = nullptr;
    QListWidget *m_phases = nullptr;
    QLabel *m_preview = nullptr;
    QComboBox *m_onion = nullptr;
    QComboBox *m_previewPhaseBox = nullptr;
    QSpinBox *m_fpsBox = nullptr;
    QTimer *m_previewTimer = nullptr;
    QUndoStack *m_undo = nullptr;
    QLabel *m_status = nullptr;
    QAction *m_actUndo = nullptr;
    QAction *m_actRedo = nullptr;
    QAction *m_actCopy = nullptr;
    QAction *m_actCut = nullptr;
    QAction *m_actPaste = nullptr;
    QAction *m_actDeleteSelection = nullptr;
    QAction *m_actDeselect = nullptr;
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
    QAction *m_actReExport = nullptr;
    BitplaneExportRecipe m_bitplaneRecipe;
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
};

} // namespace pist
