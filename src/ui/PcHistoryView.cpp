// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/PcHistoryView.h"

#include "ui/Appearance.h"

#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
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
    appearance::markMono(m_text);
    m_text->viewport()->installEventFilter(this);

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

bool PcHistoryView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_text && watched == m_text->viewport() && event->type() == QEvent::MouseButtonDblClick) {
        const auto *mouse = static_cast<const QMouseEvent *>(event);
        const QTextCursor cursor = m_text->cursorForPosition(mouse->pos());
        static const QRegularExpression hex(
            QStringLiteral(R"((?:\$|0x)?([0-9A-Fa-f]{6,8})\b)"));
        const QRegularExpressionMatch match = hex.match(cursor.block().text());
        if (match.hasMatch()) {
            bool ok = false;
            const quint32 address = match.captured(1).toUInt(&ok, 16);
            if (ok)
                emit addressActivated(address);
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace pist
