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

/// The container's real, on-screen size. The widget's Qt geometry and its native
/// X11 window can disagree (Qt sometimes reports a stale, smaller size for a
/// native dock widget), and fitting against the wrong one is what left the video
/// small and top-left in a large dock. The X11 window's own attributes are the
/// ground truth for where the video actually displays.
///
/// X11 only; returns false when PiST was built without X11 or is running off it.
bool embeddedContainerSize(quintptr windowId, int *width, int *height);

/// Resize/position the emulator's window within the container.
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
