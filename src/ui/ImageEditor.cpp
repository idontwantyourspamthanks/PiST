// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ImageEditor.h"

#include "image/StFormats.h"
#include "ui/Appearance.h"
#include "ui/ImageCanvas.h"
#include "ui/PalettePickerDialog.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QSize>
#include <QSlider>
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
    PaintCommand(ImageDocument *doc, const QVector<int> &indices, const QVector<int> &before,
                 int colour)
        : m_doc(doc)
        , m_indices(indices)
        , m_before(before)
        , m_colour(colour)
    {
        setText(QStringLiteral("paint"));
    }

    void undo() override { m_doc->restoreIndices(m_indices, m_before); }
    void redo() override
    {
        // First redo() is QUndoStack::push; the stroke already painted live.
        if (m_virgin) {
            m_virgin = false;
            return;
        }
        m_doc->fillIndices(m_indices, m_colour);
    }

private:
    ImageDocument *m_doc;
    QVector<int> m_indices;
    QVector<int> m_before;
    int m_colour;
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
    connect(m_tools, &QButtonGroup::buttonClicked, this, &ImageEditor::setTool);

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
    body->addWidget(m_scroll, 1);

    connect(m_canvas, &ImageCanvas::indicesPainted, this, &ImageEditor::paintIndices);
    connect(m_canvas, &ImageCanvas::strokeEnded, this, &ImageEditor::finishStroke);
    connect(m_canvas, &ImageCanvas::colourPicked, this, &ImageEditor::pickColour);
    connect(m_canvas, &ImageCanvas::cellSizeChanged, this, &ImageEditor::updateStatus);
    connect(m_canvas, &ImageCanvas::zoomStepsRequested, this, &ImageEditor::zoomBy);
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
    right->addWidget(new QLabel(tr("Frames"), this));
    m_frames = new QListWidget(this);
    m_frames->setMaximumWidth(120);
    connect(m_frames, &QListWidget::currentRowChanged, this, &ImageEditor::selectFrame);
    right->addWidget(m_frames, 1);
    m_addFrame = new QToolButton(this);
    m_addFrame->setIcon(appearance::icon(appearance::Icon::AddFrame));
    m_addFrame->setToolTip(tr("Add frame"));
    m_addFrame->setAutoRaise(true);
    m_addFrame->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(m_addFrame, &QToolButton::clicked, this, &ImageEditor::addFrame);
    m_dupFrame = new QToolButton(this);
    m_dupFrame->setIcon(appearance::icon(appearance::Icon::DuplicateFrame));
    m_dupFrame->setToolTip(tr("Duplicate frame"));
    m_dupFrame->setAutoRaise(true);
    m_dupFrame->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(m_dupFrame, &QToolButton::clicked, this, &ImageEditor::duplicateFrame);
    m_removeFrame = new QToolButton(this);
    m_removeFrame->setIcon(appearance::icon(appearance::Icon::RemoveFrame));
    m_removeFrame->setToolTip(tr("Delete frame"));
    m_removeFrame->setAutoRaise(true);
    m_removeFrame->setToolButtonStyle(Qt::ToolButtonIconOnly);
    connect(m_removeFrame, &QToolButton::clicked, this, &ImageEditor::removeFrame);
    auto *frameBtns = new QHBoxLayout;
    frameBtns->addWidget(m_addFrame);
    frameBtns->addWidget(m_dupFrame);
    frameBtns->addWidget(m_removeFrame);
    right->addLayout(frameBtns);
    body->addLayout(right);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    rebuildSwatches();
    refreshFrames();
    refreshCanvas();
    QTimer::singleShot(0, this, &ImageEditor::fitToView);
}

void ImageEditor::newDocument(int width, int height, PaletteKind kind)
{
    m_doc = ImageDocument::create(width, height, kind);
    m_filePath.clear();
    m_undo->clear();
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();
    m_canvas->setDocument(&m_doc);
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    refreshFrames();
    refreshCanvas();
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
    m_colour = m_doc.active().isEmpty() ? kTransparent : m_doc.active().first();
    m_canvas->setDocument(&m_doc);
    m_canvas->setCurrentColour(m_colour);
    rebuildSwatches();
    refreshFrames();
    refreshCanvas();
    notifyModified();
    fitToView();
    return true;
}

bool ImageEditor::saveFile(const QString &path)
{
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
    if (!applyImport(&m_doc, sheet, append, &error)) {
        m_lastError = error;
        return false;
    }
    m_undo->clear();
    m_canvas->setDocument(&m_doc);
    rebuildSwatches();
    refreshFrames();
    refreshCanvas();
    notifyModified();
    fitToView();
    return true;
}

bool ImageEditor::exportFile(const QString &path)
{
    const StImageFormat format = stFormatFromPath(path);
    QString error;
    QByteArray bytes;
    switch (format) {
    case StImageFormat::Pi1:
        bytes = exportPi1(m_doc, m_doc.currentFrame(), &error);
        break;
    case StImageFormat::Neo:
        bytes = exportNeo(m_doc, m_doc.currentFrame(), QFileInfo(path).completeBaseName(), &error);
        break;
    case StImageFormat::Iff:
        bytes = exportIff(m_doc, m_doc.currentFrame(), &error);
        break;
    case StImageFormat::Png:
        bytes = exportPng(m_doc, m_doc.currentFrame(), &error);
        break;
    case StImageFormat::Mbk:
        bytes = exportStosMbk(m_doc, 0, 1, &error);
        break;
    case StImageFormat::Assembler:
        bytes = exportAssembler(m_doc, m_doc.currentFrame(), &error);
        break;
    case StImageFormat::BitplaneBin:
        bytes = exportBitplanes(m_doc, m_doc.currentFrame(), &error);
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
    if (m_addFrame)
        m_addFrame->setIcon(appearance::icon(Icon::AddFrame));
    if (m_dupFrame)
        m_dupFrame->setIcon(appearance::icon(Icon::DuplicateFrame));
    if (m_removeFrame)
        m_removeFrame->setIcon(appearance::icon(Icon::RemoveFrame));
    rebuildSwatches();
    if (m_canvas) {
        m_canvas->setTool(m_tool);
        m_canvas->setCurrentColour(m_colour);
    }
}

void ImageEditor::undo()
{
    m_undo->undo();
    refreshCanvas();
    notifyModified();
}

void ImageEditor::redo()
{
    m_undo->redo();
    refreshCanvas();
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

void ImageEditor::addFrame()
{
    m_doc.addFrame();
    refreshFrames();
    refreshCanvas();
    notifyModified();
}

void ImageEditor::removeFrame()
{
    if (!m_doc.removeFrame(m_doc.currentFrame()))
        return;
    refreshFrames();
    refreshCanvas();
    notifyModified();
}

void ImageEditor::duplicateFrame()
{
    m_doc.duplicateFrame(m_doc.currentFrame());
    refreshFrames();
    refreshCanvas();
    notifyModified();
}

void ImageEditor::selectFrame(int row)
{
    if (row < 0)
        return;
    m_doc.setCurrentFrame(row);
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
    m_frames->blockSignals(true);
    m_frames->clear();
    for (int i = 0; i < m_doc.frameCount(); ++i)
        m_frames->addItem(tr("Frame %1").arg(i + 1));
    m_frames->setCurrentRow(m_doc.currentFrame());
    m_frames->blockSignals(false);
}

void ImageEditor::refreshCanvas()
{
    m_canvas->setDocument(&m_doc);
    m_canvas->updateGeometry();
    m_canvas->adjustSize();
    m_canvas->update();
    updateStatus();
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
    if (m_strokeIndices.isEmpty())
        m_strokeColour = colour;
    for (int index : indices) {
        if (m_strokeIndices.contains(index))
            continue;
        m_strokeIndices.append(index);
        m_strokeBefore.append(m_doc.pixels().value(index, kTransparent));
    }
    m_doc.fillIndices(indices, colour);
    refreshCanvas();
    updateOverspill();
    notifyModified();
}

void ImageEditor::finishStroke()
{
    if (m_strokeIndices.isEmpty())
        return;
    m_undo->push(new PaintCommand(&m_doc, m_strokeIndices, m_strokeBefore, m_strokeColour));
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

} // namespace pist
