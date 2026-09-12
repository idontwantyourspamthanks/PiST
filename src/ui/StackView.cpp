// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/StackView.h"

#include "emu/MemoryDump.h"

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
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setStretchLastSection(true);

    QFont mono = m_table->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_table->setFont(mono);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_table);
}

void StackView::clear()
{
    m_table->setRowCount(0);
}

void StackView::setStackDump(quint32 sp, const QString &response,
                             quint32 textBase, quint32 textEnd)
{
    const QList<MemoryRow> rows = parseMemoryDump(response);
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
            i == 0 ? QStringLiteral("SP→ %1").arg(address, 8, 16, QLatin1Char('0'))
                   : QStringLiteral("    %1").arg(address, 8, 16, QLatin1Char('0')));
        auto *valueItem = new QTableWidgetItem(QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0')));
        auto *noteItem = new QTableWidgetItem(note);

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

} // namespace pist
