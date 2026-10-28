// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// The source a project file implies: `<project>.pistproject` sits beside
/// `<project>.s`. The path is deliberately not stored in the file, so every
/// doorway that can be handed a project derives it the same way.
QString sourceForProject(const QString &projectPath);

/// QSettings keys read by more than one MainWindow translation unit.
///
/// One spelling per key (MIN-53): the layout keys are read by the chrome and by
/// closeEvent, and the blame switch by the actions and the git pass. A key that
/// lives in a single cluster belongs in that cluster's file instead.
inline const QString &gitBlameKey()
{
    static const QString key = QStringLiteral("git/blame");
    return key;
}

inline const QString &layoutStateKey()
{
    static const QString key = QStringLiteral("layout/state");
    return key;
}

inline const QString &layoutGeometryKey()
{
    static const QString key = QStringLiteral("layout/geometry");
    return key;
}

inline const QString &layoutWidthKey()
{
    static const QString key = QStringLiteral("layout/width");
    return key;
}

inline const QString &layoutHeightKey()
{
    static const QString key = QStringLiteral("layout/height");
    return key;
}

/// The layout stashed for the duration of a preset (see applyLayoutPreset).
inline const QString &layoutPreviousKey()
{
    static const QString key = QStringLiteral("layout/previous");
    return key;
}

} // namespace pist
