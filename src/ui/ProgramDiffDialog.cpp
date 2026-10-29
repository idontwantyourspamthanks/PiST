// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/ProgramDiffDialog.h"

#include "emu/HexFormat.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

namespace pist {

namespace {

QString hexBytes(const QByteArray &bytes)
{
    QStringList parts;
    parts.reserve(bytes.size());
    for (uchar byte : bytes)
        parts.append(QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper());
    return parts.join(QLatin1Char(' '));
}

quint32 sectionBase(const QString &section, const ProgramDiffHints &hints, bool *known)
{
    *known = false;
    if (!hints.haveBases)
        return 0;
    if (section == QLatin1String("text") && hints.textBase) {
        *known = true;
        return hints.textBase;
    }
    if (section == QLatin1String("data") && hints.dataBase) {
        *known = true;
        return hints.dataBase;
    }
    if (section == QLatin1String("bss") && hints.bssBase) {
        *known = true;
        return hints.bssBase;
    }
    return 0;
}

QString occupancy(quint32 start, int length, quint32 address)
{
    if (length <= 0)
        return {};
    const quint32 end = start + quint32(length);
    if (address >= start && address < end)
        return QStringLiteral("inside");
    return {};
}

} // namespace

ProgramDiffDialog::ProgramDiffDialog(const ProgramDiff &diff, const ProgramDiffHints &hints,
                                     QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("programDiffDialog"));
    setWindowTitle(tr("Compare with last build"));
    resize(760, 420);

    auto *layout = new QVBoxLayout(this);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("programDiffSummary"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(this);
    m_table->setObjectName(QStringLiteral("programDiffTable"));
    m_table->setColumnCount(8);
    m_table->setHorizontalHeaderLabels({tr("Section"), tr("Decision"), tr("Symbol"), tr("Offset"),
                                        tr("Length"), tr("Live"), tr("Old"), tr("New")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table);

    m_footer = new QLabel(tr("Nothing was written to the machine."), this);
    m_footer->setObjectName(QStringLiteral("programDiffFooter"));
    m_footer->setWordWrap(true);
    layout->addWidget(m_footer);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    const auto restartText = [this](const SectionChange &change) {
        const quint32 delta = change.newSize > change.oldSize ? change.newSize - change.oldSize
                                                              : change.oldSize - change.newSize;
        switch (change.reason) {
        case RestartReason::Grew:
            return tr("Grew by %1 bytes. Restart required.").arg(delta);
        case RestartReason::Shrank:
            return tr("Shrank by %1 bytes. Restart required.").arg(delta);
        case RestartReason::Moved:
            return tr("The layout moved. Restart required.");
        case RestartReason::None:
            break;
        }
        return tr("Restart required.");
    };

    if (!diff.ok) {
        m_summary->setText(diff.error);
        return;
    }

    QStringList summary;
    int row = 0;
    for (const SectionChange &change : diff.sections) {
        if (change.decision == SectionDecision::Identical) {
            summary.append(tr("%1 is unchanged").arg(change.name));
            m_table->insertRow(row);
            m_table->setItem(row, 0, new QTableWidgetItem(change.name));
            m_table->setItem(row, 1, new QTableWidgetItem(tr("Unchanged")));
            ++row;
            continue;
        }
        if (change.decision == SectionDecision::RestartRequired) {
            summary.append(tr("%1 needs a restart").arg(change.name));
            m_table->insertRow(row);
            m_table->setItem(row, 0, new QTableWidgetItem(change.name));
            m_table->setItem(row, 1, new QTableWidgetItem(tr("Restart required")));
            bool known = false;
            const quint32 base = sectionBase(change.name, hints, &known);
            if (known)
                m_table->setItem(row, 5, new QTableWidgetItem(
                                             QStringLiteral("$%1").arg(hex::hex32(base))));
            QString note = restartText(change);
            if (known && change.newSize > 0) {
                QStringList who;
                if (hints.havePc && !occupancy(base, int(change.newSize), hints.pc).isEmpty())
                    who.append(tr("The CPU is inside this section."));
                if (hints.haveReturn
                    && !occupancy(base, int(change.newSize), hints.returnAddress).isEmpty())
                    who.append(tr("A return address points into this section."));
                if (!who.isEmpty())
                    note += QLatin1Char(' ') + who.join(QLatin1Char(' '));
            }
            m_table->setItem(row, 7, new QTableWidgetItem(note));
            ++row;
            continue;
        }

        summary.append(change.ranges.size() == 1
                           ? tr("%1 has 1 differing range").arg(change.name)
                           : tr("%1 has %2 differing ranges").arg(change.name).arg(change.ranges.size()));
        bool known = false;
        const quint32 base = sectionBase(change.name, hints, &known);
        for (const ByteRange &range : change.ranges) {
            m_table->insertRow(row);
            m_table->setItem(row, 0, new QTableWidgetItem(change.name));
            m_table->setItem(row, 1, new QTableWidgetItem(tr("Differs")));
            m_table->setItem(row, 2, new QTableWidgetItem(range.symbol));
            m_table->setItem(row, 3, new QTableWidgetItem(
                                         QStringLiteral("$%1").arg(hex::hex32(range.offset))));
            m_table->setItem(row, 4, new QTableWidgetItem(QString::number(range.length)));
            QStringList who;
            if (known) {
                const quint32 live = base + range.offset;
                m_table->setItem(row, 5, new QTableWidgetItem(
                                             QStringLiteral("$%1").arg(hex::hex32(live))));
                if (hints.havePc && !occupancy(live, range.length, hints.pc).isEmpty())
                    who.append(tr("The CPU is inside this range."));
                if (hints.haveReturn && !occupancy(live, range.length, hints.returnAddress).isEmpty())
                    who.append(tr("A return address points into this range."));
            }
            const QString oldText = hexBytes(range.oldPreview);
            const QString newText = hexBytes(range.newPreview);
            m_table->setItem(row, 6, new QTableWidgetItem(oldText));
            QString shown = newText;
            if (!who.isEmpty())
                shown += QStringLiteral(" — ") + who.join(QLatin1Char(' '));
            m_table->setItem(row, 7, new QTableWidgetItem(shown));
            ++row;
        }
    }
    m_summary->setText(summary.join(QStringLiteral(". ")) + QLatin1Char('.'));
    m_table->resizeColumnsToContents();
}

} // namespace pist
