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

QJsonArray pixelsToJson(const QVector<int> &pixels)
{
    QJsonArray array;
    for (int pixel : pixels)
        array.append(pixel);
    return array;
}

QVector<int> pixelsFromJson(const QJsonArray &array, int expected)
{
    QVector<int> pixels(expected, kTransparent);
    const int n = qMin(expected, array.size());
    for (int i = 0; i < n; ++i)
        pixels[i] = array.at(i).toInt(kTransparent);
    return pixels;
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
    if (!doc.setGeometry(width, height, &error))
        doc.setGeometry(32, 32, nullptr);
    doc.m_frames = {doc.blankFrame()};
    doc.m_current = 0;
    doc.m_activeLayer = 0;
    doc.m_phases.clear();
    doc.m_modified = false;
    return doc;
}

QVector<int> ImageDocument::blankPixels() const
{
    return QVector<int>(pixelCount(), kTransparent);
}

ImageLayer ImageDocument::makeLayer(const QString &name) const
{
    ImageLayer layer;
    layer.name = name;
    layer.visible = true;
    layer.pixels = blankPixels();
    return layer;
}

ImageDocument::Frame ImageDocument::blankFrame() const
{
    Frame frame;
    frame.layers = {makeLayer(QStringLiteral("Layer 1"))};
    remesh(frame);
    return frame;
}

ImageDocument::Frame ImageDocument::blankFrameFrom(const Frame &templateFrame) const
{
    Frame frame;
    if (templateFrame.layers.isEmpty())
        return blankFrame();
    for (const ImageLayer &src : templateFrame.layers) {
        ImageLayer layer = makeLayer(src.name);
        layer.visible = src.visible;
        frame.layers.append(layer);
    }
    remesh(frame);
    return frame;
}

int ImageDocument::mergedPixel(const Frame &frame, int index)
{
    for (int i = frame.layers.size() - 1; i >= 0; --i) {
        const ImageLayer &layer = frame.layers.at(i);
        if (!layer.visible)
            continue;
        if (index < 0 || index >= layer.pixels.size())
            continue;
        const int value = layer.pixels.at(index);
        if (value >= 0)
            return value;
    }
    return kTransparent;
}

void ImageDocument::remesh(Frame &frame) const
{
    const int n = pixelCount();
    frame.composite.resize(n);
    for (int i = 0; i < n; ++i)
        frame.composite[i] = mergedPixel(frame, i);
}

void ImageDocument::remeshCurrent()
{
    remesh(m_frames[m_current]);
}

void ImageDocument::clampActiveLayer()
{
    const int n = m_frames[m_current].layers.size();
    if (n <= 0)
        m_frames[m_current].layers.append(makeLayer(QStringLiteral("Layer 1")));
    if (m_activeLayer >= m_frames[m_current].layers.size())
        m_activeLayer = m_frames[m_current].layers.size() - 1;
    if (m_activeLayer < 0)
        m_activeLayer = 0;
}

int ImageDocument::resolvedLayer(int layer) const
{
    const int n = m_frames.at(m_current).layers.size();
    const int index = layer >= 0 ? layer : m_activeLayer;
    if (index < 0 || index >= n)
        return -1;
    return index;
}

ImageLayer *ImageDocument::layerAt(int layer)
{
    const int index = resolvedLayer(layer);
    if (index < 0)
        return nullptr;
    return &m_frames[m_current].layers[index];
}

const ImageLayer *ImageDocument::layerAt(int layer) const
{
    const int index = resolvedLayer(layer);
    if (index < 0)
        return nullptr;
    return &m_frames[m_current].layers[index];
}

const QVector<int> &ImageDocument::activeLayerPixels() const
{
    const ImageLayer *layer = layerAt(-1);
    if (!layer)
        return pixels();
    return layer->pixels;
}

int ImageDocument::layerCount() const
{
    return m_frames.at(m_current).layers.size();
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
    clampActiveLayer();
    return true;
}

int ImageDocument::addFrame()
{
    const int insertAt = m_current + 1;
    m_frames.insert(insertAt, blankFrameFrom(m_frames.at(m_current)));
    m_phases = insertFramesIntoPhases(m_phases, insertAt, 1);
    m_current = insertAt;
    clampActiveLayer();
    touch();
    return m_current;
}

int ImageDocument::duplicateFrame(int source)
{
    if (source < 0 || source >= m_frames.size())
        return -1;
    const int insertAt = m_current + 1;
    Frame copy = m_frames.at(source);
    m_frames.insert(insertAt, copy);
    m_phases = insertFramesIntoPhases(m_phases, insertAt, 1);
    m_current = insertAt;
    clampActiveLayer();
    touch();
    return m_current;
}

bool ImageDocument::removeFrame(int index)
{
    if (m_frames.size() <= 1 || index < 0 || index >= m_frames.size())
        return false;
    m_frames.removeAt(index);
    m_phases = clampPhases(deleteFrameFromPhases(m_phases, index), m_frames.size());
    if (m_current >= m_frames.size())
        m_current = m_frames.size() - 1;
    else if (m_current > index)
        --m_current;
    clampActiveLayer();
    touch();
    return true;
}

bool ImageDocument::moveFrame(int from, int to)
{
    const int n = m_frames.size();
    if (from == to || from < 0 || to < 0 || from >= n || to >= n)
        return false;
    const Frame frame = m_frames.takeAt(from);
    m_frames.insert(to, frame);
    if (m_current == from)
        m_current = to;
    else if (from < to && m_current > from && m_current <= to)
        --m_current;
    else if (to < from && m_current >= to && m_current < from)
        ++m_current;
    clampActiveLayer();
    touch();
    return true;
}

bool ImageDocument::setActiveLayer(int index)
{
    if (index < 0 || index >= layerCount())
        return false;
    m_activeLayer = index;
    return true;
}

int ImageDocument::addLayer()
{
    Frame &frame = m_frames[m_current];
    frame.layers.append(makeLayer(QStringLiteral("Layer %1").arg(frame.layers.size() + 1)));
    m_activeLayer = frame.layers.size() - 1;
    remesh(frame);
    touch();
    return m_activeLayer;
}

bool ImageDocument::removeLayer(int index)
{
    Frame &frame = m_frames[m_current];
    if (frame.layers.size() <= 1 || index < 0 || index >= frame.layers.size())
        return false;
    frame.layers.removeAt(index);
    if (m_activeLayer > index)
        --m_activeLayer;
    else if (m_activeLayer == index)
        m_activeLayer = qMin(index, frame.layers.size() - 1);
    remesh(frame);
    touch();
    return true;
}

bool ImageDocument::moveLayer(int from, int to)
{
    Frame &frame = m_frames[m_current];
    const int n = frame.layers.size();
    if (from == to || from < 0 || to < 0 || from >= n || to >= n)
        return false;
    const ImageLayer layer = frame.layers.takeAt(from);
    frame.layers.insert(to, layer);
    if (m_activeLayer == from)
        m_activeLayer = to;
    else if (from < to && m_activeLayer > from && m_activeLayer <= to)
        --m_activeLayer;
    else if (to < from && m_activeLayer >= to && m_activeLayer < from)
        ++m_activeLayer;
    remesh(frame);
    touch();
    return true;
}

bool ImageDocument::renameLayer(int index, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || index < 0 || index >= layerCount())
        return false;
    m_frames[m_current].layers[index].name = trimmed;
    touch();
    return true;
}

bool ImageDocument::setLayerVisible(int index, bool visible)
{
    if (index < 0 || index >= layerCount())
        return false;
    m_frames[m_current].layers[index].visible = visible;
    remeshCurrent();
    touch();
    return true;
}

int ImageDocument::addPhase()
{
    ImagePhase phase;
    phase.name = QStringLiteral("Phase %1").arg(m_phases.size() + 1);
    phase.start = m_current;
    phase.end = m_current;
    m_phases.append(phase);
    touch();
    return m_phases.size() - 1;
}

bool ImageDocument::removePhase(int index)
{
    if (index < 0 || index >= m_phases.size())
        return false;
    m_phases.removeAt(index);
    touch();
    return true;
}

bool ImageDocument::renamePhase(int index, const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || index < 0 || index >= m_phases.size())
        return false;
    m_phases[index].name = trimmed;
    touch();
    return true;
}

bool ImageDocument::setPhaseRange(int index, int start, int end)
{
    if (index < 0 || index >= m_phases.size() || m_frames.isEmpty())
        return false;
    const int last = m_frames.size() - 1;
    const int lo = qBound(0, qMin(start, end), last);
    const int hi = qBound(0, qMax(start, end), last);
    m_phases[index].start = lo;
    m_phases[index].end = hi;
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
    for (Frame &frame : m_frames) {
        for (ImageLayer &layer : frame.layers) {
            for (int &pixel : layer.pixels)
                pixel = remap(pixel);
        }
        remesh(frame);
    }
    m_kind = kind;
    m_active = clampActive(nextActive, kind);
    m_background = remap(m_background);
    touch();
}

void ImageDocument::setPixel(int index, int cubeIndex)
{
    fillIndices({index}, cubeIndex);
}

void ImageDocument::fillIndices(const QVector<int> &indices, int cubeIndex, int layer)
{
    ImageLayer *target = layerAt(layer);
    if (!target)
        return;
    for (int index : indices) {
        if (index >= 0 && index < target->pixels.size())
            target->pixels[index] = cubeIndex;
    }
    remeshCurrent();
    touch();
}

void ImageDocument::restoreIndices(const QVector<int> &indices, const QVector<int> &values, int layer)
{
    ImageLayer *target = layerAt(layer);
    if (!target)
        return;
    const int n = qMin(indices.size(), values.size());
    for (int i = 0; i < n; ++i) {
        const int index = indices.at(i);
        if (index >= 0 && index < target->pixels.size())
            target->pixels[index] = values.at(i);
    }
    remeshCurrent();
    touch();
}

bool ImageDocument::replaceActiveLayer(const QVector<int> &pixels)
{
    ImageLayer *target = layerAt(-1);
    if (!target || pixels.size() != pixelCount())
        return false;
    target->pixels = pixels;
    remeshCurrent();
    touch();
    return true;
}

bool ImageDocument::flipActiveLayer(FlipDirection direction)
{
    return replaceActiveLayer(flipData(activeLayerPixels(), m_width, m_height, direction));
}

bool ImageDocument::shiftActiveLayer(ShiftDirection direction)
{
    return replaceActiveLayer(shiftData(activeLayerPixels(), m_width, m_height, direction));
}

int ImageDocument::generateRotations(int count)
{
    if (m_width != m_height || count < 2)
        return 0;
    const Frame source = m_frames.at(m_current);
    QVector<Frame> rotated;
    for (int i = 1; i < count; ++i) {
        const double angle = 360.0 / count * i;
        Frame frame;
        for (const ImageLayer &src : source.layers) {
            ImageLayer layer = src;
            layer.pixels = rotateIndexed(src.pixels, m_width, angle);
            frame.layers.append(layer);
        }
        remesh(frame);
        rotated.append(frame);
    }
    const int insertAt = m_current + 1;
    for (int i = 0; i < rotated.size(); ++i)
        m_frames.insert(insertAt + i, rotated.at(i));
    m_phases = insertFramesIntoPhases(m_phases, insertAt, rotated.size());
    touch();
    return rotated.size();
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
    for (const Frame &frame : m_frames) {
        QJsonObject obj;
        obj[QStringLiteral("pixels")] = pixelsToJson(frame.composite);
        QJsonArray layers;
        for (const ImageLayer &layer : frame.layers) {
            QJsonObject layerObj;
            layerObj[QStringLiteral("name")] = layer.name;
            layerObj[QStringLiteral("visible")] = layer.visible;
            layerObj[QStringLiteral("pixels")] = pixelsToJson(layer.pixels);
            layers.append(layerObj);
        }
        obj[QStringLiteral("layers")] = layers;
        frames.append(obj);
    }
    root[QStringLiteral("frames")] = frames;
    if (!m_phases.isEmpty()) {
        QJsonArray phases;
        for (const ImagePhase &phase : m_phases) {
            QJsonObject obj;
            obj[QStringLiteral("name")] = phase.name;
            obj[QStringLiteral("start")] = phase.start;
            obj[QStringLiteral("end")] = phase.end;
            phases.append(obj);
        }
        root[QStringLiteral("phases")] = phases;
    }
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
    QVector<Frame> frames;
    const QJsonArray frameArray = root.value(QStringLiteral("frames")).toArray();
    for (const QJsonValue &value : frameArray) {
        const QJsonObject obj = value.toObject();
        const QVector<int> composite = pixelsFromJson(obj.value(QStringLiteral("pixels")).toArray(),
                                                      expected);
        Frame frame;
        const QJsonArray layers = obj.value(QStringLiteral("layers")).toArray();
        if (!layers.isEmpty()) {
            for (const QJsonValue &layerValue : layers) {
                const QJsonObject layerObj = layerValue.toObject();
                ImageLayer layer;
                layer.name = layerObj.value(QStringLiteral("name")).toString(QStringLiteral("Layer"));
                layer.visible = layerObj.value(QStringLiteral("visible")).toBool(true);
                layer.pixels = pixelsFromJson(layerObj.value(QStringLiteral("pixels")).toArray(),
                                              expected);
                frame.layers.append(layer);
            }
        } else {
            ImageLayer layer = makeLayer(QStringLiteral("Layer 1"));
            layer.pixels = composite;
            frame.layers.append(layer);
        }
        remesh(frame);
        frames.append(frame);
    }
    if (frames.isEmpty()) {
        const QString message = QStringLiteral(".pim file has no frames");
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }

    QVector<ImagePhase> phases;
    const QJsonArray phaseArray = root.value(QStringLiteral("phases")).toArray();
    for (const QJsonValue &value : phaseArray) {
        const QJsonObject obj = value.toObject();
        ImagePhase phase;
        phase.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Phase"));
        phase.start = obj.value(QStringLiteral("start")).toInt(0);
        phase.end = obj.value(QStringLiteral("end")).toInt(0);
        phases.append(phase);
    }

    m_kind = kind;
    m_active = clampActive(active, kind);
    m_background = root.value(QStringLiteral("background")).toInt(0);
    m_frames = frames;
    m_phases = clampPhases(phases, m_frames.size());
    m_current = 0;
    m_activeLayer = 0;
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
