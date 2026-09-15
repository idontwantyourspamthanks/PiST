// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/NewImageDialog.h"

#include "image/ImageDocument.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSize>
#include <QSpinBox>

namespace pist {

NewImageDialog::NewImageDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Image"));
    auto *form = new QFormLayout(this);

    auto *preset = new QComboBox(this);
    preset->addItem(tr("16 × 16"), QSize(16, 16));
    preset->addItem(tr("32 × 32"), QSize(32, 32));
    preset->addItem(tr("64 × 64"), QSize(64, 64));
    preset->addItem(tr("320 × 200 (ST low-res)"), QSize(320, 200));
    preset->setCurrentIndex(1);
    form->addRow(tr("Preset"), preset);

    m_width = new QSpinBox(this);
    m_width->setRange(ImageDocument::kMinSize, ImageDocument::kMaxWidth);
    m_width->setValue(32);
    form->addRow(tr("Width"), m_width);

    m_height = new QSpinBox(this);
    m_height->setRange(ImageDocument::kMinSize, ImageDocument::kMaxHeight);
    m_height->setValue(32);
    form->addRow(tr("Height"), m_height);

    connect(preset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, preset](int) {
        const QSize size = preset->currentData().toSize();
        m_width->setValue(size.width());
        m_height->setValue(size.height());
    });

    m_palette = new QComboBox(this);
    m_palette->addItem(tr("Atari STe (4096 colours)"), int(PaletteKind::Ste));
    m_palette->addItem(tr("Atari STfm (512 colours)"), int(PaletteKind::Stfm));
    form->addRow(tr("Palette"), m_palette);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(buttons);
}

int NewImageDialog::imageWidth() const
{
    return m_width->value();
}

int NewImageDialog::imageHeight() const
{
    return m_height->value();
}

PaletteKind NewImageDialog::paletteKind() const
{
    return PaletteKind(m_palette->currentData().toInt());
}

} // namespace pist
