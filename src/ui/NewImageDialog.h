// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"

#include <QDialog>

class QComboBox;
class QSpinBox;

namespace pist {

class NewImageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewImageDialog(QWidget *parent = nullptr);

    int imageWidth() const;
    int imageHeight() const;
    PaletteKind paletteKind() const;

private:
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QComboBox *m_palette = nullptr;
};

} // namespace pist
