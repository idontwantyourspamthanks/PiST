// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "build/ProgramDiff.h"

#include "support/FileWrite.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>

#include <algorithm>

namespace pist {

namespace {

constexpr int kPreviewBytes = 8;

quint32 readBe32(const QByteArray &bytes, int offset)
{
    const auto *p = reinterpret_cast<const uchar *>(bytes.constData() + offset);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

struct TosImage
{
    quint32 textSize = 0;
    quint32 dataSize = 0;
    quint32 bssSize = 0;
    QByteArray text;
    QByteArray data;
};

bool parseTos(const QByteArray &bytes, TosImage *image, QString *error)
{
    if (bytes.size() < 28 || uchar(bytes.at(0)) != 0x60 || uchar(bytes.at(1)) != 0x1a) {
        if (error)
            *error = QObject::tr("not a TOS executable");
        return false;
    }
    image->textSize = readBe32(bytes, 2);
    image->dataSize = readBe32(bytes, 6);
    image->bssSize = readBe32(bytes, 10);
    const quint64 payload = 28ull + quint64(image->textSize) + quint64(image->dataSize);
    if (quint64(bytes.size()) < payload) {
        if (error)
            *error = QObject::tr("TOS executable is shorter than its text and data");
        return false;
    }
    image->text = bytes.mid(28, int(image->textSize));
    image->data = bytes.mid(28 + int(image->textSize), int(image->dataSize));
    return true;
}

QString nearestSymbol(const QList<SectionAnchor> &anchors, const QString &section, quint32 offset)
{
    QString name;
    quint32 best = 0;
    bool found = false;
    for (const SectionAnchor &anchor : anchors) {
        if (anchor.section != section || anchor.offset > offset || anchor.name.isEmpty())
            continue;
        if (!found || anchor.offset >= best) {
            found = true;
            best = anchor.offset;
            name = anchor.name;
        }
    }
    return name;
}

struct Place
{
    QString object;
    QString type;
    quint32 start = 0;
    quint32 end = 0;

    bool operator==(const Place &other) const
    {
        return object == other.object && type == other.type && start == other.start && end == other.end;
    }
};

QList<Place> placesFor(const LinkMap &map, const QString &section)
{
    QList<Place> places;
    for (const LinkPlacement &placement : map.placements()) {
        if (LinkMap::ourSectionFor(placement.sectionType) != section)
            continue;
        Place place;
        place.object = placement.objectFile;
        place.type = placement.sectionType.toUpper();
        place.start = placement.start;
        place.end = placement.end;
        places.append(place);
    }
    std::sort(places.begin(), places.end(), [](const Place &a, const Place &b) {
        if (a.type != b.type)
            return a.type < b.type;
        if (a.object != b.object)
            return a.object < b.object;
        if (a.start != b.start)
            return a.start < b.start;
        return a.end < b.end;
    });
    return places;
}

quint32 mappedSize(const LinkMap &map, const QString &dotted)
{
    for (const LinkSection &section : map.sections()) {
        if (section.name == dotted)
            return section.size;
    }
    return 0;
}

bool layoutMoved(const LinkMap *previous, const LinkMap *current, const QString &section)
{
    const bool havePrevious = previous && !previous->isEmpty();
    const bool haveCurrent = current && !current->isEmpty();
    if (!havePrevious && !haveCurrent)
        return false;
    // One side linked and the other did not: the bytes are not known to sit
    // at the same offsets, so a same-length diff would overclaim.
    if (havePrevious != haveCurrent)
        return true;
    const QString dotted = QLatin1Char('.') + section;
    if (mappedSize(*previous, dotted) != mappedSize(*current, dotted))
        return true;
    return placesFor(*previous, section) != placesFor(*current, section);
}

SectionChange diffBytes(const QString &name, const QByteArray &oldBytes, const QByteArray &newBytes,
                        const QList<SectionAnchor> &anchors, bool moved)
{
    SectionChange change;
    change.name = name;
    change.oldSize = quint32(oldBytes.size());
    change.newSize = quint32(newBytes.size());
    // A length change is the fact a user can see. "Moved" is only meaningful
    // when the two images are the same length and a same-length diff would
    // otherwise look patchable.
    if (oldBytes.size() != newBytes.size()) {
        change.decision = SectionDecision::RestartRequired;
        change.reason = newBytes.size() > oldBytes.size() ? RestartReason::Grew : RestartReason::Shrank;
        return change;
    }
    if (moved) {
        change.decision = SectionDecision::RestartRequired;
        change.reason = RestartReason::Moved;
        return change;
    }

    int index = 0;
    while (index < oldBytes.size()) {
        if (uchar(oldBytes.at(index)) == uchar(newBytes.at(index))) {
            ++index;
            continue;
        }
        const int start = index;
        while (index < oldBytes.size() && uchar(oldBytes.at(index)) != uchar(newBytes.at(index)))
            ++index;
        ByteRange range;
        range.offset = quint32(start);
        range.length = index - start;
        range.symbol = nearestSymbol(anchors, name, range.offset);
        range.oldPreview = oldBytes.mid(start, qMin(kPreviewBytes, range.length));
        range.newPreview = newBytes.mid(start, qMin(kPreviewBytes, range.length));
        change.ranges.append(range);
    }
    change.decision = change.ranges.isEmpty() ? SectionDecision::Identical
                                              : SectionDecision::DifferingRanges;
    return change;
}

SectionChange diffLength(const QString &name, quint32 oldSize, quint32 newSize, bool moved)
{
    SectionChange change;
    change.name = name;
    change.oldSize = oldSize;
    change.newSize = newSize;
    if (oldSize != newSize) {
        change.decision = SectionDecision::RestartRequired;
        change.reason = newSize > oldSize ? RestartReason::Grew : RestartReason::Shrank;
        return change;
    }
    if (moved) {
        change.decision = SectionDecision::RestartRequired;
        change.reason = RestartReason::Moved;
        return change;
    }
    return change;
}

bool copySnapshot(const QString &from, const QString &to, QString *error)
{
    if (!QFileInfo::exists(from))
        return true;
    QFile input(from);
    if (!input.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("cannot read '%1': %2").arg(from, input.errorString());
        return false;
    }
    const QByteArray bytes = input.readAll();
    if (input.error() != QFile::NoError) {
        if (error)
            *error = QObject::tr("cannot read '%1': %2").arg(from, input.errorString());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(to).absolutePath())) {
        if (error)
            *error = QObject::tr("cannot create '%1'").arg(QFileInfo(to).absolutePath());
        return false;
    }
    return files::write(to, bytes, error);
}

} // namespace

ProgramDiff diffTosPrograms(const QByteArray &previous, const QByteArray &current,
                            const QList<SectionAnchor> &anchors, const LinkMap *previousMap,
                            const LinkMap *currentMap)
{
    ProgramDiff diff;
    TosImage oldImage;
    TosImage newImage;
    QString error;
    if (!parseTos(previous, &oldImage, &error)) {
        diff.error = QObject::tr("Previous program: %1").arg(error);
        return diff;
    }
    if (!parseTos(current, &newImage, &error)) {
        diff.error = QObject::tr("Current program: %1").arg(error);
        return diff;
    }

    diff.sections.append(diffBytes(QStringLiteral("text"), oldImage.text, newImage.text, anchors,
                                   layoutMoved(previousMap, currentMap, QStringLiteral("text"))));
    diff.sections.append(diffBytes(QStringLiteral("data"), oldImage.data, newImage.data, anchors,
                                   layoutMoved(previousMap, currentMap, QStringLiteral("data"))));
    diff.sections.append(diffLength(QStringLiteral("bss"), oldImage.bssSize, newImage.bssSize,
                                    layoutMoved(previousMap, currentMap, QStringLiteral("bss"))));
    diff.ok = true;
    return diff;
}

QString previousBuildProgram(const QString &programPath)
{
    const QFileInfo info(programPath);
    return info.absolutePath() + QStringLiteral("/build/previous/") + info.fileName();
}

QString previousBuildLinkMap(const QString &linkMapPath)
{
    const QFileInfo info(linkMapPath);
    return info.absolutePath() + QStringLiteral("/build/previous/") + info.fileName();
}

bool snapshotPreviousBuild(const QString &programPath, const QString &linkMapPath, QString *error)
{
    if (!copySnapshot(programPath, previousBuildProgram(programPath), error))
        return false;
    if (linkMapPath.isEmpty())
        return true;
    return copySnapshot(linkMapPath, previousBuildLinkMap(linkMapPath), error);
}

} // namespace pist
