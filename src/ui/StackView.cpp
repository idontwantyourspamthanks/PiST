// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/StackView.h"

#include "emu/HexFormat.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"

#include <QHeaderView>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {
// How many stack entries to show. A call is a 4-byte return address, so 24
// longs covers a reasonable call depth plus saved registers.
constexpr int kLongs = 24;
} // namespace

StackView::StackView(QWidget *parent)
    : QWidget(parent)
{
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({tr("Address"), tr("Value"), tr("Note")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setAlternatingRowColors(true);
    appearance::markMono(m_table);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);

    connect(m_table, &QTableWidget::cellDoubleClicked, this, &StackView::onCellDoubleClicked);
}

void StackView::clear()
{
    m_table->setRowCount(0);
}

void StackView::setStackDump(quint32 sp, const QList<MemoryRow> &rows,
                             quint32 textBase, quint32 textEnd)
{
    m_sp = sp;
    m_lastRows = rows;
    m_lastTextBase = textBase;
    m_lastTextEnd = textEnd;
    m_haveDump = true;
    if (rows.isEmpty()) {
        clear();
        return;
    }

    // Flatten the rows to bytes relative to the first row's address, so a long
    // can be read at any aligned offset regardless of where rows break.
    const quint32 base = rows.first().address;
    QByteArray bytes;
    for (const MemoryRow &row : rows) {
        const int start = int(row.address - base);
        if (start < 0)
            continue;
        for (int i = 0; i < row.bytes.size(); ++i) {
            const int at = start + i;
            if (bytes.size() <= at)
                bytes.resize(at + 1);
            bytes[at] = char(row.bytes[i]);
        }
    }

    m_table->setRowCount(kLongs);
    const appearance::Colors theme = appearance::colors();
    for (int i = 0; i < kLongs; ++i) {
        const quint32 address = sp + quint32(i * 4);
        const int offset = int(address - base);
        const quint32 value = readLongBE(bytes, offset);

        QString note;
        if (textEnd > textBase && value >= textBase && value < textEnd)
            note = tr("return address?");
        else if (looksLikeAddress(value))
            note = tr("pointer?");

        auto *addrItem = new QTableWidgetItem(
            i == 0 ? tr("SP→ %1").arg(hex::hex32(address))
                   : QStringLiteral("    %1").arg(hex::hex32(address)));
        auto *valueItem = new QTableWidgetItem(hex::hex32(value));
        auto *noteItem = new QTableWidgetItem(note);
        addrItem->setForeground(i == 0 ? theme.gutterPc : theme.address);
        if (!note.isEmpty())
            noteItem->setForeground(theme.success);

        if (i == 0 || !note.isEmpty()) {
            QFont f = valueItem->font();
            f.setBold(true);
            valueItem->setFont(f);
        }

        m_table->setItem(i, 0, addrItem);
        m_table->setItem(i, 1, valueItem);
        m_table->setItem(i, 2, noteItem);
    }
    m_table->resizeColumnsToContents();
}

void StackView::applyAppearance()
{
    appearance::markMono(m_table);
    m_table->verticalHeader()->setDefaultSectionSize(fontMetrics().height() + 4);
    if (m_haveDump)
        setStackDump(m_sp, m_lastRows, m_lastTextBase, m_lastTextEnd);
}

void StackView::onCellDoubleClicked(int row, int column)
{
    Q_UNUSED(column);
    if (row < 0)
        return;
    // Read back the displayed value: the row's long if it looks like a pointer
    // (return addresses, saved pointers), else the slot's own address, so the
    // memory view can show the bytes the value points into or the slot itself.
    const QString valueText = m_table->item(row, 1) ? m_table->item(row, 1)->text() : QString();
    bool ok = false;
    const quint32 value = valueText.toUInt(&ok, 16);
    if (ok && looksLikeAddress(value)) {
        emit addressActivated(value);
        return;
    }
    emit addressActivated(m_sp + quint32(row * 4));
}

} // namespace pist
