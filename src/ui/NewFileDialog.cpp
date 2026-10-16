// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/NewFileDialog.h"

#include "ui/Appearance.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>

namespace pist {

NewFileDialog::NewFileDialog(QWidget *parent, const QString &directory)
    : QDialog(parent)
{
    setWindowTitle(tr("New File"));
    auto *form = new QFormLayout(this);

    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("newFileName"));
    m_name->setPlaceholderText(tr("Name (%1)").arg(QLatin1String(kSourceSuffix)));
    form->addRow(tr("Name"), m_name);

    m_directory = new QLineEdit(this);
    m_directory->setObjectName(QStringLiteral("newFileDirectory"));
    m_directory->setText(directory);
    auto *browse = new QPushButton(tr("Browse…"), this);
    connect(browse, &QPushButton::clicked, this, &NewFileDialog::browse);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->addWidget(m_directory, 1);
    directoryRow->addWidget(browse);
    form->addRow(tr("Directory"), directoryRow);

    m_template = new QComboBox(this);
    m_template->setObjectName(QStringLiteral("newFileTemplate"));
    for (SourceTemplate kind : allSourceTemplates()) {
        m_template->addItem(templateLabel(kind), int(kind));
        // The name says what the file is; the tooltip says what it does, which
        // is the part a first-time user cannot guess from "GEMDOS hello".
        m_template->setItemData(m_template->count() - 1, templateDescription(kind),
                                Qt::ToolTipRole);
    }
    form->addRow(tr("Template"), m_template);

    // Both of these are always in the layout, so the dialog does not resize as
    // the verdict changes under the typing.
    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("newFileMessage"));
    m_message->setWordWrap(true);
    form->addRow(m_message);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("newFileNote"));
    m_note->setWordWrap(true);
    form->addRow(m_note);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        if (spec().verify().ok)
            accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    form->addRow(m_buttons);

    connect(m_name, &QLineEdit::textChanged, this, &NewFileDialog::updateState);
    connect(m_directory, &QLineEdit::textChanged, this, &NewFileDialog::updateState);
    connect(m_template, &QComboBox::currentIndexChanged, this, &NewFileDialog::updateState);
    updateState();

    m_name->setFocus();
}

void NewFileDialog::browse()
{
    const QString start = m_directory->text().trimmed();
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("New File"), start.isEmpty() ? QDir::homePath() : start);
    if (!directory.isEmpty())
        m_directory->setText(directory);
}

void NewFileDialog::updateState()
{
    const NewFileSpec::Verdict verdict = spec().verify();

    if (QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok))
        ok->setEnabled(verdict.ok);

    const appearance::Colors colors = appearance::colors();
    auto ink = [](QLabel *label, const QColor &colour) {
        QPalette palette = label->palette();
        palette.setColor(QPalette::WindowText, colour);
        label->setPalette(palette);
    };
    m_message->setText(verdict.message);
    ink(m_message, colors.error);
    m_note->setText(verdict.note);
    ink(m_note, colors.muted);
}

NewFileSpec NewFileDialog::spec() const
{
    return NewFileSpec(m_directory->text(), m_name->text(),
                       SourceTemplate(m_template->currentData().toInt()));
}

QString NewFileDialog::filePath() const
{
    return spec().targetPath();
}

} // namespace pist
