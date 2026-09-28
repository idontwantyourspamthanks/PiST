// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmulatorDisplayWidget.h"

#include "ui/StKeyboard.h"

#include <QCursor>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>

namespace pist {

namespace {

/// The largest rect with the video's aspect that fits in `w` x `h`, centred —
/// letterbox rather than squash, so the black background shows as bars and
/// reads as intentional. Without a known video size the container is filled.
QRect fittedRect(int w, int h, int videoW, int videoH)
{
    int fitW = w, fitH = h;
    if (videoW > 0 && videoH > 0) {
        const double scale = qMin(double(w) / videoW, double(h) / videoH);
        fitW = int(videoW * scale);
        fitH = int(videoH * scale);
    }
    return { (w - fitW) / 2, (h - fitH) / 2, fitW, fitH };
}

} // namespace

EmulatorDisplayWidget::EmulatorDisplayWidget(QWidget *parent)
    : QWidget(parent)
{
    // Black behind the video: the ST border colour is not always black, and an
    // unpainted frame reads as a rendering bug.
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, Qt::black);
    setPalette(p);

    // Expand to fill the dock. The frame is drawn aspect-fit into this size
    // (paintEvent), rather than staying at the video's native resolution with
    // dead space around it.
    setMinimumSize(320, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

QSize EmulatorDisplayWidget::sizeHint() const
{
    // The ST's low-resolution screen, doubled — large enough to read, small
    // enough to share the dock. Expanding policy grows it beyond this when the
    // user gives the dock more room.
    return {640, 480};
}

void EmulatorDisplayWidget::setFrame(const QImage &frame)
{
    if (frame.isNull() && m_frame.isNull())
        return;
    m_frame = frame;
    update();
}

void EmulatorDisplayWidget::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    update();
}

void EmulatorDisplayWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    if (m_frame.isNull()) {
        // With nothing to show this is a black rectangle, which reads as a
        // broken emulator rather than as an empty panel. Say what it is.
        QPainter p(this);
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("The emulator's display appears here when a session runs "
                      "with the hatari-pist emulator."));
        return;
    }

    // The panel owns the pixels: draw the latest frame, aspect-fit and centred.
    QPainter p(this);
    p.drawImage(fittedRect(width(), height(), m_frame.width(), m_frame.height()), m_frame);

    // The grab indicator (§12.4): an accent border while input is captured,
    // plus the release hint — the user must always be able to see how to get
    // their keyboard back.
    if (m_inputCaptured) {
        p.setRenderHint(QPainter::Antialiasing);
        const QColor accent = palette().color(QPalette::Highlight);
        QPen pen(accent);
        pen.setWidth(2);
        p.setPen(pen);
        p.drawRect(rect().adjusted(1, 1, -2, -2));

        const QString hint = tr("Input captured — F12 releases");
        const QFontMetrics fm(p.font());
        const int padX = 10, padY = 6;
        const QRect textRect = fm.boundingRect(hint);
        const QRect badge((width() - textRect.width()) / 2 - padX,
                          height() - textRect.height() - padY * 2 - 8,
                          textRect.width() + padX * 2, textRect.height() + padY * 2);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(QColor(230, 230, 230));
        p.drawText(badge, Qt::AlignCenter, hint);
    }

    if (!m_paused)
        return;

    // A small badge, top-right, that stays out of the video's way but makes the
    // stopped state unmistakable. Semi-transparent so the last frame still shows
    // through beneath it.
    p.setRenderHint(QPainter::Antialiasing);
    const QString text = tr("Paused");
    QFont f = p.font();
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);
    const int padX = 10, padY = 6;
    const QRect textRect = fm.boundingRect(text);
    const QRect badge(width() - textRect.width() - padX * 2 - 8, 8,
                      textRect.width() + padX * 2, textRect.height() + padY * 2);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 170));
    p.drawRoundedRect(badge, 4, 4);
    p.setPen(QColor(230, 230, 230));
    p.drawText(badge, Qt::AlignCenter, text);
}

void EmulatorDisplayWidget::setMediaSession(bool running)
{
    if (m_mediaSession == running)
        return;
    m_mediaSession = running;
    if (!running) {
        // The session is over; a capture must never outlive it, and the
        // cursor comes back either way (§12.4: a trapped user is the bug).
        setInputCaptured(false);
        unsetCursor();
    }
}

void EmulatorDisplayWidget::setInputCaptured(bool captured)
{
    if (m_inputCaptured == captured || (captured && !m_mediaSession))
        return;
    m_inputCaptured = captured;
    if (captured) {
        // The VirtualBox model, Qt-owned (§12.4): no OS-level grab anywhere,
        // so the release key and every IDE shortcut keep working — the fork
        // is a pure machine plus transport. grabMouse keeps move events
        // flowing even outside the widget, which is what makes relative-mode
        // input possible without pointer confinement.
        grabMouse();
        grabKeyboard();
        setCursor(Qt::BlankCursor);
        m_lastMousePos = mapFromGlobal(QCursor::pos());
        m_mouseButtons = 0;
        // Moves without a button are the whole of relative-mode input, and
        // Qt only delivers those to a widget that tracks the mouse.
        setMouseTracking(true);
        m_mouseScaler.reset();
    } else {
        releaseKeyboard();
        releaseMouse();
        setMouseTracking(false);
        // Releasing over the panel while a session runs re-hides the cursor
        // on the next enter; for now restore it wherever it is.
        unsetCursor();
        // …and a grab that ends between the grabbing press and its release
        // must not swallow the next capture's first button-up.
        m_swallowGrabRelease = false;
    }
    emit inputCaptureChanged(captured);
    update();
}

void EmulatorDisplayWidget::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    // §12.3: the guest cursor is baked into the frames, so the host cursor
    // hides over the panel for the whole session; otherwise two pointers
    // show, pointing at different things.
    if (m_mediaSession)
        setCursor(Qt::BlankCursor);
}

void EmulatorDisplayWidget::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (!m_inputCaptured)
        unsetCursor();
}

void EmulatorDisplayWidget::mousePressEvent(QMouseEvent *event)
{
    if (m_mediaSession && !m_inputCaptured && event->button() == Qt::LeftButton) {
        // The grabbing click is consumed — press and release both: it
        // belongs to the grab, not to wherever the guest cursor happened
        // to be (§12.4).
        m_swallowGrabRelease = true;
        setInputCaptured(true);
        event->accept();
        return;
    }
    if (m_inputCaptured) {
        if (event->button() == Qt::LeftButton)
            m_mouseButtons |= 0x01;
        else if (event->button() == Qt::RightButton)
            m_mouseButtons |= 0x02;
        emit mouseIntent(0, 0, m_mouseButtons);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void EmulatorDisplayWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_inputCaptured && m_swallowGrabRelease && event->button() == Qt::LeftButton) {
        // The release of the grabbing click: swallowed with its press.
        m_swallowGrabRelease = false;
        event->accept();
        return;
    }
    if (m_inputCaptured) {
        if (event->button() == Qt::LeftButton)
            m_mouseButtons &= ~0x01;
        else if (event->button() == Qt::RightButton)
            m_mouseButtons &= ~0x02;
        emit mouseIntent(0, 0, m_mouseButtons);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void EmulatorDisplayWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_inputCaptured) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const QPoint hostDelta = event->pos() - m_lastMousePos;
    if (hostDelta.isNull())
        return;

    // Re-centre so the physical cursor can't pin at a screen edge; harmless
    // where the platform can't warp (offscreen, Wayland) since deltas are
    // relative anyway.
    const QPoint centre = rect().center();
    QCursor::setPos(mapToGlobal(centre));
    m_lastMousePos = centre;

    // Host pixels are not guest pixels: the frame is aspect-fit, so the
    // fitted rect's scale converts, and the accumulator keeps the fraction
    // (§12.3 — a 1.5x panel must not make the cursor outrun the hand).
    if (!m_frame.isNull()) {
        const QRect fit = fittedRect(width(), height(), m_frame.width(), m_frame.height());
        if (fit.width() > 0)
            m_mouseScaler.setScale(double(m_frame.width()) / fit.width());
    }
    const QPoint guestDelta = m_mouseScaler.mapDelta(hostDelta);
    if (!guestDelta.isNull())
        emit mouseIntent(qint16(guestDelta.x()), qint16(guestDelta.y()), m_mouseButtons);
    event->accept();
}

void EmulatorDisplayWidget::keyPressEvent(QKeyEvent *event)
{
    if (!m_inputCaptured) {
        QWidget::keyPressEvent(event);
        return;
    }
    // F12 is PiST's release key: intercepted here, never forwarded. The ST
    // has no scancode for it either (the table maps it to -1).
    if (event->key() == Qt::Key_F12) {
        setInputCaptured(false);
        event->accept();
        return;
    }
    // Autorepeat is the guest's business: the 6301 has its own typematic, so
    // only the physical edges go over the wire.
    if (!event->isAutoRepeat()) {
        const int scancode = stkbd::scancodeFromQtKey(event->key());
        if (scancode >= 0)
            emit keyIntent(scancode, true);
    }
    event->accept();
}

void EmulatorDisplayWidget::keyReleaseEvent(QKeyEvent *event)
{
    if (!m_inputCaptured) {
        QWidget::keyReleaseEvent(event);
        return;
    }
    if (event->key() == Qt::Key_F12) {
        event->accept();
        return;
    }
    if (!event->isAutoRepeat()) {
        const int scancode = stkbd::scancodeFromQtKey(event->key());
        if (scancode >= 0)
            emit keyIntent(scancode, false);
    }
    event->accept();
}

} // namespace pist
