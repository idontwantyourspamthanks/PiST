// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/MemoryView.h"

#include "emu/HexFormat.h"
#include "emu/MemoryDump.h"
#include "ui/Appearance.h"

#include <QApplication>
#include <QBrush>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
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

    auto *mark = new QToolButton(this);
    mark->setObjectName(QStringLiteral("memoryMarkButton"));
    mark->setText(tr("Mark"));
    mark->setToolTip(tr("Remember the bytes in this window"));
    mark->setAutoRaise(true);
    connect(mark, &QToolButton::clicked, this, &MemoryView::setMark);
    controls->addWidget(mark);

    m_compareButton = new QToolButton(this);
    m_compareButton->setObjectName(QStringLiteral("memoryCompareButton"));
    m_compareButton->setText(tr("Compare"));
    m_compareButton->setToolTip(tr("Highlight bytes that differ from the mark"));
    m_compareButton->setCheckable(true);
    m_compareButton->setAutoRaise(true);
    connect(m_compareButton, &QToolButton::toggled, this, &MemoryView::onCompareToggled);
    controls->addWidget(m_compareButton);

    m_previousButton = new QToolButton(this);
    m_previousButton->setObjectName(QStringLiteral("memoryPreviousStopButton"));
    m_previousButton->setText(tr("Previous"));
    m_previousButton->setToolTip(tr("Compare each stop with the previous one"));
    m_previousButton->setCheckable(true);
    m_previousButton->setAutoRaise(true);
    connect(m_previousButton, &QToolButton::toggled, this, &MemoryView::onPreviousToggled);
    controls->addWidget(m_previousButton);

    controls->addStretch(1);

    // Open another memory pane, so two regions can be watched at once.
    auto *addPane = new QToolButton(this);
    addPane->setText(QStringLiteral("+"));
    addPane->setToolTip(tr("Open another memory pane"));
    addPane->setAutoRaise(true);
    connect(addPane, &QToolButton::clicked, this, [this] { emit addPaneRequested(); });
    controls->addWidget(addPane);

    layout->addLayout(controls);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("memoryStatus"));
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(m_status);

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
    // A theme change repaints the dump already shown. It is not a new stop,
    // so a previous-stop mark must stay where the last real dump left it.
    if (!m_lastRows.isEmpty())
        paintDump(m_lastRows, false);
}

void MemoryView::applyDump(const QList<MemoryRow> &rows)
{
    paintDump(rows, true);
}

void MemoryView::paintDump(const QList<MemoryRow> &rows, bool advancePrevious)
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

    // A previous-stop repaint keeps the highlight of the comparison already
    // shown. The mark itself has moved on to this dump, ready for the next one.
    const Mark compared =
        (!advancePrevious && m_previousStop && m_haveShown) ? m_shownAgainst : m_mark;

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
        addressItem->setToolTip(regionName(dump.address));

        for (int i = 0; i < kRowBytes; ++i) {
            const int column = kFirstByteColumn + i;
            auto *item = m_table->item(row, column);
            if (!item) {
                item = new QTableWidgetItem;
                m_table->setItem(row, column, item);
            }
            item->setBackground(QBrush());
            item->setToolTip(QString());
            if (i < dump.bytes.size()) {
                const quint8 byte = dump.bytes.at(i);
                const quint32 address = dump.address + quint32(i);
                item->setText(QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper());
                item->setForeground(byte == 0 ? theme.zero : theme.hex);

                const QString region = regionName(address);
                bool differs = false;
                quint8 previous = 0;
                if (m_compare && compared.valid && address >= compared.base) {
                    const quint32 index = address - compared.base;
                    if (index < quint32(compared.bytes.size())
                        && quint8(compared.bytes.at(int(index))) != byte) {
                        differs = true;
                        previous = quint8(compared.bytes.at(int(index)));
                    }
                }
                if (differs) {
                    QColor changed = theme.changed;
                    changed.setAlpha(96);
                    item->setBackground(changed);
                    item->setForeground(theme.changed);
                    const auto byteText = [](quint8 value) {
                        return QStringLiteral("$%1")
                            .arg(value, 2, 16, QLatin1Char('0'))
                            .toUpper();
                    };
                    item->setToolTip(tr("was %1, now %2").arg(byteText(previous), byteText(byte)));
                } else if (!region.isEmpty()) {
                    QColor tint = theme.muted;
                    if (region == QLatin1String("text"))
                        tint = theme.keyword;
                    else if (region == QLatin1String("data"))
                        tint = theme.number;
                    else if (region == QLatin1String("bss"))
                        tint = theme.comment;
                    tint.setAlpha(48);
                    item->setBackground(tint);
                    item->setToolTip(region == QLatin1String("outside")
                                         ? tr("outside the program")
                                         : region);
                }
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
        chars->setToolTip(QString());
        chars->setBackground(QBrush());

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
            if (auto *item = m_table->item(row, c)) {
                item->setText(QString());
                item->setToolTip(QString());
                item->setBackground(QBrush());
            }
        }
    }

    writeComparison(compared);

    // Move the previous-stop mark only after the highlight was painted against
    // the one it replaced. A repaint then keeps that highlight via m_shownAgainst.
    if (advancePrevious && m_previousStop && !m_bytes.isEmpty()) {
        m_shownAgainst = compared;
        m_haveShown = compared.valid;
        m_mark.valid = true;
        m_mark.base = m_base;
        m_mark.bytes = m_bytes;
        m_mark.pc = m_stopPc;
        m_mark.where = m_stopWhere;
        m_mark.hasContext = m_haveStopContext;
    }
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

MemoryView::Mark MemoryView::captureMark() const
{
    Mark mark;
    mark.valid = !m_bytes.isEmpty();
    mark.base = m_base;
    mark.bytes = m_bytes;
    mark.pc = m_stopPc;
    mark.where = m_stopWhere;
    mark.hasContext = m_haveStopContext;
    return mark;
}

void MemoryView::setStopContext(quint32 pc, const QString &where)
{
    m_stopPc = pc;
    m_stopWhere = where;
    m_haveStopContext = true;
}

void MemoryView::setProgramRegions(quint32 textBase, quint32 textEnd, quint32 dataBase,
                                   quint32 dataEnd, quint32 bssBase, quint32 bssEnd)
{
    if (m_textBase == textBase && m_textEnd == textEnd && m_dataBase == dataBase
        && m_dataEnd == dataEnd && m_bssBase == bssBase && m_bssEnd == bssEnd)
        return;
    m_textBase = textBase;
    m_textEnd = textEnd;
    m_dataBase = dataBase;
    m_dataEnd = dataEnd;
    m_bssBase = bssBase;
    m_bssEnd = bssEnd;
    if (!m_lastRows.isEmpty())
        paintDump(m_lastRows, false);
}

QString MemoryView::regionName(quint32 address) const
{
    const auto inside = [](quint32 at, quint32 start, quint32 end) {
        return end > start && at >= start && at < end;
    };
    if (inside(address, m_textBase, m_textEnd))
        return QStringLiteral("text");
    if (inside(address, m_dataBase, m_dataEnd))
        return QStringLiteral("data");
    if (inside(address, m_bssBase, m_bssEnd))
        return QStringLiteral("bss");
    const bool known = (m_textEnd > m_textBase) || (m_dataEnd > m_dataBase) || (m_bssEnd > m_bssBase);
    return known ? QStringLiteral("outside") : QString();
}

QString MemoryView::comparisonStatus(const Mark &compared) const
{
    if (!m_compare || !compared.valid)
        return {};

    const quint32 curEnd = m_base + quint32(m_bytes.size());
    const quint32 markEnd = compared.base + quint32(compared.bytes.size());
    const quint32 lo = qMax(m_base, compared.base);
    const quint32 hi = qMin(curEnd, markEnd);

    QString place;
    if (!compared.where.isEmpty())
        place = compared.where;
    else if (compared.hasContext)
        place = QStringLiteral("$%1").arg(hex::hex32(compared.pc));

    const auto at = [&place](const QString &sentence) {
        return place.isEmpty() ? sentence : sentence + QStringLiteral(" at %1").arg(place);
    };

    if (hi <= lo) {
        const QString address = QStringLiteral("$%1").arg(hex::hex32(compared.base));
        if (place.isEmpty())
            return tr("The mark at %1 does not overlap this window.").arg(address);
        return tr("The mark at %1 (%2) does not overlap this window.").arg(place, address);
    }

    int differ = 0;
    for (quint32 address = lo; address < hi; ++address) {
        const int cur = int(address - m_base);
        const int old = int(address - compared.base);
        if (quint8(m_bytes.at(cur)) != quint8(compared.bytes.at(old)))
            ++differ;
    }

    const bool sameWindow = compared.base == m_base && compared.bytes.size() == m_bytes.size();
    if (!sameWindow) {
        const QString body = differ == 0 ? tr("No bytes differ")
                                         : (differ == 1 ? tr("1 byte differs")
                                                        : tr("%1 bytes differ").arg(differ));
        const QString located = place.isEmpty() ? QString() : tr(" at %1").arg(place);
        return tr("%1 in the overlap with the mark%2 ($%3, this window $%4).")
            .arg(body, located, hex::hex32(compared.base), hex::hex32(m_base));
    }
    const QString count = differ == 1 ? tr("1 byte differs from the mark")
                                      : tr("%1 bytes differ from the mark").arg(differ);
    if (differ == 0)
        return at(tr("No bytes differ from the mark")) + QLatin1Char('.');
    return at(count) + QLatin1Char('.');
}

void MemoryView::writeComparison(const Mark &compared)
{
    const QString comparison = comparisonStatus(compared);
    if (!comparison.isEmpty()) {
        m_status->setText(comparison);
        return;
    }
    if (m_previousStop && !compared.valid) {
        m_status->setText(tr("This window is the mark for the next stop."));
        return;
    }
    m_status->setText(tr("%1 bytes from $%2")
                          .arg(static_cast<int>(m_bytes.size()))
                          .arg(hex::hex32(m_base)));
}

void MemoryView::setMark()
{
    if (m_bytes.isEmpty()) {
        m_status->setText(tr("Nothing to mark."));
        return;
    }
    m_previousStop = false;
    m_haveShown = false;
    m_compare = true;
    m_mark = captureMark();
    {
        const QSignalBlocker compareBlock(m_compareButton);
        const QSignalBlocker previousBlock(m_previousButton);
        m_compareButton->setChecked(true);
        m_previousButton->setChecked(false);
    }
    if (!m_lastRows.isEmpty())
        paintDump(m_lastRows, false);
}

void MemoryView::onCompareToggled(bool on)
{
    if (on && !m_mark.valid) {
        const QSignalBlocker blocker(m_compareButton);
        m_compareButton->setChecked(false);
        m_status->setText(tr("Set a mark first."));
        return;
    }
    m_compare = on;
    if (!on) {
        m_previousStop = false;
        const QSignalBlocker blocker(m_previousButton);
        m_previousButton->setChecked(false);
    }
    if (!m_lastRows.isEmpty())
        paintDump(m_lastRows, false);
    else if (!on)
        m_status->clear();
}

void MemoryView::onPreviousToggled(bool on)
{
    if (on && m_bytes.isEmpty()) {
        const QSignalBlocker blocker(m_previousButton);
        m_previousButton->setChecked(false);
        m_status->setText(tr("Nothing to compare yet."));
        return;
    }
    m_previousStop = on;
    if (!on) {
        if (!m_lastRows.isEmpty())
            paintDump(m_lastRows, false);
        return;
    }
    // The window on screen is the baseline. The next dump is the first
    // comparison, and it replaces this mark afterwards.
    m_haveShown = false;
    m_compare = true;
    m_mark = captureMark();
    {
        const QSignalBlocker blocker(m_compareButton);
        m_compareButton->setChecked(true);
    }
    if (!m_lastRows.isEmpty())
        paintDump(m_lastRows, false);
    m_status->setText(tr("The next stop will be compared with this window."));
}

} // namespace pist
