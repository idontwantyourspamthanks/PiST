// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// What one of the assembler's or linker's messages means, in plain language.
///
/// The toolchain reports a file, a line and a message and stops there — correct
/// for a professional, opaque for someone learning. This is the small table
/// that turns those messages into a sentence a beginner can act on
/// (docs/tutorial-plan.md §11).
struct DiagnosticInfo
{
    QString pattern;  ///< the stable fragment of the message to match on
    QString meaning;  ///< what it means, and where to look
};

/// The explanation for `message`, or nullptr when the table has nothing useful
/// to say. An unmatched diagnostic shows nothing rather than a guess: a wrong
/// explanation is worse than none.
///
/// Matching is on a fragment rather than the whole string, because a vasm
/// message embeds a file name and a line number ("error 10 in line 2 of
/// \"bad.s\": number or identifier expected") that vary per build.
const DiagnosticInfo *diagnosticRef(const QString &message);

} // namespace pist
