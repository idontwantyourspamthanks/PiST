// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/HardwareView.h"

#include "ui/Appearance.h"

#include <QComboBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace pist {

HardwareView::HardwareView(QWidget *parent)
    : QWidget(parent)
{
    m_subject = new QComboBox(this);
    // The ST-family info subjects Hatari 2.6.1 knows. "video" is the default:
    // the shifter state (resolution, palette, screen address) is the one an
    // assembly developer most often wants to watch.
    m_subject->addItems({QStringLiteral("video"), QStringLiteral("mfp"),
                         QStringLiteral("acia"), QStringLiteral("ikbd"),
                         QStringLiteral("ym"), QStringLiteral("blitter"),
                         QStringLiteral("dmasnd")});

    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    appearance::markMono(m_text);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_subject);
    layout->addWidget(m_text);

    connect(m_subject, &QComboBox::currentTextChanged, this,
            [this](const QString &text) { emit subjectChanged(text); });
}

QString HardwareView::subject() const
{
    return m_subject->currentText();
}

void HardwareView::setInfo(const QString &text)
{
    m_text->setPlainText(text);
}

void HardwareView::clear()
{
    m_text->clear();
}

} // namespace pist
