// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "model/Machine.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// Default assembler arguments for a **debug** build: `-quiet -devpac -opt-x+`.
///
/// `-devpac` selects Devpac compatibility, which also disables vasm's
/// optimisations, so the code the debugger steps through matches the source. It
/// drops the DRI/GST symbol table as well, which would leave Hatari's
/// `symbols prg` with nothing to load — so the debug list adds `-opt-x+`,
/// Devpac's extended-symbols option, which puts the table back with full-length
/// names (`-opt-d+` restores it too but truncates names to eight characters).
/// Both flags verified on vasm 2.0f, and Hatari's `symbols prg` loads the
/// `-opt-x+` table.
QStringList defaultDebugAssemblerArgs();

/// Default assembler arguments for a **release** build: `-quiet -devpac -nosym`.
///
/// The same Devpac compatibility and disabled optimisations as the debug list,
/// with the symbol table stripped. `-devpac` already sets vasm's `no_symbols`, so
/// `-nosym` is strictly redundant — it is kept because it says what the mode is
/// *for*, and keeps the strip true if the `-devpac` is ever removed. Using
/// `-devpac` in both lists is the point: one source must assemble to the same
/// code in either mode; only the symbol table differs.
///
/// Neither list is a guarantee: a source's own `opt x+` re-enables the table even
/// here (it is applied during assembly, after the command line). See
/// docs/PLAN.md §4.1.
QStringList defaultReleaseAssemblerArgs();

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

    /// Assembler arguments for a debug build, in order.
    ///
    /// These are the flags PiST passes ahead of the arguments it manages itself
    /// — the output format (`-Ftos`, or `-Fvobj` per module in a linked build),
    /// `-o`, `-L`, `-I`, `-D` and the source file. They are editable because the
    /// issue behind this setting is precisely "I want to control vasm's command
    /// line"; the defaults are `defaultDebugAssemblerArgs()`.
    QStringList debugAssemblerArgs = defaultDebugAssemblerArgs();

    /// Assembler arguments for a release build, in order. Same contract as
    /// `debugAssemblerArgs`; defaults to `defaultReleaseAssemblerArgs()`.
    QStringList releaseAssemblerArgs = defaultReleaseAssemblerArgs();

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

    // --- stop hooks ------------------------------------------------------

    /// The script run at every debugger stop (docs/stop-hooks-plan.md). Empty
    /// means there is none. A relative path is resolved against the project
    /// directory on load, exactly as `includePaths` is.
    QString stopHookPath;

    /// Whether the hook runs at all. Off by default: a script that changes the
    /// machine should be switched on deliberately.
    bool stopHookEnabled = false;

    /// Whether the script's `resume()` is honoured without asking. Off by
    /// default, because a stray `resume()` at every stop is the one gesture of
    /// this feature that can surprise the person sitting at the keyboard
    /// (glm-features.md §1).
    bool stopHookAutoContinue = false;
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

/// Follow a rename of @p oldPath to @p newPath through the last-source pointer
/// and the recent list (docs/untitled-plan.md §7.2).
///
/// Entries naming the old file are rewritten in place, so the list keeps its
/// order and its length. Both are read back into a session — the startup reopen
/// and the welcome view — which is why a stale entry is a wrong answer rather
/// than a cosmetic one.
void renameSource(const QString &oldPath, const QString &newPath);

/// @name Lifetime usage counters
///
/// The welcome view's shelf (docs/untitled-plan.md §6). Local to this user's
/// QSettings like everything above: nothing leaves the machine, and the numbers
/// only ever describe this installation.
///
/// One typed reader and one `note*` writer per counter, with the key spelled
/// once inside ProjectSettings.cpp — the MIN-53 rule. Nothing outside needs the
/// key itself, because nothing outside should be incrementing these.
///
/// Counting starts when the counters shipped: an installation's numbers begin
/// at its first launch on the version that has them (day one counts), and the
/// stored first-launch date says when that was.
/// @{

int sessionCount();
int buildCount();
int runCount();
int openedFileCount();

/// The first launch's date, ISO `yyyy-MM-dd`, or empty before the first
/// `noteSessionStarted()`.
QString firstLaunchDate();

/// Count one main-window start. Records the first-launch date the first time.
void noteSessionStarted();
/// Count one finished build, successful or not.
void noteBuild();
/// Count one launched emulator session.
void noteRun();
/// Count one document opened by the user.
void noteFileOpened();

/// The previous session's summary, for the welcome view's orientation line —
/// facts about where you were, not vanity (docs/untitled-plan.md §6).
///
/// Held in memory for the session's duration and written once at teardown, so a
/// crash loses the summary rather than the work.
struct LastSession
{
    int builds = 0;
    bool lastBuildOk = false;
    QString lastFile;
    /// False until a session has been flushed: an installation that has never
    /// quit cleanly has no summary to show, which is not the same as one that
    /// built nothing.
    bool known = false;
};

/// Write @p session as "the last session". Called once, at teardown.
void flushLastSession(const LastSession &session);

/// The summary the previous session left, if it left one (`known`).
LastSession lastSession();

/// @}

} // namespace settings

} // namespace pist
