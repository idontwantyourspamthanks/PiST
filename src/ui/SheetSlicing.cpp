// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/SheetSlicing.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>

#include <algorithm>

namespace pist {

SlicePhaseDialog::SlicePhaseDialog(QWidget *parent, const QString &suggestedName, int defaultW,
                                   int defaultH, int sheetWidth)
    : QDialog(parent)
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

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

SlicePhaseDialog::Values SlicePhaseDialog::values() const
{
    return {m_name->text().trimmed(), m_x->value(), m_y->value(), m_w->value(), m_h->value(),
            m_count->value()};
}

QSpinBox *SlicePhaseDialog::makeSpin(const QString &objectName, int value, int min, int max)
{
    auto *box = new QSpinBox(this);
    box->setObjectName(objectName);
    box->setRange(min, max);
    box->setValue(value);
    return box;
}

namespace {

QImage sheetUnderlayImage(const ImportedSheet &sheet, PaletteKind kind)
{
    // Flat: the underlay is a reference, dimmed by the sheet canvas, so the
    // composed phases over it stand out — and a checkerboard under a dimmed
    // import would read as artwork.
    return indicesToImage(sheet.pixels, sheet.width, sheet.height, kind, EmptyStyle::Flat);
}

/// Upper bound on a sheet file read back from a JSON-supplied path. ST images
/// are tiny; this exists only to bound an untrusted path that points at a large
/// regular file (the read is otherwise unbounded).
constexpr qint64 kMaxSheetBytes = 64LL * 1024 * 1024;

} // namespace

void SheetSlicing::load(const ImageDocument &doc)
{
    clear();
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
            m_pixels.insert(i, imported);
            m_underlays.insert(i, sheetUnderlayImage(imported, doc.paletteKind()));
        }
    }
}

void SheetSlicing::clear()
{
    m_pixels.clear();
    m_underlays.clear();
}

void SheetSlicing::insert(int sheetIndex, const ImportedSheet &sheet, PaletteKind kind)
{
    m_pixels.insert(sheetIndex, sheet);
    m_underlays.insert(sheetIndex, sheetUnderlayImage(sheet, kind));
}

bool SheetSlicing::slicePlacedPhaseFrames(ImageDocument &doc, int phaseIndex)
{
    if (phaseIndex < 0 || phaseIndex >= doc.phases().size())
        return false;
    const ImagePhase &phase = doc.phases().at(phaseIndex);
    if (phase.sheet < 0 || !m_pixels.contains(phase.sheet))
        return false;
    const QVector<QVector<int>> cells =
        sliceSheetCells(m_pixels.value(phase.sheet), phase.x, phase.y, phase.cellW, phase.cellH,
                        phase.frames.size());
    const int savedPhase = doc.currentPhase();
    const int savedFrame = doc.currentFrame();
    bool changed = false;
    for (int k = 0; k < cells.size() && k < phase.frames.size(); ++k) {
        // Only frames that are still empty get filled: drawn frames survive
        // a move or a re-place.
        bool empty = true;
        for (int value : phase.frames.at(k).composite)
            empty &= value < 0;
        if (!empty || cells.at(k).isEmpty())
            continue;
        doc.setCurrentPhase(phaseIndex);
        doc.setCurrentFrame(k);
        doc.replaceActiveLayer(cells.at(k));
        changed = true;
    }
    doc.setCurrentPhase(savedPhase);
    doc.setCurrentFrame(savedFrame);
    return changed;
}

void SheetSlicing::sliceAddedFrame(ImageDocument &doc)
{
    const ImagePhase &phase = doc.phases().at(doc.currentPhase());
    if (phase.sheet < 0 || !m_pixels.contains(phase.sheet))
        return;
    const QVector<QVector<int>> cells =
        sliceSheetCells(m_pixels.value(phase.sheet), phase.x, phase.y, phase.cellW, phase.cellH,
                        phase.frames.size());
    const int index = doc.currentFrame();
    if (index < cells.size())
        doc.replaceActiveLayer(cells.at(index));
}

bool SheetSlicing::reSliceUntouchedFrames(ImageDocument &doc, int phaseIndex, int oldSheet,
                                          int oldX, int oldY)
{
    // The same guard its sibling slicePlacedPhaseFrames carries, for the same
    // reason: this is public on a reusable class, so a phase removed under it by
    // an undo is one stale index away, and QVector::at() on that index aborts a
    // debug build rather than returning (MIN-88).
    if (phaseIndex < 0 || phaseIndex >= doc.phases().size())
        return false;
    const ImagePhase &phase = doc.phases().at(phaseIndex);
    if (phase.sheet < 0 || !m_pixels.contains(phase.sheet))
        return false;
    const ImportedSheet &imported = m_pixels.value(phase.sheet);
    const int count = phase.frames.size();
    const QVector<QVector<int>> newCells =
        sliceSheetCells(imported, phase.x, phase.y, phase.cellW, phase.cellH, count);
    // Frames are "untouched" when they still match the cells they were cut
    // from — or when they are empty and there is no older cell to compare.
    QVector<QVector<int>> oldCells;
    if (oldSheet >= 0 && m_pixels.contains(oldSheet))
        oldCells = sliceSheetCells(m_pixels.value(oldSheet), oldX, oldY, phase.cellW, phase.cellH,
                                   count);
    const int savedPhase = doc.currentPhase();
    const int savedFrame = doc.currentFrame();
    bool changed = false;
    for (int k = 0; k < count && k < newCells.size(); ++k) {
        const QVector<int> &frame = phase.frames.at(k).composite;
        const bool untouched = oldCells.isEmpty() || frame == oldCells.at(k)
            || std::all_of(frame.cbegin(), frame.cend(), [](int value) { return value < 0; });
        if (!untouched || frame == newCells.at(k))
            continue;
        doc.setCurrentPhase(phaseIndex);
        doc.setCurrentFrame(k);
        doc.replaceActiveLayer(newCells.at(k));
        changed = true;
    }
    doc.setCurrentPhase(savedPhase);
    doc.setCurrentFrame(savedFrame);
    return changed;
}

int SheetSlicing::addPhaseFromSheet(ImageDocument &doc, int sheetIndex, const QString &name,
                                    int x, int y, int cellW, int cellH, int count)
{
    if (!m_pixels.contains(sheetIndex))
        return -1;
    const QVector<QVector<int>> cells =
        sliceSheetCells(m_pixels.value(sheetIndex), x, y, cellW, cellH, count);
    if (cells.isEmpty())
        return -1;
    const int index = doc.addPhase(name, cellW, cellH);
    doc.setPhasePlacement(index, sheetIndex, x, y);
    doc.replaceActiveLayer(cells.first());
    for (int k = 1; k < cells.size(); ++k) {
        doc.addFrame();
        doc.replaceActiveLayer(cells.at(k));
    }
    return index;
}

} // namespace pist
