// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/LinkMap.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace pist {

namespace {

/// `  00000000 .text  (size 20, allocated 1f)`
const QRegularExpression &sectionLineRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"(^\s{2}([0-9A-Fa-f]{8})\s+(\S+)\s+\(size\s+([0-9A-Fa-f]+))"));
    return re;
}

/// `           00000000 - 00000006 main.o(CODE)`
/// Indented deeper than a section line, which is what distinguishes them.
const QRegularExpression &moduleLineRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"(^\s{6,}([0-9A-Fa-f]{8})\s*-\s*([0-9A-Fa-f]{8})\s+(.+)\((\w+)\)\s*$)"));
    return re;
}

/// Whether `name` carries a directory, and so names a file by path rather than
/// by file name alone. vlink's map and diagnostics write base names only, and a
/// bare name must never be resolved against the working directory: it would
/// then coincide with whichever of our paths happened to be there.
bool namesAPath(const QString &name)
{
    return name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'));
}

} // namespace

QString canonicalModulePath(const QString &path)
{
    if (path.isEmpty())
        return {};

    const QFileInfo info(path);
    // An object the map still names but the disk no longer has (a cleaned build
    // tree) has no canonical form; the cleaned absolute path is the best that
    // comparison can do with it.
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

int matchModuleName(const QStringList &names, const QString &queried, bool *matchedByPath)
{
    if (matchedByPath)
        *matchedByPath = false;

    if (queried.isEmpty())
        return -1;

    if (namesAPath(queried)) {
        const QString wanted = canonicalModulePath(queried);
        for (int i = 0; i < names.size(); ++i) {
            if (!namesAPath(names.at(i)))
                continue; // a base name is an identity, not a location
            if (canonicalModulePath(names.at(i)) != wanted)
                continue;
            if (matchedByPath)
                *matchedByPath = true;
            return i;
        }
    }

    // No path to compare, so fall back to the base name — the only identity the
    // linker gives us — and only while one entry answers to it.
    const QString wantedName = QFileInfo(queried).fileName();
    if (wantedName.isEmpty())
        return -1;

    int found = -1;
    for (int i = 0; i < names.size(); ++i) {
        if (QFileInfo(names.at(i)).fileName() != wantedName)
            continue;
        if (found >= 0)
            return -1; // two entries share it: any answer would be a guess
        found = i;
    }
    return found;
}

void LinkMap::clear()
{
    m_sectionVa.clear();
    m_sections.clear();
    m_placements.clear();
}

QString LinkMap::ourSectionFor(const QString &sectionType)
{
    const QString type = sectionType.toUpper();
    if (type == QLatin1String("CODE"))
        return QStringLiteral("text");
    if (type == QLatin1String("DATA"))
        return QStringLiteral("data");
    if (type == QLatin1String("BSS"))
        return QStringLiteral("bss");
    return {};
}

bool LinkMap::parse(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QObject::tr("cannot read link map '%1': %2")
                         .arg(path, file.errorString());
        return false;
    }

    clear();

    QString currentSection;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();

        // A section line names the output section the following module lines
        // belong to, so track it.
        const auto section = sectionLineRe().match(line);
        if (section.hasMatch()) {
            const quint32 va = section.captured(1).toUInt(nullptr, 16);
            const QString name = section.captured(2);
            const quint32 size = section.captured(3).toUInt(nullptr, 16);

            LinkSection s;
            s.name = name;
            s.va = va;
            s.size = size;
            m_sections.append(s);
            m_sectionVa.insert(name, va);
            currentSection = name;
            continue;
        }

        // `Files:` also lists modules with a similar shape but different
        // punctuation, so match the module form strictly rather than loosely.
        const auto module = moduleLineRe().match(line);
        if (module.hasMatch() && !currentSection.isEmpty()) {
            LinkPlacement p;
            p.start = module.captured(1).toUInt(nullptr, 16);
            p.end = module.captured(2).toUInt(nullptr, 16);
            p.objectFile = module.captured(3).trimmed();
            p.sectionType = module.captured(4);
            m_placements.append(p);
        }
    }

    if (m_placements.isEmpty()) {
        if (error)
            *error = QObject::tr("no module placements found in '%1' — is it a vlink "
                                 "map file?")
                         .arg(path);
        return false;
    }

    return true;
}

bool LinkMap::moduleOffset(const QString &objectFile, const QString &sectionType,
                           quint32 *offset, bool *matchedByPath) const
{
    // Only this section's placements are candidates: a module contributes one
    // placement per section it appears in, so counting every section's would
    // make a module with both code and data look ambiguous with itself.
    QStringList names;
    QList<int> indices;
    for (int i = 0; i < m_placements.size(); ++i) {
        if (m_placements.at(i).sectionType.compare(sectionType, Qt::CaseInsensitive) != 0)
            continue;
        names.append(m_placements.at(i).objectFile);
        indices.append(i);
    }

    const int match = matchModuleName(names, objectFile, matchedByPath);
    if (match < 0) {
        // Either the map has no placement for this module, or the only ones it
        // could be are several that share its name. Answering the latter with
        // one of them is how a module ends up with another module's addresses.
        return false;
    }

    const LinkPlacement &placement = m_placements.at(indices.at(match));

    // Relative to the section, since the section's own load address varies per
    // run.
    const quint32 sectionVa = m_sectionVa.value(QStringLiteral(".") + ourSectionFor(sectionType), 0);
    if (placement.start < sectionVa)
        return false;
    if (offset)
        *offset = placement.start - sectionVa;
    return true;
}

} // namespace pist
