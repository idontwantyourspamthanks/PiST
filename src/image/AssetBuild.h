// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "image/StFormats.h"

#include <QString>

namespace pist {

/// One declared sprite export: a `.pim` phase, the blocks the bitplane
/// dialog already knows, and the file name written under the build directory.
/// The file name is a single path component (`hero.dat`), never a path.
struct AssetBuildRequest
{
    QString phaseName;
    QString outputFileName;
    BitplaneDataOptions options;
};

struct AssetBuildResult
{
    bool ok = false;
    /// The outputs already matched this document and these options, so nothing
    /// was rewritten.
    bool unchanged = false;
    QString error;
    QString dataPath;
    QString includePath;
};

/// Export `pimPath` into `buildDirectory` and write the include that `incbin`s
/// it. A failed export leaves any previous `.dat` and include in place.
///
/// The include is the same layout `bitplaneLayout()` describes: an `incbin`,
/// the byte length, the frame count and the frame stride. Callers put
/// `buildDirectory` on the assembler's include path for that build only.
AssetBuildResult buildDeclaredAsset(const QString &pimPath, const AssetBuildRequest &request,
                                    const QString &buildDirectory);

} // namespace pist
