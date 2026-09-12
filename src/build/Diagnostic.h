// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QString>

namespace pist {

/// A single assembler diagnostic, as reported on vasm's stderr.
///
/// vasm emits two shapes (see docs/PLAN.md §4.1):
///
///   error 10 in line 2 of "bad.s": number or identifier expected
///   error 3004: section attributes <r> not supported
///
/// The second form carries no file or line, so it can only be attached to the
/// build log rather than to a source location. `line` is 1-based, and 0 means
/// "no location".
struct Diagnostic
{
    enum Severity { Error, Warning };

    Severity severity = Error;
    int code = 0;
    QString file;
    int line = 0;
    QString message;

    /// Set for linker diagnostics, which identify a module and an offset within a
    /// section rather than a source line. The line map resolves these once the
    /// listings are loaded, which is what lets a link error point at the line
    /// that caused it.
    QString objectFile;
    QString section;
    quint32 sectionOffset = 0;
    bool hasObjectOffset = false;

    bool hasLocation() const { return line > 0 && !file.isEmpty(); }
};

} // namespace pist
