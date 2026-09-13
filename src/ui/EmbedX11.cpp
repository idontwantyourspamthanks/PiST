// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmbedX11.h"

#include <QGuiApplication>

#if defined(PIST_HAVE_X11)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif

namespace pist {

void mapEmbeddedWindowChildren(quintptr windowId)
{
#if defined(PIST_HAVE_X11)
    // The display connection comes from Qt's xcb platform, so this is valid
    // exactly when PiST is an X11 client — which is the same condition under
    // which embedding is offered at all.
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11)
        return;
    Display *display = x11->display();
    if (!display)
        return;

    Window root = 0, parent = 0, *children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(display, static_cast<Window>(windowId), &root, &parent,
                   &children, &count)
        && children) {
        for (unsigned int i = 0; i < count; ++i)
            XMapWindow(display, children[i]);
        XFree(children);
    }
    XFlush(display);
#else
    Q_UNUSED(windowId);
#endif
}


QImage captureWindowImage(quintptr windowId)
{
#if defined(PIST_HAVE_X11)
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11)
        return {};
    Display *display = x11->display();
    if (!display)
        return {};

    const Window window = static_cast<Window>(windowId);
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, window, &attrs)
        || attrs.width <= 0 || attrs.height <= 0)
        return {};

    XImage *image = XGetImage(display, window, 0, 0,
                              static_cast<unsigned>(attrs.width),
                              static_cast<unsigned>(attrs.height),
                              AllPlanes, ZPixmap);
    if (!image)
        return {};

    QImage result;
    // A 32-bit ZPixmap on a little-endian server is B,G,R,X in memory, which is
    // what QImage::Format_RGB32 (0xffRRGGBB) reads back. .copy() detaches before
    // the XImage's buffer is freed.
    if (image->bits_per_pixel == 32)
        result = QImage(reinterpret_cast<const uchar *>(image->data),
                        image->width, image->height, image->bytes_per_line,
                        QImage::Format_RGB32).copy();
    XDestroyImage(image);
    return result;
#else
    Q_UNUSED(windowId);
    return {};
#endif
}




void resizeEmbeddedChild(quintptr windowId, int x, int y, int width, int height)
{
#if defined(PIST_HAVE_X11)
    if (width <= 0 || height <= 0)
        return;
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11)
        return;
    Display *display = x11->display();
    if (!display)
        return;

    Window root = 0, parent = 0, *children = nullptr;
    unsigned int count = 0;
    if (XQueryTree(display, static_cast<Window>(windowId), &root, &parent,
                   &children, &count)
        && children) {
        for (unsigned int i = 0; i < count; ++i)
            XMoveResizeWindow(display, children[i], x, y,
                              static_cast<unsigned>(width),
                              static_cast<unsigned>(height));
        XFree(children);
    }
    XFlush(display);
#else
    Q_UNUSED(windowId);
    Q_UNUSED(x);
    Q_UNUSED(y);
    Q_UNUSED(width);
    Q_UNUSED(height);
#endif
}


bool embeddedContainerSize(quintptr windowId, int *width, int *height)
{
#if defined(PIST_HAVE_X11)
    auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11)
        return false;
    Display *display = x11->display();
    if (!display)
        return false;

    XWindowAttributes attrs;
    if (!XGetWindowAttributes(display, static_cast<Window>(windowId), &attrs)
        || attrs.width <= 0 || attrs.height <= 0)
        return false;
    *width = attrs.width;
    *height = attrs.height;
    return true;
#else
    Q_UNUSED(windowId);
    Q_UNUSED(width);
    Q_UNUSED(height);
    return false;
#endif
}

} // namespace pist
