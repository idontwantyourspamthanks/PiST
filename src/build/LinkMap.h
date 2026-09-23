// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// Canonical form of a module path, for matching the object file PiST assembled
/// against the name the linker recorded.
///
/// The two reach us by different routes — PiST keeps the absolute path it handed
/// the assembler, the linker writes its own idea of the name — so comparing the
/// raw strings misses, and comparing base names alone cannot tell `dir1/util.o`
/// from `dir2/util.o`: both answer to `util.o`, and the first one found wins.
/// Canonicalising both sides (symlinks, `.` and `..` resolved) leaves only
/// genuine differences. A path with no file behind it has no canonical form, so
/// it is cleaned to an absolute one instead — still comparable, just without
/// symlink resolution.
QString canonicalModulePath(const QString &path);

/// Index of the single entry in `names` that `queried` refers to, or -1 when
/// nothing matches or the name is ambiguous.
///
/// A path comparison decides whenever both sides name a path at all. vlink
/// writes base names only — measured: `-M` reduces even an absolute object path
/// to `util.o`, in the map and in its diagnostics alike — and a bare name must
/// never be treated as a path, or it could coincide with one of ours and match
/// the wrong module. Without a path match the base name decides, but only while
/// it is unambiguous: two candidates carrying it mean the answer is a coin toss,
/// and a wrong guess arms a breakpoint at an address in the wrong module, so -1
/// is returned and the caller reports the module as unplaced instead.
///
/// `matchedByPath` (optional) reports which way the answer was found.
int matchModuleName(const QStringList &names, const QString &queried,
                    bool *matchedByPath = nullptr);

/// One module's contribution to a linked output section.
struct LinkPlacement
{
    QString objectFile;   ///< as named in the map — vlink writes base names only
    QString sectionType;  ///< CODE, DATA or BSS
    quint32 start = 0;    ///< virtual address within the output section
    quint32 end = 0;
};

/// An output section of the linked image.
struct LinkSection
{
    QString name;   ///< .text, .data, .bss
    quint32 va = 0; ///< where the section itself begins
    quint32 size = 0;
};

/// The linker's map file, which is how a multi-module program can still be
/// mapped back to source.
///
/// Separate compilation breaks the assumption PiST's line mapping used to make:
/// with one module, its listing offsets can be added straight to the live section
/// base. Under a linker every module's offsets start again from zero, so each
/// module needs its own base. vlink reports exactly that, and the map is the only
/// source for it — `-lineoffsets` is empty for vobj/DRI input because those
/// object formats carry no line data (docs/PLAN.md §4.2).
///
/// `vlink -b ataritos -Mfile.map`):
///
///     Section mapping (numbers in hex):
///     ------------------------------
///       00000000 .text  (size 20, allocated 1f)
///                00000000 - 00000006 main.o(CODE)
///                00000006 - 00000020 lib1.o(CODE)
///
/// The module names in that block are base names, never paths — vlink 0.18a
/// writes `util.o` even when it was handed `/build/dir2/util.o`, in the map and
/// in its diagnostics with them. Two modules built from same-named sources in
/// different directories therefore arrive indistinguishable, which is why
/// `moduleOffset` refuses them instead of answering with the first match.
class LinkMap
{
public:
    bool parse(const QString &path, QString *error);
    void clear();

    bool isEmpty() const { return m_placements.isEmpty(); }

    QList<LinkSection> sections() const { return m_sections; }
    QList<LinkPlacement> placements() const { return m_placements; }

    /// Where a module's contribution to `sectionType` begins, as an offset from
    /// the start of that output section.
    ///
    /// The offset is relative to the section rather than absolute so that it can
    /// be added to whatever address the section is loaded at on this run: the
    /// program is relocated by GEMDOS each time it starts.
    ///
    /// The module is identified by `matchModuleName` — path first, base name
    /// while that is unambiguous — so a module the map cannot name precisely
    /// returns false rather than another module's placement. `matchedByPath`
    /// (optional) reports which way it was identified.
    bool moduleOffset(const QString &objectFile, const QString &sectionType,
                      quint32 *offset, bool *matchedByPath = nullptr) const;

    /// Section name for a map section type: CODE->text, DATA->data, BSS->bss.
    /// Returns an empty string for anything unrecognised.
    static QString ourSectionFor(const QString &sectionType);

private:
    QHash<QString, quint32> m_sectionVa;     ///< ".text" -> va
    QList<LinkSection> m_sections;
    QList<LinkPlacement> m_placements;
};

} // namespace pist
