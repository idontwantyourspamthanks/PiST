// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ImageEditor.h"

#include "image/StFormats.h"
#include "image/Transform.h"
#include "ui/AnimationPreviewWidget.h"
#include "ui/Appearance.h"
#include "ui/BitplaneExportController.h"
#include "ui/ImageCanvas.h"
#include "ui/SheetCanvas.h"
#include "ui/SheetSlicing.h"
#include "ui/PalettePickerDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QStackedWidget>
#include <QScrollArea>
#include <QSize>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWheelEvent>

namespace pist {

namespace {

class PaintCommand : public QUndoCommand
{
public:
    PaintCommand(ImageDocument *doc, int phase, int frame, int layer,
                 const QVector<int> &indices, const QVector<int> &before, int colour,
                 const QString &text)
        : m_doc(doc)
        , m_phase(phase)
        , m_frame(frame)
        , m_layer(layer)
        , m_indices(indices)
        , m_before(before)
        , m_colour(colour)
    {
        setText(text);
    }

    void undo() override
    {
        // editFrame addresses the phase and frame the stroke was made in:
        // undo must not depend on which phase the user is looking at now.
        m_doc->editFrame(m_phase, m_frame, [this] {
            m_doc->restoreIndices(m_indices, m_before, m_layer);
        });
    }
    void redo() override
    {
        // First redo() is QUndoStack::push; the stroke already painted live.
        if (m_virgin) {
            m_virgin = false;
            return;
        }
        m_doc->editFrame(m_phase, m_frame, [this] {
            m_doc->fillIndices(m_indices, m_colour, m_layer);
        });
    }

private:
    ImageDocument *m_doc;
    int m_phase = 0;
    int m_frame = 0;
    int m_layer = 0;
    QVector<int> m_indices;
    QVector<int> m_before;
    int m_colour;
    bool m_virgin = true;
};

class LayerPixelsCommand : public QUndoCommand
{
public:
    LayerPixelsCommand(ImageDocument *doc, int phase, int frame, int layer,
                       const QVector<int> &before, const QVector<int> &after,
                       const QString &text)
        : m_doc(doc)
        , m_phase(phase)
        , m_frame(frame)
        , m_layer(layer)
        , m_before(before)
        , m_after(after)
    {
        setText(text);
    }

    void undo() override
    {
        m_doc->editFrame(m_phase, m_frame, [this] {
            m_doc->setActiveLayer(m_layer);
            m_doc->replaceActiveLayer(m_before);
        });
    }
    void redo() override
    {
        if (m_virgin) {
            m_virgin = false;
            return;
        }
        m_doc->editFrame(m_phase, m_frame, [this] {
            m_doc->setActiveLayer(m_layer);
            m_doc->replaceActiveLayer(m_after);
        });
    }

private:
    ImageDocument *m_doc;
    int m_phase = 0;
    int m_frame = 0;
    int m_layer = 0;
    QVector<int> m_before;
    QVector<int> m_after;
    bool m_virgin = true;
};

class SnapshotCommand : public QUndoCommand
{
public:
    SnapshotCommand(ImageDocument *doc, const ImageDocument &before, const QString &text)
        : m_doc(doc)
        , m_before(before)
        , m_after(*doc)
    {
        setText(text);
    }

    void undo() override { *m_doc = m_before; }
    void redo() override
    {
        if (m_virgin) {
            m_virgin = false;
            return;
        }
        *m_doc = m_after;
    }

private:
    ImageDocument *m_doc;
    ImageDocument m_before;
    ImageDocument m_after;
    bool m_virgin = true;
};

QToolButton *toolButton(appearance::Icon icon, const QString &tip)
{
    auto *button = new QToolButton;
    button->setIcon(appearance::icon(icon));
    button->setToolTip(tip);
    button->setCheckable(true);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setIconSize(QSize(20, 20));
    return button;
}

QString swatchStyleSheet(const QColor &color)
{
    const QPalette pal = QApplication::palette();
    const QColor mute = pal.color(QPalette::Mid);
    QColor ring = pal.color(QPalette::Highlight);
    const int hue = color.hslHue();
    const int ringHue = ring.hslHue();
    const bool similarHue = hue < 0 || ringHue < 0 || qAbs(hue - ringHue) < 50
        || qAbs(hue - ringHue) > 310;
    if (qAbs(color.lightness() - ring.lightness()) < 48 && similarHue)
        ring = pal.color(QPalette::WindowText);
    // The index glyph uses the same idea as the ring: a light chip gets dark
    // type, a dark chip gets light type, so a yellow swatch is still readable.
    const QColor ink = color.lightness() > 140 ? QColor(0x10, 0x13, 0x10)
                                                : QColor(0xe6, 0xea, 0xe4);
    return QStringLiteral("QToolButton { background-color: %1; color: %2;"
                          " border: 2px solid %3; border-radius: 3px; }"
                          "QToolButton:checked { border: 3px solid %4; color: %2; }")
        .arg(color.name(), ink.name(), mute.name(), ring.name());
}

QString imageStatusColour(int colour)
{
    if (colour < 0)
        return QObject::tr("erase");
    return QObject::tr("colour %1").arg(colour);
}

QImage frameThumbnail(const ImageDocument &doc, int frame)
{
    const int w = doc.width();
    const int h = doc.height();
    if (w <= 0 || h <= 0)
        return {};
    // Checkerboard, like the canvas: the thumbnail of a frame shows the same
    // empty texture as the canvas showing that frame.
    const QImage image = indicesToImage(doc.frame(frame), w, h, doc.paletteKind(),
                                        EmptyStyle::Checkerboard);
    const int thumbH = 32;
    const int thumbW = qBound(24, image.width() * thumbH / h, 72);
    return image.scaled(thumbW, thumbH, Qt::KeepAspectRatio, Qt::FastTransformation);
}

appearance::Icon iconForTool(DrawTool tool)
{
    using appearance::Icon;
    switch (tool) {
    case DrawTool::Brush:
        return Icon::Brush;
    case DrawTool::Line:
        return Icon::Line;
    case DrawTool::Rect:
        return Icon::Rect;
    case DrawTool::RoundRect:
        return Icon::RoundRect;
    case DrawTool::Ellipse:
        return Icon::Ellipse;
    case DrawTool::Fill:
        return Icon::Fill;
    case DrawTool::Eyedropper:
        return Icon::Eyedropper;
    case DrawTool::Select:
        return Icon::Select;
    }
    return Icon::Brush;
}

} // namespace

QToolBar *ImageEditor::buildToolbar()
{
    auto *bar = new QToolBar(this);
    bar->setIconSize(QSize(20, 20));
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setMovable(false);
    m_tools = new QButtonGroup(this);
    m_tools->setExclusive(true);
    auto addTool = [&](DrawTool tool, const QString &tip) {
        QToolButton *button = toolButton(iconForTool(tool), tip);
        button->setProperty("tool", int(tool));
        m_tools->addButton(button);
        bar->addWidget(button);
        if (tool == DrawTool::Brush)
            button->setChecked(true);
    };
    addTool(DrawTool::Brush, tr("Brush — paint pixels"));
    addTool(DrawTool::Line, tr("Line"));
    addTool(DrawTool::Rect, tr("Rectangle outline"));
    addTool(DrawTool::RoundRect, tr("Rounded rectangle"));
    addTool(DrawTool::Ellipse, tr("Ellipse outline"));
    addTool(DrawTool::Fill, tr("Flood fill"));
    addTool(DrawTool::Eyedropper, tr("Eyedropper"));
    addTool(DrawTool::Select,
            tr("Select — drag a rectangle, drag inside it to move, Ctrl+C/X/V, arrows to nudge"));
    connect(m_tools, &QButtonGroup::buttonClicked, this, &ImageEditor::setTool);

    m_actSheetMode = new QAction(tr("Sprite sheet"), this);
    m_actSheetMode->setObjectName(QStringLiteral("imageSheetMode"));
    m_actSheetMode->setCheckable(true);
    m_actSheetMode->setToolTip(tr("Show the composed sheet and move phases around"));
    connect(m_actSheetMode, &QAction::toggled, this, &ImageEditor::setSheetMode);

    m_actNewSheet = new QAction(tr("New sheet"), this);
    m_actNewSheet->setObjectName(QStringLiteral("imageNewSheet"));
    m_actNewSheet->setToolTip(
        tr("Create a 320×200 sprite sheet to place phases on (the file name "
           "is chosen on export)"));
    connect(m_actNewSheet, &QAction::triggered, this, &ImageEditor::onNewSheet);

    m_actSheetSource = new QAction(tr("Sheet source"), this);
    m_actSheetSource->setObjectName(QStringLiteral("imageSheetSource"));
    m_actSheetSource->setCheckable(true);
    m_actSheetSource->setChecked(true);
    m_actSheetSource->setToolTip(
        tr("Show the imported sheet's pixels under the layout (the slicing "
           "reference); off shows the composed export"));
    connect(m_actSheetSource, &QAction::toggled, this, [this](bool on) {
        m_sheetCanvas->setUnderlayVisible(on);
    });

    bar->addSeparator();
    auto *size = new QSlider(Qt::Horizontal, this);
    size->setRange(1, 8);
    size->setValue(1);
    size->setMaximumWidth(100);
    size->setToolTip(tr("Brush size"));
    connect(size, &QSlider::valueChanged, this, [this](int value) {
        m_brushSize = value;
        m_canvas->setBrushSize(value);
    });
    bar->addWidget(size);

    m_actUndo = new QAction(tr("Undo"), this);
    m_actUndo->setIcon(appearance::icon(appearance::Icon::Undo));
    m_actUndo->setShortcut(QKeySequence::Undo);
    m_actUndo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actUndo, &QAction::triggered, this, &ImageEditor::undo);
    addAction(m_actUndo);
    bar->addAction(m_actUndo);
    m_actRedo = new QAction(tr("Redo"), this);
    m_actRedo->setIcon(appearance::icon(appearance::Icon::Redo));
    m_actRedo->setShortcut(QKeySequence::Redo);
    m_actRedo->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actRedo, &QAction::triggered, this, &ImageEditor::redo);
    addAction(m_actRedo);
    bar->addAction(m_actRedo);

    bar->addSeparator();
    auto addEditAction = [&](QAction *&action, appearance::Icon icon, const QString &name,
                             const QString &objectName, const QKeySequence &shortcut,
                             void (ImageEditor::*method)()) {
        action = new QAction(name, this);
        action->setIcon(appearance::icon(icon));
        action->setObjectName(objectName);
        action->setToolTip(name);
        if (!shortcut.isEmpty())
            action->setShortcut(shortcut);
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        connect(action, &QAction::triggered, this, method);
        addAction(action);
        bar->addAction(action);
    };
    addEditAction(m_actCopy, appearance::Icon::Copy, tr("Copy selection (Ctrl+C)"),
                  QStringLiteral("imageCopy"), QKeySequence::Copy, &ImageEditor::copySelection);
    addEditAction(m_actCut, appearance::Icon::Cut, tr("Cut selection (Ctrl+X)"),
                  QStringLiteral("imageCut"), QKeySequence::Cut, &ImageEditor::cutSelection);
    addEditAction(m_actPaste, appearance::Icon::Paste, tr("Paste (Ctrl+V)"),
                  QStringLiteral("imagePaste"), QKeySequence::Paste,
                  &ImageEditor::pasteClipboard);

    m_actDeleteSelection = new QAction(tr("Delete selection (Del)"), this);
    m_actDeleteSelection->setObjectName(QStringLiteral("imageDeleteSelection"));
    m_actDeleteSelection->setShortcut(QKeySequence(Qt::Key_Delete));
    m_actDeleteSelection->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actDeleteSelection, &QAction::triggered, this, &ImageEditor::deleteSelection);
    addAction(m_actDeleteSelection);

    m_actDeselect = new QAction(tr("Deselect (Esc)"), this);
    m_actDeselect->setObjectName(QStringLiteral("imageDeselect"));
    m_actDeselect->setShortcut(QKeySequence(Qt::Key_Escape));
    m_actDeselect->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actDeselect, &QAction::triggered, this, [this] { clearSelection(); });
    addAction(m_actDeselect);

    m_actGrid = new QAction(tr("Grid"), this);
    m_actGrid->setIcon(appearance::icon(appearance::Icon::Grid));
    m_actGrid->setCheckable(true);
    m_actGrid->setChecked(true);
    m_actGrid->setToolTip(tr("Show pixel grid"));
    connect(m_actGrid, &QAction::toggled, this, &ImageEditor::toggleGrid);
    bar->addAction(m_actGrid);

    m_actFit = new QAction(tr("Fit"), this);
    m_actFit->setIcon(appearance::icon(appearance::Icon::Fit));
    m_actFit->setToolTip(tr("Fit the image to the view (Ctrl+0)"));
    m_actFit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    m_actFit->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actFit, &QAction::triggered, this, &ImageEditor::fitToView);
    addAction(m_actFit);
    bar->addAction(m_actFit);

    m_actZoomOut = new QAction(tr("Zoom out"), this);
    m_actZoomOut->setIcon(appearance::icon(appearance::Icon::ZoomOut));
    m_actZoomOut->setToolTip(tr("Zoom out (Ctrl+-)"));
    m_actZoomOut->setShortcut(QKeySequence::ZoomOut);
    m_actZoomOut->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actZoomOut, &QAction::triggered, this, &ImageEditor::zoomOut);
    addAction(m_actZoomOut);
    bar->addAction(m_actZoomOut);

    m_actZoomIn = new QAction(tr("Zoom in"), this);
    m_actZoomIn->setIcon(appearance::icon(appearance::Icon::ZoomIn));
    m_actZoomIn->setToolTip(tr("Zoom in (Ctrl++)"));
    m_actZoomIn->setShortcuts({QKeySequence::ZoomIn, QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    m_actZoomIn->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actZoomIn, &QAction::triggered, this, &ImageEditor::zoomIn);
    addAction(m_actZoomIn);
    bar->addAction(m_actZoomIn);

    m_actPalette = new QAction(tr("Palette…"), this);
    m_actPalette->setIcon(appearance::icon(appearance::Icon::Palette));
    m_actPalette->setToolTip(tr("Edit the active palette"));
    connect(m_actPalette, &QAction::triggered, this, &ImageEditor::openPalettePicker);
    bar->addAction(m_actPalette);

    bar->addSeparator();
    bar->addAction(m_actSheetMode);
    bar->addAction(m_actNewSheet);
    bar->addAction(m_actSheetSource);
    m_actNewSheet->setEnabled(false);
    m_actSheetSource->setEnabled(false);

    bar->addSeparator();
    // Flip, rotate and shift are a menu: as icons on the bar they wrap under
    // about 900px, and the shortcuts stay on the actions themselves.
    auto *transformButton = new QToolButton(bar);
    transformButton->setObjectName(QStringLiteral("imageTransform"));
    transformButton->setText(tr("Transform"));
    transformButton->setToolTip(tr("Flip, rotate, and shift"));
    transformButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    transformButton->setPopupMode(QToolButton::InstantPopup);
    auto *transformMenu = new QMenu(transformButton);
    transformButton->setMenu(transformMenu);
    bar->addWidget(transformButton);
    auto addTransform = [&](QAction *&action, appearance::Icon icon, const QString &name,
                            const QString &objectName, void (ImageEditor::*method)()) {
        action = new QAction(name, this);
        action->setIcon(appearance::icon(icon));
        action->setObjectName(objectName);
        action->setToolTip(name);
        connect(action, &QAction::triggered, this, method);
        addAction(action);
        transformMenu->addAction(action);
    };
    addTransform(m_actShiftLeft, appearance::Icon::ShiftLeft, tr("Shift left"),
                 QStringLiteral("imageShiftLeft"), &ImageEditor::shiftLeft);
    m_actShiftLeft->setToolTip(tr("Shift left (wraps around)"));
    addTransform(m_actShiftRight, appearance::Icon::ShiftRight, tr("Shift right"),
                 QStringLiteral("imageShiftRight"), &ImageEditor::shiftRight);
    m_actShiftRight->setToolTip(tr("Shift right (wraps around)"));
    addTransform(m_actShiftUp, appearance::Icon::ShiftUp, tr("Shift up"),
                 QStringLiteral("imageShiftUp"), &ImageEditor::shiftUp);
    m_actShiftUp->setToolTip(tr("Shift up (wraps around)"));
    addTransform(m_actShiftDown, appearance::Icon::ShiftDown, tr("Shift down"),
                 QStringLiteral("imageShiftDown"), &ImageEditor::shiftDown);
    m_actShiftDown->setToolTip(tr("Shift down (wraps around)"));
    addTransform(m_actFlipH, appearance::Icon::FlipHorizontal, tr("Flip horizontal"),
                 QStringLiteral("imageFlipH"), &ImageEditor::flipHorizontal);
    m_actFlipH->setShortcut(QKeySequence(Qt::Key_H));
    m_actFlipH->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addTransform(m_actFlipV, appearance::Icon::FlipVertical, tr("Flip vertical"),
                 QStringLiteral("imageFlipV"), &ImageEditor::flipVertical);
    m_actFlipV->setShortcut(QKeySequence(Qt::Key_V));
    m_actFlipV->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    addTransform(m_actRotate, appearance::Icon::Rotate, tr("Bake 8-way rotations"),
                 QStringLiteral("imageRotate"), &ImageEditor::generateEightWay);
    m_actRotate->setToolTip(tr("Insert 7 clockwise rotations of this square frame"));

    auto *rotate90 = new QAction(tr("Rotate 90° clockwise"), this);
    rotate90->setShortcut(QKeySequence(Qt::Key_R));
    rotate90->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(rotate90, &QAction::triggered, this, &ImageEditor::rotate90);
    addAction(rotate90);
    transformMenu->addAction(rotate90);

    bar->addSeparator();
    // Ctrl+Shift+E: the editor's own keys are H, V, R, Del, Esc, Ctrl+0 and the
    // clipboard/zoom standards, so this one is free here and in the window's
    // menus (which use Ctrl+Shift+F3/F4/F5/F9/F11 only).
    m_actReExport = new QAction(tr("Re-export bitplane data (Ctrl+Shift+E)"), this);
    m_actReExport->setObjectName(QStringLiteral("imageReExportBitplanes"));
    m_actReExport->setIcon(appearance::icon(appearance::Icon::Save));
    m_actReExport->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E));
    m_actReExport->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(m_actReExport, &QAction::triggered, this, &ImageEditor::reExportBitplane);
    addAction(m_actReExport);
    bar->addAction(m_actReExport);
    refreshReExport();

    return bar;
}

/// Build the editing body the toolbar sits above: the swatch column, the grid
/// canvas and its sheet-mode stack, the filmstrip, and the layer/phase/preview
/// column. Everything it builds hangs off the editor's `layout`.
void ImageEditor::buildEditorBody(QVBoxLayout *layout)
{
    auto *body = new QHBoxLayout;
    layout->addLayout(body, 1);

    auto *left = new QVBoxLayout;
    m_swatchBar = new QWidget(this);
    m_swatchBar->setObjectName(QStringLiteral("imageSwatches"));
    left->addWidget(m_swatchBar);
    m_overspill = new QLabel(this);
    m_overspill->setWordWrap(true);
    left->addWidget(m_overspill);
    left->addStretch();
    body->addLayout(left);

    m_canvas = new ImageCanvas(this);
    m_canvas->setDocument(&m_doc);
    m_canvas->setCurrentColour(m_colour);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidget(m_canvas);
    m_scroll->setWidgetResizable(false);
    m_scroll->setAlignment(Qt::AlignCenter);
    m_scroll->viewport()->installEventFilter(this);
    m_sheetCanvas = new SheetCanvas(this);
    m_sheetCanvas->setObjectName(QStringLiteral("imageSheetCanvas"));
    auto *sheetScroll = new QScrollArea(this);
    sheetScroll->setWidget(m_sheetCanvas);
    sheetScroll->setWidgetResizable(true);
    m_modeStack = new QStackedWidget(this);
    m_modeStack->addWidget(m_scroll);
    m_modeStack->addWidget(sheetScroll);
    auto *canvasColumn = new QVBoxLayout;
    canvasColumn->setSpacing(4);
    canvasColumn->addWidget(m_modeStack, 1);
    body->addLayout(canvasColumn, 1);

    // m_phases does not exist yet at this point in the construction, so the
    // lambda guards instead of using it as the context object.
    connect(m_sheetCanvas, &SheetCanvas::phaseSelected, this, [this](int index) {
        if (m_phases)
            m_phases->setCurrentRow(index);
    });
    connect(m_sheetCanvas, &SheetCanvas::phaseMoved, this, [this](int index, int x, int y) {
        const ImageDocument before = m_doc;
        const int oldSheet = m_doc.phases().at(index).sheet;
        const int oldX = m_doc.phases().at(index).x;
        const int oldY = m_doc.phases().at(index).y;
        m_doc.setPhasePlacement(index, oldSheet, x, y);
        reSliceUntouchedFrames(index, oldSheet, oldX, oldY);
        pushSnapshot(before, tr("move phase"));
        refreshPhases();
        notifyModified();
    });
    connect(m_sheetCanvas, &SheetCanvas::phasePlaced, this, [this](int index, int x, int y) {
        const ImageDocument before = m_doc;
        m_doc.setPhasePlacement(index, m_sheetCanvas->sheetIndex(), x, y);
        slicePlacedPhaseFrames(index);
        pushSnapshot(before, tr("place phase"));
        refreshPhases();
        notifyModified();
    });

    connect(m_canvas, &ImageCanvas::indicesPainted, this, &ImageEditor::paintIndices);
    connect(m_canvas, &ImageCanvas::strokeEnded, this, &ImageEditor::finishStroke);
    connect(m_canvas, &ImageCanvas::colourPicked, this, &ImageEditor::pickColour);
    connect(m_canvas, &ImageCanvas::cellSizeChanged, this, &ImageEditor::updateStatus);
    connect(m_canvas, &ImageCanvas::zoomStepsRequested, this, &ImageEditor::zoomBy);
    connect(m_canvas, &ImageCanvas::selectionMoved, this, &ImageEditor::moveSelection);
    connect(m_canvas, &ImageCanvas::selectionNudged, this, &ImageEditor::nudgeSelection);
    connect(m_canvas, &ImageCanvas::selectionMade, this, [this](const QRect &rect) {
        if (!m_status)
            return;
        if (rect.isEmpty())
            m_status->setText(tr("Selection cleared."));
        else
            m_status->setText(tr("Selected %1 × %2 — drag inside to move, arrows to nudge.")
                                  .arg(rect.width())
                                  .arg(rect.height()));
    });
    connect(m_canvas, &ImageCanvas::shiftRequested, this, [this](ShiftDirection direction) {
        shiftBy(direction);
    });
    connect(m_canvas, &ImageCanvas::cursorIndexChanged, this, [this](int index) {
        if (!m_status)
            return;
        const int zoom = m_canvas->cellSize() * 100;
        if (index < 0)
            updateStatus();
        else
            m_status->setText(tr("%1 × %2  (%3, %4)  %5%  %6")
                                  .arg(m_doc.width())
                                  .arg(m_doc.height())
                                  .arg(index % m_doc.width())
                                  .arg(index / m_doc.width())
                                  .arg(zoom)
                                  .arg(imageStatusColour(m_colour)));
    });

    m_frames = new QListWidget(this);
    m_frames->setObjectName(QStringLiteral("imageFrames"));
    m_frames->setViewMode(QListView::IconMode);
    m_frames->setFlow(QListView::LeftToRight);
    m_frames->setWrapping(false);
    m_frames->setMovement(QListView::Static);
    m_frames->setResizeMode(QListView::Adjust);
    m_frames->setIconSize(QSize(72, 32));
    m_frames->setGridSize(QSize(80, 56));
    m_frames->setUniformItemSizes(true);
    m_frames->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_frames->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_frames->setFixedHeight(64);
    m_frames->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_frames->setToolTip(tr("Frames"));
    connect(m_frames, &QListWidget::currentRowChanged, this, &ImageEditor::selectFrame);
    canvasColumn->addWidget(m_frames);

    auto makeIconButton = [this](appearance::Icon icon, const QString &tip) {
        auto *button = new QToolButton(this);
        button->setIcon(appearance::icon(icon));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        return button;
    };
    auto makeTextButton = [this](const QString &text, const QString &tip) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        return button;
    };
    auto packIcons = [](QHBoxLayout *row) {
        row->setSpacing(4);
        row->setContentsMargins(0, 0, 0, 0);
        row->addStretch(1);
    };
    m_addFrame = makeIconButton(appearance::Icon::AddFrame, tr("Add frame"));
    m_addFrame->setObjectName(QStringLiteral("imageAddFrame"));
    connect(m_addFrame, &QToolButton::clicked, this, &ImageEditor::addFrame);
    m_dupFrame = makeIconButton(appearance::Icon::DuplicateFrame, tr("Duplicate frame"));
    connect(m_dupFrame, &QToolButton::clicked, this, &ImageEditor::duplicateFrame);
    m_removeFrame = makeIconButton(appearance::Icon::RemoveFrame, tr("Delete frame"));
    connect(m_removeFrame, &QToolButton::clicked, this, &ImageEditor::removeFrame);
    m_frameUp = makeTextButton(QStringLiteral("▲"), tr("Move frame up"));
    connect(m_frameUp, &QToolButton::clicked, this, &ImageEditor::moveFrameUp);
    m_frameDown = makeTextButton(QStringLiteral("▼"), tr("Move frame down"));
    connect(m_frameDown, &QToolButton::clicked, this, &ImageEditor::moveFrameDown);
    m_onion = new QComboBox(this);
    m_onion->setObjectName(QStringLiteral("imageOnion"));
    m_onion->addItem(tr("Onion: off"), 0);
    m_onion->addItem(tr("Onion: previous"), -1);
    m_onion->addItem(tr("Onion: next"), 1);
    m_onion->setToolTip(tr("Ghost a neighbouring frame over the canvas"));
    m_onion->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    connect(m_onion, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ImageEditor::onionChanged);
    auto *frameBtns = new QHBoxLayout;
    frameBtns->addWidget(m_addFrame);
    frameBtns->addWidget(m_dupFrame);
    frameBtns->addWidget(m_removeFrame);
    frameBtns->addWidget(m_frameUp);
    frameBtns->addWidget(m_frameDown);
    frameBtns->addWidget(m_onion);
    packIcons(frameBtns);
    canvasColumn->addLayout(frameBtns);

    auto *right = new QVBoxLayout;
    right->setSpacing(4);

    // The player owns the preview box, the fps box and the play/pause action;
    // it is fixed-size, so `right` gives it the same column the pieces had.
    m_previewWidget = new AnimationPreviewWidget(&m_doc, this);
    right->addWidget(m_previewWidget);

    right->addWidget(new QLabel(tr("Layers"), this));
    m_layers = new QListWidget(this);
    m_layers->setObjectName(QStringLiteral("imageLayers"));
    m_layers->setMaximumWidth(160);
    m_layers->setMaximumHeight(140);
    m_layers->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    connect(m_layers, &QListWidget::currentRowChanged, this, &ImageEditor::selectLayer);
    connect(m_layers, &QListWidget::itemChanged, this, &ImageEditor::layerVisibilityChanged);
    connect(m_layers, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        renameLayer();
    });
    right->addWidget(m_layers);
    m_addLayer = makeIconButton(appearance::Icon::AddFrame, tr("Add layer"));
    connect(m_addLayer, &QToolButton::clicked, this, &ImageEditor::addLayer);
    m_removeLayer = makeIconButton(appearance::Icon::RemoveFrame, tr("Delete layer"));
    connect(m_removeLayer, &QToolButton::clicked, this, &ImageEditor::removeLayer);
    m_layerUp = makeTextButton(QStringLiteral("▲"), tr("Move layer up"));
    connect(m_layerUp, &QToolButton::clicked, this, &ImageEditor::moveLayerUp);
    m_layerDown = makeTextButton(QStringLiteral("▼"), tr("Move layer down"));
    connect(m_layerDown, &QToolButton::clicked, this, &ImageEditor::moveLayerDown);
    auto *layerBtns = new QHBoxLayout;
    layerBtns->addWidget(m_addLayer);
    layerBtns->addWidget(m_removeLayer);
    layerBtns->addWidget(m_layerUp);
    layerBtns->addWidget(m_layerDown);
    packIcons(layerBtns);
    right->addLayout(layerBtns);

    // The list stays the selection model (tests and sheet-canvas clicks drive
    // it) but it is not a third full-height column. The combo is the control.
    m_phases = new QListWidget(this);
    m_phases->setObjectName(QStringLiteral("imagePhases"));
    m_phases->setVisible(false);
    connect(m_phases, &QListWidget::currentRowChanged, this, &ImageEditor::selectPhase);
    connect(m_phases, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        renamePhase();
    });
    m_phasePicker = new QComboBox(this);
    m_phasePicker->setObjectName(QStringLiteral("imagePhasePicker"));
    m_phasePicker->setToolTip(tr("This phase"));
    m_phasePicker->installEventFilter(this);
    connect(m_phasePicker, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int row) {
                if (row >= 0 && m_phases && m_phases->currentRow() != row)
                    m_phases->setCurrentRow(row);
            });
    auto *phaseRow = new QHBoxLayout;
    phaseRow->setSpacing(4);
    phaseRow->setContentsMargins(0, 0, 0, 0);
    phaseRow->addWidget(new QLabel(tr("This phase"), this));
    phaseRow->addWidget(m_phasePicker, 1);
    m_addPhase = makeIconButton(appearance::Icon::AddFrame, tr("Add phase"));
    m_addPhase->setObjectName(QStringLiteral("imageAddPhase"));
    connect(m_addPhase, &QToolButton::clicked, this, &ImageEditor::addPhase);
    m_removePhase = makeIconButton(appearance::Icon::RemoveFrame, tr("Delete phase"));
    connect(m_removePhase, &QToolButton::clicked, this, &ImageEditor::removePhase);
    phaseRow->addWidget(m_addPhase);
    phaseRow->addWidget(m_removePhase);
    right->addLayout(phaseRow);

    // Placement is a sheet concern: hide it until the composed sheet is up.
    m_phasePlacement = new QWidget(this);
    m_phasePlacement->setObjectName(QStringLiteral("imagePhasePlacement"));
    m_phasePlacement->setVisible(false);
    auto *placementLayout = new QVBoxLayout(m_phasePlacement);
    placementLayout->setContentsMargins(0, 0, 0, 0);
    placementLayout->setSpacing(4);
    auto *placementRow = new QHBoxLayout;
    m_phaseSheet = new QComboBox(m_phasePlacement);
    m_phaseSheet->setObjectName(QStringLiteral("imagePhaseSheet"));
    connect(m_phaseSheet, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ImageEditor::onPhasePlacementChanged);
    m_phaseX = new QSpinBox(m_phasePlacement);
    m_phaseX->setObjectName(QStringLiteral("imagePhaseX"));
    m_phaseX->setPrefix(tr("x "));
    m_phaseX->setRange(-4096, 4095);
    m_phaseY = new QSpinBox(m_phasePlacement);
    m_phaseY->setObjectName(QStringLiteral("imagePhaseY"));
    m_phaseY->setPrefix(tr("y "));
    m_phaseY->setRange(-4096, 4095);
    connect(m_phaseX, qOverload<int>(&QSpinBox::valueChanged), this,
            &ImageEditor::onPhasePlacementChanged);
    connect(m_phaseY, qOverload<int>(&QSpinBox::valueChanged), this,
            &ImageEditor::onPhasePlacementChanged);
    placementRow->addWidget(m_phaseSheet);
    placementRow->addWidget(m_phaseX);
    placementRow->addWidget(m_phaseY);
    placementLayout->addLayout(placementRow);
    auto *cellRow = new QHBoxLayout;
    m_phaseCellW = new QSpinBox(m_phasePlacement);
    m_phaseCellW->setObjectName(QStringLiteral("imagePhaseCellW"));
    m_phaseCellW->setPrefix(tr("w "));
    m_phaseCellW->setRange(1, kStScreenWidth);
    m_phaseCellH = new QSpinBox(m_phasePlacement);
    m_phaseCellH->setObjectName(QStringLiteral("imagePhaseCellH"));
    m_phaseCellH->setPrefix(tr("h "));
    m_phaseCellH->setRange(1, kStScreenHeight);
    connect(m_phaseCellW, qOverload<int>(&QSpinBox::valueChanged), this,
            &ImageEditor::onPhaseCellSizeChanged);
    connect(m_phaseCellH, qOverload<int>(&QSpinBox::valueChanged), this,
            &ImageEditor::onPhaseCellSizeChanged);
    cellRow->addWidget(m_phaseCellW);
    cellRow->addWidget(m_phaseCellH);
    placementLayout->addLayout(cellRow);
    right->addWidget(m_phasePlacement);
    right->addStretch(1);

    body->addLayout(right);
}

ImageEditor::ImageEditor(QWidget *parent)
    : QWidget(parent)
{
    m_undo = new QUndoStack(this);
    m_doc = ImageDocument::create(32, 32, PaletteKind::Ste);
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    // The re-export recipe lives with the exporter, and its announcements feed
    // the status label and the editor's own bitplaneReExported — the shell
    // prints the block map from that one.
    m_bitplaneExport = new BitplaneExportController(m_doc, this);
    connect(m_bitplaneExport, &BitplaneExportController::recipeChanged, this,
            &ImageEditor::refreshReExport);
    connect(m_bitplaneExport, &BitplaneExportController::bitplaneReExported, this,
            [this](const QString &path, const QString &scroller, const QString &error) {
                if (!error.isEmpty()) {
                    if (m_status)
                        m_status->setText(tr("Re-export failed: %1").arg(error));
                    emit bitplaneReExported(path, scroller, error);
                    return;
                }
                if (m_status) {
                    m_status->setText(scroller.isEmpty()
                        ? tr("Re-exported %1.").arg(QFileInfo(path).fileName())
                        : tr("Re-exported %1 and %2.")
                              .arg(QFileInfo(path).fileName(), QFileInfo(scroller).fileName()));
                }
                emit bitplaneReExported(path, scroller, QString());
            });

    layout->addWidget(buildToolbar());

    buildEditorBody(layout);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    rebuildSwatches();
    refreshChrome();
    QTimer::singleShot(0, this, &ImageEditor::fitToView);
}

void ImageEditor::adoptDocument()
{
    m_undo->clear();
    forgetBitplaneExport();
    m_previewWidget->stop();
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();
    m_canvas->setDocument(&m_doc);
    // The selection addresses cells of the document that just went away, so it
    // goes with it — exactly once, for every path that swaps the document.
    clearSelection();
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    refreshChrome();
    notifyModified();
    fitToView();
}

void ImageEditor::newDocument(int width, int height, PaletteKind kind)
{
    m_doc = ImageDocument::create(width, height, kind);
    m_filePath.clear();
    m_sheetSlicing.clear();
    adoptDocument();
}

void ImageEditor::replaceDocument(const ImageDocument &doc)
{
    m_doc = doc;
    m_filePath.clear();
    adoptDocument();
}

bool ImageEditor::loadFile(const QString &path)
{
    QString error;
    if (!m_doc.load(path, &error)) {
        m_lastError = error;
        return false;
    }
    m_filePath = path;
    m_sheetSlicing.load(m_doc);
    adoptDocument();
    return true;
}

bool ImageEditor::saveFile(const QString &path)
{
    // Saving onto a registered sheet path recomposes that sheet from its
    // phases; editing a sheet means editing its sprites.
    for (int i = 0; i < m_doc.sheets().size(); ++i) {
        if (QFileInfo(m_doc.sheets().at(i).path) == QFileInfo(path))
            return exportSheetFile(path, i, false);
    }
    // A still-image path saves in that format rather than as .pim JSON, so
    // editing a Degas file in place stays a Degas file.
    if (!isPimPath(path))
        return exportFile(path);
    QString error;
    if (!m_doc.save(path, &error)) {
        m_lastError = error;
        return false;
    }
    m_filePath = path;
    m_doc.setModified(false);
    notifyModified();
    return true;
}

bool ImageEditor::importFile(const QString &path, bool append)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_lastError = file.errorString();
        return false;
    }
    const StImageFormat format = stFormatFromPath(path);
    ImportedSheet sheet;
    QString error;
    if (!importStImage(file.readAll(), format, m_doc.paletteKind(), &sheet, &error)) {
        m_lastError = error;
        return false;
    }
    // Honour the dialog's choice. "Replace" (append == false) discards the
    // current document so the import becomes the whole image; "Add frame"
    // (append == true) keeps it and adds the import as another sheet target.
    // addImageTab imports into a fresh editor, where append=false already starts
    // empty, so this only changes the import-into-an-existing-image path. The
    // replacement is a fresh-editor-equivalent document — the default 32x32
    // staged phase (sizing it to the sheet instead would mis-slice the placement
    // drag) with the current palette kind, since the import was decoded against
    // it. The sheet itself is added by the shared path below.
    if (!append) {
        replaceDocument(ImageDocument::create(32, 32, m_doc.paletteKind()));
        m_sheetSlicing.clear();
    }

    // Both mutations below — adopting the file's palette and registering the
    // sheet — are part of this one import, and the undo stack has to know about
    // them. Untracked, Ctrl+Z reached back past the import: the command sitting
    // under it holds a `before` that predates the import, so assigning it wiped
    // the sheet, the adopted palette and every placement made since, and its
    // `after` could not put them back.
    //
    // Undo of an appended import therefore means "the document as it was": the
    // added sheet target goes and the palette goes back to the one in use. That
    // is what append means — the current image kept, the import added to it —
    // so undoing it must leave the current image untouched.
    //
    // "Replace" needs no command of its own: replaceDocument() above cleared the
    // history, so the import is this document's base state, exactly as a
    // File > Open is. Nothing is left on the stack that could reach back past it.
    ImageDocument before;
    if (append)
        before = m_doc;

    // Adopt the imported file's palette: phases sliced from this sheet paint
    // with its registers, so the swatch bar follows the file instead of the
    // sliced colours being reported as overspill.
    if (!sheet.active.isEmpty()) {
        m_doc.setActive(sheet.active);
        if (!m_doc.active().contains(m_colour))
            m_colour = m_doc.active().first();
    }

    // A still image becomes a sprite-sheet target: the sheet mode shows it
    // and phases are sliced out of it, rather than the pixels being edited
    // in place.
    const int index = m_doc.addSheet(path, sheet.width, sheet.height);
    m_sheetSlicing.insert(index, sheet, m_doc.paletteKind());
    if (append)
        pushSnapshot(before, tr("import sheet"));

    m_actSheetMode->setChecked(true);
    m_sheetCanvas->setSheetIndex(index);
    rebuildSwatches();
    m_canvas->setCurrentColour(m_colour);
    refreshSheetView();
    notifyModified();
    return true;
}

bool ImageEditor::exportFile(const QString &path, bool spriteSafe)
{
    const StImageFormat format = stFormatFromPath(path);
    // A `.pim` is the document's own format, so exporting one is saving the
    // document: it keeps the file path and clears the modified flag, and there
    // is no single frame to pick.
    if (format == StImageFormat::Pim)
        return saveFile(path);
    QString error;
    const ImageDocument *sheet = &m_doc;
    ImageDocument safe;
    if (spriteSafe) {
        safe = spriteSafeDocument(m_doc, &error);
        if (!error.isEmpty()) {
            m_lastError = error;
            return false;
        }
        sheet = &safe;
    }
    return writeEncodedFile(path, *sheet, format, sheet->currentFrame());
}

/// Encode `doc` in the format `path` names and write it — the one tail both
/// exporters use, so they cannot drift on which formats exist or on what an
/// encoder that produces nothing means. An unknown extension and an encoder
/// that refuses are both failures that leave the target file alone; the
/// second exporter used to accept empty output and truncate the file to zero
/// bytes.
bool ImageEditor::writeEncodedFile(const QString &path, const ImageDocument &doc,
                                   StImageFormat format, int frame)
{
    if (format == StImageFormat::Unknown) {
        m_lastError = tr("Unknown export format for %1").arg(path);
        return false;
    }
    QString error;
    const QByteArray bytes =
        encodeStImage(doc, format, frame, QFileInfo(path).completeBaseName(), &error);
    if (bytes.isEmpty()) {
        m_lastError = error.isEmpty() ? tr("Export failed") : error;
        return false;
    }
    if (!writeStImage(path, bytes, &error)) {
        m_lastError = error;
        return false;
    }
    return true;
}

bool ImageEditor::isModifiedSinceLoad() const
{
    return m_doc.isModified();
}

QString ImageEditor::displayName() const
{
    if (m_filePath.isEmpty())
        return tr("untitled.pim");
    return QFileInfo(m_filePath).fileName();
}

void ImageEditor::applyAppearance()
{
    using appearance::Icon;
    for (QAbstractButton *button : m_tools->buttons()) {
        const auto tool = DrawTool(button->property("tool").toInt());
        if (tool == DrawTool::Brush)
            continue;
        button->setIcon(appearance::icon(iconForTool(tool)));
    }
    refreshBrushIcon();
    if (m_actUndo)
        m_actUndo->setIcon(appearance::icon(Icon::Undo));
    if (m_actRedo)
        m_actRedo->setIcon(appearance::icon(Icon::Redo));
    if (m_actCopy)
        m_actCopy->setIcon(appearance::icon(Icon::Copy));
    if (m_actCut)
        m_actCut->setIcon(appearance::icon(Icon::Cut));
    if (m_actPaste)
        m_actPaste->setIcon(appearance::icon(Icon::Paste));
    if (m_actGrid)
        m_actGrid->setIcon(appearance::icon(Icon::Grid));
    if (m_actFit)
        m_actFit->setIcon(appearance::icon(Icon::Fit));
    if (m_actZoomIn)
        m_actZoomIn->setIcon(appearance::icon(Icon::ZoomIn));
    if (m_actZoomOut)
        m_actZoomOut->setIcon(appearance::icon(Icon::ZoomOut));
    if (m_actPalette)
        m_actPalette->setIcon(appearance::icon(Icon::Palette));
    if (m_actFlipH)
        m_actFlipH->setIcon(appearance::icon(Icon::FlipHorizontal));
    if (m_actFlipV)
        m_actFlipV->setIcon(appearance::icon(Icon::FlipVertical));
    if (m_actRotate)
        m_actRotate->setIcon(appearance::icon(Icon::Rotate));
    if (m_actShiftLeft)
        m_actShiftLeft->setIcon(appearance::icon(Icon::ShiftLeft));
    if (m_actShiftRight)
        m_actShiftRight->setIcon(appearance::icon(Icon::ShiftRight));
    if (m_actShiftUp)
        m_actShiftUp->setIcon(appearance::icon(Icon::ShiftUp));
    if (m_actShiftDown)
        m_actShiftDown->setIcon(appearance::icon(Icon::ShiftDown));
    if (m_actReExport)
        m_actReExport->setIcon(appearance::icon(Icon::Save));
    m_previewWidget->applyAppearance();
    if (m_addFrame)
        m_addFrame->setIcon(appearance::icon(Icon::AddFrame));
    if (m_dupFrame)
        m_dupFrame->setIcon(appearance::icon(Icon::DuplicateFrame));
    if (m_removeFrame)
        m_removeFrame->setIcon(appearance::icon(Icon::RemoveFrame));
    if (m_addLayer)
        m_addLayer->setIcon(appearance::icon(Icon::AddFrame));
    if (m_removeLayer)
        m_removeLayer->setIcon(appearance::icon(Icon::RemoveFrame));
    if (m_addPhase)
        m_addPhase->setIcon(appearance::icon(Icon::AddFrame));
    if (m_removePhase)
        m_removePhase->setIcon(appearance::icon(Icon::RemoveFrame));
    rebuildSwatches();
    if (m_canvas) {
        m_canvas->setTool(m_tool);
        m_canvas->setCurrentColour(m_colour);
    }
}

void ImageEditor::undo()
{
    m_undo->undo();
    resyncFromRestoredDocument();
    refreshChrome();
    notifyModified();
}

void ImageEditor::redo()
{
    m_undo->redo();
    resyncFromRestoredDocument();
    refreshChrome();
    notifyModified();
}

void ImageEditor::resyncFromRestoredDocument()
{
    // A command's undo/redo assigns a whole document, so every piece of editor
    // state derived from it has to follow. The current colour can be outside the
    // restored palette (undo of a palette edit). And the imported-sheet pixels
    // are a cache of what the document's sheets name — an undone import drops a
    // sheet whose pixels must go with it, a redone one brings back a sheet whose
    // pixels have to be read again. Re-deriving them is exactly what a reopen
    // does, so the two can never disagree; a document with no sheets at all (the
    // usual case) skips the read.
    if (!m_doc.active().isEmpty() && !m_doc.active().contains(m_colour))
        m_colour = m_doc.active().first();
    m_canvas->setCurrentColour(m_colour);
    if (!m_doc.sheets().isEmpty() || m_sheetSlicing.hasAnyPixels())
        m_sheetSlicing.load(m_doc);
}

void ImageEditor::setTool()
{
    auto *button = m_tools->checkedButton();
    if (!button)
        return;
    m_tool = DrawTool(button->property("tool").toInt());
    m_canvas->setTool(m_tool);
}

void ImageEditor::selectSwatch()
{
    auto *button = m_swatches->checkedButton();
    if (!button)
        return;
    m_colour = button->property("cube").toInt();
    m_canvas->setCurrentColour(m_colour);
    refreshBrushIcon();
}

void ImageEditor::openPalettePicker()
{
    PalettePickerDialog dialog(m_doc.paletteKind(), m_doc.active(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    // A palette edit is a document mutation like any other: one accepted
    // dialog is one command, or an older command's undo would assign a `before`
    // taken with the old registers and silently revert this one.
    const ImageDocument before = m_doc;
    m_doc.setActive(dialog.active());
    if (m_doc.active() != before.active())
        pushSnapshot(before, tr("change palette"));
    if (!m_doc.active().contains(m_colour) && !m_doc.active().isEmpty())
        m_colour = m_doc.active().first();
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    updateOverspill();
    notifyModified();
}

void ImageEditor::copySelection()
{
    if (!m_canvas || !m_canvas->hasSelection()) {
        if (m_status)
            m_status->setText(tr("Nothing selected — drag a rectangle with the select tool."));
        return;
    }
    const QRect rect = m_canvas->selection();
    m_clip = regionData(m_doc.activeLayerPixels(), m_doc.width(), m_doc.height(), rect);
    m_clipWidth = rect.width();
    if (m_status)
        m_status->setText(tr("Copied %1 × %2.").arg(rect.width()).arg(rect.height()));
}

void ImageEditor::cutSelection()
{
    if (m_actSheetMode->isChecked() || !m_canvas || !m_canvas->hasSelection()) {
        if (m_status)
            m_status->setText(tr("Nothing selected — drag a rectangle with the select tool."));
        return;
    }
    const QRect rect = m_canvas->selection();
    const QVector<int> before = m_doc.activeLayerPixels();
    m_clip = regionData(before, m_doc.width(), m_doc.height(), rect);
    m_clipWidth = rect.width();
    const QVector<int> after =
        clearRegion(before, m_doc.width(), m_doc.height(), rect, kTransparent);
    applyLayerEdit(before, after, tr("cut selection"));
    if (m_status)
        m_status->setText(tr("Cut %1 × %2.").arg(rect.width()).arg(rect.height()));
}

void ImageEditor::pasteClipboard()
{
    if (m_actSheetMode->isChecked())
        return;
    if (m_clip.isEmpty() || m_clipWidth <= 0) {
        if (m_status)
            m_status->setText(tr("Clipboard is empty — copy or cut a selection first."));
        return;
    }
    const QPoint anchor =
        m_canvas->hasSelection() ? m_canvas->selection().topLeft() : QPoint(0, 0);
    const QVector<int> before = m_doc.activeLayerPixels();
    const QVector<int> after =
        stampRegion(before, m_doc.width(), m_doc.height(), m_clip, m_clipWidth, anchor);
    const QRect pasted = QRect(anchor, QSize(m_clipWidth, m_clip.size() / m_clipWidth))
                             .intersected(QRect(0, 0, m_doc.width(), m_doc.height()));
    if (applyLayerEdit(before, after, tr("paste"))) {
        // The pasted patch becomes the selection so it can be nudged into place.
        m_canvas->setSelection(pasted);
        if (m_status)
            m_status->setText(tr("Pasted %1 × %2 at %3, %4.")
                                  .arg(pasted.width())
                                  .arg(pasted.height())
                                  .arg(pasted.x())
                                  .arg(pasted.y()));
    }
}

void ImageEditor::deleteSelection()
{
    if (m_actSheetMode->isChecked() || !m_canvas || !m_canvas->hasSelection())
        return;
    const QRect rect = m_canvas->selection();
    const QVector<int> before = m_doc.activeLayerPixels();
    const QVector<int> after =
        clearRegion(before, m_doc.width(), m_doc.height(), rect, kTransparent);
    if (applyLayerEdit(before, after, tr("delete selection")) && m_status)
        m_status->setText(tr("Cleared %1 × %2.").arg(rect.width()).arg(rect.height()));
}

void ImageEditor::moveSelection(const QRect &source, const QPoint &delta)
{
    if (m_actSheetMode->isChecked() || source.isEmpty() || delta.isNull())
        return;
    const QVector<int> before = m_doc.activeLayerPixels();
    const QVector<int> after =
        moveRegion(before, m_doc.width(), m_doc.height(), source, delta);
    if (applyLayerEdit(before, after, tr("move selection")))
        m_canvas->setSelection(source.translated(delta));
}

void ImageEditor::nudgeSelection(const QPoint &step)
{
    if (!m_canvas || !m_canvas->hasSelection())
        return;
    moveSelection(m_canvas->selection(), step);
}

void ImageEditor::addFrame()
{
    const ImageDocument before = m_doc;
    m_doc.addFrame();
    // A frame added to a placed phase extends the strip: pull the next cell
    // from the sheet's pixels when they are available.
    m_sheetSlicing.sliceAddedFrame(m_doc);
    pushSnapshot(before, tr("add frame"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::removeFrame()
{
    const ImageDocument before = m_doc;
    if (!m_doc.removeFrame(m_doc.currentFrame()))
        return;
    pushSnapshot(before, tr("delete frame"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::duplicateFrame()
{
    const ImageDocument before = m_doc;
    m_doc.duplicateFrame(m_doc.currentFrame());
    pushSnapshot(before, tr("duplicate frame"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::moveFrameUp()
{
    const int from = m_doc.currentFrame();
    if (from <= 0)
        return;
    const ImageDocument before = m_doc;
    if (!m_doc.moveFrame(from, from - 1))
        return;
    pushSnapshot(before, tr("move frame"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::moveFrameDown()
{
    const int from = m_doc.currentFrame();
    if (from >= m_doc.frameCount() - 1)
        return;
    const ImageDocument before = m_doc;
    if (!m_doc.moveFrame(from, from + 1))
        return;
    pushSnapshot(before, tr("move frame"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::selectFrame(int row)
{
    if (row < 0)
        return;
    m_doc.setCurrentFrame(row);
    m_previewWidget->setFrame(row);
    refreshLayers();
    refreshOnion();
    refreshPreview();
    refreshCanvas();
    updateOverspill();
}

void ImageEditor::toggleGrid(bool on)
{
    m_canvas->setShowGrid(on);
}

void ImageEditor::fitToView()
{
    if (!m_scroll || !m_canvas || m_doc.width() <= 0 || m_doc.height() <= 0)
        return;
    const QSize vp = m_scroll->viewport()->size();
    if (vp.width() < 8 || vp.height() < 8)
        return;
    const int cell = qMin(vp.width() / m_doc.width(), vp.height() / m_doc.height());
    m_canvas->setCellSize(cell);
}

void ImageEditor::zoomIn()
{
    zoomBy(1);
}

void ImageEditor::zoomOut()
{
    zoomBy(-1);
}

void ImageEditor::zoomBy(int steps)
{
    if (!m_canvas || steps == 0)
        return;
    int cell = m_canvas->cellSize();
    const int dir = steps > 0 ? 1 : -1;
    int remaining = qAbs(steps);
    while (remaining-- > 0) {
        if (dir > 0) {
            if (cell < 8)
                ++cell;
            else if (cell < 24)
                cell += 2;
            else
                cell += 4;
        } else if (cell <= 8) {
            --cell;
        } else if (cell <= 24) {
            cell -= 2;
        } else {
            cell -= 4;
        }
    }
    m_canvas->setCellSize(cell);
}

bool ImageEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (m_phasePicker && watched == m_phasePicker
        && event->type() == QEvent::MouseButtonDblClick) {
        renamePhase();
        return true;
    }
    if (m_scroll && watched == m_scroll->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            zoomBy(wheel->angleDelta().y() > 0 ? 1 : -1);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ImageEditor::rebuildSwatches()
{
    delete m_swatches;
    m_swatches = nullptr;
    qDeleteAll(m_swatchBar->findChildren<QToolButton *>());
    if (!m_swatchBar->layout())
        m_swatchBar->setLayout(new QVBoxLayout);
    auto *box = qobject_cast<QVBoxLayout *>(m_swatchBar->layout());
    while (QLayoutItem *item = box->takeAt(0))
        delete item;

    m_swatches = new QButtonGroup(m_swatchBar);
    m_swatches->setExclusive(true);
    connect(m_swatches, &QButtonGroup::buttonClicked, this, &ImageEditor::selectSwatch);

    auto add = [&](int cube, const QString &label, const QColor &color) {
        auto *button = new QToolButton(m_swatchBar);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(28, 28);
        button->setProperty("cube", cube);
        button->setText(cube < 0 ? QStringLiteral("0") : QString::number(cube));
        button->setToolTip(cube < 0 ? label
                                    : tr("Colour %1 — %2").arg(cube).arg(label));
        button->setStyleSheet(swatchStyleSheet(color));
        m_swatches->addButton(button);
        box->addWidget(button);
        if (cube == m_colour)
            button->setChecked(true);
    };

    add(kTransparent, tr("Erase"), QColor(40, 40, 40));
    for (int cube : m_doc.active())
        add(cube, cubeColor(m_doc.paletteKind(), cube).name(),
            cubeColor(m_doc.paletteKind(), cube));
    box->addStretch();
    updateOverspill();
    refreshBrushIcon();
}

void ImageEditor::refreshBrushIcon()
{
    if (!m_tools)
        return;
    const QColor paint = m_doc.displayColor(m_colour);
    for (QAbstractButton *button : m_tools->buttons()) {
        if (DrawTool(button->property("tool").toInt()) != DrawTool::Brush)
            continue;
        button->setIcon(appearance::icon(appearance::Icon::Brush, paint));
    }
}

void ImageEditor::refreshFrames()
{
    if (!m_frames)
        return;
    m_frames->blockSignals(true);
    m_frames->clear();
    for (int i = 0; i < m_doc.frameCount(); ++i) {
        auto *item = new QListWidgetItem(QString::number(i + 1), m_frames);
        item->setIcon(QIcon(QPixmap::fromImage(frameThumbnail(m_doc, i))));
        item->setToolTip(tr("Frame %1").arg(i + 1));
    }
    m_frames->setCurrentRow(m_doc.currentFrame());
    m_frames->blockSignals(false);
}

void ImageEditor::refreshLayers()
{
    if (!m_layers)
        return;
    m_layers->blockSignals(true);
    m_layers->clear();
    const QVector<ImageLayer> &layers = m_doc.layers();
    for (int display = 0; display < layers.size(); ++display) {
        const int stack = stackIndexFromDisplay(display);
        const ImageLayer &layer = layers.at(stack);
        auto *item = new QListWidgetItem(layer.name, m_layers);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(layer.visible ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, stack);
        if (stack == m_doc.activeLayer())
            m_layers->setCurrentItem(item);
    }
    m_layers->blockSignals(false);
}

void ImageEditor::refreshPhases()
{
    if (!m_phases)
        return;
    m_phases->blockSignals(true);
    // Re-derive the highlight from the model, like refreshFrames() does — the
    // widget's own row is a second source of truth and goes stale when a phase
    // before the current one is removed (the model shifts its index, the old row
    // would then point at a different phase).
    const int selected = m_doc.currentPhase();
    m_phases->clear();
    for (const ImagePhase &phase : m_doc.phases()) {
        QString placement;
        if (phase.sheet >= 0)
            placement = tr("  @%1,%2").arg(phase.x).arg(phase.y);
        m_phases->addItem(tr("%1  %2\u00d7%3%4")
                              .arg(phase.name)
                              .arg(phase.cellW)
                              .arg(phase.cellH)
                              .arg(placement));
    }
    if (selected >= 0 && selected < m_phases->count())
        m_phases->setCurrentRow(selected);
    m_phases->blockSignals(false);

    if (m_phasePicker) {
        m_phasePicker->blockSignals(true);
        m_phasePicker->clear();
        for (const ImagePhase &phase : m_doc.phases())
            m_phasePicker->addItem(phase.name);
        if (selected >= 0 && selected < m_phasePicker->count())
            m_phasePicker->setCurrentIndex(selected);
        m_phasePicker->blockSignals(false);
    }
}

void ImageEditor::setSheetMode(bool on)
{
    if (!m_modeStack)
        return;
    m_modeStack->setCurrentIndex(on ? 1 : 0);
    if (on)
        clearSelection();
    // Paint tools make no sense over the composed sheet.
    for (QAbstractButton *button : m_tools->buttons())
        button->setEnabled(!on);
    m_actNewSheet->setEnabled(on);
    m_actSheetSource->setEnabled(on);
    if (m_phasePlacement)
        m_phasePlacement->setVisible(on);
    if (on)
        refreshSheetView();
}

void ImageEditor::onNewSheet()
{
    const ImageDocument before = m_doc;
    const int index = m_doc.addSheet(QString(), 320, 200);
    pushSnapshot(before, tr("new sheet"));
    m_sheetCanvas->setSheetIndex(index);
    refreshSheetView();
    refreshPhases();
    notifyModified();
}

bool ImageEditor::sheetModeActive() const
{
    // setSheetMode() selects the sheet page of the mode stack (index 1).
    return m_modeStack && m_modeStack->currentIndex() == 1;
}

void ImageEditor::refreshSheetView()
{
    if (!m_sheetCanvas)
        return;
    int sheet = 0;
    if (m_doc.phases().at(m_doc.currentPhase()).sheet >= 0)
        sheet = m_doc.phases().at(m_doc.currentPhase()).sheet;
    else if (m_doc.sheets().isEmpty())
        sheet = -1;
    m_sheetCanvas->setSheetIndex(sheet);
    m_sheetCanvas->setUnderlay(m_sheetSlicing.underlay(sheet));
    m_sheetCanvas->setSelectedPhase(m_phases ? m_phases->currentRow() : -1);
    m_sheetCanvas->setDocument(&m_doc);
    m_sheetCanvas->updateGeometry();
    m_sheetCanvas->adjustSize();
    m_sheetCanvas->update();
}

void ImageEditor::refreshPhasePlacement()
{
    if (!m_phaseSheet || !m_phaseX || !m_phaseY)
        return;
    const ImagePhase &current = m_doc.phases().at(m_doc.currentPhase());
    m_phaseSheet->blockSignals(true);
    m_phaseSheet->clear();
    m_phaseSheet->addItem(tr("unplaced"), -1);
    for (int i = 0; i < m_doc.sheets().size(); ++i) {
        const ImageSheet &sheet = m_doc.sheets().at(i);
        m_phaseSheet->addItem(sheet.path.isEmpty()
                                  ? tr("Sheet %1").arg(i + 1)
                                  : QFileInfo(sheet.path).fileName(),
                              i);
    }
    const int index = m_phaseSheet->findData(current.sheet);
    m_phaseSheet->setCurrentIndex(index >= 0 ? index : 0);
    m_phaseSheet->blockSignals(false);

    m_phaseX->blockSignals(true);
    m_phaseY->blockSignals(true);
    const bool placed = current.sheet >= 0;
    m_phaseX->setEnabled(placed);
    m_phaseY->setEnabled(placed);
    if (placed) {
        m_phaseX->setValue(current.x);
        m_phaseY->setValue(current.y);
    }
    m_phaseX->blockSignals(false);
    m_phaseY->blockSignals(false);

    m_phaseCellW->blockSignals(true);
    m_phaseCellH->blockSignals(true);
    m_phaseCellW->setValue(current.cellW);
    m_phaseCellH->setValue(current.cellH);
    m_phaseCellW->blockSignals(false);
    m_phaseCellH->blockSignals(false);
}

void ImageEditor::onPhasePlacementChanged()
{
    const int phase = m_doc.currentPhase();
    const int sheet = m_phaseSheet->currentData().toInt();
    const int oldSheet = m_doc.phases().at(phase).sheet;
    const int oldX = m_doc.phases().at(phase).x;
    const int oldY = m_doc.phases().at(phase).y;
    const ImageDocument before = m_doc;
    if (!m_doc.setPhasePlacement(phase, sheet, m_phaseX->value(), m_phaseY->value()))
        return;
    // Attaching or moving a phase slices the art under frames that are
    // still untouched; drawn frames keep their pixels.
    slicePlacedPhaseFrames(phase);
    reSliceUntouchedFrames(phase, oldSheet, oldX, oldY);
    // One command per panel edit, with the pixels folded in: `before` predates
    // the placement and the slicing it triggers. This is not the "metadata
    // tweak" it used to claim to be — the sheet drag path for the very same
    // mutation has always snapshotted, and an untracked one here let an older
    // command's undo assign a document from before it.
    pushSnapshot(before, tr("place phase"));
    refreshPhases();
    refreshSheetView();
    notifyModified();
}

void ImageEditor::onPhaseCellSizeChanged()
{
    const ImageDocument before = m_doc;
    if (!m_doc.setPhaseCellSize(m_doc.currentPhase(), m_phaseCellW->value(),
                                m_phaseCellH->value(), nullptr)) {
        refreshPhasePlacement();   // revert the out-of-range value
        return;
    }
    pushSnapshot(before, tr("change cell size"));
    clearSelection();
    refreshCanvas();
    refreshSheetView();
    notifyModified();
}

void ImageEditor::slicePlacedPhaseFrames(int phaseIndex)
{
    // The cut itself lives with the sheet pixels and the document it slices
    // into; the refresh that follows is the editor's.
    if (!m_sheetSlicing.slicePlacedPhaseFrames(m_doc, phaseIndex))
        return;
    refreshChrome();
    refreshPreview();
    notifyModified();
}

void ImageEditor::reSliceUntouchedFrames(int phaseIndex, int oldSheet, int oldX, int oldY)
{
    if (!m_sheetSlicing.reSliceUntouchedFrames(m_doc, phaseIndex, oldSheet, oldX, oldY))
        return;
    refreshChrome();
    refreshPreview();
    notifyModified();
}

void ImageEditor::slicePhaseFromSheet()
{
    const int sheet = m_sheetCanvas->sheetIndex();
    const ImagePhase &current = m_doc.phases().at(m_doc.currentPhase());
    SlicePhaseDialog dialog(this, tr("Phase %1").arg(m_doc.phaseCount() + 1),
                            current.cellW, current.cellH,
                            m_doc.sheets().at(sheet).width);
    dialog.onChanged = [this, sheet](const SlicePhaseDialog::Values &v) {
        QVector<QRect> cells;
        for (int k = 0; k < v.count; ++k)
            cells.append(QRect(v.x + k * v.cellW, v.y, v.cellW, v.cellH));
        m_sheetCanvas->setSlicePreview(cells);
    };
    dialog.onChanged(dialog.values());

    if (dialog.exec() != QDialog::Accepted) {
        m_sheetCanvas->clearSlicePreview();
        return;
    }
    const SlicePhaseDialog::Values v = dialog.values();
    m_sheetCanvas->clearSlicePreview();
    const int index = addPhaseFromSheet(sheet, v.name, v.x, v.y, v.cellW, v.cellH, v.count);
    if (index < 0)
        return;
    // Cut cells that came out entirely empty are almost always a misplaced
    // slice: say so instead of failing silently.
    const QVector<ImageFrame> &frames = m_doc.phases().at(index).frames;
    bool anyContent = false;
    for (const ImageFrame &frame : frames)
        for (int value : frame.composite)
            anyContent |= value >= 0;
    if (m_status) {
        m_status->setText(anyContent
            ? tr("Sliced %1 frame(s) from the sheet.").arg(frames.size())
            : tr("Sliced %1 frame(s), but they are all empty — check the position "
                 "and size against the sheet.").arg(frames.size()));
    }
}

int ImageEditor::addPhaseFromSheet(int sheetIndex, const QString &name, int x, int y,
                                   int cellW, int cellH, int count)
{
    if (!m_sheetSlicing.hasSheet(sheetIndex))
        return -1;
    // The slice is one document mutation, so its undo entry is one snapshot
    // taken over it (pixels folded in, like every other mutation here).
    const ImageDocument before = m_doc;
    const int index = m_sheetSlicing.addPhaseFromSheet(m_doc, sheetIndex, name, x, y, cellW,
                                                       cellH, count);
    if (index < 0)
        return -1;
    pushSnapshot(before, tr("add phase from sheet"));
    refreshChrome();
    m_phases->setCurrentRow(index);
    notifyModified();
    return index;
}

int ImageEditor::currentSheetIndex() const
{
    if (m_doc.sheets().isEmpty())
        return -1;
    const int sheet = m_doc.phases().at(m_doc.currentPhase()).sheet;
    return sheet >= 0 ? sheet : 0;
}

bool ImageEditor::exportSheetFile(const QString &path, int sheetIndex, bool spriteSafe)
{
    QString error;
    ImageDocument composed = composeSheet(m_doc, sheetIndex, &error);
    if (!error.isEmpty()) {
        m_lastError = error;
        return false;
    }
    const StImageFormat format = stFormatFromPath(path);
    // A `.pim` carries the whole composed sheet losslessly, so there is
    // nothing for the sprite-safe colour shuffle to protect; the still-image
    // formats reserve colour 0 for it, which is what makes them re-import.
    if (spriteSafe && format != StImageFormat::Pim) {
        ImageDocument safe = spriteSafeDocument(composed, &error);
        if (!error.isEmpty()) {
            m_lastError = error;
            return false;
        }
        composed = safe;
    }
    // The composed sheet is one frame (`composeSheet` builds a single-frame
    // document), and the shared tail knows the Pim case the sheet path used
    // to miss.
    return writeEncodedFile(path, composed, format, 0);
}

void ImageEditor::forgetBitplaneExport()
{
    m_bitplaneExport->forget();
}

void ImageEditor::refreshReExport()
{
    if (!m_actReExport)
        return;
    const bool ready = m_bitplaneExport->canReExport();
    m_actReExport->setEnabled(ready);
    if (!ready) {
        m_actReExport->setToolTip(
            tr("Write the last bitplane export again, with the same phase, blocks and file "
               "and no dialog (Ctrl+Shift+E) — enabled once one has been exported"));
        return;
    }
    const QString name = QFileInfo(m_bitplaneExport->lastExportPath()).fileName();
    m_actReExport->setToolTip(m_bitplaneExport->lastExportScroller().isEmpty()
        ? tr("Write %1 again with the same phase and blocks, no dialog (Ctrl+Shift+E)").arg(name)
        : tr("Write %1 and %2 again with the same phase and blocks, no dialog (Ctrl+Shift+E)")
              .arg(name, QFileInfo(m_bitplaneExport->lastExportScroller()).fileName()));
}

void ImageEditor::setBitplaneExportScroller(const QString &scroller)
{
    m_bitplaneExport->setScroller(scroller);
}

bool ImageEditor::exportBitplaneFile(const QString &path, int phase,
                                     const BitplaneDataOptions &options)
{
    if (m_bitplaneExport->exportBitplane(path, phase, options))
        return true;
    m_lastError = m_bitplaneExport->lastError();
    return false;
}

bool ImageEditor::exportScrollDemoFile(const QString &path, int phase,
                                       const BitplaneDataOptions &options,
                                       const QString &dataFile)
{
    if (m_bitplaneExport->writeScrollDemo(path, phase, options, dataFile))
        return true;
    m_lastError = m_bitplaneExport->lastError();
    return false;
}

bool ImageEditor::reExportBitplaneData()
{
    if (m_bitplaneExport->reExport())
        return true;
    m_lastError = m_bitplaneExport->lastError();
    return false;
}

void ImageEditor::reExportBitplane()
{
    m_bitplaneExport->reExport();
}

bool ImageEditor::canReExportBitplane() const
{
    return m_bitplaneExport->canReExport();
}

QString ImageEditor::lastBitplaneExportPath() const
{
    return m_bitplaneExport->lastExportPath();
}

int ImageEditor::lastBitplaneExportPhase() const
{
    return m_bitplaneExport->lastExportPhase();
}

BitplaneDataOptions ImageEditor::lastBitplaneExportOptions() const
{
    return m_bitplaneExport->lastExportOptions();
}

void ImageEditor::refreshOnion()
{
    if (!m_canvas)
        return;
    const QVector<OnionGhost> ghosts = neighbourFrames(m_doc.currentFrame(), m_doc.frameCount(),
                                                       m_onionDistance);
    if (ghosts.isEmpty()) {
        m_canvas->setOnion({});
        return;
    }
    m_canvas->setOnion(m_doc.frame(ghosts.first().index));
}

void ImageEditor::refreshPreview()
{
    m_previewWidget->refresh();
    if (m_actRotate)
        m_actRotate->setEnabled(m_doc.width() == m_doc.height());
}

void ImageEditor::refreshCanvas()
{
    m_canvas->setDocument(&m_doc);
    m_canvas->updateGeometry();
    m_canvas->adjustSize();
    m_canvas->update();
    // Frame adds/removes and cell-size changes change the composed strips.
    refreshSheetView();
    refreshOnion();
    updateStatus();
}

void ImageEditor::refreshChrome()
{
    refreshFrames();
    refreshLayers();
    refreshPhases();
    refreshPhasePlacement();
    refreshCanvas();
    refreshPreview();
    updateOverspill();
}

void ImageEditor::updateStatus()
{
    if (!m_status)
        return;
    const int zoom = m_canvas ? m_canvas->cellSize() * 100 : 100;
    m_status->setText(tr("%1 × %2  frame %3/%4  %5%  %6")
                          .arg(m_doc.width())
                          .arg(m_doc.height())
                          .arg(m_doc.currentFrame() + 1)
                          .arg(m_doc.frameCount())
                          .arg(zoom)
                          .arg(imageStatusColour(m_colour)));
    if (m_canvas) {
        if (m_actZoomIn)
            m_actZoomIn->setEnabled(m_canvas->cellSize() < ImageCanvas::kMaxCellSize);
        if (m_actZoomOut)
            m_actZoomOut->setEnabled(m_canvas->cellSize() > ImageCanvas::kMinCellSize);
    }
}

void ImageEditor::updateOverspill()
{
    const QVector<int> extra = m_doc.overspill();
    if (extra.isEmpty()) {
        m_overspill->clear();
        return;
    }
    m_overspill->setText(tr("%1 colour(s) used but not in the active palette.")
                             .arg(extra.size()));
}

void ImageEditor::paintIndices(const QVector<int> &indices, int colour)
{
    if (indices.isEmpty())
        return;
    if (m_strokeIndices.isEmpty()) {
        m_strokeColour = colour;
        m_strokePhase = m_doc.currentPhase();
        m_strokeLayer = m_doc.activeLayer();
        m_strokeFrame = m_doc.currentFrame();
        // Fresh membership set per stroke, sized to the frame. The phase's cell
        // size is fixed for the duration of a stroke, so this covers every index.
        m_strokeSeen = QBitArray(m_doc.pixelCount(), false);
    }
    const QVector<int> &layer = m_doc.activeLayerPixels();
    for (int index : indices) {
        // Dedup so m_strokeBefore records each cell's value the first time it is
        // painted (undo must restore the original, not a colour painted earlier in
        // the same stroke). O(1) bit test replaces the O(n) contains() scan.
        if (index >= 0 && index < m_strokeSeen.size()) {
            if (m_strokeSeen.testBit(index))
                continue;
            m_strokeSeen.setBit(index);
        }
        m_strokeIndices.append(index);
        m_strokeBefore.append(layer.value(index, kTransparent));
    }
    m_doc.fillIndices(indices, colour, m_strokeLayer);
    // A mouse-move does the least that shows the new pixels: one repaint of
    // the grid canvas, whose image is rebuilt once inside paintEvent. The
    // full refresh cascade this used to run cost a second ARGB32 rebuild per
    // event (setDocument's, thrown away by paintEvent's), a scaled preview
    // QImage and QPixmap, a full-frame overspill() scan and a geometry pass
    // over the hidden sheet view. The preview and the overspill count are
    // recomputed once, in finishStroke(), where the stroke's pixels are final
    // and the sheet view (if it is the visible mode) is redrawn.
    m_canvas->invalidateImage();
    if (sheetModeActive())
        refreshSheetView();
    notifyModified();
}

void ImageEditor::finishStroke()
{
    if (m_strokeIndices.isEmpty())
        return;
    m_undo->push(new PaintCommand(&m_doc, m_strokePhase, m_strokeFrame, m_strokeLayer,
                                  m_strokeIndices, m_strokeBefore, m_strokeColour,
                                  tr("paint")));
    m_strokeIndices.clear();
    m_strokeBefore.clear();
    m_strokeSeen.clear();
    // The stroke's pixels are final now, so the two views that depend on the
    // whole frame are rebuilt once instead of once per mouse-move: the
    // animation preview and the overspill count.
    refreshPreview();
    updateOverspill();
}

void ImageEditor::pickColour(int cubeIndex)
{
    // The eye-dropper pulls the picked cube into the active set when it is not
    // in it, which edits the document's palette: that needs the same undo entry
    // an accepted palette dialog gets. Picking a colour already in the set is
    // not a document change and must not commit one.
    const ImageDocument before = m_doc;
    m_colour = cubeIndex;
    if (cubeIndex >= 0 && !m_doc.active().contains(cubeIndex))
        m_doc.toggleActive(cubeIndex);
    if (m_doc.active() != before.active())
        pushSnapshot(before, tr("add colour to palette"));
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    notifyModified();
}

void ImageEditor::notifyModified()
{
    emit modificationChanged(m_doc.isModified());
}

void ImageEditor::pushSnapshot(const ImageDocument &before, const QString &text)
{
    m_undo->push(new SnapshotCommand(&m_doc, before, text));
}

void ImageEditor::applyLayerBuffer(const QVector<int> &before, const QString &text)
{
    m_undo->push(new LayerPixelsCommand(&m_doc, m_doc.currentPhase(), m_doc.currentFrame(),
                                        m_doc.activeLayer(), before,
                                        m_doc.activeLayerPixels(), text));
}

bool ImageEditor::applyLayerEdit(const QVector<int> &before, const QVector<int> &after,
                                 const QString &text)
{
    if (after == before)
        return false;
    m_doc.replaceActiveLayer(after);
    applyLayerBuffer(before, text);
    refreshCanvas();
    refreshPreview();
    updateOverspill();
    notifyModified();
    return true;
}
void ImageEditor::clearSelection()
{
    if (m_canvas)
        m_canvas->setSelection(QRect());
}

void ImageEditor::shiftBy(ShiftDirection direction)
{
    const QVector<int> before = m_doc.activeLayerPixels();
    if (!m_doc.shiftActiveLayer(direction))
        return;
    applyLayerBuffer(before, tr("shift"));
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

int ImageEditor::stackIndexFromDisplay(int displayRow) const
{
    return m_doc.layerCount() - 1 - displayRow;
}

void ImageEditor::flipHorizontal()
{
    const QVector<int> before = m_doc.activeLayerPixels();
    if (!m_doc.flipActiveLayer(FlipDirection::Horizontal))
        return;
    applyLayerBuffer(before, tr("flip horizontal"));
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::flipVertical()
{
    const QVector<int> before = m_doc.activeLayerPixels();
    if (!m_doc.flipActiveLayer(FlipDirection::Vertical))
        return;
    applyLayerBuffer(before, tr("flip vertical"));
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::rotate90()
{
    if (m_doc.width() != m_doc.height())
        return;
    const QVector<int> before = m_doc.activeLayerPixels();
    // The exact quarter-turn (a pixel permutation), not the general resampler:
    // same picture for a right angle, and no float pass over the cell.
    if (!m_doc.replaceActiveLayer(
            rotate90Cw(before, m_doc.width(), m_doc.height(), nullptr, nullptr)))
        return;
    applyLayerBuffer(before, tr("rotate 90"));
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::generateEightWay()
{
    const ImageDocument before = m_doc;
    if (m_doc.generateRotations(8) <= 0)
        return;
    pushSnapshot(before, tr("bake rotations"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::shiftLeft()
{
    shiftBy(ShiftDirection::Left);
}

void ImageEditor::shiftRight()
{
    shiftBy(ShiftDirection::Right);
}

void ImageEditor::shiftUp()
{
    shiftBy(ShiftDirection::Up);
}

void ImageEditor::shiftDown()
{
    shiftBy(ShiftDirection::Down);
}

void ImageEditor::onionChanged()
{
    if (!m_onion)
        return;
    m_onionDistance = m_onion->currentData().toInt();
    refreshOnion();
}

void ImageEditor::addLayer()
{
    const ImageDocument before = m_doc;
    m_doc.addLayer();
    pushSnapshot(before, tr("add layer"));
    refreshLayers();
    refreshCanvas();
    notifyModified();
}

void ImageEditor::removeLayer()
{
    const ImageDocument before = m_doc;
    if (!m_doc.removeLayer(m_doc.activeLayer()))
        return;
    pushSnapshot(before, tr("delete layer"));
    refreshLayers();
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::moveLayerUp()
{
    const int from = m_doc.activeLayer();
    if (from >= m_doc.layerCount() - 1)
        return;
    const ImageDocument before = m_doc;
    if (!m_doc.moveLayer(from, from + 1))
        return;
    pushSnapshot(before, tr("move layer"));
    refreshLayers();
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::moveLayerDown()
{
    const int from = m_doc.activeLayer();
    if (from <= 0)
        return;
    const ImageDocument before = m_doc;
    if (!m_doc.moveLayer(from, from - 1))
        return;
    pushSnapshot(before, tr("move layer"));
    refreshLayers();
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::selectLayer(int row)
{
    if (row < 0)
        return;
    m_doc.setActiveLayer(stackIndexFromDisplay(row));
}

void ImageEditor::layerVisibilityChanged(QListWidgetItem *item)
{
    if (!item)
        return;
    const int stack = item->data(Qt::UserRole).toInt();
    if (stack < 0 || stack >= m_doc.layerCount())
        return;
    const bool visible = item->checkState() == Qt::Checked;
    if (m_doc.layers().at(stack).visible == visible)
        return;
    const ImageDocument before = m_doc;
    m_doc.setLayerVisible(stack, visible);
    pushSnapshot(before, tr("layer visibility"));
    refreshCanvas();
    refreshPreview();
    notifyModified();
}

void ImageEditor::renameLayer()
{
    const int index = m_doc.activeLayer();
    if (index < 0 || index >= m_doc.layerCount())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename layer"), tr("Name:"),
                                               QLineEdit::Normal, m_doc.layers().at(index).name,
                                               &ok);
    if (!ok)
        return;
    const ImageDocument before = m_doc;
    if (!m_doc.renameLayer(index, name))
        return;
    pushSnapshot(before, tr("rename layer"));
    refreshLayers();
    notifyModified();
}

void ImageEditor::addPhase()
{
    // Over a freshly imported sheet, adding a phase slices cells out of it.
    const int sheet = m_sheetCanvas ? m_sheetCanvas->sheetIndex() : -1;
    if (m_actSheetMode->isChecked() && sheet >= 0 && m_sheetSlicing.hasSheet(sheet)) {
        slicePhaseFromSheet();
        return;
    }
    if (m_actSheetMode->isChecked() && sheet >= 0 && m_status) {
        m_status->setText(tr("This sheet's pixels are not loaded in this session — "
                             "re-import the file to slice phases from it."));
    }

    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("New phase"), tr("Name:"),
                                               QLineEdit::Normal,
                                               tr("Phase %1").arg(m_doc.phaseCount() + 1), &ok);
    if (!ok)
        return;
    const int cellW = QInputDialog::getInt(this, tr("New phase"), tr("Sprite width:"),
                                           m_doc.width(), 1, 320, 1, &ok);
    if (!ok)
        return;
    const int cellH = QInputDialog::getInt(this, tr("New phase"), tr("Sprite height:"),
                                           m_doc.height(), 1, 200, 1, &ok);
    if (!ok)
        return;
    const ImageDocument before = m_doc;
    const int index = m_doc.addPhase(name, cellW, cellH);
    pushSnapshot(before, tr("add phase"));
    refreshChrome();
    m_phases->setCurrentRow(index);
    notifyModified();
}

void ImageEditor::removePhase()
{
    const int row = m_phases->currentRow();
    const ImageDocument before = m_doc;
    if (!m_doc.removePhase(row))
        return;
    pushSnapshot(before, tr("delete phase"));
    refreshChrome();
    notifyModified();
}

void ImageEditor::selectPhase(int row)
{
    // Selecting a phase switches the editing context: the frames panel, the
    // canvas and the preview all follow the phase's own frames.
    if (row < 0 || row >= m_doc.phases().size() || !m_doc.setCurrentPhase(row))
        return;
    // The selection is a rectangle in the old phase's grid; the new phase may
    // have a different cell size, so start clean.
    clearSelection();
    refreshPreview();
    refreshChrome();
}

void ImageEditor::renamePhase()
{
    const int row = m_phases->currentRow();
    if (row < 0 || row >= m_doc.phases().size())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename phase"), tr("Name:"),
                                               QLineEdit::Normal, m_doc.phases().at(row).name, &ok);
    if (!ok)
        return;
    const ImageDocument before = m_doc;
    if (!m_doc.renamePhase(row, name))
        return;
    pushSnapshot(before, tr("rename phase"));
    refreshPhases();
    notifyModified();
}

} // namespace pist
