// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/GalleryCard.h"

#include "ui/Appearance.h"

#include <QEnterEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

namespace pist {

GalleryCard::GalleryCard(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("galleryCard"));
    setFrameShape(QFrame::NoFrame);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    m_body = new QVBoxLayout(this);
    m_body->setContentsMargins(16, 14, 16, 14);
    m_body->setSpacing(6);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("galleryCardTitle"));
    QFont title = m_title->font();
    title.setBold(true);
    title.setPointSizeF(title.pointSizeF() * 1.05);
    m_title->setFont(title);
    m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_body->addWidget(m_title);

    m_detail = new QLabel(this);
    m_detail->setObjectName(QStringLiteral("galleryCardDetail"));
    m_detail->setWordWrap(true);
    m_detail->setAttribute(Qt::WA_TransparentForMouseEvents);
    QPalette detail = m_detail->palette();
    detail.setColor(QPalette::WindowText, appearance::colors().muted);
    m_detail->setPalette(detail);
    m_body->addWidget(m_detail);

    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("galleryCardPreview"));
    m_preview->setVisible(false);
    m_preview->setAttribute(Qt::WA_TransparentForMouseEvents);
    appearance::markMono(m_preview);
    QFont preview = m_preview->font();
    preview.setPointSizeF(preview.pointSizeF() * 0.9);
    m_preview->setFont(preview);
    QPalette ink = m_preview->palette();
    ink.setColor(QPalette::WindowText, appearance::colors().muted);
    m_preview->setPalette(ink);
    m_body->addWidget(m_preview);
}

void GalleryCard::setTitle(const QString &title)
{
    m_title->setText(title);
}

void GalleryCard::setDetail(const QString &detail)
{
    m_detail->setText(detail);
    m_detail->setVisible(!detail.isEmpty());
}

void GalleryCard::setPreview(const QString &text)
{
    m_preview->setText(text);
    m_preview->setVisible(!text.isEmpty());
}

void GalleryCard::setSelected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    update();
}

void GalleryCard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(1, 1, -1.5, -1.5);
    QColor fill = palette().color(QPalette::Base);
    if (m_selected)
        fill = palette().color(QPalette::Button);
    else if (m_hover || hasFocus())
        fill = palette().color(QPalette::AlternateBase);

    QColor border = palette().color(m_selected || hasFocus() ? QPalette::Highlight : QPalette::Mid);
    painter.setPen(QPen(border, m_selected ? 2.0 : 1.0));
    painter.setBrush(fill);
    painter.drawRoundedRect(box, 10, 10);
}

void GalleryCard::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
        emit activated();
    QFrame::mouseReleaseEvent(event);
}

void GalleryCard::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
        || event->key() == Qt::Key_Space) {
        emit activated();
        return;
    }
    QFrame::keyPressEvent(event);
}

void GalleryCard::enterEvent(QEnterEvent *event)
{
    m_hover = true;
    update();
    QFrame::enterEvent(event);
}

void GalleryCard::leaveEvent(QEvent *event)
{
    m_hover = false;
    update();
    QFrame::leaveEvent(event);
}

} // namespace pist
