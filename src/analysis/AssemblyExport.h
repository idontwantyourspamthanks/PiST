// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "analysis/AnalysisNotes.h"

#include <QByteArray>
#include <QString>

namespace pist {

/// Motorola source for `bytes`, with `notes` applied. Referenced addresses that
/// have no name gain a `loc_<hex>` label so a later assemble can reproduce
/// pc-relative displacements; those names exist only in the returned text.
/// Empty, with `*error` set, when `bytes` is not a TOS executable.
QString exportAssembly(const QByteArray &bytes, const AnalysisNotes &notes = {},
                       QString *error = nullptr);

/// Assemble `source` with vasm (`-Ftos`) and compare the resulting text, data
/// and bss to `original`. True only when those bytes and sizes are equal.
/// False, with `*error` set, when the assembler is missing, the assemble fails,
/// or the image differs — a difference is not reported as a reconstruction.
bool assembledImageMatches(const QString &assembler, const QString &source,
                           const QByteArray &original, QString *error = nullptr);

} // namespace pist
