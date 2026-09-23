// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/Appearance.h"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QSet>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

namespace pist {
namespace appearance {

namespace {

/// The twelve palette roles both themes fill. Every one is a QColor, so a bare
/// argument list cannot tell a transposition from a theme; naming each role
/// where it is chosen is what makes a swap a compile error instead of a wrong
/// theme shipped.
struct PaletteScheme
{
    QColor window;
    QColor base;
    QColor alternate;
    QColor text;
    QColor disabled;
    QColor button;
    QColor highlight;
    QColor highlightedText;
    QColor mid;
    QColor light;
    QColor dark;
    QColor link;
};

/// GEM-green accent: the ST's desktop colour, tuned so it still works as a
/// selection highlight rather than a CRT phosphor.
void fillSharedRoles(QPalette &p, const PaletteScheme &scheme)
{
    p.setColor(QPalette::Window, scheme.window);
    p.setColor(QPalette::WindowText, scheme.text);
    p.setColor(QPalette::Disabled, QPalette::WindowText, scheme.disabled);
    p.setColor(QPalette::Base, scheme.base);
    p.setColor(QPalette::AlternateBase, scheme.alternate);
    p.setColor(QPalette::ToolTipBase, scheme.button);
    p.setColor(QPalette::ToolTipText, scheme.text);
    p.setColor(QPalette::Text, scheme.text);
    p.setColor(QPalette::Disabled, QPalette::Text, scheme.disabled);
    p.setColor(QPalette::Button, scheme.button);
    p.setColor(QPalette::ButtonText, scheme.text);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, scheme.disabled);
    p.setColor(QPalette::BrightText, QColor(0xe8, 0x5a, 0x4a));
    p.setColor(QPalette::Link, scheme.link);
    p.setColor(QPalette::Highlight, scheme.highlight);
    p.setColor(QPalette::HighlightedText, scheme.highlightedText);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, scheme.disabled);
    p.setColor(QPalette::PlaceholderText, scheme.disabled);
    p.setColor(QPalette::Light, scheme.light);
    p.setColor(QPalette::Midlight, scheme.button);
    p.setColor(QPalette::Mid, scheme.mid);
    p.setColor(QPalette::Dark, scheme.dark);
    p.setColor(QPalette::Shadow, scheme.dark);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    p.setColor(QPalette::Accent, scheme.highlight);
#endif
}

QPalette darkPalette()
{
    PaletteScheme scheme;
    scheme.window = QColor(0x1a, 0x1d, 0x1a);
    scheme.base = QColor(0x10, 0x13, 0x10); // the editor
    scheme.alternate = QColor(0x22, 0x27, 0x22);
    scheme.text = QColor(0xe6, 0xea, 0xe4);
    scheme.disabled = QColor(0x7a, 0x82, 0x7a);
    scheme.button = QColor(0x24, 0x28, 0x24);
    scheme.highlight = QColor(0x2f, 0xa0, 0x4c); // GEM green
    scheme.highlightedText = QColor(0x07, 0x14, 0x0a);
    scheme.mid = QColor(0x3a, 0x42, 0x3a);
    scheme.light = QColor(0x40, 0x48, 0x40);
    scheme.dark = QColor(0x0c, 0x0e, 0x0c);
    scheme.link = QColor(0x5e, 0xd4, 0x74);

    QPalette p;
    fillSharedRoles(p, scheme);
    return p;
}

QPalette lightPalette()
{
    PaletteScheme scheme;
    scheme.window = QColor(0xf2, 0xf4, 0xf0);
    scheme.base = QColor(0xfc, 0xfd, 0xfb);
    scheme.alternate = QColor(0xe8, 0xec, 0xe6);
    scheme.text = QColor(0x1a, 0x1e, 0x1a);
    scheme.disabled = QColor(0x86, 0x8c, 0x86);
    scheme.button = QColor(0xe6, 0xea, 0xe4);
    scheme.highlight = QColor(0x1e, 0x7a, 0x38);
    scheme.highlightedText = QColor(0xff, 0xff, 0xff);
    scheme.mid = QColor(0xc4, 0xcc, 0xc2);
    scheme.light = QColor(0xff, 0xff, 0xff);
    scheme.dark = QColor(0x8a, 0x92, 0x88);
    scheme.link = QColor(0x1a, 0x6e, 0x32);

    QPalette p;
    fillSharedRoles(p, scheme);
    return p;
}

/// Chrome only: uses palette roles so the same sheet covers dark and light.
/// Not applied for "system", which must keep the platform's own look.
QString chromeStyleSheet()
{
    return QStringLiteral(R"(
QToolBar {
    background: palette(window);
    border: none;
    spacing: 4px;
    padding: 2px 8px;
    border-bottom: 1px solid palette(mid);
}
QToolBar::separator {
    width: 1px;
    margin: 6px 6px;
    background: palette(mid);
}
QStatusBar {
    background: palette(window);
    border-top: 1px solid palette(mid);
}
QMenuBar {
    background: palette(window);
    border-bottom: 1px solid palette(mid);
    spacing: 2px;
}
QMenuBar::item {
    padding: 4px 8px;
    background: transparent;
}
QMenuBar::item:selected {
    background: palette(highlight);
    color: palette(highlighted-text);
}
QDockWidget {
    border: 1px solid palette(mid);
}
QDockWidget::title {
    background: palette(button);
    padding: 5px 8px;
    text-align: left;
}
QTabWidget::pane {
    border: none;
    background: palette(base);
}
QTabBar::tab {
    background: transparent;
    color: palette(window-text);
    padding: 5px 12px;
    border: none;
    margin-right: 1px;
}
QTabBar::tab:selected {
    color: palette(window-text);
    border-bottom: 2px solid palette(highlight);
}
QTabBar::tab:hover:!selected {
    background: palette(button);
}
QHeaderView::section {
    background: palette(window);
    color: palette(window-text);
    padding: 4px 8px;
    border: none;
    border-bottom: 1px solid palette(mid);
    font-weight: 600;
}
QTableView, QTreeView, QListView, QPlainTextEdit {
    background: palette(base);
    color: palette(text);
    border: none;
    selection-background-color: palette(highlight);
    selection-color: palette(highlighted-text);
    alternate-background-color: palette(alternate-base);
}
QLineEdit, QSpinBox, QComboBox, QFontComboBox {
    background: palette(base);
    color: palette(text);
    border: 1px solid palette(mid);
    border-radius: 3px;
    padding: 3px 6px;
    selection-background-color: palette(highlight);
    selection-color: palette(highlighted-text);
}
QComboBox QAbstractItemView {
    background: palette(base);
    color: palette(text);
    selection-background-color: palette(highlight);
    selection-color: palette(highlighted-text);
}
QPushButton {
    background: palette(button);
    color: palette(button-text);
    border: 1px solid palette(mid);
    border-radius: 3px;
    padding: 4px 12px;
}
QPushButton:hover {
    border-color: palette(highlight);
}
QPushButton:default {
    border: 1px solid palette(highlight);
}
QPushButton:disabled {
    color: palette(mid);
}
QToolButton {
    background: transparent;
    border: none;
    border-radius: 3px;
    padding: 4px;
}
QToolButton:hover {
    background: palette(button);
}
QToolButton:pressed {
    background: palette(mid);
}
QMainWindow::separator {
    background: palette(mid);
    width: 4px;
    height: 4px;
}
QScrollBar:vertical {
    background: palette(window);
    width: 10px;
    margin: 0;
    border: none;
}
QScrollBar::handle:vertical {
    background: palette(mid);
    min-height: 24px;
    border-radius: 4px;
}
QScrollBar:horizontal {
    background: palette(window);
    height: 10px;
    margin: 0;
    border: none;
}
QScrollBar::handle:horizontal {
    background: palette(mid);
    min-width: 24px;
    border-radius: 4px;
}
QScrollBar::add-line, QScrollBar::sub-line {
    width: 0;
    height: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: none;
}
QGroupBox {
    border: 1px solid palette(mid);
    border-radius: 4px;
    margin-top: 1.2em;
    padding: 8px;
}
    QGroupBox::title {
    subcontrol-origin: margin;
    left: 10px;
    padding: 0 4px;
}
)");
}

/// The palette colors() serves from. colors() runs per table cell, per row, per
/// keystroke and per paint frame, and every call used to build a QSettings (which
/// re-reads the settings file: ~13 µs measured on this machine) inside
/// darkModeActive, then rebuild 25 QColors, for an answer that cannot change in
/// between. Two things can change it, and neither is looked up per call:
///
///  - the stored theme, whose only entry point is applyTheme() — that is where
///    this cache is dropped;
///  - the application palette, which "system" resolves its darkness from and
///    which QApplication can restyle without applyTheme() being involved (a
///    desktop theme change arrives that way). QPalette::cacheKey() identifies
///    its state in an O(1) compare, so that case stays live.
struct PaletteCache
{
    bool valid = false;
    qint64 paletteKey = 0;
    Colors palette;
};

PaletteCache &paletteCache()
{
    static PaletteCache cache;
    return cache;
}

} // namespace

// One accessor per persisted key, beside the reader that consumes it. The
// writer in SettingsDialog reads the same key through these, so the two halves
// of a preference cannot drift — a typo on one side used to reset the setting to
// its default with nothing to point at (MIN-53). Same shape as
// SetupDialog::dismissalKey() and ProjectSettings' accessors.
const QString &themeKey()
{
    static const QString key = QStringLiteral("appearance/theme");
    return key;
}

const QString &shortcutSchemeKey()
{
    static const QString key = QStringLiteral("appearance/shortcuts");
    return key;
}

const QString &fontSizeKey()
{
    static const QString key = QStringLiteral("appearance/fontSize");
    return key;
}

const QString &fontFamilyKey()
{
    static const QString key = QStringLiteral("appearance/fontFamily");
    return key;
}

QString theme()
{
    return QSettings().value(themeKey(), QStringLiteral("dark")).toString();
}

QString shortcutScheme()
{
    const QString scheme = QSettings().value(shortcutSchemeKey(), QStringLiteral("pist"))
                               .toString();
    return scheme == QLatin1String("common") ? scheme : QStringLiteral("pist");
}

int editorPointSize()
{
    return QSettings().value(fontSizeKey(), 0).toInt();
}

QString editorFontFamily()
{
    return QSettings().value(fontFamilyKey()).toString();
}

QStringList editorFontChoices()
{
    const QStringList installed = QFontDatabase::families();
    auto matchInstalled = [&](const QString &name) {
        for (const QString &family : installed) {
            if (family.compare(name, Qt::CaseInsensitive) == 0)
                return family;
        }
        return QString();
    };

    // Tried first, and included even when Qt does not report them as
    // fixed-pitch (it often misses faces that are clearly mono).
    static const char *const preferred[] = {
        "JetBrains Mono",
        "Cascadia Code",
        "Cascadia Mono",
        "Fira Code",
        "Fira Mono",
        "Source Code Pro",
        "IBM Plex Mono",
        "Hack",
        "Iosevka",
        "Inconsolata",
        "Ubuntu Mono",
        "DejaVu Sans Mono",
        "Noto Sans Mono",
        "Liberation Mono",
        "FreeMono",
        "Cousine",
        "Consolas",
        "Menlo",
        "Monaco",
        "Courier New",
        nullptr
    };

    QStringList out;
    QSet<QString> seen;
    for (int i = 0; preferred[i]; ++i) {
        const QString got = matchInstalled(QLatin1String(preferred[i]));
        if (got.isEmpty() || seen.contains(got.toLower()))
            continue;
        seen.insert(got.toLower());
        out.append(got);
    }
    for (const QString &generic : {QStringLiteral("Monospace"), QStringLiteral("Courier")}) {
        if (seen.contains(generic.toLower()))
            continue;
        seen.insert(generic.toLower());
        out.append(generic);
    }
    for (const QString &family : installed) {
        if (!QFontDatabase::isFixedPitch(family) || seen.contains(family.toLower()))
            continue;
        seen.insert(family.toLower());
        out.append(family);
    }
    return out;
}

QFont editorFont()
{
    QFont font;
    const QString family = editorFontFamily();
    if (!family.isEmpty()) {
        font = QFont(family);
        font.setStyleHint(QFont::TypeWriter);
        font.setFixedPitch(true);
    } else {
        font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    const int size = editorPointSize();
    font.setPointSize(size > 0 ? size : font.pointSize() + 1);
    return font;
}

void applyTheme()
{
    // Captured on first use, which main.cpp arranges to be before any
    // restyling, so "system" can restore what the platform gave us.
    static const QString initialStyle = QApplication::style()->objectName();
    static const QPalette initialPalette = QApplication::palette();

    // Whatever the widgets paint with is derived from the theme set here, so the
    // cache colors() serves from is dropped with it.
    paletteCache().valid = false;

    const QString t = theme();
    if (t == QLatin1String("dark")) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("fusion")));
        QApplication::setPalette(darkPalette());
        qApp->setStyleSheet(chromeStyleSheet());
    } else if (t == QLatin1String("light")) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("fusion")));
        QApplication::setPalette(lightPalette());
        qApp->setStyleSheet(chromeStyleSheet());
    } else {
        qApp->setStyleSheet(QString());
        QApplication::setStyle(QStyleFactory::create(initialStyle));
        QApplication::setPalette(initialPalette);
    }
}

bool darkModeActive()
{
    const QString t = theme();
    if (t == QLatin1String("dark"))
        return true;
    if (t == QLatin1String("light"))
        return false;
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}

namespace {

/// The colours for an effective darkness. Literals only — nothing here reads a
/// setting — so darkness is the cache's whole key.
Colors buildColors(bool dark)
{
    Colors c;
    if (dark) {
        c.gutter = QColor(0x16, 0x19, 0x16);
        // Quiet text has to clear 4.5:1 on the surface it is painted on.
        // Comments and gutter numerals share this ink; zero bytes stay a
        // step dimmer and still clear the editor base.
        c.gutterText = QColor(0x84, 0x8c, 0x82);
        c.gutterPc = QColor(0x2f, 0xa0, 0x4c);
        c.breakpoint = QColor(0xe0, 0x5a, 0x4a);
        c.error = QColor(0xe0, 0x5a, 0x4a);
        c.executionLine = QColor(0x1e, 0x3a, 0x24);
        c.currentLine = QColor(0x1a, 0x22, 0x1c);
        c.searchMatch = QColor(0x4a, 0x3e, 0x18);
        c.pcRow = QColor(0x1e, 0x3a, 0x24);
        c.changed = QColor(0x7e, 0xe0, 0x8a);
        c.address = QColor(0x8a, 0x96, 0x88);
        c.hex = QColor(0xe6, 0xea, 0xe4);
        c.ascii = QColor(0xb0, 0xc4, 0xa8);
        c.zero = QColor(0x7a, 0x82, 0x78);
        c.success = QColor(0x5e, 0xd4, 0x74);
        c.warning = QColor(0xe0, 0xa0, 0x40);
        c.muted = QColor(0x84, 0x8c, 0x82);
        c.keyword = QColor(0x8e, 0xe0, 0x9a);
        c.registerName = QColor(0xc8, 0xb4, 0x5a);
        c.number = QColor(0xe0, 0xb8, 0x4a);
        c.string = QColor(0xe8, 0x78, 0x58);
        c.directive = QColor(0x7e, 0xc0, 0xd8);
        c.label = QColor(0xe8, 0xe6, 0xd8);
        c.comment = QColor(0x84, 0x8c, 0x82);
    } else {
        c.gutter = QColor(0xee, 0xf1, 0xec);
        c.gutterText = QColor(0x6a, 0x72, 0x6a);
        c.gutterPc = QColor(0x1e, 0x7a, 0x38);
        c.breakpoint = QColor(0xc0, 0x38, 0x28);
        c.error = QColor(0xc0, 0x38, 0x28);
        c.executionLine = QColor(0xd4, 0xed, 0xd8);
        c.currentLine = QColor(0xee, 0xf4, 0xee);
        c.searchMatch = QColor(0xff, 0xf0, 0xa8);
        c.pcRow = QColor(0xd4, 0xed, 0xd8);
        c.changed = QColor(0x14, 0x6a, 0x2a);
        c.address = QColor(0x5a, 0x66, 0x58);
        c.hex = QColor(0x1a, 0x1e, 0x1a);
        c.ascii = QColor(0x2a, 0x5a, 0x32);
        c.zero = QColor(0xa0, 0xa8, 0xa0);
        c.success = QColor(0x1a, 0x7a, 0x32);
        c.warning = QColor(0xa0, 0x60, 0x00);
        c.muted = QColor(0x86, 0x8c, 0x86);
        c.keyword = QColor(0x1a, 0x7a, 0x32);
        c.registerName = QColor(0x7a, 0x5a, 0x10);
        c.number = QColor(0x8a, 0x5a, 0x00);
        c.string = QColor(0xb0, 0x38, 0x20);
        c.directive = QColor(0x18, 0x60, 0x80);
        c.label = QColor(0x1a, 0x3a, 0x20);
        c.comment = QColor(0x6a, 0x70, 0x68);
    }
    return c;
}

} // namespace

Colors colors()
{
    PaletteCache &cache = paletteCache();
    const qint64 key = QApplication::palette().cacheKey();
    if (!cache.valid || cache.paletteKey != key) {
        cache.palette = buildColors(darkModeActive());
        cache.paletteKey = key;
        cache.valid = true;
    }
    return cache.palette;
}

EditorTheme editorTheme()
{
    const Colors c = colors();
    EditorTheme theme;
    theme.font = editorFont();
    theme.dark = darkModeActive();

    theme.keyword = c.keyword;
    theme.registerName = c.registerName;
    theme.number = c.number;
    theme.string = c.string;
    theme.directive = c.directive;
    theme.label = c.label;
    theme.comment = c.comment;

    theme.currentLine = c.currentLine;
    theme.executionLine = c.executionLine;
    theme.searchMatch = c.searchMatch;
    theme.error = c.error;
    theme.gutter = c.gutter;
    theme.gutterText = c.gutterText;
    theme.gutterPc = c.gutterPc;
    theme.breakpoint = c.breakpoint;
    theme.muted = c.muted;
    theme.warning = c.warning;
    return theme;
}

void markMono(QWidget *widget)
{
    if (!widget)
        return;
    widget->setProperty("pistMono", true);
    widget->setFont(editorFont());
}

void applyMonoFonts(QWidget *root)
{
    if (!root)
        return;
    const QFont font = editorFont();
    const auto apply = [&](QWidget *w) {
        if (w && w->property("pistMono").toBool())
            w->setFont(font);
    };
    apply(root);
    const auto widgets = root->findChildren<QWidget *>();
    for (QWidget *w : widgets)
        apply(w);
}

} // namespace appearance
} // namespace pist
