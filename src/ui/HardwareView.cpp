// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/HardwareView.h"

#include "ui/Appearance.h"

#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QStringList>
#include <QVBoxLayout>
#include <QVector>

namespace pist {

HardwareView::HardwareView(QWidget *parent)
    : QWidget(parent)
{
    m_subject = new QComboBox(this);
    // The ST-family info subjects Hatari 2.6.1 knows. "video" is the default.
    // Hatari's `info video` prints the screen address and the refresh rate;
    // resolution and the palette are not in that transcript.
    //
    // One pair per subject, not two parallel lists: an id added without its
    // tip used to be an out-of-range `tips.at(i)` (undefined behaviour in a
    // release build). A plain struct + initializer list keeps the two halves
    // impossible to desynchronise.
    struct Subject {
        QString id;
        QString tip;
    };
    const QVector<Subject> subjects = {
        {QStringLiteral("video"), tr("Shifter — screen address and refresh")},
        {QStringLiteral("mfp"), tr("MFP 68901 — timers and serial")},
        {QStringLiteral("acia"), tr("ACIA 6850 — serial")},
        {QStringLiteral("ikbd"), tr("IKBD — keyboard and mouse")},
        {QStringLiteral("ym"), tr("YM2149 — sound")},
        {QStringLiteral("blitter"), tr("Blitter — block copy")},
        {QStringLiteral("dmasnd"), tr("DMA sound")},
    };
    for (int i = 0; i < subjects.size(); ++i) {
        m_subject->addItem(subjects.at(i).id);
        m_subject->setItemData(i, subjects.at(i).tip, Qt::ToolTipRole);
    }
    m_subject->setToolTip(subjects.first().tip);

    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("hardwareSummary"));
    m_summary->setWordWrap(true);
    m_summary->hide();

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    appearance::markMono(m_text);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_subject);
    layout->addWidget(m_summary);
    layout->addWidget(m_text);

    connect(m_subject, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        // The transcript and the header on screen describe the subject just
        // left. With no session running nobody re-requests `info <subject>`, so
        // without this the previous subject's dump sits under the new label —
        // a hardware inspector whose text contradicts its own heading.
        clear();
        const int row = m_subject->findText(text);
        if (row >= 0)
            m_subject->setToolTip(m_subject->itemData(row, Qt::ToolTipRole).toString());
        emit subjectChanged(text);
    });
}

QString HardwareView::subject() const
{
    return m_subject->currentText();
}

void HardwareView::setInfo(const HardwareSummary &summary)
{
    m_text->setPlainText(summary.transcript);

    // The header is the summary the report's parser produced, rendered — not
    // re-derived here. It shows only what the report actually states:
    // resolution and the palette are not in `info video`, so they are not
    // invented.
    QStringList parts;
    if (summary.hasScreenBase) {
        // Eight hex digits, as everywhere else addresses are shown; the report
        // writes them lower-case with an `0x`, which is not what the pane says.
        const QString address =
            QStringLiteral("%1").arg(summary.screenBase, 8, 16, QLatin1Char('0')).toUpper();
        parts.append(tr("Screen $%1").arg(address));
    }
    if (summary.refreshHz)
        parts.append(tr("%1 Hz").arg(summary.refreshHz));
    if (!summary.overscan.isEmpty())
        parts.append(tr("overscan %1").arg(summary.overscan));
    const QString header = parts.join(QStringLiteral(" · "));
    m_summary->setText(header);
    m_summary->setVisible(!header.isEmpty());
}

void HardwareView::clear()
{
    m_text->clear();
    m_summary->clear();
    m_summary->hide();
}

} // namespace pist
