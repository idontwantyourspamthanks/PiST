// SPDX-License-Identifier: GPL-2.0-or-later
//
// pist - an IDE for Atari ST assembly development

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

/// Directories to search for TOS ROM images, in priority order:
///
///   1. `$PIST_TOS_DIR` when set (explicit override, also used by the test suite)
///   2. the OS data location for Hatari (`GenericDataLocation/hatari`), which
///      covers `/usr/share/hatari` on Linux and the equivalent elsewhere
///   3. platform conventions (Homebrew and MacPorts prefixes on macOS)
///   4. `<dir of hatari executable>/../share/hatari`, for a custom install prefix
///   5. the executable's own directory, but only on Windows and macOS, where
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

} // namespace paths

} // namespace pist
