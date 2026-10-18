// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/ProgramLineMap.h"

#include <QString>
#include <QVector>

namespace pist {

/// One symbol the assembler defined, and where its definition sits in the
/// source.
///
/// `file`/`line` are what the rest of the IDE needs: ProgramLineMap resolves an
/// address from exactly this pair, and the editor navigates with it. A symbol
/// vasm's own table knows but no source line defines — a name from a
/// command-line `-D`, or one a macro built out of a parameter — has no source
/// position, so `file` is empty and `line` is 0 rather than a guess.
struct SymbolEntry
{
    QString name;
    QString file;
    int line = 0;

    /// Whether the definition is an assignment (`name equ value`, `set`, `=`)
    /// rather than a label. An equate has a *value*, not a place: it emits no
    /// bytes, so its line has no address and the line after it belongs to
    /// somebody else. Recorded because a resolver that treated an equate as a
    /// bare label would silently return the address of whatever follows it.
    bool equate = false;
};

/// The symbol definitions in a listing written by `vasmm68k_mot -L`.
///
/// The listing carries two kinds of definition, and neither is complete alone:
///
///   * the body, where a `label:` at the start of a source line or a
///     `name equ value` / `name set value` / `name = value` assignment is the
///     definition. Only the body knows the source line, which is what makes a
///     symbol navigable;
///   * the `Symbols by name` / `Symbols by value` tables vasm appends, which
///     also carry symbols the source never spells out as a definition —
///     command-line defines and macro-generated names. They are position-less,
///     so a body definition always wins. The two are read in whichever order
///     they appear (each row's shape says which table it belongs to), and the
///     tables' own columns are never taken for names: reading a by-name row with
///     the by-value shape would otherwise report the `SS:HHHHHHHH` offset beside
///     a hex-shaped label as a symbol.
///
/// Listing shape:
///
///     Source: "/tmp/x.s"
///                                 1: kMax        equ     10
///                                 2: start:
///     00:00000000 7000            3:         moveq   #0,d0
///
/// Macro internals are skipped: both the lines between `MACRO`/`name MACRO` and
/// its `ENDM`, and the expansion lines vasm numbers `1M`, `2M` … . A line in a
/// macro body is a template, not a definition in the assembled program — vasm
/// emits it only when the macro is expanded, and a label there takes its address
/// from the call site, not from the body. Names such as these still appear in
/// the appended tables, so they are reported position-less rather than guessed
/// at a body line.
///
/// `sourceFile` selects whose definitions to keep. A listing covers the file
/// that was assembled and everything it included, and an included file is a
/// separate document, so a caller asking about one file gets only its symbols.
/// An empty `sourceFile` keeps every file's. Matching is LineMap::sameSource's,
/// so a full path from the listing and the caller's base name agree.
///
/// A name is reported once, at its first definition: vasm's own table names a
/// symbol once, and reuse of a name (a `.loop` local label in two routines) is
/// a position within a scope that this flat list cannot express.
///
/// Returns an empty list if the listing cannot be read.
QVector<SymbolEntry> symbolsFromListing(const QString &listingPath,
                                        const QString &sourceFile);

/// Where a label lives, as far as the IDE can tell: the source line to break at
/// and, once a session has supplied live section bases, that line's address.
struct LabelSite
{
    QString file;             ///< the base name breakpoints are keyed by
    int line = 0;             ///< the first code line at or after the definition
    quint32 address = 0;
    bool hasAddress = false;  ///< false until the program map has live bases
};

/// Why a label could not be turned into a place to break, or an address.
enum class LabelProblem { None, UnknownName, NoSourcePosition, NoCode };

/// Resolve `name` through `symbols` and `map`.
///
/// One rule, several callers — the remote `breakpoint <label>` verb and a hook
/// script's `sym()` — so a label means the same thing wherever it is named. A
/// label written on its own line has no code there (`count:` followed by the
/// instruction on the next line is the norm), so the site is the *first code line
/// at or after* the definition; an `equ` has no code of its own and is a refusal
/// rather than whatever instruction happens to follow it. A symbol with no source
/// position (a command-line define, a name a macro built) cannot be named at all.
///
/// The caller phrases the refusal, because its reader differs: a protocol reply
/// and a script error are not the same sentence.
bool labelSite(const QVector<SymbolEntry> &symbols, const ProgramLineMap &map,
               const QString &name, LabelSite *site, LabelProblem *problem);

/// Where a label *is*, for a caller that wants an address rather than a place to
/// break: the line its definition shares the address of, or — for a label alone
/// on its line (`buffer:` followed by `ds.b 256`) — the first line at or after it
/// that emits anything, in any section.
struct LabelAddress
{
    int line = 0;
    quint32 address = 0;
};

/// Why a label could not be given an address.
enum class LabelAddressProblem { None, UnknownName, NoSourcePosition, Equate, NoAddress, NotLive };

/// Resolve `name` to the address it has in this run.
///
/// This is `labelSite`'s sibling, not its twin: a breakpoint wants the first line
/// that *executes*, while a script naming a data label wants where that data
/// lives. An equate is refused rather than answered with its line's successor.
bool labelAddress(const QVector<SymbolEntry> &symbols, const ProgramLineMap &map,
                  const QString &name, LabelAddress *address,
                  LabelAddressProblem *problem);

} // namespace pist
