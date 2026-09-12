// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmulatorDisplayWidget.h"

#include "ui/EmbedX11.h"

#include <QPalette>
#include <QResizeEvent>
#include <QTimer>

namespace pist {

EmulatorDisplayWidget::EmulatorDisplayWidget(QWidget *parent)
    : QWidget(parent)
{
    // Native, so winId() is a real X11 window ID rather than an alias for the
    // top-level window's: the reparenting needs a window of our own to attach
    // to. WA_DontCreateNativeAncestors keeps the dock and main window ordinary.
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);

    // Black behind the video: the ST border colour is not always black, and an
    // unpainted frame reads as a rendering bug.
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, Qt::black);
    setPalette(p);

    // Expand to fill the dock. The display tracks this size (see fitEmbedded),
    // scaling the way Hatari does when its own window is resized, rather than
    // leaving the video at its native resolution with dead space around it.
    setMinimumSize(320, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // The dock's layout settles over the first moments after launch, and this
    // widget's Qt size can lag the size the underlying X11 window actually ends
    // up with. Re-fit a few times so the embedded display lands on the settled
    // size, not the transient one.
    m_settleTimer = new QTimer(this);
    m_settleTimer->setInterval(150);
    connect(m_settleTimer, &QTimer::timeout, this, [this] {
        fitEmbedded();
        if (++m_settleTicks >= 12)
            m_settleTimer->stop();
    });
}

void EmulatorDisplayWidget::showEmbedded()
{
    // Hatari never maps the window it created hidden, so map it, then make it
    // fill this widget. Done together because a mapped-but-wrong-sized window is
    // exactly the black-with-dead-space look this is avoiding.
    mapEmbeddedWindowChildren(winId());
    m_settleTicks = 0;
    m_settleTimer->start();
    fitEmbedded();
}

void EmulatorDisplayWidget::fitEmbedded()
{
    // Fit from the X server's idea of the container's size, not the widget's,
    // which can be stale here.
    fitEmbeddedWindowToContainer(winId());
}

void EmulatorDisplayWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The dock changed size, so the embedded window must change with it. Hatari
    // rescales its renderer to match, as it does for a hand-resized window.
    fitEmbedded();
}

} // namespace pist
