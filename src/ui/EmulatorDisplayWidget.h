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

public slots:
    /// Resize to the emulator's reported video size. Negative or zero sizes are
    /// ignored: they mean a transient state, not a real mode.
    void setEmulatorSize(int width, int height);
};

} // namespace pist
