// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/PcHistoryView.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>

namespace pist {

PcHistoryView::PcHistoryView(QWidget *parent)
    : QWidget(parent)
{
    m_text = new QPlainTextEdit(this);
    m_text->setReadOnly(true);
    m_text->setPlaceholderText(
        tr("The recent program counters appear here once the machine stops.\n"
           "It tracks the execution path from the start of the session."));
    QFont mono = m_text->font();
    mono.setFamily(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::TypeWriter);
    m_text->setFont(mono);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_text);
}

void PcHistoryView::setHistory(const QString &text)
{
    m_text->setPlainText(text);
}

void PcHistoryView::clear()
{
    m_text->clear();
}

} // namespace pist
