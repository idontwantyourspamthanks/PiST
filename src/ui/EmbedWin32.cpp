// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmbedWin32.h"

#ifdef Q_OS_WIN
#  define NOMINMAX
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace pist {

#ifdef Q_OS_WIN

namespace {

HWND toHwnd(quintptr window)
{
    return reinterpret_cast<HWND>(window);
}

/// What EnumWindows walks: the process to match, and the best window seen so
/// far.
struct WindowSearch
{
    DWORD pid = 0;
    HWND best = nullptr;
    bool bestVisible = false;
    long long bestArea = -1;
};

BOOL CALLBACK considerWindow(HWND hwnd, LPARAM param)
{
    auto *search = reinterpret_cast<WindowSearch *>(param);

    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid != search->pid)
        return TRUE;
    // An owned window is a dialog or a popup, and a tool window is something
    // like a floating bar: neither is the video.
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;

    RECT rect{};
    if (!GetWindowRect(hwnd, &rect))
        return TRUE;
    const long long area = static_cast<long long>(rect.right - rect.left)
        * (rect.bottom - rect.top);
    const bool visible = IsWindowVisible(hwnd) != FALSE;

    // A visible window beats a hidden one, and of those the video window is the
    // largest thing the emulator's process owns.
    const bool better = search->best == nullptr
        || (visible && !search->bestVisible)
        || (visible == search->bestVisible && area > search->bestArea);
    if (!better)
        return TRUE;

    search->best = hwnd;
    search->bestVisible = visible;
    search->bestArea = area;
    return TRUE;
}

} // namespace

#endif // Q_OS_WIN

quintptr findEmulatorWindow(qint64 processId)
{
#ifdef Q_OS_WIN
    if (processId <= 0)
        return 0;
    WindowSearch search;
    search.pid = static_cast<DWORD>(processId);
    EnumWindows(considerWindow, reinterpret_cast<LPARAM>(&search));
    return reinterpret_cast<quintptr>(search.best);
#else
    Q_UNUSED(processId);
    return 0;
#endif
}

bool windowHandleValid(quintptr window)
{
#ifdef Q_OS_WIN
    return window != 0 && IsWindow(toHwnd(window)) != FALSE;
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool windowClientSize(quintptr window, int *width, int *height)
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(window) || !width || !height)
        return false;
    RECT client{};
    if (!GetClientRect(toHwnd(window), &client))
        return false;
    *width = static_cast<int>(client.right - client.left);
    *height = static_cast<int>(client.bottom - client.top);
    return *width > 0 && *height > 0;
#else
    Q_UNUSED(window);
    Q_UNUSED(width);
    Q_UNUSED(height);
    return false;
#endif
}

void moveEmbeddedChild(quintptr child, int x, int y, int width, int height)
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(child) || width <= 0 || height <= 0)
        return;
    HWND hwnd = toHwnd(child);

    // Already there? The container re-fits on a timer, and a redundant
    // MoveWindow repaints the video for nothing. GetWindowRect reports screen
    // coordinates, so the comparison goes through the parent's client space —
    // the space MoveWindow itself takes for a child window.
    RECT rect{};
    if (GetWindowRect(hwnd, &rect)) {
        const HWND parent = GetParent(hwnd);
        POINT origin{rect.left, rect.top};
        if (parent && ScreenToClient(parent, &origin) && origin.x == x && origin.y == y
            && rect.right - rect.left == width && rect.bottom - rect.top == height) {
            return;
        }
    }

    MoveWindow(hwnd, x, y, width, height, TRUE);
#else
    Q_UNUSED(child);
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);
#endif
}

bool embedForeignWindow(quintptr child, quintptr container)
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(child) || !windowHandleValid(container))
        return false;
    HWND hwnd = toHwnd(child);
    HWND parent = toHwnd(container);
    if (GetParent(hwnd) == parent)
        return true;

    // Hide it first: between Hatari showing its window and this call the window
    // sits on the desktop, and the shorter that flash the more the dock reads as
    // the emulator's own panel rather than a window that jumped.
    ShowWindow(hwnd, SW_HIDE);

    // The styles next, and the caption with them: MSDN's order for a window
    // coming off the desktop is WS_POPUP cleared and WS_CHILD set before
    // SetParent, and a title bar inside a dock is not embedding.
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      (style & ~static_cast<LONG_PTR>(WS_POPUP | WS_OVERLAPPEDWINDOW))
                          | WS_CHILD | WS_VISIBLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
                          & ~static_cast<LONG_PTR>(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE
                                                   | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE));

    if (!SetParent(hwnd, parent)) {
        // Leave the emulator the usable top-level window it was: a half-changed
        // style on a window we do not own is worse than no attempt at all.
        SetWindowLongPtrW(hwnd, GWL_STYLE, style);
        ShowWindow(hwnd, SW_SHOW);
        return false;
    }
    // FRAMECHANGED is what applies the new styles to a window already on
    // screen; without it the frame survives until the next resize.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    return true;
#else
    Q_UNUSED(child);
    Q_UNUSED(container);
    return false;
#endif
}

namespace {

#ifdef Q_OS_WIN
/// A key forwarded down and not yet up, indexed by scan code with the
/// extended-key bit as bit 8: left and right Shift share a virtual key, not a
/// scan code.
struct HeldKey
{
    bool down = false;
    bool sys = false;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
};
HeldKey sHeldKeys[0x200];

int heldKeyIndex(LPARAM lParam)
{
    return static_cast<int>((lParam >> 16) & 0x1ff);
}
#endif

} // namespace

bool forwardEmbeddedKey(quintptr child, void *message)
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(child) || !message)
        return false;
    const MSG *msg = static_cast<const MSG *>(message);
    const bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
    const bool up = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
    if (!down && !up)
        return false;
    // SDL reads the scan code and extended bit from lParam and the virtual key
    // from wParam, so the message goes over exactly as Windows produced it.
    PostMessageW(toHwnd(child), msg->message, msg->wParam, msg->lParam);
    HeldKey &held = sHeldKeys[heldKeyIndex(msg->lParam)];
    held.down = down;
    held.sys = msg->message == WM_SYSKEYDOWN;
    held.wParam = msg->wParam;
    held.lParam = msg->lParam;
    return true;
#else
    Q_UNUSED(child);
    Q_UNUSED(message);
    return false;
#endif
}

void releaseEmbeddedKeys(quintptr child)
{
#ifdef Q_OS_WIN
    const bool live = windowHandleValid(child);
    for (HeldKey &held : sHeldKeys) {
        if (!held.down)
            continue;
        held.down = false;
        if (!live)
            continue;
        // Repeat count 1, previous state down, transition up.
        const LPARAM upParam = (held.lParam & 0x01ff0000) | 1 | (LPARAM(1) << 30)
            | (LPARAM(1) << 31);
        PostMessageW(toHwnd(child), held.sys ? WM_SYSKEYUP : WM_KEYUP, held.wParam, upParam);
    }
#else
    Q_UNUSED(child);
#endif
}

bool isEmbeddedChildClick(quintptr child, void *message)
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(child) || !message)
        return false;
    const MSG *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_PARENTNOTIFY)
        return false;
    const UINT ev = LOWORD(msg->wParam);
    return ev == WM_LBUTTONDOWN || ev == WM_RBUTTONDOWN || ev == WM_MBUTTONDOWN;
#else
    Q_UNUSED(child);
    Q_UNUSED(message);
    return false;
#endif
}

void releaseForeignWindow(quintptr child)
{
#ifdef Q_OS_WIN
    if (!windowHandleValid(child))
        return;
    HWND hwnd = toHwnd(child);
    // The way back, in the order MSDN gives for it: off the parent first, then
    // WS_CHILD cleared and the top-level styles restored.
    SetParent(hwnd, nullptr);
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW) | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ShowWindow(hwnd, SW_SHOW);
#else
    Q_UNUSED(child);
#endif
}

} // namespace pist
