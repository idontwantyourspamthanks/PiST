// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once
#include <QImage>


#include <QtGlobal>

namespace pist {

/// Map every child of the given native window ID.
///
/// Hatari reparents its SDL window into the container but never maps it: the
/// window is created SDL_WINDOW_HIDDEN and Control_ReparentWindow does not map
/// it, so without this the embedded display stays black. The reparent happens
/// right before Hatari reports its video size, so this is called then. The SDL
/// window is the container's only child, so mapping every child is both safe
/// and sufficient.
///
/// X11 only; a no-op when PiST was built without X11 or is running off it.
void mapEmbeddedWindowChildren(quintptr windowId);

/// Resize the emulator's window to a given size, positioned at the container's
/// origin. The size comes from the widget's current Qt geometry, which is the
/// size the dock has actually allocated — fitting from the container's X11 window
/// instead would use whatever size that window happened to be created at, which
/// can be stale and larger than the dock, pushing the video out of view.
///
/// X11 only; a no-op when PiST was built without X11 or is running off it.
void resizeEmbeddedChild(quintptr windowId, int x, int y, int width, int height);

/// Capture the visible contents of a native window, including any reparented
/// foreign children (the embedded emulator's SDL window).
///
/// This bypasses QScreen::grabWindow, which returns a black image for a
/// top-level window under XWayland on this setup — the same reason the remote
/// `screenshot` command cannot use it. XGetImage is used instead, which is what
/// external capture tools do and which reads the real frame buffer.
///
/// X11 only; returns a null image when PiST was built without X11 or is running
/// off it.
QImage captureWindowImage(quintptr windowId);

} // namespace pist
