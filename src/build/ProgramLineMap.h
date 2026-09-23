// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include "build/LineMap.h"
#include "build/LinkMap.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// Source-line mapping for a whole program, which may be built from several
/// separately-assembled modules.
///
/// Each module is assembled on its own, so each has its own listing whose
/// offsets start again from zero. Mapping an address back to a line therefore
/// needs to know where every module was placed, which the linker's map file
/// provides. For a single-module build there is no linker and no map, and the
/// one module's base is the live section base directly — so the same code path
/// serves both, with the map's offsets simply all zero.
///
/// A module is identified by path: the source file for a breakpoint, the object
/// file for a linker diagnostic. Base names are a fallback for the cases that
/// need one — the editor holds `prog.s` while the build assembled
/// `/project/prog.s` — and a fallback only while one module answers to the name.
/// Two `util.s` in different directories are two modules, and a name that could
/// mean either of them resolves nothing rather than the first registered.
///
/// Two phases, mirroring the debugger's constraints:
///
///   1. parse each module's listing (at build time);
///   2. resolve bases once the program is running, because GEMDOS relocates it
///      on every launch and the load address is only known after the entry stop
///      (docs/PLAN.md §5 rule 6).
class ProgramLineMap
{
public:
    /// One assembled module.
    struct Module
    {
        QString sourceFile;  ///< absolute path; identifies the module
        QString objectFile;  ///< absolute path as handed to the linker; matched
                             ///< by path, then by base name when unambiguous
        LineMap lines;

        /// Live addresses for this module, computed by resolve().
        LineMap::SectionBases bases;
        bool placed = false; ///< false when the map has no entry for this module
    };

    void clear();

    /// Register a module and read its listing. Returns false if the listing
    /// cannot be parsed, which is reported but does not abandon the build: the
    /// program still assembles and runs, only source mapping is lost.
    bool addModule(const QString &sourceFile, const QString &objectFile,
                   const QString &listingPath, QString *error);

    /// Supply the linker's map. Absent for a single-module build.
    void setLinkMap(const LinkMap &map);

    /// Record the program's live section bases, as reported by `info basepage`,
    /// and compute each module's placement.
    void setLiveBases(const LineMap::SectionBases &live);

    bool isEmpty() const;
    bool isResolved() const { return m_resolved; }

    QStringList sourceFiles() const;

    /// Source line to address. `file` may be a full path or a base name; with
    /// several modules the one it names must be unmistakable — a file name two
    /// modules share resolves nothing rather than the first registered.
    bool addressFor(const QString &file, int line, quint32 *address) const;
    /// Like addressFor but resolves only executable sections — for breakpoint
    /// arming, which must not arm at a data/bss address (finding B10).
    bool codeAddressFor(const QString &file, int line, quint32 *address) const;

    /// First line at or after `line` in `file` that emitted code — 0 when none.
    /// Base-independent: it reads listing entries only, so unlike codeAddressFor
    /// it works before a session supplies live bases. Resolving a code label to a
    /// breakable line uses this; arming still resolves the address with
    /// codeAddressFor.
    int nextCodeLine(const QString &file, int line) const;

    /// Address back to source line, searching every module and picking the one
    /// whose span contains it.
    bool lineFor(quint32 address, LineMap::Address *result) const;

    /// Modules the linker map had no placement for, so they cannot be mapped.
    /// Named by file name, as the rest of the IDE knows them — except where
    /// several of them share one, the case that refused them, which are named by
    /// path instead so a diagnostic can tell the user which files collided.
    QStringList unplacedModules() const;

    /// Exclusive end address of the program's text, across every placed module,
    /// once the live bases are known. Zero until then, or when no listing
    /// recorded a text extent.
    quint32 textEnd() const;

    /// Resolve a module and section offset to a source line.
    ///
    /// The linker reports errors as `main.o (CODE+0x4): ...` — a module and an
    /// offset, not an address — so this works without the program running, which
    /// is what lets a link error point at the offending source line.
    bool lineForObjectOffset(const QString &objectFile, const QString &section,
                             quint32 offset, LineMap::Address *result) const;

private:
    /// The module a source file refers to — by path first, then by base name
    /// while one module answers to it (see matchModuleName). nullptr when no
    /// module matches, or when two share the name.
    const Module *moduleForSource(const QString &file) const;
    /// The same question asked of a linker diagnostic's object file.
    const Module *moduleForObject(const QString &objectFile) const;

    /// Whether another module answers to this object file's base name, so the
    /// map's bare `util.o(CODE)` — all vlink writes — cannot say which of them
    /// it placed.
    bool objectNameIsShared(const QString &objectFile) const;

    /// Where the linker put a module's `sectionType`, or false when the map
    /// cannot say. A base-name match is refused while `objectNameIsShared`:
    /// accepting it would give every module of that name one module's address.
    bool placementOffset(const Module &module, const QString &sectionType,
                         quint32 *offset) const;

    QList<Module> m_modules;
    LinkMap m_linkMap;
    bool m_resolved = false;
};

} // namespace pist
