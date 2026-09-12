// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/LinkMap.h"

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

} // namespace

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
            *error = QStringLiteral("cannot read link map '%1': %2")
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
            *error = QStringLiteral("no module placements found in '%1' — is it a vlink "
                                    "map file?")
                         .arg(path);
        return false;
    }

    return true;
}

bool LinkMap::moduleOffset(const QString &objectFile, const QString &sectionType,
                           quint32 *offset) const
{
    const QString wanted = QFileInfo(objectFile).fileName();

    for (const LinkPlacement &p : m_placements) {
        // Match by base name: the map records whatever path was passed to the
        // linker, which may be absolute.
        if (QFileInfo(p.objectFile).fileName() != wanted)
            continue;
        if (p.sectionType.compare(sectionType, Qt::CaseInsensitive) != 0)
            continue;

        // Relative to the section, since the section's own load address varies
        // per run.
        const quint32 sectionVa = m_sectionVa.value(QStringLiteral(".") + ourSectionFor(sectionType), 0);
        if (p.start < sectionVa)
            return false;
        if (offset)
            *offset = p.start - sectionVa;
        return true;
    }

    return false;
}

} // namespace pist
