// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "editor/DiagnosticRef.h"

#include <QList>

namespace pist {

namespace {

/// The table. Every entry is anchored to wording the parsers already pin in
/// tests/tst_parsers.cpp (docs/tutorial-plan.md §11), so an entry cannot be
/// written against a message the toolchain does not actually print.
const QList<DiagnosticInfo> &table()
{
    static const QList<DiagnosticInfo> entries = {
        { QStringLiteral("number or identifier expected"),
          QStringLiteral("The assembler could not read an operand. A stray quote, a missing # "
                         "or a name that is not defined yet are the usual causes.") },
        { QStringLiteral("section attributes"),
          QStringLiteral("Those section flags are not ones the Atari output module accepts; "
                         "check the attributes on the section directive.") },
        { QStringLiteral("output module doesn't allow multiple sections of the same type"),
          QStringLiteral("Two sections of the same kind in one module. Use a single section, or "
                         "give the second a different kind (docs/PLAN.md §4.3).") },
        { QStringLiteral("Reference to undefined symbol"),
          QStringLiteral("A symbol is used but defined nowhere: check the spelling, whether the "
                         "file that defines it is incbin'd or listed under Project Settings ▸ "
                         "Additional sources, and whether it is a local label used in another "
                         "module.") },
        { QStringLiteral("unknown mnemonic"),
          QStringLiteral("The assembler read something in the instruction field that is not an "
                         "instruction. In Motorola syntax a label must start at column 0 — an "
                         "indented symbol definition, or an equate, is read as a mnemonic.") },
    };
    return entries;
}

} // namespace

const DiagnosticInfo *diagnosticRef(const QString &message)
{
    for (const DiagnosticInfo &entry : table()) {
        if (message.contains(entry.pattern, Qt::CaseInsensitive))
            return &entry;
    }
    return nullptr;
}

} // namespace pist
