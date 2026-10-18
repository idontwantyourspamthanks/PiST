// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/NewImageDialog.h"

#include "image/ImageDocument.h"
#include "image/Palette.h"
#include "ui/Appearance.h"
#include "ui/GalleryCard.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSize>
#include <QSpinBox>
#include <QVBoxLayout>

namespace pist {
namespace {

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel *muted(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setWordWrap(true);
    QPalette ink = label->palette();
    ink.setColor(QPalette::WindowText, appearance::colors().muted);
    label->setPalette(ink);
    return label;
}

/// The preset's shape, drawn to its own aspect so 16×16 and 320×200 do not
/// look like the same square with a different caption.
class AspectGlyph : public QWidget
{
public:
    explicit AspectGlyph(QSize aspect, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_aspect(aspect)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedHeight(72);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(8, 6, -8, -6);
        // One scale for the sprites, so 16, 32 and 64 are different sizes,
        // and the low-resolution screen is drawn as a screen.
        const bool screen = m_aspect.width() >= 200;
        const qreal scale = screen
            ? qMin(box.width() / m_aspect.width(), box.height() / m_aspect.height())
            : qMin(box.width(), box.height()) * 0.78 / 64.0;
        const QSizeF drawn(m_aspect.width() * scale, m_aspect.height() * scale);
        const QRectF frame(box.center().x() - drawn.width() / 2,
                           box.center().y() - drawn.height() / 2, drawn.width(), drawn.height());
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawRoundedRect(frame, 2, 2);
    }

private:
    QSize m_aspect;
};

/// Eight of the palette's default colours, so the two cubes are not only names.
class SwatchRow : public QWidget
{
public:
    explicit SwatchRow(PaletteKind kind, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_kind(kind)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedHeight(18);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QVector<int> indices = defaultActiveIndices(m_kind);
        const int n = qMin(8, indices.size());
        if (n <= 0)
            return;
        const int gap = 4;
        const int width = qMax(8, (this->width() - gap * (n - 1)) / n);
        const int height = qMin(this->height(), 14);
        const int y = (this->height() - height) / 2;
        for (int i = 0; i < n; ++i) {
            painter.fillRect(i * (width + gap), y, width, height, cubeColor(m_kind, indices.at(i)));
        }
    }

private:
    PaletteKind m_kind;
};

} // namespace

NewImageDialog::NewImageDialog(QWidget *parent, const QString &directory)
    : QDialog(parent)
    , m_directory(directory)
{
    setWindowTitle(tr("New Image"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(28, 24, 28, 20);
    root->setSpacing(14);

    auto *heading = new QLabel(tr("New image"), this);
    QFont headingFont = heading->font();
    headingFont.setPointSizeF(headingFont.pointSizeF() * 1.6);
    headingFont.setBold(true);
    heading->setFont(headingFont);
    root->addWidget(heading);
    root->addWidget(muted(tr("A sprite or a screen, in the ST's own colours, saved as a .pim."), this));

    root->addWidget(sectionLabel(tr("Size"), this));

    const struct {
        QSize size;
        const char *title;
        const char *detail;
    } presets[] = {
        { {16, 16}, QT_TR_NOOP("16 × 16"), QT_TR_NOOP("A small sprite") },
        { {32, 32}, QT_TR_NOOP("32 × 32"), QT_TR_NOOP("A sprite") },
        { {64, 64}, QT_TR_NOOP("64 × 64"), QT_TR_NOOP("A large sprite") },
        { {320, 200}, QT_TR_NOOP("320 × 200"), QT_TR_NOOP("Low resolution") },
    };

    auto *presetRow = new QHBoxLayout;
    presetRow->setSpacing(12);
    for (const auto &preset : presets) {
        auto *card = new GalleryCard(this);
        card->setProperty("preset", preset.size);
        card->setTitle(tr(preset.title));
        card->setDetail(tr(preset.detail));
        card->body()->insertWidget(0, new AspectGlyph(preset.size, card));
        card->setMinimumWidth(140);
        presetRow->addWidget(card, 1);
        connect(card, &GalleryCard::activated, this, [this, size = preset.size] {
            m_applyingPreset = true;
            m_width->setValue(size.width());
            m_height->setValue(size.height());
            m_applyingPreset = false;
            syncChoices();
        });
    }
    root->addLayout(presetRow);

    auto *sizeRow = new QHBoxLayout;
    sizeRow->setSpacing(16);
    auto addSpin = [this, sizeRow](const QString &label, QSpinBox *&box, int max) {
        auto *column = new QVBoxLayout;
        column->setSpacing(4);
        column->addWidget(muted(label, this));
        box = new QSpinBox(this);
        box->setRange(ImageDocument::kMinSize, max);
        box->setMinimumHeight(32);
        box->setMinimumWidth(120);
        column->addWidget(box);
        sizeRow->addLayout(column);
    };
    addSpin(tr("Width"), m_width, ImageDocument::kMaxWidth);
    addSpin(tr("Height"), m_height, ImageDocument::kMaxHeight);
    sizeRow->addStretch(1);
    root->addLayout(sizeRow);
    m_width->setValue(32);
    m_height->setValue(32);

    root->addWidget(sectionLabel(tr("Palette"), this));
    auto *paletteRow = new QHBoxLayout;
    paletteRow->setSpacing(12);
    const struct {
        PaletteKind kind;
        const char *title;
        const char *detail;
    } palettes[] = {
        { PaletteKind::Ste, QT_TR_NOOP("STe"), QT_TR_NOOP("4,096 colours. Four bits a channel.") },
        { PaletteKind::Stfm, QT_TR_NOOP("STfm"), QT_TR_NOOP("512 colours. The original cube.") },
    };
    for (const auto &palette : palettes) {
        auto *card = new GalleryCard(this);
        card->setProperty("paletteKind", int(palette.kind));
        card->setTitle(tr(palette.title));
        card->setDetail(tr(palette.detail));
        card->body()->addWidget(new SwatchRow(palette.kind, card));
        paletteRow->addWidget(card, 1);
        connect(card, &GalleryCard::activated, this, [this, kind = palette.kind] {
            m_kind = kind;
            syncChoices();
        });
    }
    root->addLayout(paletteRow);

    root->addWidget(sectionLabel(tr("File"), this));
    m_file = new QLineEdit(this);
    m_file->setObjectName(QStringLiteral("newImageFileName"));
    m_file->setPlaceholderText(tr("Name (*.pim)"));
    m_file->setMinimumHeight(32);
    auto *browse = new QPushButton(tr("Browse…"), this);
    browse->setMinimumHeight(32);
    connect(browse, &QPushButton::clicked, this, &NewImageDialog::browse);
    auto *fileRow = new QHBoxLayout;
    fileRow->setSpacing(8);
    fileRow->addWidget(m_file, 1);
    fileRow->addWidget(browse);
    root->addLayout(fileRow);

    // The note follows the field rather than the directory it started in: Browse…
    // writes an absolute path, and a typed path may carry one, so a note frozen at
    // construction could name a folder the file will not be written to.
    auto *savedIn = muted(QString(), this);
    const auto refreshSavedIn = [this, savedIn] {
        const QString path = filePath();
        const QString folder = path.isEmpty() ? m_directory : QFileInfo(path).absolutePath();
        savedIn->setText(tr("Saved in %1").arg(QDir::toNativeSeparators(
            folder.isEmpty() ? QDir::currentPath() : folder)));
    };
    connect(m_file, &QLineEdit::textChanged, this, [refreshSavedIn] { refreshSavedIn(); });
    refreshSavedIn();
    root->addWidget(savedIn);

    root->addStretch(1);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (QPushButton *ok = m_buttons->button(QDialogButtonBox::Ok))
        ok->setText(tr("Create"));
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
    root->addWidget(m_buttons);

    connect(m_file, &QLineEdit::textChanged, this, &NewImageDialog::updateOkButton);
    connect(m_width, &QSpinBox::valueChanged, this, [this] {
        if (!m_applyingPreset)
            syncChoices();
    });
    connect(m_height, &QSpinBox::valueChanged, this, [this] {
        if (!m_applyingPreset)
            syncChoices();
    });
    updateOkButton();
    syncChoices();

    layout()->activate();
    const QSize needed = layout()->minimumSize().expandedTo(QSize(720, 0));
    setMinimumSize(needed);
    resize(needed.expandedTo(QSize(840, 0)));
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

void NewImageDialog::syncChoices()
{
    const QSize size(m_width->value(), m_height->value());
    for (GalleryCard *card : findChildren<GalleryCard *>()) {
        if (card->property("preset").isValid())
            card->setSelected(card->property("preset").toSize() == size);
        if (card->property("paletteKind").isValid())
            card->setSelected(card->property("paletteKind").toInt() == int(m_kind));
    }
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
    return m_kind;
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
