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
    // Both paths are kept absolute — the object path is the identity the
    // linker's map is matched against, and reducing it to a base name is what
    // made `dir1/util.o` and `dir2/util.o` one module (see LinkMap).
    module.sourceFile = canonicalModulePath(sourceFile);
    module.objectFile = canonicalModulePath(objectFile);

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
    // A single-module build has no linker map: the module *is* the program, so
    // it owns the whole section and starts where the section does. With more
    // than one module a map was expected, and without it no module's placement
    // is knowable — marking them placed would resolve their lines into the
    // live section base, arming breakpoints inside the first module that never
    // fire (or fire in the wrong one) while reporting themselves as armed.
    // Leave them unplaced so unplacedModules() tells the truth instead.
    const bool singleModule = m_modules.size() == 1;
    for (Module &module : m_modules) {
        if (m_linkMap.isEmpty()) {
            module.bases = singleModule ? live : LineMap::SectionBases();
            module.placed = singleModule;
            continue;
        }

        quint32 textOffset = 0;
        quint32 dataOffset = 0;
        quint32 bssOffset = 0;

        const bool hasText = placementOffset(module, QStringLiteral("CODE"), &textOffset);
        const bool hasData = placementOffset(module, QStringLiteral("DATA"), &dataOffset);
        const bool hasBss = placementOffset(module, QStringLiteral("BSS"), &bssOffset);

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

const ProgramLineMap::Module *ProgramLineMap::moduleForSource(const QString &file) const
{
    QStringList names;
    names.reserve(m_modules.size());
    for (const Module &module : m_modules)
        names.append(module.sourceFile);

    const int match = matchModuleName(names, file);
    return match < 0 ? nullptr : &m_modules.at(match);
}

const ProgramLineMap::Module *ProgramLineMap::moduleForObject(const QString &objectFile) const
{
    QStringList names;
    names.reserve(m_modules.size());
    for (const Module &module : m_modules)
        names.append(module.objectFile);

    const int match = matchModuleName(names, objectFile);
    return match < 0 ? nullptr : &m_modules.at(match);
}

bool ProgramLineMap::objectNameIsShared(const QString &objectFile) const
{
    if (objectFile.isEmpty())
        return false;

    const QString name = QFileInfo(objectFile).fileName();
    int sameName = 0;
    for (const Module &module : m_modules) {
        if (QFileInfo(module.objectFile).fileName() == name && ++sameName > 1)
            return true;
    }
    return false;
}

bool ProgramLineMap::placementOffset(const Module &module, const QString &sectionType,
                                     quint32 *offset) const
{
    bool matchedByPath = false;
    if (!m_linkMap.moduleOffset(module.objectFile, sectionType, offset, &matchedByPath))
        return false;

    // The map names modules by base name alone (LinkMap), so a name-match answer
    // is only meaningful while the name belongs to one of ours: with two
    // `util.o` registered, the placement is one of them and taking it would put
    // both at one address — the silent wrong-module arming this lookup exists to
    // stop. A path match needs no such guard; it named the exact file.
    return matchedByPath || !objectNameIsShared(module.objectFile);
}

bool ProgramLineMap::lineForObjectOffset(const QString &objectFile, const QString &section,
                                         quint32 offset, LineMap::Address *result) const
{
    const Module *module = moduleForObject(objectFile);
    if (!module)
        return false;

    return module->lines.lineForSectionOffset(section, offset, result);
}

QStringList ProgramLineMap::unplacedModules() const
{
    QList<const Module *> unplaced;
    for (const Module &module : m_modules) {
        if (!module.placed)
            unplaced.append(&module);
    }

    // A module is named the way the rest of the IDE refers to it, by file name,
    // so the log line reads as the user's own file. Two modules left unplaced
    // because they share that name are the case where it cannot: the log would
    // say the same name twice and mean two different files, which is exactly the
    // ambiguity that refused them — so those are named by path, the only thing
    // that tells them apart.
    QStringList baseNames;
    for (const Module *module : unplaced)
        baseNames.append(QFileInfo(module->sourceFile).fileName());

    QStringList names;
    for (int i = 0; i < unplaced.size(); ++i) {
        names.append(baseNames.count(baseNames.at(i)) > 1 ? unplaced.at(i)->sourceFile
                                                          : baseNames.at(i));
    }
    return names;
}

quint32 ProgramLineMap::textEnd() const
{
    quint32 end = 0;
    for (const Module &module : m_modules) {
        if (!module.placed || module.bases.text == 0)
            continue;
        // A module placed by the linker starts where the map puts it, and its
        // listing's extent is relative to that start.
        const quint32 span = module.lines.textEnd();
        if (span == 0)
            continue;
        end = qMax(end, module.bases.text + span);
    }
    return end;
}

bool ProgramLineMap::addressFor(const QString &file, int line, quint32 *address) const
{
    if (!m_resolved)
        return false;

    // One module answers for a source file — the one the path names, or the only
    // one its base name fits. Anything less certain resolves nothing: the
    // previous "whichever module comes first" handed the address of one module's
    // line to an identical line in another, which is exactly what arms a
    // breakpoint in the wrong one.
    const Module *module = moduleForSource(file);
    if (!module || !module->placed)
        return false;

    return module->lines.addressFor(file, line, module->bases, address);
}

bool ProgramLineMap::codeAddressFor(const QString &file, int line, quint32 *address) const
{
    if (!m_resolved)
        return false;

    const Module *module = moduleForSource(file);
    if (!module || !module->placed)
        return false;

    return module->lines.codeAddressFor(file, line, module->bases, address);
}

int ProgramLineMap::nextCodeLine(const QString &file, int line) const
{
    const Module *module = moduleForSource(file);
    if (!module)
        return 0;

    return module->lines.nextCodeLine(file, line);
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
