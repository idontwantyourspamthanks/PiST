// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace pist {

/// Maps assembly source lines to machine addresses using vasm's `-L` listing.
///
/// This avoids needing DWARF or a cross-GDB: the listing already contains a
/// `section:offset` entry for every line that emitted bytes. Live section base
/// addresses come from Hatari's `info basepage` at runtime, so the mapping
/// stays correct regardless of where GEMDOS loaded the program.
///
/// Listing shape (whitespace is significant only as a separator):
///
///     Sections:
///     00: "text" (0-C)
///     01: "data" (0-4)
///
///     Source: "sym.s"
///                                 1:     text
///     00:00000000 58425241        2:     dc.b    "XBRA"
///     00:00000008 702A            4: start: move.l  #42,d0
class LineMap
{
public:
    /// Live section base addresses, as read from the emulator's basepage.
    struct SectionBases
    {
        quint32 text = 0;
        quint32 data = 0;
        quint32 bss = 0;

        bool isValid() const { return text != 0; }
    };

    struct Address
    {
        QString file;
        int line = 0;
    };

    /// Parse a listing produced by `vasmm68k_mot -L <file>`.
    /// Returns false and sets `error` if the file cannot be read.
    bool parseListing(const QString &path, QString *error);

    /// Drop all parsed state.
    void clear();

    bool isEmpty() const { return m_entries.isEmpty(); }

    /// Source files contributing to this listing, in first-seen order.
    QStringList sourceFiles() const { return m_sourceFiles; }

    /// Address of a source line, resolved against live section bases.
    /// Returns false when the line emitted no code or data.
    bool addressFor(const QString &file, int line, const SectionBases &bases, quint32 *address) const;
    /// Like addressFor but resolves only executable sections (text/code). A
    /// breakpoint armed at a data/bss address never fires, so the breakpoint
    /// consumer uses this; watchpoints resolve data and keep using addressFor.
    bool codeAddressFor(const QString &file, int line, const SectionBases &bases,
                        quint32 *address) const;

    /// First line at or after `line` in `file` whose entry is in an executable
    /// section (text/code) — 0 when none. Base-independent: it reads the
    /// listing entries only, so it works before a session supplies live bases.
    /// A label on its own line emits no bytes, so resolving a code label to a
    /// breakable line starts here, not at the definition line.
    int nextCodeLine(const QString &file, int line) const;

    /// Nearest source line at or before `address`. Ignored if the address falls
    /// outside every known section.
    bool lineFor(quint32 address, const SectionBases &bases, Address *result) const;

    /// Nearest source line at or before `offset` within `section` ("text", "code",
    /// ...). Used to place a linker diagnostic, which names a module and a section
    /// offset rather than an address — the linked addresses are not known until
    /// the program runs, but offsets within a module are known immediately.
    bool lineForSectionOffset(const QString &section, quint32 offset, Address *result) const;

    /// Section base for a listing section name ("text", "data", "bss").
    static quint32 baseForSection(const QString &section, const SectionBases &bases);

    /// Exclusive end offset of the listing's text section, from the `(start-end)`
    /// header — zero when the listing recorded no extent for it. The offset is
    /// relative to the section base, like every other offset in the listing.
    quint32 textEnd() const;

    /// Whether a source path recorded in a listing refers to `queried`.
    ///
    /// vasm writes the source path exactly as it was given on the command line,
    /// so a build invoked with an absolute path produces `Source: "/tmp/x.s"`
    /// while the rest of the IDE identifies the open file by base name. Comparing
    /// the two literally silently matches nothing, which breaks both breakpoint
    /// arming and the PC-to-line highlight, so compare by base name as a fallback.
    static bool sameSource(const QString &recorded, const QString &queried);

private:
    struct Entry
    {
        QString section;
        quint32 offset = 0;
        QString file;
        int line = 0;
    };
    bool addressForImpl(const QString &file, int line, const SectionBases &bases,
                        quint32 *address, bool codeOnly) const;

    QHash<QString, QString> m_sectionNames; // listing index -> section name

    /// Section name -> end offset (exclusive), from the listing's `(start-end)`
    /// header. Used to bound a line's span, so an address past the end of a
    /// section does not resolve to that section's final line.
    QHash<QString, quint32> m_sectionEnds;
    QList<Entry> m_entries;                 // sorted by section, then offset
    QStringList m_sourceFiles;
};

} // namespace pist
