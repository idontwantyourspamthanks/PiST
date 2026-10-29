// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/AssetExportDialog.h"

#include "image/ImageDocument.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pist {

namespace {

bool singleFileName(const QString &name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    return QFileInfo(name).fileName() == name;
}

class AssetExportDialog : public QDialog
{
public:
    AssetExportDialog(const ImageDocument &doc, const QString &source, const AssetExport &asset,
                      QWidget *parent)
        : QDialog(parent)
        , m_source(source)
    {
        setObjectName(QStringLiteral("assetExportDialog"));
        setWindowTitle(tr("Sprite export"));

        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        layout->addLayout(form);

        auto *pathRow = new QHBoxLayout;
        m_path = new QLineEdit(source, this);
        m_path->setReadOnly(true);
        auto *browse = new QPushButton(tr("Browse…"), this);
        pathRow->addWidget(m_path);
        pathRow->addWidget(browse);
        form->addRow(tr("Document:"), pathRow);
        connect(browse, &QPushButton::clicked, this, [this] { browseSource(); });

        m_phase = new QComboBox(this);
        m_phase->setObjectName(QStringLiteral("assetPhase"));
        for (const ImagePhase &phase : doc.phases())
            m_phase->addItem(phase.name);
        const int existing = m_phase->findText(asset.phase);
        if (existing >= 0)
            m_phase->setCurrentIndex(existing);
        form->addRow(tr("Phase:"), m_phase);

        m_output = new QLineEdit(this);
        m_output->setObjectName(QStringLiteral("assetOutput"));
        const QString output = asset.output.isEmpty()
                                   ? QFileInfo(source).completeBaseName() + QStringLiteral(".dat")
                                   : asset.output;
        m_output->setText(output);
        form->addRow(tr("Output file:"), m_output);

        m_palette = new QCheckBox(tr("Palette"), this);
        m_sprite = new QCheckBox(tr("Sprite"), this);
        m_masked = new QCheckBox(tr("Mask"), this);
        m_shifted = new QCheckBox(tr("Pre-shifted"), this);
        m_shiftedMasked = new QCheckBox(tr("Pre-shifted mask"), this);
        m_palette->setChecked(asset.palette);
        m_sprite->setChecked(asset.sprite);
        m_masked->setChecked(asset.masked);
        m_shifted->setChecked(asset.shifted);
        m_shiftedMasked->setChecked(asset.shiftedMasked);
        form->addRow(tr("Blocks:"), m_palette);
        form->addRow(QString(), m_sprite);
        form->addRow(QString(), m_masked);
        form->addRow(QString(), m_shifted);
        form->addRow(QString(), m_shiftedMasked);

        m_preShifts = new QComboBox(this);
        m_preShifts->addItems({QStringLiteral("2"), QStringLiteral("4"), QStringLiteral("8")});
        const int shiftIndex = m_preShifts->findText(QString::number(asset.preShifts));
        m_preShifts->setCurrentIndex(shiftIndex >= 0 ? shiftIndex : 1);
        form->addRow(tr("Pre-shifts:"), m_preShifts);

        m_transparent = new QSpinBox(this);
        m_transparent->setRange(-1, 15);
        m_transparent->setValue(asset.transparent);
        m_transparent->setToolTip(tr("-1 keeps every painted colour. 0 is the usual background."));
        form->addRow(tr("Transparent:"), m_transparent);

        m_error = new QLabel(this);
        m_error->setWordWrap(true);
        layout->addWidget(m_error);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        connect(buttons, &QDialogButtonBox::accepted, this, &AssetExportDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
    }

    AssetExport asset() const
    {
        AssetExport asset;
        asset.source = m_source;
        asset.phase = m_phase->currentText();
        asset.output = m_output->text().trimmed();
        asset.palette = m_palette->isChecked();
        asset.sprite = m_sprite->isChecked();
        asset.masked = m_masked->isChecked();
        asset.shifted = m_shifted->isChecked();
        asset.shiftedMasked = m_shiftedMasked->isChecked();
        asset.preShifts = m_preShifts->currentText().toInt();
        asset.transparent = m_transparent->value();
        return asset;
    }

    void accept() override
    {
        if (m_phase->currentText().isEmpty()) {
            m_error->setText(tr("Choose a phase."));
            return;
        }
        if (!singleFileName(m_output->text().trimmed())) {
            m_error->setText(tr("The output name must be a single file name, such as hero.dat."));
            return;
        }
        QDialog::accept();
    }

private:
    void browseSource();

    QString m_source;
    QLineEdit *m_path = nullptr;
    QComboBox *m_phase = nullptr;
    QLineEdit *m_output = nullptr;
    QCheckBox *m_palette = nullptr;
    QCheckBox *m_sprite = nullptr;
    QCheckBox *m_masked = nullptr;
    QCheckBox *m_shifted = nullptr;
    QCheckBox *m_shiftedMasked = nullptr;
    QComboBox *m_preShifts = nullptr;
    QSpinBox *m_transparent = nullptr;
    QLabel *m_error = nullptr;
};

void AssetExportDialog::browseSource()
{
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Sprite document"), QFileInfo(m_source).absolutePath(),
        tr("PiST sprites (*.pim)"));
    if (path.isEmpty())
        return;
    ImageDocument doc;
    QString error;
    if (!doc.load(path, &error)) {
        QMessageBox::warning(this, tr("Sprite document"), error);
        return;
    }
    m_source = path;
    m_path->setText(path);
    const QString selected = m_phase->currentText();
    m_phase->clear();
    for (const ImagePhase &phase : doc.phases())
        m_phase->addItem(phase.name);
    const int existing = m_phase->findText(selected);
    if (existing >= 0)
        m_phase->setCurrentIndex(existing);
    if (m_output->text().trimmed().isEmpty())
        m_output->setText(QFileInfo(path).completeBaseName() + QStringLiteral(".dat"));
}

} // namespace

bool editAssetExport(QWidget *parent, AssetExport *asset)
{
    if (!asset)
        return false;
    QString source = asset->source;
    if (source.isEmpty()) {
        source = QFileDialog::getOpenFileName(parent, QObject::tr("Sprite document"), QString(),
                                              QObject::tr("PiST sprites (*.pim)"));
        if (source.isEmpty())
            return false;
    }
    ImageDocument doc;
    QString error;
    if (!doc.load(source, &error)) {
        QMessageBox::warning(parent, QObject::tr("Sprite document"),
                             error.isEmpty() ? QObject::tr("Could not read %1.").arg(source) : error);
        return false;
    }
    AssetExportDialog dialog(doc, source, *asset, parent);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    *asset = dialog.asset();
    return true;
}

} // namespace pist
