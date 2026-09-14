// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// Fetching and installing the external programs (and the ROM) PiST drives.
///
/// Everything here is local and synchronous — verify, extract, build, install —
/// so each piece is exercisable by a test with a fabricated archive and no
/// network. The download itself lives in the UI (ui/SetupDialog), which is the
/// only part a test cannot reach.
///
/// The licence posture is unchanged by installing something: vasm and EmuTOS
/// arrive unmodified from their authors, are driven by command-line arguments
/// only, and the user is shown the URL and checksum before anything is fetched
/// (docs/PLAN.md §7 — a convenience, never a silent download).
namespace toolchain {

/// The pinned upstream artifacts the setup dialog offers.
///
/// These are the same URL/checksum pins as .github/workflows/ci.yml and
/// release.yml. The duplication is deliberate: the application cannot read the
/// workflows at run time, and an upstream change must fail loudly in both
/// places rather than silently altering what is tested or fetched.
namespace pins {

inline constexpr const char *kVasmVersion = "2.0f";
inline constexpr const char *kVasmUrl =
    "http://sun.hasenbraten.de/vasm/release/vasm.tar.gz";
inline constexpr const char *kVasmSha256 =
    "c84b2de1cbb87831795fe64a85c5d9a7002a766e3a7c30b0a2d7d5e99d878f49";

inline constexpr const char *kEmuTosVersion = "1.4";
inline constexpr const char *kEmuTosUrl =
    "https://sourceforge.net/projects/emutos/files/emutos/1.4/emutos-1024k-1.4.zip/download";
inline constexpr const char *kEmuTosSha256 =
    "dc9fbef6455a24ee8955cccd565588c718ba675fd54bc5a749003ac4bbd7f7e1";
/// The wanted member of the EmuTOS archive, matched by suffix because the zip
/// nests it in a versioned directory.
inline constexpr const char *kEmuTosImageSuffix = "etos1024k.img";

} // namespace pins

/// Lowercase hex sha256 of the file, or empty when it cannot be read.
QString sha256OfFile(const QString &path);

/// True when a source build of vasm is possible here: `make` plus a C
/// compiler. When false the setup dialog explains rather than offers — on
/// Windows that means pointing at the release bundle, which carries a prebuilt
/// vasm, since building one needs MSYS2/mingw.
bool canBuildVasm();

/// Unpack a .tar.gz into destDir (created as needed) with the system `tar`,
/// which every supported platform provides (Windows 10+ ships bsdtar as
/// tar.exe). Returns the top-level directory the extraction created.
QString unpackTarGz(const QString &archivePath, const QString &destDir, QString *error);

/// Build vasm in sourceDir (`make CPU=m68k SYNTAX=mot`) and return the built
/// binary's path. The make output goes to `log` either way.
QString buildVasm(const QString &sourceDir, QString *log, QString *error);

/// Install a tool binary into suggestedInstallDir() so discovery finds it on
/// the next lookup, and return the installed path. The executable bit is set
/// where the platform needs one (on Windows the suffix decides instead).
QString installToolBinary(const QString &binaryPath, QString *error);

/// Verify the tarball against expectedSha256, then unpack, build and install
/// vasm. Returns the installed path. This is the composition the setup dialog
/// runs on a worker thread; expectedSha256 is a parameter (rather than reading
/// the pin directly) so a test can drive it with a fabricated archive.
QString installVasmFromTarball(const QString &tarballPath, const QString &expectedSha256,
                               QString *log, QString *error);

/// Extract the member whose name ends with `nameSuffix` from a zip archive to
/// destPath. Members are listed and matched exactly, so no wildcard semantics
/// are involved; the extraction itself is done by the first available of
/// `unzip`, `tar` (bsdtar — GNU tar cannot read zips) or `python3`.
bool extractZipMember(const QString &zipPath, const QString &nameSuffix,
                      const QString &destPath, QString *error);

/// Verify the zip against expectedSha256, then extract the pinned EmuTOS image
/// into paths::suggestedRomDir() and return the installed path.
QString installEmuTosFromZip(const QString &zipPath, const QString &expectedSha256,
                             QString *error);

} // namespace toolchain

} // namespace pist
