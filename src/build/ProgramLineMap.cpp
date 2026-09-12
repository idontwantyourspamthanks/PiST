// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/ProgramLineMap.h"

#include <QFileInfo>

namespace pist {

void ProgramLineMap::clear()
{
    m_modules.clear();
    m_linkMap.clear();
    m_resolved = false;
}

bool ProgramLineMap::addModule(const QString &sourceFile, const QString &objectFile,
                               const QString &listingPath, QString *error)
{
    Module module;
    module.sourceFile = sourceFile;
    module.objectFile = QFileInfo(objectFile).fileName();

    if (!module.lines.parseListing(listingPath, error))
        return false;

    m_modules.append(module);
    m_resolved = false;
    return true;
}

void ProgramLineMap::setLinkMap(const LinkMap &map)
{
    m_linkMap = map;
    m_resolved = false;
}

void ProgramLineMap::setLiveBases(const LineMap::SectionBases &live)
{
    for (Module &module : m_modules) {
        // A single-module build has no linker map, so the module owns the whole
        // section and starts where the section does.
        if (m_linkMap.isEmpty()) {
            module.bases = live;
            module.placed = true;
            continue;
        }

        quint32 textOffset = 0;
        quint32 dataOffset = 0;
        quint32 bssOffset = 0;

        const bool hasText =
            m_linkMap.moduleOffset(module.objectFile, QStringLiteral("CODE"), &textOffset);
        const bool hasData =
            m_linkMap.moduleOffset(module.objectFile, QStringLiteral("DATA"), &dataOffset);
        const bool hasBss =
            m_linkMap.moduleOffset(module.objectFile, QStringLiteral("BSS"), &bssOffset);

        // A module may legitimately contribute to only some sections — a file of
        // pure data, or one with no initialised data. Those sections simply have
        // no base for this module; lookups fall back to the others.
        module.bases.text = hasText ? live.text + textOffset : 0;
        module.bases.data = hasData ? live.data + dataOffset : 0;
        module.bases.bss = hasBss ? live.bss + bssOffset : 0;
        module.placed = hasText || hasData || hasBss;
    }

    m_resolved = live.isValid();
}

bool ProgramLineMap::isEmpty() const
{
    for (const Module &module : m_modules) {
        if (!module.lines.isEmpty())
            return false;
    }
    return true;
}

QStringList ProgramLineMap::sourceFiles() const
{
    QStringList files;
    for (const Module &module : m_modules)
        files.append(module.sourceFile);
    return files;
}

bool ProgramLineMap::lineForObjectOffset(const QString &objectFile, const QString &section,
                                         quint32 offset, LineMap::Address *result) const
{
    const QString wanted = QFileInfo(objectFile).fileName();

    for (const Module &module : m_modules) {
        if (module.objectFile != wanted)
            continue;
        return module.lines.lineForSectionOffset(section, offset, result);
    }

    return false;
}

QStringList ProgramLineMap::unplacedModules() const
{
    QStringList unplaced;
    for (const Module &module : m_modules) {
        if (!module.placed)
            unplaced.append(QFileInfo(module.sourceFile).fileName());
    }
    return unplaced;
}

bool ProgramLineMap::addressFor(const QString &file, int line, quint32 *address) const
{
    if (!m_resolved)
        return false;

    for (const Module &module : m_modules) {
        if (!module.placed)
            continue;
        // Match by path or base name, since the listing records whichever the
        // build passed and the caller may hold either.
        if (!LineMap::sameSource(module.sourceFile, file))
            continue;
        if (module.lines.addressFor(file, line, module.bases, address))
            return true;
    }

    return false;
}

bool ProgramLineMap::lineFor(quint32 address, LineMap::Address *result) const
{
    if (!m_resolved)
        return false;

    // Ask every module. Each module's own lookup is bounded by its section
    // extents, so a module that does not contain this address declines rather
    // than claiming it — which matters because modules overlap in the address
    // space only if something is badly wrong.
    for (const Module &module : m_modules) {
        if (!module.placed)
            continue;
        if (module.lines.lineFor(address, module.bases, result))
            return true;
    }

    return false;
}

} // namespace pist
