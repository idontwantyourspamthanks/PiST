// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/NewFileDialog.h"

#include "ui/Appearance.h"
#include "ui/FileTemplates.h"
#include "ui/GalleryCard.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

namespace pist {
namespace {

/// A few lines of the template, skipping the banner, so a card can show what
/// the file actually starts with. Blank has nothing to show.
QString templatePreview(SourceTemplate kind)
{
    const QString text = QString::fromLatin1(templateBytes(kind));
    QStringList lines;
    for (QString line : text.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('*'))
            || trimmed.startsWith(QLatin1Char(';')))
            continue;
        const int comment = line.indexOf(QLatin1Char(';'));
        if (comment >= 0)
            line.truncate(comment);
        if (line.trimmed().isEmpty())
            continue;
        line.replace(QLatin1Char('\t'), QStringLiteral("  "));
        lines << line;
        if (lines.size() == 4)
            break;
    }
    return lines.join(QLatin1Char('\n'));
}

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

} // namespace

NewFileDialog::NewFileDialog(QWidget *parent, const QString &directory)
    : QDialog(parent)
{
    setWindowTitle(tr("New File"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(16);

    auto *heading = new QLabel(tr("New file"), this);
    heading->setObjectName(QStringLiteral("newFileHeading"));
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);

    auto *intro = new QLabel(
        tr("A source on disk, from a starting point you can assemble."), this);
    intro->setObjectName(QStringLiteral("newFileIntro"));
    intro->setWordWrap(true);
    QPalette introInk = intro->palette();
    introInk.setColor(QPalette::WindowText, appearance::colors().muted);
    intro->setPalette(introInk);
    root->addWidget(intro);

    root->addWidget(sectionLabel(tr("Name"), this));
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("newFileName"));
    m_name->setPlaceholderText(tr("Name (%1)").arg(QLatin1String(kSourceSuffix)));
    m_name->setMinimumHeight(32);
    root->addWidget(m_name);

    root->addWidget(sectionLabel(tr("Directory"), this));
    m_directory = new QLineEdit(this);
    m_directory->setObjectName(QStringLiteral("newFileDirectory"));
    m_directory->setText(directory);
    m_directory->setMinimumHeight(32);
    auto *browse = new QPushButton(tr("Browse…"), this);
    browse->setMinimumHeight(32);
    connect(browse, &QPushButton::clicked, this, &NewFileDialog::browse);
    auto *directoryRow = new QHBoxLayout;
    directoryRow->setSpacing(8);
    directoryRow->addWidget(m_directory, 1);
    directoryRow->addWidget(browse);
    root->addLayout(directoryRow);

    root->addWidget(sectionLabel(tr("Template"), this));

    // The combo stays the transaction's source of truth (and what the tests
    // drive). The gallery is how a person chooses. It is not shown.
    m_template = new QComboBox(this);
    m_template->setObjectName(QStringLiteral("newFileTemplate"));
    m_template->hide();

    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    int index = 0;
    for (SourceTemplate kind : allSourceTemplates()) {
        m_template->addItem(templateLabel(kind), int(kind));
        auto *card = new GalleryCard(this);
        card->setProperty("templateKind", int(kind));
        card->setTitle(templateLabel(kind));
        card->setDetail(templateDescription(kind));
        card->setPreview(templatePreview(kind));
        card->setMinimumHeight(card->property("templateKind").toInt() == int(SourceTemplate::Blank)
                                    ? 112
                                    : 168);
        grid->addWidget(card, index / 2, index % 2);
        connect(card, &GalleryCard::activated, this, [this, kind] {
            const int at = m_template->findData(int(kind));
            if (at >= 0)
                m_template->setCurrentIndex(at);
        });
        ++index;
    }
    root->addLayout(grid);

    m_message = new QLabel(this);
    m_message->setObjectName(QStringLiteral("newFileMessage"));
    m_message->setWordWrap(true);
    m_message->setMinimumHeight(m_message->fontMetrics().height());
    root->addWidget(m_message);

    m_note = new QLabel(this);
    m_note->setObjectName(QStringLiteral("newFileNote"));
    m_note->setWordWrap(true);
    m_note->setMinimumHeight(m_note->fontMetrics().height());
    root->addWidget(m_note);

    root->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok))
        ok->setText(tr("Create"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, [this] {
        if (spec().verify().ok)
            accept();
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(m_buttons);

    connect(m_name, &QLineEdit::textChanged, this, &NewFileDialog::updateState);
    connect(m_directory, &QLineEdit::textChanged, this, &NewFileDialog::updateState);
    connect(m_template, &QComboBox::currentIndexChanged, this, &NewFileDialog::updateState);
    updateState();

    // The gallery's height is the content. A shorter window compresses the
    // grid and the verdict line lands on top of a card.
    layout()->activate();
    const QSize needed = layout()->minimumSize().expandedTo(QSize(760, 0));
    setMinimumSize(needed);
    resize(needed.expandedTo(QSize(860, 0)));

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

    const int kind = m_template->currentData().toInt();
    for (GalleryCard *card : findChildren<GalleryCard *>())
        card->setSelected(card->property("templateKind").toInt() == kind);
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
