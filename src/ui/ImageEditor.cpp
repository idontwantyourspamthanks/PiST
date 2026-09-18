// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ImageEditor.h"

#include "image/StFormats.h"
#include "image/Transform.h"
#include "ui/Appearance.h"
#include "ui/ImageCanvas.h"
#include "ui/SheetCanvas.h"
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
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPixmap>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QFormLayout>
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
                 const QVector<int> &indices, const QVector<int> &before, int colour)
        : m_doc(doc)
        , m_phase(phase)
        , m_frame(frame)
        , m_layer(layer)
        , m_indices(indices)
        , m_before(before)
        , m_colour(colour)
    {
        setText(QStringLiteral("paint"));
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
    return QStringLiteral("QToolButton { background-color: %1; border: 2px solid %2;"
                          " border-radius: 3px; }"
                          "QToolButton:checked { border: 3px solid %3; }")
        .arg(color.name(), mute.name(), ring.name());
}

/// The "slice a phase out of the imported sheet" dialog. Every field change
/// repaints the preview boxes on the sheet canvas, so the numbers can be
/// checked against the art before anything is cut.
class SlicePhaseDialog : public QDialog
{
public:
    SlicePhaseDialog(QWidget *parent, const QString &suggestedName, int defaultW,
                     int defaultH, int sheetWidth)
    {
        setWindowTitle(tr("Slice phase from sheet"));
        auto *form = new QFormLayout(this);

        m_name = new QLineEdit(suggestedName, this);
        m_name->setObjectName(QStringLiteral("sliceName"));
        form->addRow(tr("Name:"), m_name);

        m_w = makeSpin(QStringLiteral("sliceCellW"), defaultW, 1, 320);
        form->addRow(tr("Sprite width:"), m_w);
        m_h = makeSpin(QStringLiteral("sliceCellH"), defaultH, 1, 200);
        form->addRow(tr("Sprite height:"), m_h);
        m_x = makeSpin(QStringLiteral("sliceX"), 0, 0, sheetWidth - 1);
        form->addRow(tr("First cell x:"), m_x);
        m_y = makeSpin(QStringLiteral("sliceY"), 0, 0, 199);
        form->addRow(tr("First cell y:"), m_y);
        // A strip runs left to right, so pre-fill how many whole cells fit.
        m_count = makeSpin(QStringLiteral("sliceCount"),
                           qMax(1, (sheetWidth - m_x->value()) / qMax(1, m_w->value())), 1, 99);
        form->addRow(tr("Frame count:"), m_count);

        auto connect_ = [this](QSpinBox *box) {
            connect(box, qOverload<int>(&QSpinBox::valueChanged), this,
                    [this] { if (onChanged) onChanged(values()); });
        };
        connect_(m_w);
        connect_(m_h);
        connect_(m_x);
        connect_(m_y);
        connect_(m_count);
        connect(m_name, &QLineEdit::textChanged, this,
                [this] { if (onChanged) onChanged(values()); });

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                             this);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        form->addRow(buttons);
    }

    struct Values {
        QString name;
        int x = 0;
        int y = 0;
        int cellW = 32;
        int cellH = 32;
        int count = 1;
    };

    Values values() const
    {
        return {m_name->text().trimmed(), m_x->value(), m_y->value(), m_w->value(),
                m_h->value(), m_count->value()};
    }

    std::function<void(const Values &)> onChanged;

private:
    QSpinBox *makeSpin(const QString &objectName, int value, int min, int max)
    {
        auto *box = new QSpinBox(this);
        box->setObjectName(objectName);
        box->setRange(min, max);
        box->setValue(value);
        return box;
    }

    QLineEdit *m_name = nullptr;
    QSpinBox *m_w = nullptr;
    QSpinBox *m_h = nullptr;
    QSpinBox *m_x = nullptr;
    QSpinBox *m_y = nullptr;
    QSpinBox *m_count = nullptr;
};

QImage sheetUnderlayImage(const ImportedSheet &sheet, PaletteKind kind)
{
    QImage underlay(sheet.width, sheet.height, QImage::Format_ARGB32);
    for (int y = 0; y < sheet.height; ++y) {
        auto *line = reinterpret_cast<QRgb *>(underlay.scanLine(y));
        for (int x = 0; x < sheet.width; ++x) {
            const int value = sheet.pixels.at(y * sheet.width + x);
            if (value < 0)
                line[x] = qRgb(50, 50, 50);
            else {
                const Rgb rgb = cubeRgb(kind, value);
                line[x] = qRgb(rgb.r, rgb.g, rgb.b);
            }
        }
    }
    return underlay;
}

/// Upper bound on a sheet file read back from a JSON-supplied path. ST images
/// are tiny; this exists only to bound an untrusted path that points at a large
/// regular file (the read is otherwise unbounded — see loadSheetPixels).
constexpr qint64 kMaxSheetBytes = 64LL * 1024 * 1024;

/// (Re)load the pixel data of every sheet target that has a readable file,
/// so slicing works after a document is reopened. Sheets without a path or
/// with a missing file stay pixel-less until re-imported.
void loadSheetPixels(ImageDocument &doc, QHash<int, ImportedSheet> &pixels,
                     QHash<int, QImage> &underlays)
{
    pixels.clear();
    underlays.clear();
    for (int i = 0; i < doc.sheets().size(); ++i) {
        const ImageSheet &sheet = doc.sheets().at(i);
        // The path comes from the file being loaded, so treat it as untrusted:
        // require a regular file (not /dev/zero, a FIFO or a directory — all of
        // which QFileInfo::exists reports true) and bound its size, or readAll()
        // on a special or huge file hangs the open or exhausts memory.
        const QFileInfo info(sheet.path);
        if (sheet.path.isEmpty() || !info.isFile() || info.size() > kMaxSheetBytes)
            continue;
        QFile file(sheet.path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        ImportedSheet imported;
        if (importStImage(file.readAll(), stFormatFromPath(sheet.path), doc.paletteKind(),
                          &imported, nullptr)) {
            pixels.insert(i, imported);
            underlays.insert(i, sheetUnderlayImage(imported, doc.paletteKind()));
        }
    }
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

ImageEditor::ImageEditor(QWidget *parent)
    : QWidget(parent)
{
    m_undo = new QUndoStack(this);
    m_doc = ImageDocument::create(32, 32, PaletteKind::Ste);
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

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
    auto addTransform = [&](QAction *&action, appearance::Icon icon, const QString &name,
                            const QString &objectName, void (ImageEditor::*method)()) {
        action = new QAction(name, this);
        action->setIcon(appearance::icon(icon));
        action->setObjectName(objectName);
        action->setToolTip(name);
        connect(action, &QAction::triggered, this, method);
        addAction(action);
        bar->addAction(action);
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

    layout->addWidget(bar);

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
    body->addWidget(m_modeStack, 1);

    // m_phases does not exist yet at this point in the constructor, so the
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
            m_status->setText(tr("%1 × %2  frame %3/%4  %5%")
                                  .arg(m_doc.width())
                                  .arg(m_doc.height())
                                  .arg(m_doc.currentFrame() + 1)
                                  .arg(m_doc.frameCount())
                                  .arg(zoom));
        else
            m_status->setText(tr("%1 × %2  (%3, %4)  %5%")
                                  .arg(m_doc.width())
                                  .arg(m_doc.height())
                                  .arg(index % m_doc.width())
                                  .arg(index / m_doc.width())
                                  .arg(zoom));
    });

    auto *right = new QVBoxLayout;
    right->setSpacing(4);

    right->addWidget(new QLabel(tr("Frames"), this));
    m_frames = new QListWidget(this);
    m_frames->setObjectName(QStringLiteral("imageFrames"));
    m_frames->setMaximumWidth(160);
    connect(m_frames, &QListWidget::currentRowChanged, this, &ImageEditor::selectFrame);
    right->addWidget(m_frames, 1);

    auto makeIconButton = [this](appearance::Icon icon, const QString &tip) {
        auto *button = new QToolButton(this);
        button->setIcon(appearance::icon(icon));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        return button;
    };
    m_addFrame = makeIconButton(appearance::Icon::AddFrame, tr("Add frame"));
    m_addFrame->setObjectName(QStringLiteral("imageAddFrame"));
    connect(m_addFrame, &QToolButton::clicked, this, &ImageEditor::addFrame);
    m_dupFrame = makeIconButton(appearance::Icon::DuplicateFrame, tr("Duplicate frame"));
    connect(m_dupFrame, &QToolButton::clicked, this, &ImageEditor::duplicateFrame);
    m_removeFrame = makeIconButton(appearance::Icon::RemoveFrame, tr("Delete frame"));
    connect(m_removeFrame, &QToolButton::clicked, this, &ImageEditor::removeFrame);
    m_frameUp = new QToolButton(this);
    m_frameUp->setText(QStringLiteral("▲"));
    m_frameUp->setToolTip(tr("Move frame up"));
    m_frameUp->setAutoRaise(true);
    connect(m_frameUp, &QToolButton::clicked, this, &ImageEditor::moveFrameUp);
    m_frameDown = new QToolButton(this);
    m_frameDown->setText(QStringLiteral("▼"));
    m_frameDown->setToolTip(tr("Move frame down"));
    m_frameDown->setAutoRaise(true);
    connect(m_frameDown, &QToolButton::clicked, this, &ImageEditor::moveFrameDown);
    auto *frameBtns = new QHBoxLayout;
    frameBtns->addWidget(m_addFrame);
    frameBtns->addWidget(m_dupFrame);
    frameBtns->addWidget(m_removeFrame);
    frameBtns->addWidget(m_frameUp);
    frameBtns->addWidget(m_frameDown);
    right->addLayout(frameBtns);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("imagePreview"));
    m_preview->setMinimumSize(96, 96);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setScaledContents(false);
    right->addWidget(m_preview);

    auto *playRow = new QHBoxLayout;
    m_actPlay = new QAction(tr("Play"), this);
    m_actPlay->setIcon(appearance::icon(appearance::Icon::Run));
    m_actPlay->setCheckable(true);
    m_actPlay->setToolTip(tr("Play animation"));
    connect(m_actPlay, &QAction::toggled, this, &ImageEditor::togglePlay);
    auto *playBtn = new QToolButton(this);
    playBtn->setObjectName(QStringLiteral("imagePlay"));
    playBtn->setDefaultAction(m_actPlay);
    playBtn->setAutoRaise(true);
    playRow->addWidget(playBtn);
    m_fpsBox = new QSpinBox(this);
    m_fpsBox->setObjectName(QStringLiteral("imageFps"));
    m_fpsBox->setRange(1, 60);
    m_fpsBox->setValue(m_fps);
    m_fpsBox->setSuffix(QStringLiteral(" fps"));
    m_fpsBox->setMaximumWidth(88);
    connect(m_fpsBox, qOverload<int>(&QSpinBox::valueChanged), this, &ImageEditor::fpsChanged);
    playRow->addWidget(m_fpsBox);
    playRow->addStretch();
    right->addLayout(playRow);

    m_onion = new QComboBox(this);
    m_onion->setObjectName(QStringLiteral("imageOnion"));
    m_onion->addItem(tr("Onion: off"), 0);
    m_onion->addItem(tr("Onion: previous"), -1);
    m_onion->addItem(tr("Onion: next"), 1);
    m_onion->setToolTip(tr("Ghost a neighbouring frame over the canvas"));
    connect(m_onion, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ImageEditor::onionChanged);
    right->addWidget(m_onion);

    m_previewPhaseBox = new QComboBox(this);
    m_previewPhaseBox->setObjectName(QStringLiteral("imagePreviewPhase"));
    connect(m_previewPhaseBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ImageEditor::previewPhaseChanged);
    right->addWidget(m_previewPhaseBox);

    right->addWidget(new QLabel(tr("Layers"), this));
    m_layers = new QListWidget(this);
    m_layers->setObjectName(QStringLiteral("imageLayers"));
    m_layers->setMaximumWidth(160);
    m_layers->setMaximumHeight(140);
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
    m_layerUp = new QToolButton(this);
    m_layerUp->setText(QStringLiteral("▲"));
    m_layerUp->setToolTip(tr("Move layer up"));
    m_layerUp->setAutoRaise(true);
    connect(m_layerUp, &QToolButton::clicked, this, &ImageEditor::moveLayerUp);
    m_layerDown = new QToolButton(this);
    m_layerDown->setText(QStringLiteral("▼"));
    m_layerDown->setToolTip(tr("Move layer down"));
    m_layerDown->setAutoRaise(true);
    connect(m_layerDown, &QToolButton::clicked, this, &ImageEditor::moveLayerDown);
    auto *layerBtns = new QHBoxLayout;
    layerBtns->addWidget(m_addLayer);
    layerBtns->addWidget(m_removeLayer);
    layerBtns->addWidget(m_layerUp);
    layerBtns->addWidget(m_layerDown);
    right->addLayout(layerBtns);

    right->addWidget(new QLabel(tr("Phases"), this));
    m_phases = new QListWidget(this);
    m_phases->setObjectName(QStringLiteral("imagePhases"));
    m_phases->setMaximumWidth(160);
    m_phases->setMaximumHeight(100);
    connect(m_phases, &QListWidget::currentRowChanged, this, &ImageEditor::selectPhase);
    connect(m_phases, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        renamePhase();
    });
    right->addWidget(m_phases);
    m_addPhase = makeIconButton(appearance::Icon::AddFrame, tr("Add phase"));
    m_addPhase->setObjectName(QStringLiteral("imageAddPhase"));
    connect(m_addPhase, &QToolButton::clicked, this, &ImageEditor::addPhase);
    m_removePhase = makeIconButton(appearance::Icon::RemoveFrame, tr("Delete phase"));
    connect(m_removePhase, &QToolButton::clicked, this, &ImageEditor::removePhase);
    auto *phaseBtns = new QHBoxLayout;
    phaseBtns->addWidget(m_addPhase);
    phaseBtns->addWidget(m_removePhase);
    right->addLayout(phaseBtns);

    // The current phase's strip placement on a sprite sheet.
    auto *placementRow = new QHBoxLayout;
    m_phaseSheet = new QComboBox(this);
    m_phaseSheet->setObjectName(QStringLiteral("imagePhaseSheet"));
    connect(m_phaseSheet, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &ImageEditor::onPhasePlacementChanged);
    m_phaseX = new QSpinBox(this);
    m_phaseX->setObjectName(QStringLiteral("imagePhaseX"));
    m_phaseX->setPrefix(tr("x "));
    m_phaseX->setRange(-4096, 4095);
    m_phaseY = new QSpinBox(this);
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
    right->addLayout(placementRow);
    auto *cellRow = new QHBoxLayout;
    m_phaseCellW = new QSpinBox(this);
    m_phaseCellW->setObjectName(QStringLiteral("imagePhaseCellW"));
    m_phaseCellW->setPrefix(tr("w "));
    m_phaseCellW->setRange(1, kStScreenWidth);
    m_phaseCellH = new QSpinBox(this);
    m_phaseCellH->setObjectName(QStringLiteral("imagePhaseCellH"));
    m_phaseCellH->setPrefix(tr("h "));
    m_phaseCellH->setRange(1, kStScreenHeight);
    connect(m_phaseCellW, qOverload<int>(&QSpinBox::valueChanged), this,
            &ImageEditor::onPhaseCellSizeChanged);
    connect(m_phaseCellH, qOverload<int>(&QSpinBox::valueChanged), this,
            &ImageEditor::onPhaseCellSizeChanged);
    cellRow->addWidget(m_phaseCellW);
    cellRow->addWidget(m_phaseCellH);
    right->addLayout(cellRow);
    m_previewTimer = new QTimer(this);
    connect(m_previewTimer, &QTimer::timeout, this, &ImageEditor::previewTick);

    body->addLayout(right);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    rebuildSwatches();
    refreshChrome();
    QTimer::singleShot(0, this, &ImageEditor::fitToView);
}

void ImageEditor::newDocument(int width, int height, PaletteKind kind)
{
    m_doc = ImageDocument::create(width, height, kind);
    m_filePath.clear();
    m_undo->clear();
    m_importedSheets.clear();
    m_sheetUnderlays.clear();
    if (m_actPlay)
        m_actPlay->setChecked(false);
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();
    m_canvas->setDocument(&m_doc);
    clearSelection();
    clearSelection();
    clearSelection();
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    refreshChrome();
    notifyModified();
    fitToView();
}

void ImageEditor::replaceDocument(const ImageDocument &doc)
{
    m_doc = doc;
    m_filePath.clear();
    m_undo->clear();
    if (m_actPlay)
        m_actPlay->setChecked(false);
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();
    m_canvas->setDocument(&m_doc);
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    refreshChrome();
    notifyModified();
    fitToView();
}

bool ImageEditor::loadFile(const QString &path)
{
    QString error;
    if (!m_doc.load(path, &error)) {
        m_lastError = error;
        return false;
    }
    m_filePath = path;
    m_undo->clear();
    if (m_actPlay)
        m_actPlay->setChecked(false);
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();
    m_canvas->setDocument(&m_doc);
    m_canvas->setCurrentColour(m_colour);
    loadSheetPixels(m_doc, m_importedSheets, m_sheetUnderlays);
    rebuildSwatches();
    refreshChrome();
    notifyModified();
    fitToView();
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
    Q_UNUSED(append);
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
    m_importedSheets.insert(index, sheet);
    m_sheetUnderlays.insert(index, sheetUnderlayImage(sheet, m_doc.paletteKind()));

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
    QString error;
    const ImageDocument *sheet = &m_doc;
    ImageDocument safe;
    if (spriteSafe && format != StImageFormat::Pim) {
        safe = spriteSafeDocument(m_doc, &error);
        if (!error.isEmpty()) {
            m_lastError = error;
            return false;
        }
        sheet = &safe;
    }
    QByteArray bytes;
    switch (format) {
    case StImageFormat::Pi1:
        bytes = exportPi1(*sheet, sheet->currentFrame(), &error);
        break;
    case StImageFormat::Neo:
        bytes = exportNeo(*sheet, sheet->currentFrame(), QFileInfo(path).completeBaseName(), &error);
        break;
    case StImageFormat::Iff:
        bytes = exportIff(*sheet, sheet->currentFrame(), &error);
        break;
    case StImageFormat::Png:
        bytes = exportPng(*sheet, sheet->currentFrame(), &error);
        break;
    case StImageFormat::Mbk:
        bytes = exportStosMbk(*sheet, 0, 1, &error);
        break;
    case StImageFormat::Assembler:
        bytes = exportAssembler(*sheet, sheet->currentFrame(), &error);
        break;
    case StImageFormat::BitplaneBin:
        bytes = exportBitplanes(*sheet, sheet->currentFrame(), &error);
        break;
    case StImageFormat::Pim:
        return saveFile(path);
    default:
        m_lastError = tr("Unknown export format for %1").arg(path);
        return false;
    }
    if (bytes.isEmpty()) {
        m_lastError = error.isEmpty() ? tr("Export failed") : error;
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_lastError = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        m_lastError = file.errorString();
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
    if (m_actPlay)
        m_actPlay->setIcon(appearance::icon(m_playing ? Icon::Pause : Icon::Run));
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
    refreshChrome();
    notifyModified();
}

void ImageEditor::redo()
{
    m_undo->redo();
    refreshChrome();
    notifyModified();
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
    m_doc.setActive(dialog.active());
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
    const ImagePhase &phase = m_doc.phases().at(m_doc.currentPhase());
    if (phase.sheet >= 0 && m_importedSheets.contains(phase.sheet)) {
        const QVector<QVector<int>> cells =
            sliceSheetCells(m_importedSheets.value(phase.sheet), phase.x, phase.y,
                            phase.cellW, phase.cellH, phase.frames.size());
        const int index = m_doc.currentFrame();
        if (index < cells.size())
            m_doc.replaceActiveLayer(cells.at(index));
    }
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
    if (!m_playing)
        m_previewFrame = row;
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
        button->setToolTip(label);
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
    for (int i = 0; i < m_doc.frameCount(); ++i)
        m_frames->addItem(tr("Frame %1").arg(i + 1));
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
    if (!m_phases || !m_previewPhaseBox)
        return;
    m_phases->blockSignals(true);
    const int selected = m_phases->currentRow();
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

    m_previewPhaseBox->blockSignals(true);
    m_previewPhaseBox->clear();
    for (int i = 0; i < m_doc.phases().size(); ++i)
        m_previewPhaseBox->addItem(m_doc.phases().at(i).name, i);
    const int phaseIndex = m_previewPhaseBox->findData(m_previewPhase);
    m_previewPhaseBox->setCurrentIndex(phaseIndex >= 0 ? phaseIndex : 0);
    if (phaseIndex < 0 && m_previewPhaseBox->count() > 0)
        m_previewPhase = m_previewPhaseBox->itemData(0).toInt();
    m_previewPhaseBox->blockSignals(false);
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
    m_sheetCanvas->setUnderlay(m_sheetUnderlays.value(sheet));
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
    if (!m_doc.setPhasePlacement(phase, sheet, m_phaseX->value(), m_phaseY->value()))
        return;
    // Attaching or moving a phase slices the art under frames that are
    // still untouched; drawn frames keep their pixels.
    slicePlacedPhaseFrames(phase);
    reSliceUntouchedFrames(phase, oldSheet, oldX, oldY);
    // Placement edits are metadata tweaks, like phase renames: no snapshot.
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
    if (phaseIndex < 0 || phaseIndex >= m_doc.phases().size())
        return;
    const ImagePhase &phase = m_doc.phases().at(phaseIndex);
    if (phase.sheet < 0 || !m_importedSheets.contains(phase.sheet))
        return;
    const QVector<QVector<int>> cells =
        sliceSheetCells(m_importedSheets.value(phase.sheet), phase.x, phase.y,
                        phase.cellW, phase.cellH, phase.frames.size());
    const int savedPhase = m_doc.currentPhase();
    const int savedFrame = m_doc.currentFrame();
    bool changed = false;
    for (int k = 0; k < cells.size() && k < phase.frames.size(); ++k) {
        // Only frames that are still empty get filled: drawn frames survive
        // a move or a re-place.
        bool empty = true;
        for (int value : phase.frames.at(k).composite)
            empty &= value < 0;
        if (!empty || cells.at(k).isEmpty())
            continue;
        m_doc.setCurrentPhase(phaseIndex);
        m_doc.setCurrentFrame(k);
        m_doc.replaceActiveLayer(cells.at(k));
        changed = true;
    }
    m_doc.setCurrentPhase(savedPhase);
    m_doc.setCurrentFrame(savedFrame);
    if (changed) {
        refreshChrome();
        refreshPreview();
        notifyModified();
    }
}

void ImageEditor::reSliceUntouchedFrames(int phaseIndex, int oldSheet, int oldX, int oldY)
{
    const ImagePhase &phase = m_doc.phases().at(phaseIndex);
    if (phase.sheet < 0 || !m_importedSheets.contains(phase.sheet))
        return;
    const ImportedSheet &imported = m_importedSheets.value(phase.sheet);
    const int count = phase.frames.size();
    const QVector<QVector<int>> newCells =
        sliceSheetCells(imported, phase.x, phase.y, phase.cellW, phase.cellH, count);
    // Frames are "untouched" when they still match the cells they were cut
    // from — or when they are empty and there is no older cell to compare.
    QVector<QVector<int>> oldCells;
    if (oldSheet >= 0 && m_importedSheets.contains(oldSheet))
        oldCells = sliceSheetCells(m_importedSheets.value(oldSheet), oldX, oldY,
                                   phase.cellW, phase.cellH, count);
    const int savedPhase = m_doc.currentPhase();
    const int savedFrame = m_doc.currentFrame();
    bool changed = false;
    for (int k = 0; k < count && k < newCells.size(); ++k) {
        const QVector<int> &frame = phase.frames.at(k).composite;
        const bool untouched = oldCells.isEmpty() || frame == oldCells.at(k)
            || std::all_of(frame.cbegin(), frame.cend(), [](int value) { return value < 0; });
        if (!untouched || frame == newCells.at(k))
            continue;
        m_doc.setCurrentPhase(phaseIndex);
        m_doc.setCurrentFrame(k);
        m_doc.replaceActiveLayer(newCells.at(k));
        changed = true;
    }
    m_doc.setCurrentPhase(savedPhase);
    m_doc.setCurrentFrame(savedFrame);
    if (changed) {
        refreshChrome();
        refreshPreview();
        notifyModified();
    }
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
    if (!m_importedSheets.contains(sheetIndex))
        return -1;
    const QVector<QVector<int>> cells =
        sliceSheetCells(m_importedSheets.value(sheetIndex), x, y, cellW, cellH, count);
    if (cells.isEmpty())
        return -1;
    const ImageDocument before = m_doc;
    const int index = m_doc.addPhase(name, cellW, cellH);
    m_doc.setPhasePlacement(index, sheetIndex, x, y);
    m_doc.replaceActiveLayer(cells.first());
    for (int k = 1; k < cells.size(); ++k) {
        m_doc.addFrame();
        m_doc.replaceActiveLayer(cells.at(k));
    }
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
    if (spriteSafe) {
        ImageDocument safe = spriteSafeDocument(composed, &error);
        if (!error.isEmpty()) {
            m_lastError = error;
            return false;
        }
        composed = safe;
    }

    const StImageFormat format = stFormatFromPath(path);
    QByteArray bytes;
    switch (format) {
    case StImageFormat::Pi1:
        bytes = exportPi1(composed, 0, &error);
        break;
    case StImageFormat::Neo:
        bytes = exportNeo(composed, 0, QFileInfo(path).completeBaseName(), &error);
        break;
    case StImageFormat::Iff:
        bytes = exportIff(composed, 0, &error);
        break;
    case StImageFormat::Png:
        bytes = exportPng(composed, 0, &error);
        break;
    case StImageFormat::Mbk:
        bytes = exportStosMbk(composed, 0, 1, &error);
        break;
    case StImageFormat::Assembler:
        bytes = exportAssembler(composed, 0, &error);
        break;
    case StImageFormat::BitplaneBin:
        bytes = exportBitplanes(composed, 0, &error);
        break;
    default:
        m_lastError = tr("Unknown export format for %1").arg(path);
        return false;
    }
    if (!error.isEmpty()) {
        m_lastError = error;
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
}

bool ImageEditor::exportBitplaneFile(const QString &path, int phase,
                                     const BitplaneDataOptions &options)
{
    QString error;
    const QByteArray bytes = exportBitplaneData(m_doc, phase, options, &error);
    if (bytes.isEmpty()) {
        m_lastError = error.isEmpty() ? tr("Export failed") : error;
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
}

bool ImageEditor::exportScrollDemoFile(const QString &path, int phase,
                                       const BitplaneDataOptions &options,
                                       const QString &dataFile)
{
    QString error;
    const QByteArray bytes = exportScrollDemo(m_doc, phase, options, dataFile, &error);
    if (bytes.isEmpty()) {
        m_lastError = error.isEmpty() ? tr("Export failed") : error;
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || file.write(bytes) != bytes.size()) {
        m_lastError = file.errorString();
        return false;
    }
    return true;
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
    if (!m_preview)
        return;
    if (!m_playing)
        m_previewFrame = m_doc.currentFrame();
    m_previewFrame = qBound(0, m_previewFrame, m_doc.frameCount() - 1);
    QImage image(m_doc.width(), m_doc.height(), QImage::Format_ARGB32);
    const QVector<int> &pixels = m_doc.frame(m_previewFrame);
    for (int y = 0; y < m_doc.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < m_doc.width(); ++x) {
            const int cube = pixels.value(y * m_doc.width() + x, kTransparent);
            if (cube < 0) {
                const bool checker = ((x + y) & 1) == 0;
                line[x] = checker ? qRgb(40, 40, 40) : qRgb(70, 70, 70);
            } else {
                const Rgb rgb = cubeRgb(m_doc.paletteKind(), cube);
                line[x] = qRgb(rgb.r, rgb.g, rgb.b);
            }
        }
    }
    const QSize box = m_preview->size().expandedTo(QSize(96, 96));
    m_preview->setPixmap(QPixmap::fromImage(
        image.scaled(box, Qt::KeepAspectRatio, Qt::FastTransformation)));
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
    m_status->setText(tr("%1 × %2  frame %3/%4  %5%")
                          .arg(m_doc.width())
                          .arg(m_doc.height())
                          .arg(m_doc.currentFrame() + 1)
                          .arg(m_doc.frameCount())
                          .arg(zoom));
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
    }
    const QVector<int> &layer = m_doc.activeLayerPixels();
    for (int index : indices) {
        if (m_strokeIndices.contains(index))
            continue;
        m_strokeIndices.append(index);
        m_strokeBefore.append(layer.value(index, kTransparent));
    }
    m_doc.fillIndices(indices, colour, m_strokeLayer);
    refreshCanvas();
    refreshPreview();
    updateOverspill();
    notifyModified();
}

void ImageEditor::finishStroke()
{
    if (m_strokeIndices.isEmpty())
        return;
    m_undo->push(new PaintCommand(&m_doc, m_strokePhase, m_strokeFrame, m_strokeLayer,
                                  m_strokeIndices, m_strokeBefore, m_strokeColour));
    m_strokeIndices.clear();
    m_strokeBefore.clear();
}

void ImageEditor::pickColour(int cubeIndex)
{
    m_colour = cubeIndex;
    if (cubeIndex >= 0 && !m_doc.active().contains(cubeIndex))
        m_doc.toggleActive(cubeIndex);
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
    if (!m_doc.replaceActiveLayer(rotateIndexed(before, m_doc.width(), 90)))
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

void ImageEditor::togglePlay(bool on)
{
    m_playing = on;
    if (m_playing) {
        m_previewFrame = 0;
        m_previewTimer->start(qMax(1, 1000 / qMax(1, m_fps)));
        m_actPlay->setIcon(appearance::icon(appearance::Icon::Pause));
        m_actPlay->setToolTip(tr("Pause animation"));
        refreshPreview();
    } else {
        m_previewTimer->stop();
        m_previewFrame = m_doc.currentFrame();
        m_actPlay->setIcon(appearance::icon(appearance::Icon::Run));
        m_actPlay->setToolTip(tr("Play animation"));
        refreshPreview();
    }
}

void ImageEditor::previewTick()
{
    const int count = m_previewPhase >= 0 && m_previewPhase < m_doc.phases().size()
        ? m_doc.phases().at(m_previewPhase).frames.size()
        : m_doc.frameCount();
    m_previewFrame = nextPreviewFrame(m_previewFrame, count, 0, count - 1);
    refreshPreview();
}

void ImageEditor::fpsChanged(int fps)
{
    m_fps = qBound(1, fps, 60);
    if (m_playing)
        m_previewTimer->start(qMax(1, 1000 / m_fps));
}

void ImageEditor::previewPhaseChanged(int index)
{
    if (!m_previewPhaseBox || index < 0)
        return;
    m_previewPhase = m_previewPhaseBox->itemData(index).toInt();
    if (m_playing)
        m_previewFrame = 0;
    refreshPreview();
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
    if (m_actSheetMode->isChecked() && sheet >= 0 && m_importedSheets.contains(sheet)) {
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
    if (m_previewPhase == row)
        m_previewPhase = 0;
    else if (m_previewPhase > row)
        --m_previewPhase;
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
    // The animation preview plays the selected phase, not whatever was
    // previewed before.
    if (m_previewPhase != row) {
        m_previewPhase = row;
        m_previewPhaseBox->blockSignals(true);
        const int index = m_previewPhaseBox->findData(row);
        m_previewPhaseBox->setCurrentIndex(index >= 0 ? index : 0);
        m_previewPhaseBox->blockSignals(false);
        refreshPreview();
    }
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
