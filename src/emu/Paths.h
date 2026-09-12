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
///   3. the OS data location for Hatari (`GenericDataLocation/hatari`), which
///      covers `/usr/share/hatari` on Linux and the equivalent elsewhere
///   4. platform conventions (Homebrew and MacPorts prefixes on macOS)
///   5. `<dir of hatari executable>/../share/hatari`, for a custom install prefix
///   6. the executable's own directory, but only on Windows and macOS, where
///      Hatari's release bundles ship their data files beside the binary
///
/// Non-existent paths, and paths that exist but are not directories, are
/// omitted — so the list is also safe to show to the user as "directories
/// searched".
QStringList tosSearchPaths();

/// Base directory for per-session state (the generated bootstrap script, the
/// control socket, and the isolated config directory).
///
/// Uses the platform temp location rather than a hard-coded path. Kept as short
/// as the platform allows, because on Unix the control socket path must fit in
/// `sockaddr_un::sun_path` (108 bytes on Linux); Windows uses named pipes and
/// has no such limit.
QString sessionBaseDir();

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
/// Age-based rather than "delete everything" because another PiST instance may be
/// running, and its live session must not be removed from under it.
void pruneStaleSessions(int maxAgeMinutes = 120);

} // namespace paths

} // namespace pist
