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

/// Resize the emulator's window to fill the container's ACTUAL current size, read
/// from the X server rather than from the widget. This matters because the
/// container is a native window in a dock, and its Qt size() can lag the size the
/// dock has given the underlying X11 window — so fitting from size() sometimes
/// uses a stale, too-small value and the video ends up with dead space around it.
///
/// X11 only; a no-op when PiST was built without X11 or is running off it.
void fitEmbeddedWindowToContainer(quintptr windowId);

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
