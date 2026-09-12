// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "emu/Machine.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// Settings for one project: what to build, how, and how to run it.
///
/// Kept as a plain value type so it can be persisted, copied and compared
/// without dragging in widgets. Emulator settings are expressed as command-line
/// arguments when a session starts; PiST never writes or mutates a user's
/// `hatari.cfg` (docs/PLAN.md §1, §5 rule 7).
struct ProjectSettings
{
    // --- build -----------------------------------------------------------

    /// The assembly source being built.
    QString sourceFile;

    /// `-I` search paths, in order.
    QStringList includePaths;

    /// `-D` defines, as `NAME` or `NAME=value`.
    QStringList defines;

    /// Target CPU as vasm's `-m` suffix, e.g. "68000". Empty leaves vasm's default.
    QString cpu = QStringLiteral("68000");

    /// Extra arguments appended verbatim, for anything not yet modelled.
    QStringList extraBuildArgs;

    // --- emulator --------------------------------------------------------

    Machine machine = Machine::St;

    /// TOS ROM image. When empty, the best ROM for `machine` is chosen from the
    /// standard locations at launch time.
    QString tosPath;

    /// Monitor: mono / rgb / vga / tv.
    QString monitor = QStringLiteral("mono");

    /// ST RAM in MiB (1..14). 0 leaves Hatari's 512 KiB default.
    int memSizeMiB = 1;

    QString hardDiskImage;
    QStringList floppyImages;

    bool fastForward = true;

    /// Extra arguments appended to the emulator command line.
    QStringList extraEmulatorArgs;

    // --- discovery -------------------------------------------------------

    /// Directories actually searched for ROMs, for diagnostics.
    static QStringList romSearchPaths();

    /// Every ROM found, in priority order.
    static QList<struct TosRom> availableRoms();
};

/// Persist and restore a project's settings.
///
/// Two layers, deliberately:
///
///   - a **project file** (`<name>.pistproject`, JSON) beside the source, holding
///     everything above, so a project is portable and reviewable;
///   - **application state** for the most recently used project paths, so the
///     IDE reopens where the user left off and a brand-new session still has
///     sensible defaults.
///
/// Neither touches Hatari's own configuration.
namespace settings {

inline constexpr const char *kProjectSuffix = ".pistproject";

/// Default path of the project file for a source file.
QString projectFileFor(const QString &sourcePath);

bool save(const ProjectSettings &settings, const QString &path, QString *error);
bool load(ProjectSettings *settings, const QString &path, QString *error);

/// Remember/recall the last used paths, via QSettings.
void rememberLastProject(const QString &projectPath, const QString &sourcePath);
QString lastProjectPath();
QString lastSourcePath();

} // namespace settings

} // namespace pist
