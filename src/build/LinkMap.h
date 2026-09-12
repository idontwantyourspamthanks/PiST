// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace pist {

/// One module's contribution to a linked output section.
struct LinkPlacement
{
    QString objectFile;   ///< as named in the map, matched by base name
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
/// Parsed shape (`vlink -b ataritos -Mfile.map`):
///
///     Section mapping (numbers in hex):
///     ------------------------------
///       00000000 .text  (size 20, allocated 1f)
///                00000000 - 00000006 main.o(CODE)
///                00000006 - 00000020 lib1.o(CODE)
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
    bool moduleOffset(const QString &objectFile, const QString &sectionType,
                      quint32 *offset) const;

    /// Section name for a map section type: CODE->text, DATA->data, BSS->bss.
    /// Returns an empty string for anything unrecognised.
    static QString ourSectionFor(const QString &sectionType);

private:
    QHash<QString, quint32> m_sectionVa;     ///< ".text" -> va
    QList<LinkSection> m_sections;
    QList<LinkPlacement> m_placements;
};

} // namespace pist
