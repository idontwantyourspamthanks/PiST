// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmulatorDisplayWidget.h"

#include "ui/StKeyboard.h"

#include <QCursor>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QScreen>
#include <QVariantAnimation>

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

void EmulatorDisplayWidget::setPaused(bool paused, bool atEntry, const QString &continueHint)
{
    const bool entry = paused && atEntry;
    if (m_paused == paused && m_atEntry == entry && m_continueHint == continueHint)
        return;

    const bool wasPaused = m_paused;
    const bool wasAtEntry = m_atEntry;
    m_paused = paused;
    m_atEntry = entry;
    m_continueHint = continueHint;

    if (!m_paused) {
        if (m_pulseAnimation)
            m_pulseAnimation->stop();
        m_pulse = 1.0;
        setAccessibleName(QString());
        setAccessibleDescription(QString());
        update();
        return;
    }

    // The card is the one "is it hung?" answer, so it gets the attention
    // treatment exactly once when it first appears — not on every repaint and
    // not for later stops, where the user knows why the machine halted.
    if (m_atEntry && (!wasPaused || !wasAtEntry))
        startAttentionPulse();

    if (m_atEntry) {
        setAccessibleName(tr("Emulator paused at program entry"));
        setAccessibleDescription(
            tr("The program has loaded and the emulator is paused before it runs. "
               "%1 resumes it.")
                .arg(continueLabel()));
    } else {
        setAccessibleName(tr("Emulator paused"));
        setAccessibleDescription(tr("%1 resumes the program.").arg(continueLabel()));
    }
    update();
}

void EmulatorDisplayWidget::setContinueHint(const QString &continueHint)
{
    if (m_continueHint == continueHint)
        return;
    m_continueHint = continueHint;
    if (m_paused)
        update();
}

QString EmulatorDisplayWidget::continueLabel() const
{
    return m_continueHint.isEmpty() ? tr("Continue")
                                    : tr("Continue (%1)").arg(m_continueHint);
}

EmulatorDisplayWidget::PauseCardLayout EmulatorDisplayWidget::pauseCardLayout() const
{
    // Geometry is derived from the press and paint alike, so the whole card
    // is a target — a button that looks clickable and isn't is worse than no
    // button at all.
    constexpr int pad = 20;
    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(font().pointSizeF() * 1.25);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics bodyMetrics(font());

    const int cardWidth = qBound(240, width() - 48, 460);
    const int innerWidth = cardWidth - 2 * pad;
    const int bodyHeight =
        bodyMetrics
            .boundingRect(QRect(0, 0, innerWidth, 10000), Qt::AlignLeft | Qt::TextWordWrap,
                          tr("The emulator is paused before your program runs."))
            .height();
    const int buttonHeight = bodyMetrics.height() + 14;
    const int cardHeight =
        pad + titleMetrics.height() + 8 + bodyHeight + 16 + buttonHeight + pad;

    PauseCardLayout layout;
    layout.card = QRect((width() - cardWidth) / 2, (height() - cardHeight) / 2, cardWidth,
                        cardHeight);
    layout.title = QRect(layout.card.x() + pad, layout.card.y() + pad, innerWidth,
                         titleMetrics.height());
    layout.body = QRect(layout.card.x() + pad, layout.title.bottom() + 8, innerWidth, bodyHeight);
    layout.button = QRect(layout.card.x() + pad, layout.body.bottom() + 16, innerWidth,
                          buttonHeight);
    return layout;
}

void EmulatorDisplayWidget::paintPauseCard(QPainter &p)
{
    const PauseCardLayout layout = pauseCardLayout();
    p.setRenderHint(QPainter::Antialiasing);

    // Dark, but not opaque: the last frame showing round the edges is the
    // evidence that the machine really did load and is merely stopped. The
    // outline keeps the card legible when there is no frame at all (black).
    p.setPen(QPen(QColor(90, 90, 90), 1));
    p.setBrush(QColor(0, 0, 0, 205));
    p.drawRoundedRect(layout.card.adjusted(0, 0, -1, -1), 8, 8);

    QFont titleFont = p.font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(p.font().pointSizeF() * 1.25);
    p.setFont(titleFont);
    p.setPen(QColor(240, 240, 240));
    p.drawText(layout.title, Qt::AlignLeft | Qt::AlignVCenter, tr("Program loaded"));

    p.setFont(font());
    p.setPen(QColor(210, 210, 210));
    p.drawText(layout.body, Qt::AlignLeft | Qt::TextWordWrap,
               tr("The emulator is paused before your program runs."));

    // The highlighted button is what pulses: a moving bright rectangle in a
    // still panel is hard to miss, and it settles rather than blinking
    // forever.
    QColor accent = palette().color(QPalette::Highlight);
    accent.setAlphaF(0.45 + 0.55 * m_pulse);
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawRoundedRect(layout.button, 6, 6);
    p.setPen(palette().color(QPalette::HighlightedText));
    p.drawText(layout.button, Qt::AlignCenter, continueLabel());
}

void EmulatorDisplayWidget::paintPausedBadge(QPainter &p)
{
    // A small badge, top-right, that stays out of the video's way but makes the
    // stopped state unmistakable. Semi-transparent so the last frame still shows
    // through beneath it. The Continue hint rides along: the state and the way
    // out of it should never need a manual.
    p.setRenderHint(QPainter::Antialiasing);
    const QString text = m_continueHint.isEmpty()
                             ? tr("Paused")
                             : tr("Paused — %1 continues").arg(m_continueHint);
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

void EmulatorDisplayWidget::startAttentionPulse()
{
    if (!isVisible())
        return; // A hidden panel has nobody to attract; show it at rest.
    if (!m_pulseAnimation) {
        m_pulseAnimation = new QVariantAnimation(this);
        m_pulseAnimation->setDuration(1100);
        m_pulseAnimation->setStartValue(1.0);
        m_pulseAnimation->setKeyValueAt(0.5, 0.25);
        m_pulseAnimation->setEndValue(1.0);
        m_pulseAnimation->setLoopCount(2);
        m_pulseAnimation->setEasingCurve(QEasingCurve::InOutSine);
        connect(m_pulseAnimation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
                    m_pulse = value.toReal();
                    update();
                });
        connect(m_pulseAnimation, &QVariantAnimation::finished, this, [this] {
            m_pulse = 1.0;
            update();
        });
    }
    m_pulse = 1.0;
    m_pulseAnimation->start();
}

void EmulatorDisplayWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);

    QPainter p(this);

    if (m_frame.isNull()) {
        // With nothing to show this is a black rectangle, which reads as a
        // broken emulator rather than as an empty panel. Say what it is —
        // unless a stop is already known, in which case the pause overlay
        // below is the better answer (the entry stop can beat the first
        // frame, and that is exactly when the user needs the card).
        if (!m_paused) {
            p.setPen(QColor(150, 150, 150));
            p.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap,
                       tr("The emulator's display appears here when a session runs "
                          "with the hatari-pist emulator."));
            return;
        }
    } else {
        // The panel owns the pixels: draw the latest frame, aspect-fit and centred.
        p.drawImage(fittedRect(width(), height(), m_frame.width(), m_frame.height()), m_frame);
    }

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

    if (m_atEntry)
        paintPauseCard(p);
    else
        paintPausedBadge(p);
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

void EmulatorDisplayWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // The attention pulse is a one-shot; a hidden panel must not keep
    // repainting for it, and a later show must not resume mid-breath.
    if (m_pulseAnimation) {
        m_pulseAnimation->stop();
        m_pulse = 1.0;
    }
}

void EmulatorDisplayWidget::mousePressEvent(QMouseEvent *event)
{
    // The entry card is a button. It has to be answered before the input-grab
    // branch below, or a session that is stopped (and therefore not captured)
    // would treat the click as "grab the keyboard" and appear to do nothing.
    if (m_paused && m_atEntry && event->button() == Qt::LeftButton
        && pauseCardLayout().card.contains(event->pos())) {
        emit resumeRequested();
        event->accept();
        return;
    }
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

    const QPoint here = event->pos();
    const QPoint hostDelta = here - m_lastMousePos;
    m_lastMousePos = here;
    if (hostDelta.isNull())
        return;

    // Deltas are event-to-event: correct for a relative mouse and for a guest
    // driver that integrates the host pointer absolutely (VirtualBox mouse
    // integration), where re-centring on every move is overwritten by the next
    // injected event and turns each delta into the full offset from centre —
    // one axis amplified, the other pinned. The host cursor is blank while
    // captured, so letting it roam costs nothing; the one thing it must not do
    // is pin at a screen edge and stop delivering motion, so warp, and re-base,
    // only when it gets close to one.
    const QPoint globalHere = mapToGlobal(here);
    if (QScreen *screen = QGuiApplication::screenAt(globalHere)) {
        const QRect geom = screen->geometry();
        const int margin = 32;
        if (globalHere.x() < geom.x() + margin || globalHere.y() < geom.y() + margin
            || globalHere.x() > geom.right() - margin
            || globalHere.y() > geom.bottom() - margin) {
            const QPoint centre = rect().center();
            QCursor::setPos(mapToGlobal(centre));
            m_lastMousePos = centre;
        }
    }

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
