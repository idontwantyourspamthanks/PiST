// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/NewImageDialog.h"

#include "image/ImageDocument.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSize>
#include <QSpinBox>

namespace pist {

NewImageDialog::NewImageDialog(QWidget *parent, const QString &directory)
    : QDialog(parent)
    , m_directory(directory)
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

    m_file = new QLineEdit(this);
    m_file->setObjectName(QStringLiteral("newImageFileName"));
    m_file->setPlaceholderText(tr("Name (*.pim)"));
    auto *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &NewImageDialog::browse);
    auto *fileRow = new QHBoxLayout;
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    form->addRow(tr("File"), fileRow);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        const QString path = filePath();
        if (path.isEmpty())
            return;
        if (QFileInfo::exists(path)) {
            const auto answer = QMessageBox::warning(
                this, tr("New Image"),
                tr("%1 already exists.\n\nReplace it?").arg(QFileInfo(path).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes)
                return;
        }
        accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(m_buttons);

    connect(m_file, &QLineEdit::textChanged, this, &NewImageDialog::updateOkButton);
    updateOkButton();
}

void NewImageDialog::browse()
{
    QString start = filePath();
    if (start.isEmpty()) {
        start = m_directory;
        if (start.isEmpty())
            start = QDir::homePath();
    }
    const QString path = QFileDialog::getSaveFileName(
        this, tr("New image file"), start, tr("PiST images (*.pim)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (!path.isEmpty())
        m_file->setText(path);
}

void NewImageDialog::updateOkButton()
{
    if (QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok))
        ok->setEnabled(!filePath().isEmpty());
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

QString NewImageDialog::filePath() const
{
    const QString text = m_file->text().trimmed();
    if (text.isEmpty())
        return {};
    QFileInfo info(text);
    if (info.isRelative() && !m_directory.isEmpty())
        info = QFileInfo(QDir(m_directory), text);
    QString path = info.absoluteFilePath();
    if (!path.endsWith(QLatin1String(".pim"), Qt::CaseInsensitive))
        path += QStringLiteral(".pim");
    return path;
}

} // namespace pist
