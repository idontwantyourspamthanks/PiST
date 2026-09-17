// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QColor>
#include <QCursor>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

class QWidget;

namespace pist {
namespace appearance {

/// Application-wide appearance preferences. These live in QSettings (per user),
/// never in the project file: a theme is a taste, not a project property.
///
///   appearance/theme       "dark" (default) | "light" | "system"
///   appearance/fontSize    editor point size; 0/absent = platform default + 1
///   appearance/fontFamily  monospace family; empty/absent = platform fixed font

enum class Icon {
    Open,
    Save,
    Settings,
    Build,
    Run,
    Stop,
    Pause,
    Continue,
    Step,
    StepOver,
    ClearBreakpoints,
    Brush,
    Line,
    Rect,
    RoundRect,
    Ellipse,
    Fill,
    Eyedropper,
    Undo,
    Redo,
    Grid,
    Fit,
    ZoomIn,
    ZoomOut,
    Palette,
    AddFrame,
    DuplicateFrame,
    RemoveFrame,
    FlipHorizontal,
    FlipVertical,
    Rotate,
    Onion,
    ShiftLeft,
    ShiftRight,
    ShiftUp,
    ShiftDown,
    Select,
    Copy,
    Cut,
    Paste,
};

/// Mouse glyph over the sprite canvas. The hotspot is the paint tip
/// (bristles, spout, dropper point), the selection frame's bottom-right
/// corner, or the crosshair centre.
enum class CanvasCursor {
    Brush,
    Crosshair,
    Fill,
    Eyedropper,
    Selection,
};

/// Colours that widgets paint with, as opposed to QPalette roles. Driven by
/// the effective darkness of the current theme (including "system").
struct Colors
{
    QColor gutter;
    QColor gutterText;
    QColor gutterPc;
    QColor breakpoint;
    QColor error;
    QColor executionLine;
    QColor currentLine;
    QColor pcRow;
    QColor changed;
    QColor address;
    QColor hex;
    QColor ascii;
    QColor zero;
    QColor success;
    QColor warning;
    QColor muted;
    QColor keyword;
    QColor registerName;
    QColor number;
    QColor string;
    QColor directive;
    QColor label;
    QColor comment;
};

QString theme();
int editorPointSize();
QString editorFontFamily();

/// Installed families suitable for the editor: a short preferred list of
/// programming fonts (when present), then any other fixed-pitch face.
QStringList editorFontChoices();

/// The monospace font implied by the family and size preferences. Used by the
/// editor and the debug panes so they stay in one typeface.
QFont editorFont();

/// Apply the configured theme application-wide. Safe to call again whenever
/// the preference changes. For "system" the style and palette captured at
/// startup are restored, which is why applyTheme() must first run before
/// anything else restyles the application (main.cpp does this).
void applyTheme();

/// The effective darkness of the current theme, resolving "system" by
/// inspecting the active palette. Drives editor and debug-pane colours.
bool darkModeActive();

Colors colors();

/// Toolbar glyph. For `Icon::Brush`, a valid opaque `paint` fills the brush head.
QIcon icon(Icon id, const QColor &paint = QColor());
QIcon windowIcon();
/// The Atari mark from docs/atari-2.svg. The menu heading is the Fuji peaks
/// in the theme ink colour; the About box uses the full mark including the
/// wordmark, in black on the GEM grey face.
QIcon atariLogoIcon();
QPixmap atariLogoPixmap(int logicalHeight, bool wordmark = true);
/// Canvas pointer. For `CanvasCursor::Brush`, `paint` fills the bristle well.
QCursor canvasCursor(CanvasCursor id, const QColor &paint = QColor());

/// Mark a widget as using the application monospace font, and apply it now.
/// applyMonoFonts() later walks a tree of these.
void markMono(QWidget *widget);
void applyMonoFonts(QWidget *root);

} // namespace appearance
} // namespace pist
