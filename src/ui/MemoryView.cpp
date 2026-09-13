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
#include <QToolButton>
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

    // Open another memory pane, so two regions can be watched at once.
    auto *addPane = new QToolButton(this);
    addPane->setText(QStringLiteral("+"));
    addPane->setToolTip(tr("Open another memory pane"));
    addPane->setAutoRaise(true);
    connect(addPane, &QToolButton::clicked, this, [this] { emit addPaneRequested(); });
    controls->addWidget(addPane);

    layout->addLayout(controls);

    m_table = new QTableWidget(kRows, kFirstByteColumn + kRowBytes + 1, this);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);

    QFont mono = m_table->font();
    mono.setFamily(QStringLiteral("monospace"));
    m_table->setFont(mono);

    layout->addWidget(m_table);

    connect(m_addressEdit, &QLineEdit::returnPressed, this, &MemoryView::onAddressEntered);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &MemoryView::onCellDoubleClicked);
    connect(m_table, &QTableWidget::itemChanged, this, &MemoryView::onByteEdited);
}

quint32 MemoryView::currentAddress() const
{
    return m_base;
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

void MemoryView::onCellDoubleClicked(int row, int column)
{
    // Double-click, not single: a single click must stay free to select a cell.
    if (column < kFirstByteColumn || column >= kFirstByteColumn + kRowBytes)
        return;

    // A cell shows one byte, but a pointer is four. Re-align down to even, which
    // is the only alignment the 68000 itself would use for a long access.
    int offset = row * kRowBytes + (column - kFirstByteColumn);
    offset -= offset % 2;

    const quint32 value = readLongBE(m_bytes, offset);
    if (looksLikeAddress(value)) {
        goToAddress(value);
        return;
    }
    // Not an address: edit the byte in place. Addresses follow; data edits.
    if (m_editingEnabled) {
        if (auto *item = m_table->item(row, column))
            m_table->editItem(item);
    }
}

void MemoryView::clear()
{
    m_table->blockSignals(true);
    for (int r = 0; r < m_table->rowCount(); ++r) {
        for (int c = 0; c < m_table->columnCount(); ++c) {
            if (auto *item = m_table->item(r, c))
                item->setText(QString());
            else
                m_table->setItem(r, c, new QTableWidgetItem(QString()));
        }
    }
    m_table->blockSignals(false);
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
    m_bytes.clear();

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

        // Keep the raw bytes so a cell's pointer can be read back; only the
        // bytes actually shown count, so a short dump does not let a click read
        // past the data it is displaying.
        m_bytes.append(reinterpret_cast<const char *>(dump.bytes.constData()),
                       qMin(dump.bytes.size(), static_cast<qsizetype>(kRowBytes)));

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
    // The bytes actually shown: the final row may be short, so
    // `rows.size() * kRowBytes` overstates the dump.
    m_status->setText(tr("%1 bytes from $%2")
                          .arg(static_cast<int>(m_bytes.size()))
                          .arg(hex8(m_base)));
}

void MemoryView::setEditingEnabled(bool enabled)
{
    m_editingEnabled = enabled;
    m_table->setEditTriggers(enabled ? QAbstractItemView::EditKeyPressed
                                     : QAbstractItemView::NoEditTriggers);
}

void MemoryView::onByteEdited(QTableWidgetItem *item)
{
    if (!m_editingEnabled || !item)
        return;
    const int row = item->row();
    const int column = item->column();
    if (column < kFirstByteColumn || column >= kFirstByteColumn + kRowBytes)
        return; // address or ASCII column — not a writable byte

    // Parse the committed byte as hex; reject anything that is not 1-2 digits.
    const QString text = item->text().trimmed();
    bool ok = false;
    const quint32 value = text.toUInt(&ok, 16);
    if (!ok || text.length() > 2 || value > 0xff) {
        // Restore the shown byte on bad input.
        m_table->blockSignals(true);
        const int offset = row * kRowBytes + (column - kFirstByteColumn);
        const quint32 current = offset < m_bytes.size() ? quint8(m_bytes.at(offset)) : 0;
        item->setText(QStringLiteral("%1").arg(current, 2, 16, QLatin1Char('0')).toUpper());
        m_table->blockSignals(false);
        return;
    }

    const quint32 address = m_base + row * kRowBytes + (column - kFirstByteColumn);
    emit memoryEdited(address, value);
}

} // namespace pist
