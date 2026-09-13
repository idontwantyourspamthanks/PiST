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
    // to.
    setAttribute(Qt::WA_NativeWindow);

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

QSize EmulatorDisplayWidget::sizeHint() const
{
    // The ST's low-resolution screen, doubled — large enough to read, small
    // enough to share the dock. Expanding policy grows it beyond this when the
    // user gives the dock more room.
    return {640, 480};
}

void EmulatorDisplayWidget::setVideoSize(int width, int height)
{
    if (width > 0 && height > 0) {
        m_videoW = width;
        m_videoH = height;
    }
}

void EmulatorDisplayWidget::showEmbedded()
{
    // Hatari never maps the window it created hidden, so map it, then fit it.
    mapEmbeddedWindowChildren(winId());
    m_settleTicks = 0;
    m_settleTimer->start();
    fitEmbedded();
}

void EmulatorDisplayWidget::fitEmbedded()
{
    const int w = width(), h = height();
    if (w <= 0 || h <= 0)
        return;

    int fitW = w, fitH = h;
    if (m_videoW > 0 && m_videoH > 0) {
        // Largest size that keeps the video's aspect and still fits: letterbox
        // rather than squash. The black background shows as the bars, which reads
        // as intentional rather than a rendering bug.
        const double scale = qMin(double(w) / m_videoW, double(h) / m_videoH);
        fitW = int(m_videoW * scale);
        fitH = int(m_videoH * scale);
    }
    const int x = (w - fitW) / 2;
    const int y = (h - fitH) / 2;

    // Fit to the widget's current Qt geometry — the size the dock has actually
    // allocated. The container's X11 window can hold a stale, larger size from
    // when it was created, and fitting to that pushes the video out of the dock.
    resizeEmbeddedChild(winId(), x, y, fitW, fitH);
}

void EmulatorDisplayWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The dock changed size, so the embedded window must change with it. Hatari
    // rescales its renderer to match, as it does for a hand-resized window.
    fitEmbedded();
}

} // namespace pist
