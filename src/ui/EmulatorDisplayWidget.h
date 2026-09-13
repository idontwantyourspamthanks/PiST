// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QWidget>

namespace pist {

/// Hosts the emulator's display when it runs embedded rather than as its own
/// top-level window.
///
/// The mechanics are X11 reparenting, done by Hatari itself: PiST names this
/// widget's native window ID in `PARENT_WIN_ID`, and Hatari reparents its SDL
/// window into it (src/control.c). That only works when both processes are X11
/// clients of the same display, which is why the application is pinned to the
/// xcb platform on Linux and why the child is pinned to SDL_VIDEODRIVER=x11.
/// On a Wayland-native or non-X11 session there is no such window ID and the
/// widget is not used; the emulator then runs as a separate window.
///
/// The widget is a plain black surface. Input does not need forwarding: the
/// reparented SDL window is a real X11 child, so the display server delivers
/// keyboard and mouse to it directly when it is focused.
class EmulatorDisplayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EmulatorDisplayWidget(QWidget *parent = nullptr);

    /// A plain QWidget has an invalid sizeHint, which makes a dock size it to its
    /// minimum instead of a usable video size — which is why the display was
    /// stuck small with dead space around it. Offer the ST low-res size doubled.
    QSize sizeHint() const override;

public slots:
    /// Record the emulator's native video size, which drives the aspect-preserved
    /// fit. Called when the emulator reports it, right after the reparent.
    void setVideoSize(int width, int height);

    /// Show the embedded window and fit it within this widget.
    void showEmbedded();

    /// Fit the embedded window within this widget, preserving the video's aspect
    /// ratio and centring it (letterbox) rather than squashing it to fill.
    void fitEmbedded();

    /// Show or hide the "paused" hint. The embedded panel renders no frames
    /// while the debugger is stopped, which on its own reads as a crash; a small
    /// badge makes the stopped state legible instead.
    void setPaused(bool paused);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    class QTimer *m_settleTimer = nullptr;
    int m_settleTicks = 0;
    /// The emulator's native video size, for the aspect-preserved fit.
    int m_videoW = 0;
    int m_videoH = 0;
    /// Whether the debugger is stopped (drives the paused hint).
    bool m_paused = false;
};

} // namespace pist
