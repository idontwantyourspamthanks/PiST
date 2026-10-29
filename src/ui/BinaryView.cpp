// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/BinaryView.h"

#include "image/StFormats.h"
#include "support/FileWrite.h"

#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

BinaryView::BinaryView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("binaryView"));

    m_name = new QLabel(tr("No file"), this);
    m_name->setObjectName(QStringLiteral("binaryName"));

    m_offset = new QLineEdit(this);
    m_offset->setObjectName(QStringLiteral("binaryOffset"));
    m_offset->setPlaceholderText(tr("offset (hex)"));

    m_width = new QSpinBox(this);
    m_width->setObjectName(QStringLiteral("binaryWidth"));
    m_width->setRange(16, 320);
    m_width->setSingleStep(16);
    m_width->setValue(16);

    m_height = new QSpinBox(this);
    m_height->setObjectName(QStringLiteral("binaryHeight"));
    m_height->setRange(1, 200);
    m_height->setValue(16);

    m_palette = new QLineEdit(this);
    m_palette->setObjectName(QStringLiteral("binaryPalette"));
    m_palette->setPlaceholderText(tr("palette offset (hex, optional)"));

    m_preview = new QPushButton(tr("Preview"), this);
    m_preview->setObjectName(QStringLiteral("binaryPreview"));

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("binaryStatus"));

    m_hex = new QTableWidget(16, 16, this);
    m_hex->setObjectName(QStringLiteral("binaryHex"));
    m_hex->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hex->horizontalHeader()->hide();
    m_hex->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);

    auto *controls = new QHBoxLayout;
    controls->addWidget(m_offset);
    controls->addWidget(m_width);
    controls->addWidget(m_height);
    controls->addWidget(m_palette);
    controls->addWidget(m_preview);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_name);
    layout->addLayout(controls);
    layout->addWidget(m_status);
    layout->addWidget(m_hex, 1);

    connect(m_preview, &QPushButton::clicked, this, &BinaryView::preview);
    connect(m_offset, &QLineEdit::returnPressed, this, &BinaryView::showOffset);
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
    m_offset->setText(QStringLiteral("0"));
    showOffset();
    m_status->setText(tr("%1 bytes").arg(bytes.size()));
}

bool BinaryView::parseHex(const QString &text, quint32 *value) const
{
    QString hex = text.trimmed();
    if (hex.startsWith(QLatin1Char('$')))
        hex.remove(0, 1);
    else if (hex.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        hex.remove(0, 2);
    bool ok = false;
    const uint parsed = hex.toUInt(&ok, 16);
    if (!ok || !value)
        return false;
    *value = parsed;
    return true;
}

void BinaryView::showOffset()
{
    quint32 base = 0;
    if (!parseHex(m_offset->text(), &base))
        base = 0;
    if (qint64(base) > m_bytes.size())
        base = quint32(m_bytes.size());

    for (int row = 0; row < 16; ++row) {
        const quint32 address = base + quint32(row * 16);
        m_hex->setVerticalHeaderItem(row, new QTableWidgetItem(QStringLiteral("%1").arg(
            address, 8, 16, QLatin1Char('0'))));
        for (int column = 0; column < 16; ++column) {
            const qint64 at = qint64(base) + row * 16 + column;
            QString text;
            if (at >= 0 && at < m_bytes.size())
                text = QStringLiteral("%1").arg(uchar(m_bytes.at(int(at))), 2, 16, QLatin1Char('0'));
            auto *item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            m_hex->setItem(row, column, item);
        }
    }
}

void BinaryView::preview()
{
    if (m_bytes.isEmpty()) {
        m_status->setText(tr("No file"));
        return;
    }
    quint32 offset = 0;
    if (!parseHex(m_offset->text(), &offset)) {
        m_status->setText(tr("Bad offset"));
        return;
    }
    const int width = m_width->value();
    const int height = m_height->value();
    const int need = (width / 16) * 8 * height;
    if (qint64(offset) + need > m_bytes.size()) {
        m_status->setText(tr("Range runs past the end of the file"));
        return;
    }
    const QByteArray planes = m_bytes.mid(int(offset), need);

    QByteArray palette;
    const QString paletteText = m_palette->text().trimmed();
    if (!paletteText.isEmpty()) {
        quint32 paletteOffset = 0;
        if (!parseHex(paletteText, &paletteOffset)) {
            m_status->setText(tr("Bad palette offset"));
            return;
        }
        if (qint64(paletteOffset) + 32 > m_bytes.size()) {
            m_status->setText(tr("Palette runs past the end of the file"));
            return;
        }
        palette = m_bytes.mid(int(paletteOffset), 32);
    }

    ImageDocument doc;
    QString error;
    if (!imageFromLowResBitplanes(planes, width, height, palette, &doc, &error)) {
        m_status->setText(error);
        return;
    }
    const QString path = QDir::temp().filePath(
        QStringLiteral("pist-preview-%1.pim").arg(++m_previewSerial));
    if (!files::write(path, doc.toJson(), &error)) {
        m_status->setText(error);
        return;
    }
    m_status->setText(tr("Preview written"));
    emit previewReady(path);
}

} // namespace pist
