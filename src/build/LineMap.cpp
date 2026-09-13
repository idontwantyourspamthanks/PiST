// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/LineMap.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>

namespace pist {

namespace {

// `00: "text" (0-C)`
// These patterns contain `)"`, so raw strings need a custom delimiter.
const QRegularExpression &sectionRe()
{
    static const QRegularExpression re(
        QStringLiteral(R"RX(^\s*([0-9A-Fa-f]{2}):\s+"([^"]+)"\s*\(([0-9A-Fa-f]+)-([0-9A-Fa-f]+)\))RX"));
    return re;
}

// `Source: "sym.s"`
const QRegularExpression &sourceRe()
{
    static const QRegularExpression re(QStringLiteral(R"RX(^\s*Source:\s+"([^"]+)")RX"));
    return re;
}

// `00:00000008 702A            4: start: move.l  #42,d0`
// The bytes column may be empty or wrap onto continuation lines that carry no
// line number; those are skipped because the line-number group is mandatory.
const QRegularExpression &entryRe()
{
    static const QRegularExpression re(QStringLiteral(
        R"RX(^\s*([0-9A-Fa-f]{2}):([0-9A-Fa-f]{8})\s+[0-9A-Fa-f]*\s+(\d+):)RX"));
    return re;
}

QString normalised(const QString &path)
{
    // Compare paths case-insensitively on platforms that need it, but keep
    // comparison simple: vasm echoes the path exactly as given on the command
    // line, so exact matching is enough for now.
    return path;
}

} // namespace

void LineMap::clear()
{
    m_sectionNames.clear();
    m_sectionEnds.clear();
    m_entries.clear();
    m_sourceFiles.clear();
}

bool LineMap::parseListing(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("cannot read listing '%1': %2").arg(path, file.errorString());
        return false;
    }

    clear();

    QString currentFile;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString raw = stream.readLine();

        auto it = sourceRe().match(raw);
        if (it.hasMatch()) {
            currentFile = it.captured(1);
            if (!m_sourceFiles.contains(currentFile))
                m_sourceFiles.append(currentFile);
            continue;
        }

        // `Source: "x.s"` also matches sectionRe's shape? No: sectionRe requires
        // a hex index, so order does not matter here.
        auto section = sectionRe().match(raw);
        if (section.hasMatch()) {
            const QString index = section.captured(1).toUpper();
            const QString name = section.captured(2);
            m_sectionNames.insert(index, name);
            // vasm writes `(start-end)` where `end` is the section *size*, i.e.
            // already exclusive: a 0xC-byte section reads `(0-C)` and its last
            // byte is at 0xB. Adding one here made every span a byte too long, so
            // a section could claim an address belonging to the next one — which
            // is how two modules in a linked program both matched the same PC.
            m_sectionEnds.insert(name, section.captured(4).toUInt(nullptr, 16));
            continue;
        }

        auto entry = entryRe().match(raw);
        if (entry.hasMatch()) {
            const QString index = entry.captured(1).toUpper();
            Entry e;
            e.section = m_sectionNames.value(index);
            e.offset = entry.captured(2).toUInt(nullptr, 16);
            e.file = normalised(currentFile);
            e.line = entry.captured(3).toInt();
            if (!e.section.isEmpty() && e.line > 0)
                m_entries.append(e);
        }
    }

    std::sort(m_entries.begin(), m_entries.end(), [](const Entry &a, const Entry &b) {
        if (a.section != b.section)
            return a.section < b.section;
        return a.offset < b.offset;
    });

    return true;
}

bool LineMap::sameSource(const QString &recorded, const QString &queried)
{
    if (recorded.isEmpty() || queried.isEmpty())
        return false;
    if (recorded == queried)
        return true;
    // Fall back to base names: the listing may carry an absolute path from the
    // build command line while callers hold just the file name.
    return QFileInfo(recorded).fileName() == QFileInfo(queried).fileName();
}

quint32 LineMap::baseForSection(const QString &section, const SectionBases &bases)
{
    // The section name depends on the output format: `-Ftos` writes "text",
    // "data", "bss" while `-Fvobj` (the format used for linking) writes "CODE",
    // "DATA", "BSS". Accepting only one spelling silently resolves nothing for
    // the other, which is exactly what happened the first time a linked build
    // was tried.
    const QString name = section.toLower();
    if (name == QLatin1String("text") || name == QLatin1String("code"))
        return bases.text;
    if (name == QLatin1String("data"))
        return bases.data;
    if (name == QLatin1String("bss"))
        return bases.bss;
    return 0;
}

quint32 LineMap::textEnd() const
{
    quint32 end = 0;
    for (auto it = m_sectionEnds.constBegin(); it != m_sectionEnds.constEnd(); ++it) {
        const QString name = it.key().toLower();
        if (name == QLatin1String("text") || name == QLatin1String("code"))
            end = qMax(end, it.value());
    }
    return end;
}

bool LineMap::addressFor(const QString &file, int line, const SectionBases &bases,
                         quint32 *address) const
{
    for (const Entry &e : m_entries) {
        if (e.line != line || !sameSource(e.file, file))
            continue;
        const quint32 base = baseForSection(e.section, bases);
        if (base == 0)
            continue;
        if (address)
            *address = base + e.offset;
        return true;
    }
    return false;
}

bool LineMap::lineForSectionOffset(const QString &section, quint32 offset, Address *result) const
{
    const QString wanted = section.toLower();
    const Entry *best = nullptr;
    quint32 bestOffset = 0;

    for (const Entry &e : m_entries) {
        if (e.section.toLower() != wanted)
            continue;
        if (e.offset > offset)
            continue;
        if (!best || e.offset > bestOffset) {
            best = &e;
            bestOffset = e.offset;
        }
    }

    if (!best)
        return false;

    // Bounded the same way as lineFor: the section extent limits the last entry,
    // so an offset past it belongs to no line rather than to the final one.
    const quint32 spanEnd = m_sectionEnds.value(best->section, 0);
    if (spanEnd != 0 && offset >= spanEnd)
        return false;

    if (result) {
        result->file = best->file;
        result->line = best->line;
    }
    return true;
}

bool LineMap::lineFor(quint32 address, const SectionBases &bases, Address *result) const
{
    if (!bases.isValid())
        return false;

    // Find the entry with the greatest address <= the one asked about, *within
    // the same section*. The span bound matters: without it, any address past the
    // last entry — ROM, the stack, or the tail of BSS — would resolve to the
    // program's final source line, and the editor would highlight and scroll to a
    // line with nothing to do with the PC.
    const Entry *best = nullptr;
    quint32 bestAddress = 0;
    QString bestSection;

    for (const Entry &e : m_entries) {
        const quint32 base = baseForSection(e.section, bases);
        if (base == 0)
            continue;
        const quint32 addr = base + e.offset;
        if (addr > address)
            continue;
        if (!best || addr > bestAddress) {
            best = &e;
            bestAddress = addr;
            bestSection = e.section;
        }
    }

    if (!best)
        return false;

    // A PC past the end of the section is not inside any line's code, so there is
    // no honest line to report. The bound comes from the listing's own
    // `(start-end)` extent — `00: "text" (0-12)` — which is authoritative, with
    // the next entry used only if a listing omitted the extent.
    bool haveSpan = false;
    quint32 spanEnd = 0;

    if (m_sectionEnds.contains(bestSection)) {
        spanEnd = baseForSection(bestSection, bases) + m_sectionEnds.value(bestSection);
        haveSpan = true;
    } else {
        for (const Entry &e : m_entries) {
            if (e.section != bestSection)
                continue;
            const quint32 addr = baseForSection(e.section, bases) + e.offset;
            if (addr > bestAddress && (!haveSpan || addr < spanEnd)) {
                spanEnd = addr;
                haveSpan = true;
            }
        }
    }

    if (haveSpan && address >= spanEnd)
        return false;

    if (result) {
        result->file = best->file;
        result->line = best->line;
    }
    return true;
}

} // namespace pist
