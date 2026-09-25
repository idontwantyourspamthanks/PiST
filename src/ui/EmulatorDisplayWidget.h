// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QAbstractNativeEventFilter>
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
/// the mouse does too, because it goes to the window under the cursor. The
/// keyboard does not: the adopted window is never focused by the click, so
/// keystrokes stay in PiST. This panel takes Qt focus instead — on adoption
/// and when the adopted window is clicked — and while it has it, an
/// application native-event filter posts every keystroke on to Hatari's
/// window. The cross-process DPI reset remains something to confirm on a
/// Windows machine (docs/PLAN.md §9).
class EmulatorDisplayWidget : public QWidget, public QAbstractNativeEventFilter
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

    /// Windows: while this panel has focus and a window is adopted, send each
    /// keystroke to that window instead of to PiST.
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

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
    /// Pointer motion in ST pixels, and the buttons held (bit 0 left, bit 1
    /// right), while this panel is showing an in-process frame.
    void hostMouse(int dx, int dy, int buttons);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool event(QEvent *event) override;
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

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
    /// Scale one pointer sample into ST pixels and emit it. The first sample
    /// after entering the panel records the position and does not move.
    void forwardHostMouse(const QPoint &pos, Qt::MouseButtons buttons);
    /// Drop a button the host is no longer holding, and forget the pointer
    /// position so the next entry does not fling the ST cursor.
    void releaseHostMouse();

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
    /// A key has been forwarded to the adopted window since it was adopted.
    bool m_keyForwarded = false;
    /// SDL keycodes currently held, so a focus loss can release them.
    QSet<int> m_heldSyms;
    /// Pointer position in 256ths of a video pixel, and whether one exists.
    int m_pointerX = 0;
    int m_pointerY = 0;
    bool m_havePointer = false;
    /// Buttons last sent, so a focus loss can release them.
    int m_pointerButtons = 0;
};

} // namespace pist
