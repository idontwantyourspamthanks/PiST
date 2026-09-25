// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QtGlobal>

namespace pist {

/// Embedding the emulator's display on Windows.
///
/// There is no handshake to ask for. Hatari's `PARENT_WIN_ID` reparenting is
/// compiled in only under `HAVE_X11 && SDL_VIDEO_DRIVER_X11` (upstream
/// `src/sdl/screen.c`, `Screen_ReparentWindow`), so on Windows the emulator
/// always creates its own visible top-level window. What Windows does allow is
/// moving a window that belongs to another process into one of ours:
/// `SetParent`, with `WS_POPUP` cleared and `WS_CHILD` set *before* the call,
/// which is the order MSDN requires of a window coming off the desktop. PiST
/// finds the emulator's window by process id — nothing else identifies it —
/// and then keeps it fitted to the container.
///
/// Naming the container in `PARENT_WIN_ID` anyway would be actively harmful:
/// the flag check that creates the SDL window `SDL_WINDOW_BORDERLESS |
/// SDL_WINDOW_HIDDEN` is *not* inside that X11 guard, and nothing in a Windows
/// build ever maps the window again. An attach that failed would then leave the
/// user with no emulator on screen at all. Attaching to the window Hatari
/// already showed means every failure path here ends at the detached window
/// that works today.
///
/// Mouse arrives on its own: a click hits whichever window is under the cursor,
/// and that is Hatari's. Keyboard does not. Keystrokes go to the focused
/// window, and a cross-process child does not become focused when it is
/// clicked. Joining the two threads' input queues to focus it did not deliver
/// keys either, and a joined queue stalls PiST's input whenever Hatari stops
/// pumping messages at a debugger stop. So PiST keeps the keyboard, and
/// `forwardEmbeddedKey` posts each keystroke to Hatari's window as Windows
/// produced it. SDL builds its key event from that message alone and does not
/// require keyboard focus to send it.
///
/// Windows only. Every entry point is a no-op (or false, or 0) elsewhere, so
/// callers need no platform guards of their own.
///
/// One caveat the platform imposes rather than this code: a cross-process
/// `SetParent` force-resets the *child's* process DPI awareness when the two
/// disagree (MSDN), so adopting can change how Hatari scales its rendering
/// relative to the physical-pixel fit. Both sides being per-monitor-v2 (Qt 6,
/// and SDL2 in the bundled fork) should keep them agreeing; it is recorded in
/// docs/PLAN.md §9 as a thing to confirm on a real Windows machine.

/// The emulator's top-level window: the one belonging to `processId` that a
/// user would recognise as its video, or 0 when the process has none (yet).
/// The window appears some time after the process starts, so callers poll.
quintptr findEmulatorWindow(qint64 processId);

/// True while `window` is a live window handle. A reparented foreign window
/// dies with its own process — the emulator exiting, or Hatari destroying and
/// recreating the window for a guest resolution change — and this is how the
/// container notices.
bool windowHandleValid(quintptr window);

/// Make `child`, a foreign process's top-level window, a borderless child of
/// `container` filling it. Returns false — leaving `child` exactly as it was —
/// when either handle is dead or the reparent fails.
bool embedForeignWindow(quintptr child, quintptr container);

/// Undo embedForeignWindow(): `child` becomes an ordinary top-level window
/// again. Called before the container goes away, because destroying a window
/// destroys its children, foreign ones included.
void releaseForeignWindow(quintptr child);

/// If `message` (a Windows MSG) is a key press or release, post it unchanged to
/// `child` and return true, so the caller can swallow it. The keys it sent down
/// are remembered for `releaseEmbeddedKeys`.
bool forwardEmbeddedKey(quintptr child, void *message);

/// Post a release to `child` for every key `forwardEmbeddedKey` sent down and
/// not up, so focus leaving the panel does not leave a key held in the ST.
/// With a dead `child` the record is just cleared.
void releaseEmbeddedKeys(quintptr child);

/// True when `message` is the container's notification that `child` was
/// clicked.
bool isEmbeddedChildClick(quintptr child, void *message);

/// A window's client size, in physical pixels. Read on the emulator's window
/// before it is embedded this is the video size Hatari chose, which is what the
/// aspect-preserved fit needs: Windows gets no control-socket size report, that
/// being X11-only as well.
bool windowClientSize(quintptr window, int *width, int *height);

/// Move and resize the embedded child inside its container, in the container's
/// client coordinates. Does nothing when the window is already there, so a
/// caller that re-fits on a timer does not repaint the video for nothing.
void moveEmbeddedChild(quintptr child, int x, int y, int width, int height);

} // namespace pist
