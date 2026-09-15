// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "image/ImageDocument.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace pist {

namespace {

QVector<int> clampActive(const QVector<int> &indices, PaletteKind kind)
{
    const int maxIndex = cubeSize(kind) - 1;
    QVector<int> out;
    for (int index : indices) {
        if (index < 0 || index > maxIndex)
            continue;
        if (out.size() >= kMaxActive)
            break;
        out.append(index);
    }
    if (out.isEmpty())
        out = defaultActiveIndices(kind);
    return out;
}

} // namespace

ImageDocument::ImageDocument()
{
    m_active = defaultActiveIndices(m_kind);
    m_background = m_active.isEmpty() ? 0 : m_active.first();
    m_frames.append(blankFrame());
}

ImageDocument ImageDocument::create(int width, int height, PaletteKind kind)
{
    ImageDocument doc;
    doc.m_kind = kind;
    doc.m_active = defaultActiveIndices(kind);
    doc.m_background = doc.m_active.isEmpty() ? 0 : doc.m_active.first();
    QString error;
    if (!doc.setGeometry(width, height, &error)) {
        doc.setGeometry(32, 32, nullptr);
    }
    doc.m_frames = {doc.blankFrame()};
    doc.m_current = 0;
    doc.m_modified = false;
    return doc;
}

QVector<int> ImageDocument::blankFrame() const
{
    return QVector<int>(pixelCount(), kTransparent);
}

bool ImageDocument::setGeometry(int width, int height, QString *error)
{
    if (width < kMinSize || width > kMaxWidth || height < kMinSize || height > kMaxHeight) {
        const QString message = QStringLiteral("image size must be between %1×%2 and %3×%4")
                                    .arg(kMinSize)
                                    .arg(kMinSize)
                                    .arg(kMaxWidth)
                                    .arg(kMaxHeight);
        if (error)
            *error = message;
        m_lastError = message;
        return false;
    }
    m_width = width;
    m_height = height;
    return true;
}

void ImageDocument::replaceWith(const ImageDocument &other)
{
    *this = other;
    touch();
}

bool ImageDocument::setCurrentFrame(int index)
{
    if (index < 0 || index >= m_frames.size())
        return false;
    m_current = index;
    return true;
}

int ImageDocument::addFrame()
{
    m_frames.append(blankFrame());
    m_current = m_frames.size() - 1;
    touch();
    return m_current;
}

int ImageDocument::duplicateFrame(int source)
{
    if (source < 0 || source >= m_frames.size())
        return -1;
    m_frames.append(m_frames.at(source));
    m_current = m_frames.size() - 1;
    touch();
    return m_current;
}

bool ImageDocument::removeFrame(int index)
{
    if (m_frames.size() <= 1 || index < 0 || index >= m_frames.size())
        return false;
    m_frames.removeAt(index);
    if (m_current >= m_frames.size())
        m_current = m_frames.size() - 1;
    else if (m_current > index)
        --m_current;
    touch();
    return true;
}

void ImageDocument::setActive(const QVector<int> &indices)
{
    m_active = clampActive(indices, m_kind);
    touch();
}

bool ImageDocument::toggleActive(int cubeIndex)
{
    if (cubeIndex < 0 || cubeIndex >= cubeSize(m_kind))
        return false;
    const int pos = m_active.indexOf(cubeIndex);
    if (pos >= 0) {
        if (m_active.size() <= 1)
            return false;
        m_active.removeAt(pos);
        touch();
        return true;
    }
    if (m_active.size() >= kMaxActive)
        return false;
    m_active.append(cubeIndex);
    touch();
    return true;
}

void ImageDocument::setBackground(int cubeIndex)
{
    m_background = cubeIndex;
    touch();
}

void ImageDocument::setPaletteKind(PaletteKind kind)
{
    if (kind == m_kind)
        return;
    auto remap = [this, kind](int index) {
        if (index < 0)
            return index;
        return nearestCubeIndex(kind, cubeRgb(m_kind, index));
    };
    QVector<int> nextActive;
    for (int index : m_active) {
        const int mapped = remap(index);
        if (!nextActive.contains(mapped) && nextActive.size() < kMaxActive)
            nextActive.append(mapped);
    }
    for (QVector<int> &frame : m_frames) {
        for (int &pixel : frame)
            pixel = remap(pixel);
    }
    m_kind = kind;
    m_active = clampActive(nextActive, kind);
    m_background = remap(m_background);
    touch();
}

void ImageDocument::setPixel(int index, int cubeIndex)
{
    if (index < 0 || index >= pixelCount())
        return;
    m_frames[m_current][index] = cubeIndex;
    touch();
}

void ImageDocument::fillIndices(const QVector<int> &indices, int cubeIndex)
{
    QVector<int> &frame = m_frames[m_current];
    for (int index : indices) {
        if (index >= 0 && index < frame.size())
            frame[index] = cubeIndex;
    }
    touch();
}

void ImageDocument::restoreIndices(const QVector<int> &indices, const QVector<int> &values)
{
    QVector<int> &frame = m_frames[m_current];
    const int n = qMin(indices.size(), values.size());
    for (int i = 0; i < n; ++i) {
        const int index = indices.at(i);
        if (index >= 0 && index < frame.size())
            frame[index] = values.at(i);
    }
    touch();
}

QVector<int> ImageDocument::overspill() const
{
    const QSet<int> activeSet(m_active.begin(), m_active.end());
    QSet<int> extra;
    for (int pixel : pixels()) {
        if (pixel >= 0 && !activeSet.contains(pixel))
            extra.insert(pixel);
    }
    QVector<int> out(extra.begin(), extra.end());
    std::sort(out.begin(), out.end());
    return out;
}

QColor ImageDocument::displayColor(int cubeIndex) const
{
    if (cubeIndex < 0)
        return QColor(Qt::transparent);
    return cubeColor(m_kind, cubeIndex);
}

QByteArray ImageDocument::toJson() const
{
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("pist.image");
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("width")] = m_width;
    root[QStringLiteral("height")] = m_height;
    root[QStringLiteral("palette")] = paletteKindName(m_kind);
    QJsonArray active;
    for (int index : m_active)
        active.append(index);
    root[QStringLiteral("active")] = active;
    root[QStringLiteral("background")] = m_background;
    QJsonArray frames;
    for (const QVector<int> &frame : m_frames) {
        QJsonObject obj;
        QJsonArray pixels;
        for (int pixel : frame)
            pixels.append(pixel);
        obj[QStringLiteral("pixels")] = pixels;
        frames.append(obj);
    }
    root[QStringLiteral("frames")] = frames;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool ImageDocument::fromJson(const QByteArray &json, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        const QString message = QStringLiteral("not a valid .pim file: %1")
                                    .arg(parseError.errorString());
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String("pist.image")) {
        const QString message = QStringLiteral("not a PiST image (missing format pist.image)");
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }

    PaletteKind kind = PaletteKind::Ste;
    if (!paletteKindFromName(root.value(QStringLiteral("palette")).toString(), &kind))
        kind = PaletteKind::Ste;

    QString geoError;
    if (!setGeometry(root.value(QStringLiteral("width")).toInt(),
                     root.value(QStringLiteral("height")).toInt(), &geoError)) {
        if (error)
            *error = geoError;
        return false;
    }

    QVector<int> active;
    const QJsonArray activeArray = root.value(QStringLiteral("active")).toArray();
    for (const QJsonValue &value : activeArray)
        active.append(value.toInt());

    const int expected = pixelCount();
    QVector<QVector<int>> frames;
    const QJsonArray frameArray = root.value(QStringLiteral("frames")).toArray();
    for (const QJsonValue &value : frameArray) {
        const QJsonArray pixels = value.toObject().value(QStringLiteral("pixels")).toArray();
        QVector<int> frame(expected, kTransparent);
        const int n = qMin(expected, pixels.size());
        for (int i = 0; i < n; ++i)
            frame[i] = pixels.at(i).toInt(kTransparent);
        frames.append(frame);
    }
    if (frames.isEmpty()) {
        const QString message = QStringLiteral(".pim file has no frames");
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }

    m_kind = kind;
    m_active = clampActive(active, kind);
    m_background = root.value(QStringLiteral("background")).toInt(0);
    m_frames = frames;
    m_current = 0;
    m_modified = false;
    return true;
}

bool ImageDocument::load(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString message = QStringLiteral("cannot read '%1': %2").arg(path, file.errorString());
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }
    return fromJson(file.readAll(), error);
}

bool ImageDocument::save(const QString &path, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString message = QStringLiteral("cannot write '%1': %2").arg(path, file.errorString());
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }
    const QByteArray json = toJson();
    if (file.write(json) != json.size()) {
        const QString message = QStringLiteral("could not write '%1': %2").arg(path, file.errorString());
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }
    return true;
}

} // namespace pist
