// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/Appearance.h"

#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

namespace pist {
namespace appearance {

namespace {

/// The classic "Fusion dark" palette: the only dark scheme available on every
/// platform without depending on the host's own style plugins.
QPalette darkPalette()
{
    const QColor window(0x35, 0x35, 0x35);
    const QColor base(0x25, 0x25, 0x25);
    const QColor text(0xe8, 0xe8, 0xe8);
    const QColor disabled(0x7f, 0x7f, 0x7f);
    const QColor highlight(0x2a, 0x82, 0xda);

    QPalette p;
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, window);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Disabled, QPalette::Text, disabled);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    p.setColor(QPalette::BrightText, Qt::red);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::black);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    p.setColor(QPalette::PlaceholderText, disabled);
    return p;
}

} // namespace

QString theme()
{
    return QSettings().value(QStringLiteral("appearance/theme"),
                             QStringLiteral("system"))
        .toString();
}

int editorPointSize()
{
    return QSettings().value(QStringLiteral("appearance/fontSize"), 0).toInt();
}

void applyTheme()
{
    // Captured on first use, which main.cpp arranges to be before any
    // restyling, so "system" can restore what the platform gave us.
    static const QString initialStyle = QApplication::style()->objectName();
    static const QPalette initialPalette = QApplication::palette();

    const QString t = theme();
    if (t == QLatin1String("dark")) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("fusion")));
        QApplication::setPalette(darkPalette());
    } else if (t == QLatin1String("light")) {
        QApplication::setStyle(QStyleFactory::create(QStringLiteral("fusion")));
        QApplication::setPalette(QApplication::style()->standardPalette());
    } else {
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

} // namespace appearance
} // namespace pist
