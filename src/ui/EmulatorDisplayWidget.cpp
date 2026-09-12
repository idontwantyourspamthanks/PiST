// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmulatorDisplayWidget.h"

#include <QPalette>

namespace pist {

namespace {
// A reasonable surface to show before the emulator reports its real size: the
// ST's low-resolution screen, doubled, which is also roughly what Hatari opens.
constexpr int kDefaultWidth = 640;
constexpr int kDefaultHeight = 400;
} // namespace

EmulatorDisplayWidget::EmulatorDisplayWidget(QWidget *parent)
    : QWidget(parent)
{
    // Native, so winId() is a real X11 window ID rather than an alias for the
    // top-level window's: the reparenting needs a window of our own to attach
    // to. WA_DontCreateNativeAncestors keeps the dock and main window ordinary.
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);

    // Black behind the video: the ST border colour is not always black, and an
    // unpainted frame reads as a rendering bug.
    setAutoFillBackground(true);
    QPalette p = palette();
    p.setColor(QPalette::Window, Qt::black);
    setPalette(p);

    setMinimumSize(kDefaultWidth, kDefaultHeight);
}

void EmulatorDisplayWidget::setEmulatorSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    // Fixed rather than minimum: the reparented SDL window keeps its own size
    // and sits at our origin, so matching it exactly is what avoids clipping
    // (too small) or dead space (too large).
    setFixedSize(width, height);
}

} // namespace pist
