// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MemoryView.h"

#include "emu/MemoryDump.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

/// Column layout: address, then one column per byte, then the character column.
constexpr int kAddressColumn = 0;
constexpr int kFirstByteColumn = 1;
constexpr int kCharColumn = kFirstByteColumn + 16;

QString hex8(quint32 value)
{
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

} // namespace

MemoryView::MemoryView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *controls = new QHBoxLayout;
    controls->addWidget(new QLabel(tr("Address:"), this));

    m_addressEdit = new QLineEdit(this);
    m_addressEdit->setPlaceholderText(QStringLiteral("00012596"));
    m_addressEdit->setMaximumWidth(120);
    m_addressEdit->setToolTip(tr("Hexadecimal address. Accepts $ or 0x prefixes too."));
    controls->addWidget(m_addressEdit);

    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color: palette(mid);"));
    controls->addWidget(m_status);
    controls->addStretch(1);

    layout->addLayout(controls);

    m_table = new QTableWidget(kRows, kFirstByteColumn + kRowBytes + 1, this);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);

    QFont mono = m_table->font();
    mono.setFamily(QStringLiteral("monospace"));
    m_table->setFont(mono);

    layout->addWidget(m_table);

    connect(m_addressEdit, &QLineEdit::returnPressed, this, &MemoryView::onAddressEntered);
}

void MemoryView::onAddressEntered()
{
    QString text = m_addressEdit->text().trimmed();
    if (text.isEmpty())
        return;

    // Accept the prefixes Hatari itself accepts, so a user can paste an address
    // straight out of the debugger output.
    text.remove(QLatin1Char('$'));
    if (text.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        text.remove(0, 2);

    bool ok = false;
    const quint32 address = text.toUInt(&ok, 16);
    if (!ok) {
        m_status->setText(tr("Not a hexadecimal address: %1").arg(m_addressEdit->text()));
        return;
    }

    goToAddress(address);
}

void MemoryView::goToAddress(quint32 address)
{
    // Align down so rows line up, which is what makes the display readable.
    m_base = address - (address % kRowBytes);
    m_addressEdit->setText(hex8(m_base));
    emit dumpRequested(m_base, kRowBytes * kRows);
}

void MemoryView::refresh()
{
    emit dumpRequested(m_base, kRowBytes * kRows);
}

void MemoryView::clear()
{
    for (int r = 0; r < m_table->rowCount(); ++r) {
        for (int c = 0; c < m_table->columnCount(); ++c) {
            if (auto *item = m_table->item(r, c))
                item->setText(QString());
            else
                m_table->setItem(r, c, new QTableWidgetItem(QString()));
        }
    }
}

void MemoryView::applyDump(const QString &response)
{
    const QList<MemoryRow> rows = parseMemoryDump(response);
    if (rows.isEmpty()) {
        m_status->setText(tr("No memory data returned."));
        return;
    }

    clear();
    m_table->setRowCount(kRows);

    int row = 0;
    for (const MemoryRow &dump : rows) {
        if (row >= kRows)
            break;

        auto *addressItem = m_table->item(row, kAddressColumn);
        if (!addressItem) {
            addressItem = new QTableWidgetItem;
            m_table->setItem(row, kAddressColumn, addressItem);
        }
        addressItem->setText(hex8(dump.address));

        for (int i = 0; i < kRowBytes; ++i) {
            const int column = kFirstByteColumn + i;
            auto *item = m_table->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(row, column, item);
            }
            item->setText(i < dump.bytes.size()
                              ? QStringLiteral("%1").arg(dump.bytes.at(i), 2, 16, QLatin1Char('0')).toUpper()
                              : QString());
        }

        auto *chars = m_table->item(row, kCharColumn);
        if (!chars) {
            chars = new QTableWidgetItem;
            m_table->setItem(row, kCharColumn, chars);
        }
        chars->setText(renderMemoryChars(dump.bytes));

        ++row;
    }

    // Blank any rows the emulator did not return, rather than leaving stale data
    // from a previous address visible.
    for (; row < kRows; ++row) {
        for (int c = 0; c < m_table->columnCount(); ++c) {
            if (auto *item = m_table->item(row, c))
                item->setText(QString());
        }
    }

    m_table->resizeColumnsToContents();
    m_status->setText(tr("%1 bytes from $%2")
                          .arg(rows.size() * kRowBytes)
                          .arg(hex8(m_base)));
}

} // namespace pist
