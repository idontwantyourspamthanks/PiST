// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "model/Machine.h"

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

    /// The assembly source being built. With additional sources present this is
    /// the entry module, and must be listed first: TOS begins executing at the
    /// start of text, so its code has to come first in the link.
    QString sourceFile;

    /// Further sources to assemble and link with the primary one. Each is
    /// assembled separately, which is what makes multi-file projects possible.
    QStringList additionalSources;

    /// `-I` search paths, in order.
    QStringList includePaths;

    /// `-D` defines, as `NAME` or `NAME=value`.
    QStringList defines;

    /// Target CPU as vasm's `-m` suffix, e.g. "68000". Empty leaves vasm's default.
    QString cpu = QStringLiteral("68000");

    /// Extra arguments appended verbatim, for anything not yet modelled.
    QStringList extraBuildArgs;

    /// Explicit tool paths from project or user settings. Empty means
    /// "discover it", which searches beside the app, the per-user tools
    /// directory, and PATH (src/toolchain/Toolchain.h).
    QString assemblerPath;
    QString linkerPath;
    QString hatariPath;

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

    /// Debug transport: "native" (stock Hatari, stdin/prompt framing), "hrdb"
    /// (a fork with the HRDB listener, driven over TCP), or "auto" (default —
    /// follow the launched binary's probed capability; the bundled emulator is
    /// the fork). Explicit values exist for forcing one side of a mismatched
    /// setup, e.g. a stock Hatari when a project must be shared with one.
    QString debugBackend = QStringLiteral("auto");

};

/// Persist and restore a project's settings.
///
/// Two layers, deliberately:
///
///   - a **project file** (`<name>.pistproject`, JSON) beside the source, holding
///     everything above, so a project is portable and reviewable;
///   - **application state** for the most recently used source paths, so the IDE
///     reopens where the user left off and a brand-new session still has sensible
///     defaults.
///
/// Neither touches Hatari's own configuration.
namespace settings {

inline constexpr const char *kProjectSuffix = ".pistproject";

/// Derived output paths for a source file.
///
/// Defined once because the build and the launch must agree on them: the build
/// writes `<base>.prg` and the launch runs it, so if the two derivations ever
/// diverged the IDE would either fail to find what it just built or, worse, run
/// a stale binary from a previous build.
struct OutputPaths
{
    QString program;  ///< <base>.prg
    QString listing;  ///< <base>.lst
    QString project;  ///< <base>.pistproject

    bool isValid() const { return !program.isEmpty(); }
};

OutputPaths outputPathsFor(const QString &sourcePath);

/// Default path of the project file for a source file.
QString projectFileFor(const QString &sourcePath);

/// Write @p settings to @p path.
///
/// Stored strings are written exactly as given: nothing is re-relativised
/// against the project, so a save never rewrites a path the user typed.
bool save(const ProjectSettings &settings, const QString &path, QString *error);

/// Read @p path into @p settings.
///
/// Relative `includePaths` and `additionalSources` entries are resolved against
/// the project file's own directory, so a project means the same thing wherever
/// PiST was started; absolute entries are kept as written.
///
/// A file written by a newer PiST (a `version` this build does not know) is
/// refused, as is any entry of the wrong type, and @p error says why. Nothing is
/// written in either case, so the file on disk is left exactly as it was.
bool load(ProjectSettings *settings, const QString &path, QString *error);

/// Remember the source file the session is on, and move it to the head of the
/// recent list. The writer behind `lastSourcePath()` and `recentSources()`.
///
/// The project file path used to be stored here too, under `last/project`; no
/// reader ever existed for it, so the key is gone rather than carried (MIN-51).
void rememberLastSource(const QString &sourcePath);

/// The QSettings keys behind the two below, one accessor per key beside its
/// reader, so the writer and the reader cannot spell one key two ways (MIN-53).
const QString &lastSourceKey();
const QString &recentSourcesKey();

/// The source file the last session was on, for the "reopen where you left off"
/// path, and the MRU list behind File ▸ Open Recent.
QString lastSourcePath();
/// Most-recently-opened source files, MRU first, for File ▸ Open Recent.
QStringList recentSources();

} // namespace settings

} // namespace pist
