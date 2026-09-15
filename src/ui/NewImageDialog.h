// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/Palette.h"

#include <QDialog>
#include <QString>

class QComboBox;
class QDialogButtonBox;
class QLineEdit;
class QSpinBox;

namespace pist {

class NewImageDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewImageDialog(QWidget *parent = nullptr, const QString &directory = QString());

    int imageWidth() const;
    int imageHeight() const;
    PaletteKind paletteKind() const;
    /// Absolute `.pim` path from the file field, or empty when unset.
    QString filePath() const;

private:
    void browse();
    void updateOkButton();

    QString m_directory;
    QSpinBox *m_width = nullptr;
    QSpinBox *m_height = nullptr;
    QComboBox *m_palette = nullptr;
    QLineEdit *m_file = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

} // namespace pist
