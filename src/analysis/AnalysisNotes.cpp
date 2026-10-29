// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "analysis/AnalysisNotes.h"

#include "support/FileWrite.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>

#include <algorithm>
#include <limits>

namespace pist {

namespace {

bool letter(QChar c)
{
    return (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
        || (c >= QLatin1Char('a') && c <= QLatin1Char('z'));
}

bool digit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

bool validName(const QString &name)
{
    if (name.isEmpty() || name.size() > 64)
        return false;
    const QChar first = name.at(0);
    if (!(letter(first) || first == QLatin1Char('_') || first == QLatin1Char('.')))
        return false;
    for (const QChar c : name) {
        if (!(letter(c) || digit(c) || c == QLatin1Char('_') || c == QLatin1Char('.')
              || c == QLatin1Char('$')))
            return false;
    }
    return true;
}

bool validComment(const QString &text)
{
    if (text.isEmpty() || text.size() > 200)
        return false;
    for (const QChar c : text)
        if (c.category() == QChar::Other_Control)
            return false;
    return true;
}

bool fits(quint32 address, quint32 length)
{
    return length > 0 && length <= std::numeric_limits<quint32>::max() - address;
}

bool readU32(const QJsonObject &object, const QString &key, quint32 *out)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (number < 0.0 || number > double(std::numeric_limits<quint32>::max()))
        return false;
    const quint32 whole = quint32(number);
    if (double(whole) != number)
        return false;
    *out = whole;
    return true;
}

QVector<AnalysisOverride> withoutSpan(const QVector<AnalysisOverride> &ranges, quint32 address,
                                      quint32 length)
{
    const quint32 end = address + length;
    QVector<AnalysisOverride> kept;
    for (const AnalysisOverride &range : ranges) {
        const quint32 rangeEnd = range.address + range.length;
        if (rangeEnd <= address || range.address >= end) {
            kept.append(range);
            continue;
        }
        if (range.address < address) {
            AnalysisOverride head = range;
            head.length = address - range.address;
            kept.append(head);
        }
        if (rangeEnd > end) {
            AnalysisOverride tail = range;
            tail.address = end;
            tail.length = rangeEnd - end;
            kept.append(tail);
        }
    }
    return kept;
}

void mergeAdjacent(QVector<AnalysisOverride> *ranges)
{
    std::sort(ranges->begin(), ranges->end(),
              [](const AnalysisOverride &left, const AnalysisOverride &right) {
                  return left.address < right.address;
              });
    QVector<AnalysisOverride> merged;
    for (const AnalysisOverride &range : *ranges) {
        if (!merged.isEmpty() && merged.last().kind == range.kind
            && merged.last().address + merged.last().length == range.address) {
            merged.last().length += range.length;
            continue;
        }
        merged.append(range);
    }
    *ranges = merged;
}

void cover(QVector<AnalysisOverride> *ranges, const AnalysisOverride &range)
{
    *ranges = withoutSpan(*ranges, range.address, range.length);
    ranges->append(range);
    mergeAdjacent(ranges);
}

bool notesEmpty(const AnalysisNotes &notes)
{
    return notes.names.isEmpty() && notes.comments.isEmpty() && notes.overrides.isEmpty();
}

QJsonArray nameArray(const AnalysisNotes &notes)
{
    QList<quint32> addresses = notes.names.keys();
    std::sort(addresses.begin(), addresses.end());
    QJsonArray array;
    for (quint32 address : addresses) {
        QJsonObject object;
        object.insert(QStringLiteral("address"), double(address));
        object.insert(QStringLiteral("name"), notes.names.value(address));
        array.append(object);
    }
    return array;
}

QJsonArray commentArray(const AnalysisNotes &notes)
{
    QList<quint32> addresses = notes.comments.keys();
    std::sort(addresses.begin(), addresses.end());
    QJsonArray array;
    for (quint32 address : addresses) {
        QJsonObject object;
        object.insert(QStringLiteral("address"), double(address));
        object.insert(QStringLiteral("text"), notes.comments.value(address));
        array.append(object);
    }
    return array;
}

QJsonArray overrideArray(const AnalysisNotes &notes)
{
    QJsonArray array;
    for (const AnalysisOverride &range : notes.overrides) {
        QJsonObject object;
        object.insert(QStringLiteral("address"), double(range.address));
        object.insert(QStringLiteral("length"), double(range.length));
        object.insert(QStringLiteral("kind"),
                      range.kind == AnalysisOverride::Kind::Code ? QStringLiteral("code")
                                                                : QStringLiteral("data"));
        array.append(object);
    }
    return array;
}

} // namespace

QString analysisNotesPath(const QString &programPath)
{
    return programPath + QLatin1String(kAnalysisNotesSuffix);
}

bool loadAnalysisNotes(const QString &path, AnalysisNotes *out, QString *error)
{
    if (out)
        *out = {};
    if (!QFileInfo::exists(path))
        return true;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QObject::tr("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QObject::tr("%1: analysis notes are not valid JSON").arg(path);
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toDouble() != 1.0) {
        if (error)
            *error = QObject::tr("%1: analysis notes version is not 1").arg(path);
        return false;
    }

    AnalysisNotes notes;
    const auto fail = [&](const QString &reason) {
        if (error)
            *error = QObject::tr("%1: %2").arg(path, reason);
        return false;
    };
    const auto readEntries = [&](const char *key, bool names) {
        const QJsonValue value = root.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return true;
        if (!value.isArray())
            return fail(QObject::tr("analysis notes %1 are not a list").arg(QLatin1String(key)));
        for (const QJsonValue &entry : value.toArray()) {
            if (!entry.isObject())
                return fail(QObject::tr("analysis notes have an invalid %1").arg(QLatin1String(key)));
            const QJsonObject object = entry.toObject();
            quint32 address = 0;
            if (!readU32(object, QStringLiteral("address"), &address))
                return fail(QObject::tr("analysis notes have an invalid address"));
            const QJsonValue textValue = object.value(names ? QStringLiteral("name") : QStringLiteral("text"));
            if (!textValue.isString())
                return fail(QObject::tr("analysis notes have an invalid %1").arg(QLatin1String(key)));
            const QString text = textValue.toString();
            if (text.isEmpty())
                continue;
            if (names) {
                if (!validName(text))
                    return fail(QObject::tr("analysis notes have an invalid name"));
                notes.names.insert(address, text);
            } else {
                if (!validComment(text))
                    return fail(QObject::tr("analysis notes have an invalid comment"));
                notes.comments.insert(address, text);
            }
        }
        return true;
    };
    if (!readEntries("names", true) || !readEntries("comments", false))
        return false;

    const QJsonValue ranges = root.value(QStringLiteral("overrides"));
    if (!ranges.isUndefined() && !ranges.isNull()) {
        if (!ranges.isArray())
            return fail(QObject::tr("analysis notes overrides are not a list"));
        for (const QJsonValue &entry : ranges.toArray()) {
            if (!entry.isObject())
                return fail(QObject::tr("analysis notes have an invalid range"));
            const QJsonObject object = entry.toObject();
            AnalysisOverride range;
            if (!readU32(object, QStringLiteral("address"), &range.address)
                || !readU32(object, QStringLiteral("length"), &range.length) || !fits(range.address, range.length))
                return fail(QObject::tr("analysis notes have an invalid range"));
            const QString kind = object.value(QStringLiteral("kind")).toString();
            if (kind == QLatin1String("code")) {
                if (range.address & 1u)
                    return fail(QObject::tr("analysis notes have an invalid range"));
                range.kind = AnalysisOverride::Kind::Code;
            } else if (kind == QLatin1String("data")) {
                range.kind = AnalysisOverride::Kind::Data;
            } else {
                return fail(QObject::tr("analysis notes have an invalid range"));
            }
            cover(&notes.overrides, range);
        }
    }

    if (out)
        *out = notes;
    return true;
}

bool saveAnalysisNotes(const QString &path, const AnalysisNotes &notes, QString *error)
{
    if (notesEmpty(notes)) {
        if (!QFileInfo::exists(path))
            return true;
        QFile file(path);
        if (!file.remove()) {
            if (error)
                *error = QObject::tr("Cannot remove %1: %2").arg(path, file.errorString());
            return false;
        }
        return true;
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("names"), nameArray(notes));
    root.insert(QStringLiteral("comments"), commentArray(notes));
    root.insert(QStringLiteral("overrides"), overrideArray(notes));
    return files::write(path, QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

bool setAnalysisName(AnalysisNotes *notes, quint32 address, const QString &name, QString *error)
{
    if (!notes)
        return false;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        notes->names.remove(address);
        return true;
    }
    if (!validName(trimmed)) {
        if (error)
            *error = QObject::tr("A name must be at most 64 characters and start with a letter, "
                                 "'_' or '.', then use only letters, digits, '_', '.' and '$'.");
        return false;
    }
    notes->names.insert(address, trimmed);
    return true;
}

bool setAnalysisComment(AnalysisNotes *notes, quint32 address, const QString &text, QString *error)
{
    if (!notes)
        return false;
    if (text.isEmpty()) {
        notes->comments.remove(address);
        return true;
    }
    if (!validComment(text)) {
        if (error)
            *error = QObject::tr("A comment must be a single line of at most 200 characters.");
        return false;
    }
    notes->comments.insert(address, text);
    return true;
}

bool setAnalysisOverride(AnalysisNotes *notes, quint32 address, quint32 length,
                         AnalysisOverride::Kind kind, QString *error)
{
    if (!notes)
        return false;
    if (!fits(address, length)) {
        if (error)
            *error = QObject::tr("A range needs a length that stays inside the address space.");
        return false;
    }
    if (kind == AnalysisOverride::Kind::Code && (address & 1u)) {
        if (error)
            *error = QObject::tr("A code range starts on an even address.");
        return false;
    }
    AnalysisOverride range;
    range.address = address;
    range.length = length;
    range.kind = kind;
    cover(&notes->overrides, range);
    return true;
}

bool clearAnalysisOverride(AnalysisNotes *notes, quint32 address, quint32 length, QString *error)
{
    if (!notes)
        return false;
    if (!fits(address, length)) {
        if (error)
            *error = QObject::tr("A range needs a length that stays inside the address space.");
        return false;
    }
    notes->overrides = withoutSpan(notes->overrides, address, length);
    return true;
}

} // namespace pist
