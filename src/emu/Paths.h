// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QStringList>

namespace pist {

/// Where Hatari keeps its data files, and where pist keeps per-session state.
///
/// Both need resolving per platform: neither `/tmp` nor `/usr/share/hatari` has
/// a macOS or Windows equivalent, and Hatari's own data directory differs (it
/// sits beside the executable on Windows).
namespace paths {

/// Environment variable that overrides the TOS ROM search path.
inline constexpr const char *kTosDirEnvVar = "PIST_TOS_DIR";

/// `share/emutos` and `share/hatari` directories found by walking up from
/// `applicationDir`, for bundles that carry their own ROM.
///
/// Exposed separately from tosSearchPaths() because that function always starts
/// from the running executable, so the layout rules could not otherwise be
/// exercised by a test — which is how a released archive came to ship a ROM that
/// nothing searched. Only directories that exist are returned.
QStringList bundledDataSearchPaths(const QString &applicationDir);

/// Directories to search for TOS ROM images, in priority order:
///
///   1. `$PIST_TOS_DIR` when set (explicit override, also used by the test suite)
///   2. `share/emutos` and `share/hatari` found by walking up from the
///      executable's own directory, which is how every release archive is laid
///      out: `bin/pist` beside `share/emutos`, `usr/bin/pist` beside
///      `usr/share/emutos` inside an AppImage, and `Contents/MacOS/pist` beside
///      `../share` in a macOS bundle. These come first because the ROM an archive
///      carries is the one its release process verified, so it is what makes a
///      fresh download run; it is still only a default, and a ROM chosen in
///      Project Settings or named by `$PIST_TOS_DIR` takes precedence
///   3. the per-user ROM directory a first-run download installs into
///      (`suggestedRomDir()`); after the bundled data, because the archive's
///      verified ROM should win over a previously fetched one
///   4. the OS data location for Hatari (`GenericDataLocation/hatari`), which
///      covers `/usr/share/hatari` on Linux and the equivalent elsewhere
///   5. platform conventions (Homebrew and MacPorts prefixes on macOS)
///   6. `<dir of hatari executable>/../share/hatari`, for a custom install prefix
///   7. the executable's own directory, but only on Windows and macOS, where
///      Hatari's release bundles ship their data files beside the binary
///
/// Non-existent paths, and paths that exist but are not directories, are
/// omitted — so the list is also safe to show to the user as "directories
/// searched".
QStringList tosSearchPaths();

/// Where a downloaded ROM image is installed so that tosSearchPaths() finds it
/// on the next lookup: `~/.local/share/PiST/roms` on Linux and the equivalent
/// elsewhere. The counterpart of toolchain::suggestedInstallDir() for data —
/// the first-run setup installs EmuTOS here, and it only works as a default
/// because the search below includes it.
QString suggestedRomDir();

/// Base directory for per-session state (the generated bootstrap script, the
/// control socket, and the isolated config directory).
///
/// Uses the platform temp location rather than a hard-coded path. Kept as short
/// as the platform allows, because on Unix the control socket path must fit in
/// `sockaddr_un::sun_path` (108 bytes on Linux); Windows uses named pipes and
/// has no such limit.
QString sessionBaseDir();

/// Directory holding documents extracted from floppy images for editing: one
/// subdirectory per image, created on demand.
///
/// Deliberately *not* under sessionBaseDir(). A session directory is deleted
/// once it looks old — that is what `pruneStaleSessions` is for — but an
/// extracted document is the file behind an open, editable tab, and saving that
/// tab writes the bytes back into the floppy image. A debugging or editing
/// session that sees no other activity for a few hours must not have that file
/// deleted from under the editor.
///
/// Sits under the per-user cache location, next to the data directory
/// `suggestedRomDir()` uses: the files are regenerable from the image, so they
/// may be cleaned up like any cache, but they outlive a single session.
///
/// Created (with its parents) on first use; the returned path is empty only if
/// the platform has no cache location at all.
QString documentExtractDir();

/// Create a directory, reporting failure. Used for session directories and for
/// the location of the generated bootstrap script, which are the same thing at
/// different points in the launch.
bool ensureDirectory(const QString &dir, QString *error);

/// Remove a session directory and everything in it. Safe to call on a path that
/// does not exist, and never follows non-directories.
void removeSessionDir(const QString &dir);

/// Delete session directories older than `maxAgeMinutes`, left behind by a
/// crashed or killed session. Called once at startup.
///
/// Each session creates a directory holding the generated debugger script, the
/// control socket and an isolated Hatari config tree, so without this the temp
/// location grows by one directory per run, indefinitely.
///
/// Age alone cannot tell debris from live data: a session directory keeps the
/// modification time it got at launch (nothing writes to it afterwards), so a
/// debugger left stopped for hours looks exactly like a crashed run, and
/// deleting it unlinks the control socket and the HOME tree Hatari is running
/// from. Directories are therefore named `<pid>-<counter>` (see
/// `MainWindow::makeSessionDir`), and a directory whose pid still names a running
/// process is skipped whatever its age. Names that do not parse that way fall
/// back to plain age-based pruning.
///
/// This is the implementation of the invariant that the age heuristic exists
/// for at all: **another PiST instance may be running, and its live session must
/// not be removed from under it**. Age-based pruning alone did not honour that
/// guarantee — it could not tell a quiet session from a dead one.
///
/// Also sweeps the extracted-document tree, so the one startup call covers
/// everything PiST leaves behind in user-writable locations.
void pruneStaleSessions(int maxAgeMinutes = 120);

/// Delete extracted floppy documents older than `maxAgeMinutes`, keeping the
/// directory itself.
///
/// `documentExtractDir()` needs a lifetime rule of its own now that it sits
/// outside sessionBaseDir(): staying under the session tree is what used to
/// collect it, and moving it out would otherwise let orphaned copies accumulate
/// for good.
///
/// A startup sweep rather than deletion when the document or its tab closes:
/// the documents are regenerated from the image and only meaningful to the
/// instance that extracted them, so a generous age bound is enough to stop them
/// accumulating, and it needs no bookkeeping in the UI. Called by
/// `pruneStaleSessions()`, which every instance already runs once at startup.
///
/// Each per-image directory is aged by the newest document *inside* it, not by
/// its own timestamp: a directory's mtime moves only when an entry is added or
/// removed, so a document being edited every day would otherwise be swept out
/// from under its open tab after a week.
void pruneExtractedDocuments(int maxAgeMinutes = 7 * 24 * 60);

} // namespace paths

} // namespace pist
