// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>
#include <QStringList>

namespace pist {

/// An external program PiST drives: the assembler or the emulator.
///
/// Both are invoked as subprocesses through command-line arguments and are never
/// patched or linked against, which is what keeps their licences clean and means
/// a newer or user-supplied build simply works (docs/PLAN.md §7).
struct ToolInfo
{
    QString path;     ///< resolved absolute path, or empty when not found
    QString version;  ///< first version-looking token from `--version`, if cheap
    QString name;     ///< human-facing name, e.g. "vasmm68k_mot"

    /// Why an explicit path from settings did not resolve. Empty in every other
    /// case: "nothing was found" needs no explanation, but a path the user typed
    /// that does not run is their mistake to correct, and the callers that show
    /// it must be able to tell the two apart instead of printing the bare
    /// program name as if discovery had found it.
    QString reason;

    bool found() const { return !path.isEmpty(); }
};

/// Where to look, in priority order, and what to do when nothing is found.
namespace toolchain {

/// The assembler. `overridePath` (from project or user settings) wins when set.
ToolInfo findAssembler(const QString &overridePath = QString());

/// The emulator. `overridePath` (from project or user settings) wins when set.
ToolInfo findEmulator(const QString &overridePath = QString());

/// The linker, needed only when a project has more than one source file.
///
/// Separate assembly requires linking, and the linker is a different program with
/// its own licence and its own (absent) binaries — see NOTICE.
ToolInfo findLinker(const QString &overridePath = QString());

/// Directories searched, for reporting when a tool is missing.
QStringList searchPaths();

/// A short instruction for obtaining the assembler, shown when it is missing.
///
/// This is the honest form of "batteries included": vasm's licence permits
/// redistribution *unmodified* for non-commercial use, so it may be bundled in
/// release artefacts and fetched on first run, but it is never patched and the
/// user is told plainly where it comes from.
QString assemblerInstallHint();

QString emulatorInstallHint();

/// How to obtain the linker, shown when a multi-source project cannot be built.
QString linkerInstallHint();

/// Suggested destination for a first-run download of a bundled tool, matching
/// `searchPaths()` so a fetch is found on the next launch without configuration.
QString suggestedInstallDir();

} // namespace toolchain

} // namespace pist
