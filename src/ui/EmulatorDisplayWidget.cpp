// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmulatorDisplayWidget.h"

#include "ui/EmbedX11.h"
#include "ui/EmbedWin32.h"
#include "ui/StKeyboard.h"

#include <QCursor>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPalette>
#include <QPainter>
#include <QResizeEvent>
#include <QTimer>

namespace pist {

namespace {

/// How often to look for the emulator's window before it has been found. It
/// appears once Hatari has configured its video, a moment after the process
/// starts; 50 ms keeps the detached window's flash on the desktop to a couple
/// of frames before it moves into the dock.
constexpr int kAttachPollMs = 50;
/// Stop looking after this many empty ticks (10 s at kAttachPollMs). The
/// emulator then either never created a window or has died, and the panel says
/// so rather than waiting forever.
constexpr int kAttachGiveUpTicks = 200;
/// Once adopted, how often to check on it. The emulator replaces its window on
/// a guest resolution change, and it resizes the window it has when the video
/// mode changes, so the poll re-adopts or re-fits whichever that needs.
constexpr int kAttachedPollMs = 500;

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
    // Native, so winId() is a real window of our own — an X11 window ID on X11,
    // an HWND on Windows — rather than an alias for the top-level window's:
    // both embedding paths attach the emulator's display to it.
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

#ifdef Q_OS_WIN
    // The adoption poll. Windows has no reparenting handshake and no signal
    // that the emulator's window now exists, so the container goes looking.
    m_attachTimer = new QTimer(this);
    m_attachTimer->setInterval(kAttachPollMs);
    connect(m_attachTimer, &QTimer::timeout, this,
            &EmulatorDisplayWidget::pollForeignWindow);
#endif
}

EmulatorDisplayWidget::~EmulatorDisplayWidget()
{
    // The adopted window belongs to the emulator, not to Qt. Releasing it
    // first is what stops Windows from destroying it along with this
    // container, which would leave a running emulator with no display at all.
    releaseForeignWindow(m_childWindow);
    m_childWindow = 0;
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

void EmulatorDisplayWidget::setFrame(const QImage &frame)
{
    if (frame.isNull()) {
        if (m_frame.isNull())
            return;
        m_frame = QImage();
        m_videoAttached = false;
        update();
        return;
    }

    if (m_frame.size() != frame.size())
        setVideoSize(frame.width(), frame.height());
    m_frame = frame;
    m_videoAttached = true;
    update();
}

void EmulatorDisplayWidget::showEmbedded()
{
    // Hatari never maps the window it created hidden, so map it, then fit it.
    mapEmbeddedWindowChildren(winId());
    m_videoAttached = true;
    m_settleTicks = 0;
    m_settleTimer->start();
    fitEmbedded();
}

void EmulatorDisplayWidget::attachEmulatorProcess(qint64 processId)
{
#ifdef Q_OS_WIN
    m_foreignPid = processId;
    m_attachTicks = 0;
    m_attachFailed = false;
    m_attachTimer->setInterval(kAttachPollMs);
    m_attachTimer->start();
#else
    // Everywhere else the emulator reparents itself into this window and
    // reports its video size; there is nothing to go and look for.
    Q_UNUSED(processId);
#endif
}

void EmulatorDisplayWidget::pollForeignWindow()
{
#ifdef Q_OS_WIN
    if (windowHandleValid(m_childWindow)) {
        // Adopted and alive. Hatari resizes its own window on a guest video-mode
        // change (SDL_SetWindowSize) and nothing reports that on Windows, so a
        // child whose client size no longer matches the rect we placed is both
        // the only signal and the new video size. The second condition is the
        // spam guard: a window whose own sizing refuses our rect (an SDL
        // minimum, say) must not produce a re-fit and a console line twice a
        // second forever — act only on a size we have not already adopted.
        int childW = 0, childH = 0;
        if (windowClientSize(m_childWindow, &childW, &childH)
            && (childW != m_lastFitW || childH != m_lastFitH)
            && (childW != m_videoW || childH != m_videoH)) {
            setVideoSize(childW, childH);
            emit embedEvent(tr("the emulator resized its window to %1x%2; re-fitting")
                                .arg(childW)
                                .arg(childH));
            fitEmbedded();
        }
        // moveEmbeddedChild() leaves a window that is already where it belongs
        // alone, so this costs nothing while nothing moves.
        fitEmbedded();
        if (m_attachTimer->interval() != kAttachedPollMs)
            m_attachTimer->setInterval(kAttachedPollMs);
        return;
    }
    if (m_childWindow != 0)
        emit embedEvent(tr("the emulator's window is gone; looking for its replacement"));
    m_childWindow = 0;
    m_videoAttached = false;
    m_lastFitW = 0;
    m_lastFitH = 0;

    const quintptr found = findEmulatorWindow(m_foreignPid);
    if (!found) {
        if (++m_attachTicks < kAttachGiveUpTicks)
            return;
        m_attachTimer->stop();
        m_attachFailed = true;
        emit embedEvent(tr("the emulator never showed a window; it keeps its own"));
        update();
        return;
    }

    // The window's own client size is the video size Hatari chose, which the
    // fit needs to keep the aspect. Windows gets no control-socket report:
    // that, like the reparenting, is X11-only upstream.
    int videoW = 0, videoH = 0;
    const bool haveSize = windowClientSize(found, &videoW, &videoH);
    if (haveSize)
        setVideoSize(videoW, videoH);

    if (!embedForeignWindow(found, winId())) {
        // The emulator keeps the window it already has — the mode that works
        // today — and the panel says where the display went.
        m_attachTimer->stop();
        m_attachFailed = true;
        emit embedEvent(tr("could not adopt the emulator's window; it keeps its own"));
        update();
        return;
    }

    m_childWindow = found;
    m_videoAttached = true;
    m_attachTicks = 0;
    // The dock's own layout can still be settling around a fresh session, so
    // re-fit a few times rather than trusting the first geometry.
    m_settleTicks = 0;
    m_settleTimer->start();
    fitEmbedded();
    emit embedEvent(haveSize ? tr("adopted the emulator's window, video %1x%2")
                                     .arg(videoW)
                                     .arg(videoH)
                             : tr("adopted the emulator's window"));
#endif
}

void EmulatorDisplayWidget::clearEmbedded()
{
    if (m_attachTimer)
        m_attachTimer->stop();
    releaseForeignWindow(m_childWindow);
    m_childWindow = 0;
    m_foreignPid = -1;
    m_attachTicks = 0;
    m_attachFailed = false;
    m_videoAttached = false;
    m_lastFitW = 0;
    m_lastFitH = 0;
    // A media session's last frame described that session; leaving it on the
    // panel would read as the emulator's current screen.
    m_frame = QImage();
    update();
}

void EmulatorDisplayWidget::fitEmbedded()
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(m_childWindow))
        return;

    // Physical pixels on both sides: the container's client rect and the
    // child's position inside it live in the same space, which Qt's logical
    // size does not on a scaled display.
    int w = 0, h = 0;
    if (!windowClientSize(winId(), &w, &h) || w <= 0 || h <= 0) {
        w = int(width() * devicePixelRatioF());
        h = int(height() * devicePixelRatioF());
    }
    if (w <= 0 || h <= 0)
        return;

    const QRect fit = fittedRect(w, h, m_videoW, m_videoH);
    moveEmbeddedChild(m_childWindow, fit.x(), fit.y(), fit.width(), fit.height());
    m_lastFitW = fit.width();
    m_lastFitH = fit.height();
#else
    // Fit against the container's real on-screen size, not Qt geometry: for a
    // native dock widget the two can disagree, and fitting against a stale small
    // Qt size is what left the video small and top-left in a large dock.
    int w = 0, h = 0;
    if (!embeddedContainerSize(winId(), &w, &h) || w <= 0 || h <= 0) {
        w = width();
        h = height();
    }
    if (w <= 0 || h <= 0)
        return;

    const QRect fit = fittedRect(w, h, m_videoW, m_videoH);
    resizeEmbeddedChild(winId(), fit.x(), fit.y(), fit.width(), fit.height());
#endif
}

void EmulatorDisplayWidget::setPaused(bool paused)
{
    if (m_paused == paused)
        return;
    m_paused = paused;
    update();
}

void EmulatorDisplayWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // The dock changed size, so the embedded window must change with it. Hatari
    // rescales its renderer to match, as it does for a hand-resized window.
    fitEmbedded();
}

void EmulatorDisplayWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    if (!m_videoAttached) {
        // With nothing embedded this was a black rectangle, which on a platform
        // that cannot embed at all — or when the adoption failed — reads as a
        // broken emulator rather than as an empty panel. Say what it is.
        QPainter p(this);
        p.setPen(QColor(150, 150, 150));
        p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                   m_attachFailed
                       ? tr("The emulator kept its own window.")
                       : tr("The emulator's display appears here when a session runs "
                            "with the display embedded."));
        return;
    }

    // Media mode: the panel owns the pixels, so draw the latest frame with the
    // same aspect-preserving fit the embedded window gets.
    if (!m_frame.isNull()) {
        QPainter p(this);
        p.drawImage(fittedRect(width(), height(), m_frame.width(), m_frame.height()), m_frame);
    }

    // The grab indicator (§12.4): an accent border while input is captured,
    // plus the release hint — the user must always be able to see how to get
    // their keyboard back.
    if (m_inputCaptured) {
        QPainter p(this);
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
    QPainter p(this);
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
