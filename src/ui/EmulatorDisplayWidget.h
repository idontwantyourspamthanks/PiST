// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QImage>
#include "ui/MouseScaling.h"
#include <QWidget>

class QPainter;
class QVariantAnimation;

namespace pist {

/// The emulator's display panel (docs/PLAN.md §12).
///
/// The emulator (the hatari-pist fork) runs windowless and pushes its
/// frames over the media channel; this panel paints them, aspect-fit and
/// centred. An emulator without the channel keeps its own top-level window
/// and the panel stays hidden.
///
/// Input is the VirtualBox model (§12.4): clicking the panel captures the
/// keyboard and mouse for the guest — grabMouse/grabKeyboard, a blank
/// cursor (the guest cursor is baked into the frames, §12.3), events
/// translated and emitted as keyIntent/mouseIntent. F12 releases (it is
/// never forwarded to the guest); a focus change or a session end releases
/// too, because a trapped user is the failure mode. The one exception is
/// the entry-stop card: a click inside it is Continue (resumeRequested),
/// not a grab, and it is handled before the capture branch.
class EmulatorDisplayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EmulatorDisplayWidget(QWidget *parent = nullptr);

    /// A plain QWidget has an invalid sizeHint, which makes a dock size it to its
    /// minimum instead of a usable video size. Offer the ST low-res size doubled.
    QSize sizeHint() const override;

public slots:
    /// Draw one frame of the media channel: aspect-fit and centred. A null
    /// image forgets it and returns the panel to its empty state.
    void setFrame(const QImage &frame);

    /// Tell the panel whether a media session is running. While one is, the
    /// host cursor hides over the panel (docs/PLAN.md §12.3: the guest
    /// cursor is baked into every frame, so a visible host cursor would be a
    /// second pointer arguing with it). Ending the session also releases any
    /// input capture and restores the cursor.
    void setMediaSession(bool running);

    /// The VirtualBox-style input grab (docs/PLAN.md §12.4).
    void setInputCaptured(bool captured);
    bool inputCaptured() const { return m_inputCaptured; }

    /// Show or hide the "paused" hint. The panel renders no new frames while
    /// the debugger is stopped, which on its own reads as a crash; a small
    /// badge makes the stopped state legible instead.
    ///
    /// `atEntry` marks the first stop of a session — the debugger halting at
    /// the program's entry point before any of it has run. That one gets a
    /// centred card naming the state and offering Continue, because a user
    /// seeing the loaded screen and no motion otherwise has no way to tell a
    /// paused emulator from a hung one. `continueHint` is the Continue
    /// action's shortcut text, already formatted for display.
    void setPaused(bool paused, bool atEntry = false, const QString &continueHint = QString());

    /// Refresh the shortcut shown on the pause card and badge (the shortcut
    /// scheme can change while a session is stopped).
    void setContinueHint(const QString &continueHint);

signals:
    /// The user clicked the entry-stop card. The window answers it with
    /// Continue — the card is a button, and clicking the panel while stopped
    /// must not look like it did nothing.
    void resumeRequested();
    /// One key for the guest, already translated to an ST scancode
    /// (ui/StKeyboard). Emitted only while input is captured.
    void keyIntent(int scancode, bool down);
    /// One mouse move (guest pixels) or button change for the guest, deltas
    /// already scaled through MouseScaler; buttons bit0 = left, bit1 = right.
    void mouseIntent(qint16 dx, qint16 dy, quint8 buttons);
    /// Capture state changed (for the "Release Input" action's enablement).
    void inputCaptureChanged(bool captured);

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    /// The entry-stop card's geometry, shared by painting and hit testing.
    struct PauseCardLayout
    {
        QRect card;
        QRect title;
        QRect body;
        QRect button;
    };
    PauseCardLayout pauseCardLayout() const;
    QString continueLabel() const;
    void paintPauseCard(QPainter &p);
    void paintPausedBadge(QPainter &p);
    void startAttentionPulse();

    /// The latest media-channel frame, null in the empty state.
    QImage m_frame;
    /// Whether the debugger is stopped (drives the paused hint).
    bool m_paused = false;
    /// The stop is the session's entry stop (drives the centred card).
    bool m_atEntry = false;
    /// Continue's shortcut, shown beside the paused hint.
    QString m_continueHint;
    /// 1.0 at rest; the entry card breathes between this and its dim value.
    qreal m_pulse = 1.0;
    /// One short pulse when the entry card first appears, then rest.
    QVariantAnimation *m_pulseAnimation = nullptr;
    /// A media session is running (drives host-cursor hiding and the grab).
    bool m_mediaSession = false;
    /// The VirtualBox-style grab is active (docs/PLAN.md §12.4).
    bool m_inputCaptured = false;
    /// Host→guest delta scaling with a sub-pixel accumulator (§12.3).
    MouseScaler m_mouseScaler;
    /// Last cursor position used for deltas, widget coordinates.
    QPoint m_lastMousePos;
    /// Current button state as the MOUSE message carries it (bit0 L, bit1 R).
    quint8 m_mouseButtons = 0;
    /// The release belonging to the grabbing click is swallowed with it.
    bool m_swallowGrabRelease = false;
};

} // namespace pist
