// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MemoryView.h"

#include "emu/HexFormat.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"

#include <QApplication>
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
    m_addressEdit->setPlaceholderText(tr("address"));
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
    m_table->setToolTip(tr("Double-click a byte to edit it; Alt+double-click follows a pointer."));
    m_table->setShowGrid(false);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setAlternatingRowColors(true);
    appearance::markMono(m_table);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);

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
    m_pendingEdit = false;
    m_staleDumps = 0;
    m_base = address - (address % kRowBytes);
    m_addressEdit->setText(hex::hex32(m_base));
    // The old region's bytes must not survive under the new base: until the
    // fresh dump arrives, blank beats wrong — and a discarded stale dump
    // (applyDump refuses one not starting at m_base) leaves exactly this.
    m_lastRows.clear();
    clear();
    emit dumpRequested(m_base, kRowBytes * kRows);
}

void MemoryView::refresh()
{
    emit dumpRequested(m_base, kRowBytes * kRows);
}

void MemoryView::onCellDoubleClicked(int row, int column)
{
    if (column < kFirstByteColumn || column >= kFirstByteColumn + kRowBytes)
        return;

    // A cell shows one byte, but a pointer is four. Re-align down to even, which
    // is the only alignment the 68000 itself would use for a long access.
    int offset = row * kRowBytes + (column - kFirstByteColumn);
    offset -= offset % 2;
    const quint32 value = readLongBE(m_bytes, offset);
    // Pointer-following is Alt+double-click: most byte cells sit inside a
    // 4-byte window that looks like an address (any code address, most
    // counters), so letting a plain double-click navigate made most of the
    // table uneditable — it navigated instead of editing. (Ctrl+double-click
    // would be macOS's secondary-click gesture and might never arrive.)
    if ((QApplication::keyboardModifiers() & Qt::AltModifier)
        && looksLikeAddress(value)) {
        goToAddress(value);
        return;
    }
    if (m_editingEnabled) {
        if (auto *item = m_table->item(row, column))
            m_table->editItem(item);
    }
}

void MemoryView::clear()
{
    // Self-contained: blanking the table must never fire itemChanged, whether
    // called from applyDump's rewrite or goToAddress's navigation — each blank
    // would be read by onByteEdited as an invalid user edit and restored from
    // the backing store, resurrecting the OLD region's bytes under the new
    // base. QSignalBlocker composes correctly under an outer blocker; a raw
    // blockSignals pair does not.
    const QSignalBlocker blocker(m_table);
    for (int r = 0; r < m_table->rowCount(); ++r) {
        for (int c = 0; c < m_table->columnCount(); ++c) {
            if (auto *item = m_table->item(r, c))
                item->setText(QString());
            else
                m_table->setItem(r, c, new QTableWidgetItem(QString()));
        }
    }
    // The backing store must go too: onByteEdited's invalid-commit restore and
    // the Alt+double-click pointer-follow both read it.
    m_bytes.clear();
    m_status->clear();
}

void MemoryView::applyAppearance()
{
    appearance::markMono(m_table);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    if (!m_lastRows.isEmpty())
        applyDump(m_lastRows);
}

void MemoryView::applyDump(const QList<MemoryRow> &rows)
{
    if (rows.isEmpty()) {
        m_status->setText(tr("No memory data returned."));
        return;
    }

    // A dump whose first row is not the current base is stale — requested
    // before the latest navigation. Discarding it keeps the display, the
    // status text and the edit-write addresses all on m_base; showing it would
    // display one region while an edit wrote to another.
    if (rows.first().address != m_base)
        return;

    if (m_pendingEdit) {
        QByteArray flat;
        for (const MemoryRow &dump : rows) {
            flat.append(reinterpret_cast<const char *>(dump.bytes.constData()),
                        qMin(dump.bytes.size(), static_cast<qsizetype>(kRowBytes)));
        }
        const int offset = static_cast<int>(m_pendingAddress - m_base);
        if (offset >= 0 && offset < flat.size()
            && quint8(flat.at(offset)) != m_pendingValue) {
            if (++m_staleDumps <= kMaxStaleDumps)
                return;
        }
        m_pendingEdit = false;
        m_staleDumps = 0;
    }

    m_lastRows = rows;

    // Programmatic rewrites are not user edits: with editing enabled (the
    // stopped state), every setText would otherwise fire itemChanged →
    // onByteEdited → memoryEdited, and each of those is a debugger write plus
    // a refresh — one dump would start an unbounded write/refresh storm that
    // steamrollers any in-progress edit. Block for the whole rewrite, not
    // just the clear.
    const QSignalBlocker blocker(m_table);
    clear();
    m_table->setRowCount(kRows);
    m_bytes.clear();
    const appearance::Colors theme = appearance::colors();

    int row = 0;
    for (const MemoryRow &dump : rows) {
        if (row >= kRows)
            break;

        auto *addressItem = m_table->item(row, kAddressColumn);
        if (!addressItem) {
            addressItem = new QTableWidgetItem;
            m_table->setItem(row, kAddressColumn, addressItem);
        }
        addressItem->setText(hex::hex32(dump.address));
        addressItem->setForeground(theme.address);

        for (int i = 0; i < kRowBytes; ++i) {
            const int column = kFirstByteColumn + i;
            auto *item = m_table->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(row, column, item);
            }
            if (i < dump.bytes.size()) {
                const quint8 byte = dump.bytes.at(i);
                item->setText(QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper());
                item->setForeground(byte == 0 ? theme.zero : theme.hex);
            } else {
                item->setText(QString());
            }
        }

        auto *chars = m_table->item(row, kCharColumn);
        if (!chars) {
            chars = new QTableWidgetItem;
            m_table->setItem(row, kCharColumn, chars);
        }
        chars->setText(renderMemoryChars(dump.bytes));
        chars->setForeground(theme.ascii);

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

    // `rows.size() * kRowBytes` overstates the dump.
    m_status->setText(tr("%1 bytes from $%2")
                          .arg(static_cast<int>(m_bytes.size()))
                          .arg(hex::hex32(m_base)));
}

void MemoryView::setEditingEnabled(bool enabled)
{
    m_editingEnabled = enabled;
    m_table->setEditTriggers(enabled ? QAbstractItemView::EditKeyPressed
                                     : QAbstractItemView::NoEditTriggers);
    // An editable pane must be focusable: NoFocus would keep F2 and the
    // in-place editor from ever receiving keys.
    m_table->setFocusPolicy(enabled ? Qt::StrongFocus : Qt::NoFocus);
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
        const QSignalBlocker blocker(m_table);
        const int offset = row * kRowBytes + (column - kFirstByteColumn);
        const quint32 current = offset < m_bytes.size() ? quint8(m_bytes.at(offset)) : 0;
        item->setText(QStringLiteral("%1").arg(current, 2, 16, QLatin1Char('0')).toUpper());
        return;
    }

    const quint32 address = m_base + row * kRowBytes + (column - kFirstByteColumn);
    m_pendingEdit = true;
    m_pendingAddress = address;
    m_pendingValue = static_cast<quint8>(value);
    m_staleDumps = 0;
    emit memoryEdited(address, value);
}

} // namespace pist
