// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/BinaryView.h"

#include "image/BinaryScan.h"
#include "image/ImageDocument.h"
#include "image/Palette.h"
#include "image/StFormats.h"
#include "support/FileWrite.h"
#include "ui/Appearance.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QListWidget>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

/// The sprite editor's preview box is 96. This one is the picture itself, so
/// a 16-pixel sprite lands on an integer scale.
constexpr int kPreviewBox = 160;

} // namespace

BinaryView::BinaryView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("binaryView"));

    m_name = new QLabel(tr("No file"), this);
    m_name->setObjectName(QStringLiteral("binaryName"));

    m_mode = new QComboBox(this);
    m_mode->setObjectName(QStringLiteral("binaryMode"));
    m_mode->addItem(tr("Image"), int(Focus::Image));
    m_mode->addItem(tr("Audio"), int(Focus::Audio));
    m_mode->addItem(tr("Data"), int(Focus::Data));
    m_mode->setToolTip(tr("Image shows the picture. Audio shows the sample. Data is the hex only."));

    m_offset = new QSpinBox(this);
    m_offset->setObjectName(QStringLiteral("binaryOffset"));
    m_offset->setDisplayIntegerBase(16);
    m_offset->setRange(0, 0);
    m_offset->setAccelerated(true);
    m_offset->setToolTip(tr("Where the picture and the sample start, in hex. The arrows step by one byte."));

    m_firstData = new QPushButton(tr("First data"), this);
    m_firstData->setObjectName(QStringLiteral("binaryFirstData"));
    m_firstData->setToolTip(tr("Move the offset to the first line that is not all zeros."));

    m_width = new QSpinBox(this);
    m_width->setObjectName(QStringLiteral("binaryWidth"));
    m_width->setRange(16, 320);
    m_width->setSingleStep(16);
    m_width->setValue(16);
    m_width->setToolTip(tr("Width of the picture, in pixels. A multiple of 16, from 16 to 320."));

    m_height = new QSpinBox(this);
    m_height->setObjectName(QStringLiteral("binaryHeight"));
    m_height->setRange(1, 200);
    m_height->setValue(16);
    m_height->setToolTip(tr("Height of the picture, in pixels, from 1 to 200."));

    m_palette = new QSpinBox(this);
    m_palette->setObjectName(QStringLiteral("binaryPalette"));
    m_palette->setDisplayIntegerBase(16);
    m_palette->setRange(-1, 0);
    m_palette->setSpecialValueText(tr("default"));
    m_palette->setValue(-1);
    m_palette->setAccelerated(true);
    m_palette->setToolTip(tr("Hex offset of a 16-colour palette in this same file. "
                             "The arrows step by one byte. The lowest value uses the "
                             "default STE palette."));

    m_preview = new QPushButton(tr("Edit image"), this);
    m_preview->setObjectName(QStringLiteral("binaryPreview"));
    m_preview->setToolTip(tr("Open this range in the sprite editor."));

    m_image = new QLabel(this);
    m_image->setObjectName(QStringLiteral("binaryImagePreview"));
    m_image->setFixedSize(kPreviewBox, kPreviewBox);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setScaledContents(false);

    m_sampleLength = new QSpinBox(this);
    m_sampleLength->setObjectName(QStringLiteral("binarySampleLength"));
    m_sampleLength->setRange(1, 65536);
    m_sampleLength->setValue(256);
    m_sampleLength->setToolTip(tr("How many bytes to save as 8-bit mono audio, starting at the offset."));

    m_rate = new QSpinBox(this);
    m_rate->setObjectName(QStringLiteral("binaryRate"));
    m_rate->setRange(4000, 50066);
    m_rate->setValue(8000);
    m_rate->setToolTip(tr("Sample rate in hertz, from 4000 to 50066."));

    m_signed = new QCheckBox(tr("Signed"), this);
    m_signed->setObjectName(QStringLiteral("binarySigned"));
    m_signed->setToolTip(tr("The sample bytes are signed. Clear this for the unsigned bytes "
                            "an ST sample uses."));

    m_saveWav = new QPushButton(tr("Save WAV…"), this);
    m_saveWav->setObjectName(QStringLiteral("binarySaveWav"));
    m_saveWav->setToolTip(tr("Save the sample at the offset as a WAV file."));

    m_scan = new QPushButton(tr("Scan"), this);
    m_scan->setObjectName(QStringLiteral("binaryScan"));
    m_scan->setToolTip(tr("Find AVR, IFF, Ice, and Atomik in this file."));

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("binaryStatus"));

    m_hex = new QTableWidget(16, 17, this);
    m_hex->setObjectName(QStringLiteral("binaryHex"));
    m_hex->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hex->setToolTip(tr("Hex bytes, with the ASCII of each line in the last column."));
    m_hex->horizontalHeader()->hide();
    m_hex->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    appearance::markMono(m_hex);
    m_hex->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    const int hexWidth = m_hex->fontMetrics().horizontalAdvance(QStringLiteral("ff")) + 12;
    const int asciiWidth = m_hex->fontMetrics().horizontalAdvance(QString(16, QLatin1Char('M'))) + 8;
    for (int column = 0; column < 16; ++column)
        m_hex->setColumnWidth(column, hexWidth);
    m_hex->setColumnWidth(16, asciiWidth);

    m_hits = new QListWidget(this);
    m_hits->setObjectName(QStringLiteral("binaryHits"));
    m_hits->setToolTip(tr("Matches from Scan. Choosing one moves the offset there."));

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_mode);
    controls->addWidget(m_offset);
    controls->addWidget(m_firstData);
    controls->addWidget(m_width);
    controls->addWidget(m_height);
    controls->addWidget(m_palette);
    controls->addWidget(m_preview);
    controls->addWidget(m_scan);

    m_audioRow = new QWidget(this);
    m_audioRow->setObjectName(QStringLiteral("binaryAudioRow"));
    auto *samples = new QHBoxLayout(m_audioRow);
    samples->setContentsMargins(0, 0, 0, 0);
    samples->addWidget(m_sampleLength);
    samples->addWidget(m_rate);
    samples->addWidget(m_signed);
    samples->addWidget(m_saveWav);
    samples->addStretch(1);

    auto *left = new QVBoxLayout;
    left->addWidget(m_name);
    left->addLayout(controls);
    left->addWidget(m_audioRow);
    left->addWidget(m_status);
    left->addWidget(m_hex, 1);
    left->addWidget(m_hits);

    m_previewColumn = new QWidget(this);
    m_previewColumn->setObjectName(QStringLiteral("binaryPreviewColumn"));
    auto *heading = new QLabel(tr("Preview"), m_previewColumn);
    QFont headingFont = heading->font();
    headingFont.setBold(true);
    heading->setFont(headingFont);
    auto *side = new QVBoxLayout(m_previewColumn);
    side->setContentsMargins(0, 0, 0, 0);
    side->addWidget(heading);
    side->addWidget(m_image);
    side->addStretch(1);

    auto *layout = new QHBoxLayout(this);
    layout->addLayout(left, 1);
    layout->addWidget(m_previewColumn);

    connect(m_preview, &QPushButton::clicked, this, &BinaryView::preview);
    connect(m_firstData, &QPushButton::clicked, this, &BinaryView::firstData);
    connect(m_scan, &QPushButton::clicked, this, &BinaryView::scan);
    connect(m_hits, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0)
            return;
        const QListWidgetItem *item = m_hits->item(row);
        if (!item)
            return;
        const int offset = item->data(Qt::UserRole).toInt();
        const QString kind = item->data(Qt::UserRole + 1).toString();
        applyKind(kind);
        {
            QSignalBlocker blocker(m_offset);
            m_offset->setValue(offset);
        }
        offsetEdited();
        if (kind == QLatin1String("iff"))
            emit iffHitReady(m_bytes.mid(offset));
    });
    connect(m_saveWav, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, tr("Save WAV"), QString(),
                                                          tr("WAV audio (*.wav)"));
        if (path.isEmpty())
            return;
        QString error;
        if (!writeSample(path, &error))
            m_status->setText(error);
    });
    connect(m_offset, qOverload<int>(&QSpinBox::valueChanged), this, &BinaryView::offsetEdited);
    connect(m_width, qOverload<int>(&QSpinBox::valueChanged), this, &BinaryView::refreshPreview);
    connect(m_height, qOverload<int>(&QSpinBox::valueChanged), this, &BinaryView::refreshPreview);
    connect(m_palette, qOverload<int>(&QSpinBox::valueChanged), this, &BinaryView::refreshPreview);
    connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, &BinaryView::setViewMode);
    setViewMode(int(Focus::Image));
}

void BinaryView::setIdentity(const QString &path, const QString &originKey, const QString &name)
{
    m_path = path;
    m_key = originKey;
    if (!name.isEmpty())
        m_name->setText(name);
}

QString BinaryView::filePath() const
{
    return m_path;
}

QString BinaryView::originKey() const
{
    return m_key;
}

QString BinaryView::displayName() const
{
    return m_name->text();
}

void BinaryView::setBytes(const QByteArray &bytes, const QString &name)
{
    constexpr int kLimit = 2 * 1024 * 1024;
    if (bytes.size() > kLimit) {
        m_status->setText(tr("File is larger than %1 bytes").arg(kLimit));
        return;
    }
    m_bytes = bytes;
    m_name->setText(name);
    {
        QSignalBlocker blocker(m_offset);
        QSignalBlocker paletteBlock(m_palette);
        const int end = qMax(0, int(bytes.size()));
        m_offset->setRange(0, end);
        m_offset->setValue(0);
        m_palette->setRange(-1, end);
        m_palette->setValue(-1);
    }
    showOffset();
    m_status->setText(tr("%1 bytes").arg(bytes.size()));
    identifyAtOffset();
    refreshPreview();
}

void BinaryView::firstData()
{
    if (m_bytes.isEmpty()) {
        m_status->setText(tr("No file"));
        return;
    }
    constexpr int kLine = 16;
    int line = -1;
    for (int at = 0; at < m_bytes.size(); at += kLine) {
        const int end = qMin(at + kLine, int(m_bytes.size()));
        for (int i = at; i < end; ++i) {
            if (m_bytes.at(i) != '\0') {
                line = at;
                break;
            }
        }
        if (line >= 0)
            break;
    }
    if (line < 0) {
        m_status->setText(tr("No non-zero line"));
        return;
    }
    {
        QSignalBlocker blocker(m_offset);
        m_offset->setValue(line);
    }
    offsetEdited();
}

void BinaryView::offsetEdited()
{
    showOffset();
    identifyAtOffset();
    refreshPreview();
}

void BinaryView::showOffset()
{
    const quint32 base = quint32(qMax(0, m_offset->value()));

    for (int row = 0; row < 16; ++row) {
        const quint32 address = base + quint32(row * 16);
        m_hex->setVerticalHeaderItem(row, new QTableWidgetItem(QStringLiteral("%1").arg(
            address, 8, 16, QLatin1Char('0'))));
        QString ascii;
        for (int column = 0; column < 16; ++column) {
            const qint64 at = qint64(base) + row * 16 + column;
            QString text;
            if (at >= 0 && at < m_bytes.size()) {
                const uchar byte = uchar(m_bytes.at(int(at)));
                text = QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0'));
                ascii.append(byte >= 32 && byte < 127 ? QChar(byte) : QLatin1Char('.'));
            }
            auto *item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_hex->setItem(row, column, item);
        }
        auto *asciiItem = new QTableWidgetItem(ascii);
        asciiItem->setFlags(asciiItem->flags() & ~Qt::ItemIsEditable);
        m_hex->setItem(row, 16, asciiItem);
    }
}

bool BinaryView::imageAtControls(ImageDocument *doc, QString *error) const
{
    if (m_bytes.isEmpty()) {
        if (error)
            *error = tr("No file");
        return false;
    }
    const quint32 offset = quint32(qMax(0, m_offset->value()));
    const int width = m_width->value();
    const int height = m_height->value();
    const int need = (width / 16) * 8 * height;
    if (qint64(offset) + need > m_bytes.size()) {
        if (error)
            *error = tr("Range runs past the end of the file");
        return false;
    }
    const QByteArray planes = m_bytes.mid(int(offset), need);

    QByteArray palette;
    if (m_palette->value() >= 0) {
        const quint32 paletteOffset = quint32(m_palette->value());
        if (qint64(paletteOffset) + 32 > m_bytes.size()) {
            if (error)
                *error = tr("Palette runs past the end of the file");
            return false;
        }
        palette = m_bytes.mid(int(paletteOffset), 32);
    }
    return imageFromLowResBitplanes(planes, width, height, palette, doc, error);
}

void BinaryView::setViewMode(int mode)
{
    m_focus = Focus(qBound(0, mode, int(Focus::Data)));
    if (m_mode->currentIndex() != int(m_focus)) {
        QSignalBlocker blocker(m_mode);
        m_mode->setCurrentIndex(int(m_focus));
    }
    const bool image = m_focus == Focus::Image;
    const bool audio = m_focus == Focus::Audio;
    m_width->setVisible(image);
    m_height->setVisible(image);
    m_palette->setVisible(image);
    m_preview->setVisible(image);
    m_previewColumn->setVisible(image);
    m_audioRow->setVisible(audio);
    if (image)
        refreshPreview();
}

void BinaryView::applyKind(const QString &kind)
{
    if (kind == QLatin1String("iff"))
        setViewMode(int(Focus::Image));
    else if (kind == QLatin1String("avr"))
        setViewMode(int(Focus::Audio));
    else if (kind == QLatin1String("ice") || kind == QLatin1String("atomik"))
        setViewMode(int(Focus::Data));
}

void BinaryView::refreshPreview()
{
    if (m_focus != Focus::Image)
        return;
    ImageDocument doc;
    QString error;
    if (!imageAtControls(&doc, &error)) {
        m_image->clear();
        m_image->setToolTip(error);
        return;
    }
    const QImage image = indicesToImage(doc.pixels(), doc.width(), doc.height(), doc.paletteKind(),
                                        EmptyStyle::Checkerboard);
    m_image->setPixmap(QPixmap::fromImage(
        image.scaled(m_image->size(), Qt::KeepAspectRatio, Qt::FastTransformation)));
    m_image->setToolTip(tr("%1×%2").arg(doc.width()).arg(doc.height()));
}

void BinaryView::preview()
{
    ImageDocument doc;
    QString error;
    if (!imageAtControls(&doc, &error)) {
        m_status->setText(error);
        return;
    }
    const QString path = QDir::temp().filePath(
        QStringLiteral("pist-preview-%1.pim").arg(++m_previewSerial));
    if (!files::write(path, doc.toJson(), &error)) {
        m_status->setText(error);
        return;
    }
    m_status->setText(tr("Opened in the editor"));
    emit previewReady(path);
}

bool BinaryView::writeSample(const QString &path, QString *error)
{
    const quint32 offset = quint32(qMax(0, m_offset->value()));
    const int length = m_sampleLength->value();
    if (qint64(offset) + length > m_bytes.size()) {
        if (error)
            *error = tr("Range runs past the end of the file");
        return false;
    }
    const QByteArray wav = wavFromPcm8(m_bytes.mid(int(offset), length), m_rate->value(),
                                      m_signed->isChecked(), error);
    if (wav.isEmpty())
        return false;
    if (!files::write(path, wav, error))
        return false;
    m_status->setText(tr("WAV written"));
    return true;
}

void BinaryView::scan()
{
    m_hits->clear();
    const QVector<MagicHit> found = scanMagic(m_bytes);
    for (const MagicHit &hit : found) {
        auto *item = new QListWidgetItem(tr("%1  %2")
                                             .arg(hit.offset, 8, 16, QLatin1Char('0'))
                                             .arg(hit.kind));
        item->setData(Qt::UserRole, hit.offset);
        item->setData(Qt::UserRole + 1, hit.kind);
        m_hits->addItem(item);
    }
    if (found.isEmpty())
        m_status->setText(tr("No AVR, IFF, Ice, or Atomik"));
}

void BinaryView::showHit(int offset, const QString &kind)
{
    applyKind(kind);
    {
        QSignalBlocker blocker(m_offset);
        m_offset->setValue(offset);
    }
    offsetEdited();
}

void BinaryView::identifyAtOffset()
{
    const quint32 offset = quint32(qMax(0, m_offset->value()));
    const QByteArray tail = m_bytes.mid(int(offset));

    AvrInfo avr;
    QString error;
    if (parseAvrHeader(tail.left(128), &avr, &error)) {
        if (avr.rate < m_rate->minimum() || avr.rate > m_rate->maximum()) {
            m_status->setText(error);
            return;
        }
        if (int(avr.length) >= m_sampleLength->minimum() && int(avr.length) <= m_sampleLength->maximum())
            m_sampleLength->setValue(int(avr.length));
        m_rate->setValue(avr.rate);
        m_signed->setChecked(avr.signedSamples);
        m_status->setText(tr("AVR, %1 Hz, %2 bytes").arg(avr.rate).arg(avr.length));
        return;
    }

    const QString packer = packerMagic(tail);
    if (packer == QLatin1String("ice")) {
        m_status->setText(tr("Ice, not unpacked"));
        return;
    }
    if (packer == QLatin1String("atomik")) {
        m_status->setText(tr("Atomik, not unpacked"));
        return;
    }

    const SndhTags tags = parseSndhTags(tail.left(4096));
    if (tags.found) {
        QString text = tags.title.isEmpty() ? tr("SNDH") : tags.title;
        if (!tags.composer.isEmpty())
            text += tr(" — %1").arg(tags.composer);
        m_status->setText(text);
    }
}

} // namespace pist
