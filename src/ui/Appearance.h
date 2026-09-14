// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

class QPalette;

namespace pist {
namespace appearance {

/// The application-wide appearance preferences. These live in QSettings (per
/// user), never in the project file: a theme is a taste, not a project
/// property.
///
///   appearance/theme     "system" (default) | "light" | "dark"
///   appearance/fontSize  editor point size; 0/absent = the platform default

QString theme();
int editorPointSize();

/// Apply the configured theme application-wide. Safe to call again whenever
/// the preference changes. For "system" the style and palette captured at
/// startup are restored, which is why applyTheme() must first run before
/// anything else restyles the application (main.cpp does this).
void applyTheme();

/// The effective darkness of the current theme, resolving "system" by
/// inspecting the active palette. Drives the editor's syntax colours.
bool darkModeActive();

} // namespace appearance
} // namespace pist
