// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QImage>
#include <QSet>
#include <QWidget>

namespace pist {

/// Hosts the emulator's display when it runs embedded rather than as its own
/// top-level window.
///
/// Two mechanisms, one per platform, because neither generalizes:
///
/// - X11: reparenting done by Hatari itself. PiST names this widget's native
///   window ID in `PARENT_WIN_ID`, and Hatari reparents its SDL window into it
///   (src/sdl/screen.c). That only works when both processes are X11 clients of
///   the same display, which is why the application is pinned to the xcb
///   platform on Linux and why the child is pinned to SDL_VIDEODRIVER=x11.
/// - Windows: reparenting done by PiST, with `SetParent` (ui/EmbedWin32.h).
///   Hatari's side of the X11 handshake is compiled out there, so the emulator
///   creates its own window and this widget adopts it, by process id, once it
///   exists.
///
/// Anywhere else — Wayland without XWayland, macOS — there is no window to
/// adopt. The in-process core has no window either: `setFrame()` paints the
/// picture the core returned, letterboxed the same way a foreign window is fit.
///
/// Input needs no forwarding on X11: the reparented window is a real X11 child,
/// so the display server delivers keyboard and mouse to it directly. On Windows
/// the adopted window is a real child too, but whether it takes keyboard focus
/// on a click — and what the documented cross-process DPI-awareness reset does
/// to its scale — are known risks to confirm on a Windows machine
/// (docs/PLAN.md §9), not verified behaviour.
class EmulatorDisplayWidget : public QWidget
{
    Q_OBJECT

public:
    explicit EmulatorDisplayWidget(QWidget *parent = nullptr);
    /// Releases an adopted Windows window before this container goes away:
    /// destroying a window destroys its children, foreign ones included.
    ~EmulatorDisplayWidget() override;

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

    /// Paint one frame from the in-process core. The image is copied by the
    /// caller before it crosses threads; this stores that copy and letterboxes
    /// it. A later `clearEmbedded()` drops it.
    void setFrame(const QImage &image);

    /// Fit the embedded window within this widget, preserving the video's aspect
    /// ratio and centring it (letterbox) rather than squashing it to fill.
    void fitEmbedded();

    /// Show or hide the "paused" hint. The embedded panel renders no frames
    /// while the debugger is stopped, which on its own reads as a crash; a small
    /// badge makes the stopped state legible instead.
    void setPaused(bool paused);

    /// Windows: adopt the display of the emulator running as `processId`. The
    /// window does not exist yet when a session starts, so this begins a poll
    /// that adopts it as soon as it does. A no-op on every other platform,
    /// where Hatari reparents itself and reports its size instead.
    void attachEmulatorProcess(qint64 processId);

    /// Forget the embedded display: the session ended, so whatever window was
    /// here is gone. Releases an adopted one first, and returns the panel to
    /// the empty state it paints for itself.
    void clearEmbedded();

signals:
    /// One line about the Windows adoption: what was adopted, at what size, and
    /// what happened when it could not be. The console is the only record of
    /// which branch ran on a machine this code cannot be tested on.
    void embedEvent(const QString &message);
    /// A key while this panel is showing an in-process frame and has focus.
    /// `sdlSym` is an SDL_Keycode, `sdlMod` is SDL_Keymod, `down` is a press.
    void hostKey(int sdlSym, int sdlMod, bool down);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *event) override;

private:
    /// One tick of the Windows adoption poll: adopt the emulator's window when
    /// it appears, notice when the emulator resizes or replaces the adopted one,
    /// and say on the panel when it never appears at all.
    /// The rect the last fit placed the adopted window at, in the container's
    /// client pixels. A child whose client size no longer matches it was resized
    /// by the emulator itself — a guest video-mode change — and that size is
    /// then the new video size.
    int m_lastFitW = 0;
    int m_lastFitH = 0;

    void pollForeignWindow();
    /// Translate one Qt key into the host-key signal. Auto-repeat is ignored:
    /// the ST keyboard generates its own repeat from a key that stays down.
    void forwardHostKey(class QKeyEvent *event);
    /// Release every key this panel still holds, so focus leaving does not
    /// leave a key down inside the ST.
    void releaseHeldKeys();

    class QTimer *m_settleTimer = nullptr;
    int m_settleTicks = 0;
    /// The emulator's native video size, for the aspect-preserved fit.
    int m_videoW = 0;
    int m_videoH = 0;
    /// Whether the debugger is stopped (drives the paused hint).
    bool m_paused = false;
    /// The adopted emulator window (Windows only), 0 when there is none.
    quintptr m_childWindow = 0;
    /// The process whose window is being looked for.
    qint64 m_foreignPid = -1;
    class QTimer *m_attachTimer = nullptr;
    int m_attachTicks = 0;
    /// Real video has landed here, on either platform. Until then the panel
    /// says what it is, rather than being an unexplained black rectangle.
    bool m_videoAttached = false;
    /// The last frame from the in-process core. Empty when the picture is a
    /// reparented window, or when the session has ended.
    QImage m_frame;
    /// The Windows adoption was attempted and did not happen.
    bool m_attachFailed = false;
    /// SDL keycodes currently held, so a focus loss can release them.
    QSet<int> m_heldSyms;
};

} // namespace pist
