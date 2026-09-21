// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/HardwareView.h"

#include "ui/Appearance.h"

#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QStringList>
#include <QVBoxLayout>

namespace pist {

HardwareView::HardwareView(QWidget *parent)
    : QWidget(parent)
{
    m_subject = new QComboBox(this);
    // The ST-family info subjects Hatari 2.6.1 knows. "video" is the default.
    // Hatari's `info video` prints the screen address and the refresh rate;
    // resolution and the palette are not in that transcript.
    const QStringList ids = {QStringLiteral("video"), QStringLiteral("mfp"),
                             QStringLiteral("acia"), QStringLiteral("ikbd"),
                             QStringLiteral("ym"),   QStringLiteral("blitter"),
                             QStringLiteral("dmasnd")};
    const QStringList tips = {
        tr("Shifter — screen address and refresh"),
        tr("MFP 68901 — timers and serial"),
        tr("ACIA 6850 — serial"),
        tr("IKBD — keyboard and mouse"),
        tr("YM2149 — sound"),
        tr("Blitter — block copy"),
        tr("DMA sound"),
    };
    for (int i = 0; i < ids.size(); ++i) {
        m_subject->addItem(ids.at(i));
        m_subject->setItemData(i, tips.at(i), Qt::ToolTipRole);
    }
    m_subject->setToolTip(tips.at(0));

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
        m_summary->clear();
        m_summary->hide();
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

void HardwareView::setInfo(const QString &text)
{
    m_text->setPlainText(text);

    // Only the lines Hatari actually prints. Resolution and the palette are
    // not among them, so the header does not invent them.
    QStringList parts;
    const QRegularExpression base(
        QStringLiteral(R"(Video base\s*:\s*(?:0x|\$)?([0-9A-Fa-f]+))"));
    const QRegularExpression rate(QStringLiteral(R"(Refresh rate\s*:\s*(\d+)\s*Hz)"));
    const QRegularExpression overscan(QStringLiteral(R"(V-overscan\s*:\s*(\S+))"));
    const QRegularExpressionMatch baseMatch = base.match(text);
    if (baseMatch.hasMatch())
        parts.append(tr("Screen $%1").arg(baseMatch.captured(1).toUpper()));
    const QRegularExpressionMatch rateMatch = rate.match(text);
    if (rateMatch.hasMatch())
        parts.append(tr("%1 Hz").arg(rateMatch.captured(1)));
    const QRegularExpressionMatch overMatch = overscan.match(text);
    if (overMatch.hasMatch())
        parts.append(tr("overscan %1").arg(overMatch.captured(1)));
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
