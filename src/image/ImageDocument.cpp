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

QVector<int> pixelsFromJson(const QJsonArray &array, int expected, qint64 &budget)
{
    // Allocate from the *declared* cell size, so an empty pixels array still
    // demands the full buffer. Bound the running total against the input size
    // *before* allocating: a legitimate file spends at least one JSON byte per
    // pixel, so it can never declare more pixels than it has bytes, while a
    // hostile one can. A negative budget is the reject sentinel fromJson checks.
    if (budget < expected) {
        budget = -1;
        return {};
    }
    budget -= expected;
    QVector<int> pixels(expected, kTransparent);
    const int n = qMin(expected, array.size());
    for (int i = 0; i < n; ++i)
        pixels[i] = array.at(i).toInt(kTransparent);
    return pixels;
}

QJsonObject frameToJson(const ImageFrame &frame)
{
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
    return obj;
}

ImageFrame frameFromJson(const QJsonObject &obj, int cellW, int cellH, qint64 &budget)
{
    const int expected = cellW * cellH;
    ImageFrame frame;
    frame.composite
        = pixelsFromJson(obj.value(QStringLiteral("pixels")).toArray(), expected, budget);
    const QJsonArray layers = obj.value(QStringLiteral("layers")).toArray();
    if (layers.size() > ImageDocument::kMaxLayersPerFrame) {
        budget = -1;
        return frame;
    }
    if (!layers.isEmpty()) {
        for (const QJsonValue &layerValue : layers) {
            const QJsonObject layerObj = layerValue.toObject();
            ImageLayer layer;
            layer.name = layerObj.value(QStringLiteral("name")).toString(QStringLiteral("Layer"));
            layer.visible = layerObj.value(QStringLiteral("visible")).toBool(true);
            layer.pixels = pixelsFromJson(layerObj.value(QStringLiteral("pixels")).toArray(),
                                          expected, budget);
            frame.layers.append(layer);
        }
    } else {
        // A composite-only frame (the v1 shape) becomes a single layer.
        ImageLayer layer;
        layer.name = QStringLiteral("Layer 1");
        layer.pixels = frame.composite;
        frame.layers.append(layer);
    }
    return frame;
}

} // namespace

ImageDocument::ImageDocument()
{
    m_active = defaultActiveIndices(m_kind);
    m_background = m_active.isEmpty() ? 0 : m_active.first();

    ImagePhase phase;
    phase.name = QStringLiteral("Phase 1");
    phase.cellW = 32;
    phase.cellH = 32;
    m_phases.append(phase);
    m_phases.last().frames.append(blankFrame());
}

ImageDocument ImageDocument::create(int width, int height, PaletteKind kind)
{
    ImageDocument doc;
    doc.m_kind = kind;
    doc.m_active = defaultActiveIndices(kind);
    doc.m_background = doc.m_active.isEmpty() ? 0 : doc.m_active.first();

    QString error;
    if (!doc.setPhaseCellSize(0, width, height, &error))
        doc.setPhaseCellSize(0, 32, 32, nullptr);
    doc.m_phases[0].frames = {doc.blankFrame()};
    doc.m_currentPhase = 0;
    doc.m_currentFrame = 0;
    doc.m_activeLayer = 0;
    doc.m_modified = false;
    return doc;
}

ImagePhase &ImageDocument::phase()
{
    return m_phases[m_currentPhase];
}

const ImageFrame *ImageDocument::frameAt(int index) const
{
    const QVector<ImageFrame> &frames = phase().frames;
    if (index < 0 || index >= frames.size())
        return nullptr;
    return &frames.at(index);
}

ImageFrame *ImageDocument::frameAt(int index)
{
    QVector<ImageFrame> &frames = phase().frames;
    if (index < 0 || index >= frames.size())
        return nullptr;
    return &frames[index];
}

const QVector<int> &ImageDocument::frame(int index) const
{
    return phase().frames.at(index).composite;
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

ImageFrame ImageDocument::blankFrame() const
{
    ImageFrame frame;
    frame.layers = {makeLayer(QStringLiteral("Layer 1"))};
    remesh(frame);
    return frame;
}

ImageFrame ImageDocument::blankFrameFrom(const ImageFrame &templateFrame) const
{
    ImageFrame frame;
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

int ImageDocument::mergedPixel(const ImageFrame &frame, int index)
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

void ImageDocument::remesh(ImageFrame &frame) const
{
    const int n = pixelCount();
    frame.composite.resize(n);
    for (int i = 0; i < n; ++i)
        frame.composite[i] = mergedPixel(frame, i);
}

void ImageDocument::remeshCurrent()
{
    remesh(phase().frames[m_currentFrame]);
}

void ImageDocument::clampActiveLayer()
{
    const int n = phase().frames[m_currentFrame].layers.size();
    if (n <= 0)
        phase().frames[m_currentFrame].layers.append(makeLayer(QStringLiteral("Layer 1")));
    if (m_activeLayer >= phase().frames[m_currentFrame].layers.size())
        m_activeLayer = phase().frames[m_currentFrame].layers.size() - 1;
    if (m_activeLayer < 0)
        m_activeLayer = 0;
}

int ImageDocument::resolvedLayer(int layer) const
{
    const int n = phase().frames.at(m_currentFrame).layers.size();
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
    return &phase().frames[m_currentFrame].layers[index];
}

const ImageLayer *ImageDocument::layerAt(int layer) const
{
    const int index = resolvedLayer(layer);
    if (index < 0)
        return nullptr;
    return &phase().frames.at(m_currentFrame).layers[index];
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
    return phase().frames.at(m_currentFrame).layers.size();
}

QVector<int> ImageDocument::resizedPixels(const QVector<int> &pixels, int oldW, int oldH,
                                          int newW, int newH)
{
    QVector<int> out(newW * newH, kTransparent);
    for (int y = 0; y < qMin(oldH, newH); ++y) {
        for (int x = 0; x < qMin(oldW, newW); ++x)
            out[y * newW + x] = pixels.at(y * oldW + x);
    }
    return out;
}

bool ImageDocument::setPhaseCellSize(int phaseIndex, int width, int height, QString *error)
{
    if (phaseIndex < 0 || phaseIndex >= m_phases.size())
        return false;
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
    ImagePhase &phase = m_phases[phaseIndex];
    if (phase.cellW == width && phase.cellH == height)
        return true;
    const int oldW = phase.cellW;
    const int oldH = phase.cellH;
    phase.cellW = width;
    phase.cellH = height;
    for (ImageFrame &frame : phase.frames) {
        for (ImageLayer &layer : frame.layers)
            layer.pixels = resizedPixels(layer.pixels, oldW, oldH, width, height);
        remesh(frame);
    }
    touch();
    return true;
}

void ImageDocument::replaceWith(const ImageDocument &other)
{
    *this = other;
    touch();
}

bool ImageDocument::setCurrentPhase(int index)
{
    if (index < 0 || index >= m_phases.size())
        return false;
    m_currentPhase = index;
    m_currentFrame = 0;
    clampActiveLayer();
    return true;
}

int ImageDocument::addPhase(const QString &name, int cellW, int cellH)
{
    ImagePhase phase;
    phase.name = name.trimmed().isEmpty() ? QStringLiteral("Phase %1").arg(m_phases.size() + 1)
                                          : name.trimmed();
    phase.cellW = qBound(kMinSize, cellW, kMaxWidth);
    phase.cellH = qBound(kMinSize, cellH, kMaxHeight);
    phase.frames.append(blankFrame());
    m_phases.append(phase);
    m_currentPhase = m_phases.size() - 1;
    m_currentFrame = 0;
    clampActiveLayer();
    touch();
    return m_currentPhase;
}

bool ImageDocument::removePhase(int index)
{
    if (index < 0 || index >= m_phases.size() || m_phases.size() <= 1)
        return false;
    m_phases.removeAt(index);
    if (m_currentPhase >= m_phases.size())
        m_currentPhase = m_phases.size() - 1;
    else if (m_currentPhase > index)
        --m_currentPhase; // removing an earlier phase shifts the current one down
    m_currentFrame = 0;
    clampActiveLayer();
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

bool ImageDocument::setPhasePlacement(int index, int sheet, int x, int y)
{
    if (index < 0 || index >= m_phases.size())
        return false;
    if (sheet >= m_sheets.size())
        return false;
    m_phases[index].sheet = sheet;
    m_phases[index].x = x;
    m_phases[index].y = y;
    touch();
    return true;
}

int ImageDocument::addSheet(const QString &path, int width, int height)
{
    ImageSheet sheet;
    sheet.path = path;
    sheet.width = qBound(kMinSize, width, kMaxWidth);
    sheet.height = qBound(kMinSize, height, kMaxHeight);
    m_sheets.append(sheet);
    touch();
    return m_sheets.size() - 1;
}

bool ImageDocument::setCurrentFrame(int index)
{
    if (index < 0 || index >= phase().frames.size())
        return false;
    m_currentFrame = index;
    clampActiveLayer();
    return true;
}

int ImageDocument::addFrame()
{
    const int insertAt = m_currentFrame + 1;
    phase().frames.insert(insertAt, blankFrameFrom(phase().frames.at(m_currentFrame)));
    m_currentFrame = insertAt;
    clampActiveLayer();
    touch();
    return m_currentFrame;
}

int ImageDocument::duplicateFrame(int source)
{
    if (source < 0 || source >= phase().frames.size())
        return -1;
    const int insertAt = m_currentFrame + 1;
    ImageFrame copy = phase().frames.at(source);
    phase().frames.insert(insertAt, copy);
    m_currentFrame = insertAt;
    clampActiveLayer();
    touch();
    return m_currentFrame;
}

bool ImageDocument::removeFrame(int index)
{
    QVector<ImageFrame> &frames = phase().frames;
    if (frames.size() <= 1 || index < 0 || index >= frames.size())
        return false;
    frames.removeAt(index);
    if (m_currentFrame >= frames.size())
        m_currentFrame = frames.size() - 1;
    else if (m_currentFrame > index)
        --m_currentFrame;
    clampActiveLayer();
    touch();
    return true;
}

bool ImageDocument::moveFrame(int from, int to)
{
    QVector<ImageFrame> &frames = phase().frames;
    const int n = frames.size();
    if (from == to || from < 0 || to < 0 || from >= n || to >= n)
        return false;
    const ImageFrame frame = frames.takeAt(from);
    frames.insert(to, frame);
    if (m_currentFrame == from)
        m_currentFrame = to;
    else if (from < to && m_currentFrame > from && m_currentFrame <= to)
        --m_currentFrame;
    else if (to < from && m_currentFrame >= to && m_currentFrame < from)
        ++m_currentFrame;
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
    ImageFrame &frame = phase().frames[m_currentFrame];
    frame.layers.append(makeLayer(QStringLiteral("Layer %1").arg(frame.layers.size() + 1)));
    m_activeLayer = frame.layers.size() - 1;
    remesh(frame);
    touch();
    return m_activeLayer;
}

bool ImageDocument::removeLayer(int index)
{
    ImageFrame &frame = phase().frames[m_currentFrame];
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
    ImageFrame &frame = phase().frames[m_currentFrame];
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
    phase().frames[m_currentFrame].layers[index].name = trimmed;
    touch();
    return true;
}

bool ImageDocument::setLayerVisible(int index, bool visible)
{
    if (index < 0 || index >= layerCount())
        return false;
    phase().frames[m_currentFrame].layers[index].visible = visible;
    remeshCurrent();
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
    for (ImagePhase &phase : m_phases) {
        for (ImageFrame &frame : phase.frames) {
            for (ImageLayer &layer : frame.layers) {
                for (int &pixel : layer.pixels)
                    pixel = remap(pixel);
            }
            remesh(frame);
        }
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
    return replaceActiveLayer(flipData(activeLayerPixels(), width(), height(), direction));
}

bool ImageDocument::shiftActiveLayer(ShiftDirection direction)
{
    return replaceActiveLayer(shiftData(activeLayerPixels(), width(), height(), direction));
}

int ImageDocument::generateRotations(int count)
{
    if (width() != height() || count < 2)
        return 0;
    const ImageFrame source = phase().frames.at(m_currentFrame);
    QVector<ImageFrame> rotated;
    for (int i = 1; i < count; ++i) {
        const double angle = 360.0 / count * i;
        ImageFrame frame;
        for (const ImageLayer &src : source.layers) {
            ImageLayer layer = src;
            layer.pixels = rotateIndexed(src.pixels, width(), angle);
            frame.layers.append(layer);
        }
        remesh(frame);
        rotated.append(frame);
    }
    const int insertAt = m_currentFrame + 1;
    for (int i = 0; i < rotated.size(); ++i)
        phase().frames.insert(insertAt + i, rotated.at(i));
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
    root[QStringLiteral("version")] = 2;
    root[QStringLiteral("palette")] = paletteKindName(m_kind);
    QJsonArray active;
    for (int index : m_active)
        active.append(index);
    root[QStringLiteral("active")] = active;
    root[QStringLiteral("background")] = m_background;

    QJsonArray sheets;
    for (const ImageSheet &sheet : m_sheets) {
        QJsonObject obj;
        obj[QStringLiteral("path")] = sheet.path;
        obj[QStringLiteral("width")] = sheet.width;
        obj[QStringLiteral("height")] = sheet.height;
        sheets.append(obj);
    }
    root[QStringLiteral("sheets")] = sheets;

    QJsonArray phases;
    for (const ImagePhase &phase : m_phases) {
        QJsonObject obj;
        obj[QStringLiteral("name")] = phase.name;
        obj[QStringLiteral("cellW")] = phase.cellW;
        obj[QStringLiteral("cellH")] = phase.cellH;
        obj[QStringLiteral("sheet")] = phase.sheet;
        obj[QStringLiteral("x")] = phase.x;
        obj[QStringLiteral("y")] = phase.y;
        QJsonArray frames;
        for (const ImageFrame &frame : phase.frames)
            frames.append(frameToJson(frame));
        obj[QStringLiteral("frames")] = frames;
        phases.append(obj);
    }
    root[QStringLiteral("phases")] = phases;
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
    if (root.value(QStringLiteral("version")).toInt(0) != 2) {
        const QString message = QStringLiteral("unsupported .pim version (expected 2)");
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    }

    PaletteKind kind = PaletteKind::Ste;
    if (!paletteKindFromName(root.value(QStringLiteral("palette")).toString(), &kind))
        kind = PaletteKind::Ste;

    // A hostile file can declare a huge structure in almost no bytes: each frame
    // allocates from the *declared* cell size, not from the pixels present. Bound
    // the total pixel allocation against the input size (a real file spends at
    // least one JSON byte per pixel) and cap the container counts.
    qint64 pixelBudget = json.size();
    const auto reject = [&](const QString &message) {
        m_lastError = message;
        if (error)
            *error = message;
        return false;
    };

    QVector<ImageSheet> sheets;
    const QJsonArray sheetArray = root.value(QStringLiteral("sheets")).toArray();
    if (sheetArray.size() > kMaxSheets)
        return reject(QStringLiteral(".pim declares too many sheets (%1)").arg(sheetArray.size()));
    for (const QJsonValue &value : sheetArray) {
        const QJsonObject obj = value.toObject();
        ImageSheet sheet;
        sheet.path = obj.value(QStringLiteral("path")).toString();
        sheet.width = qBound(kMinSize, obj.value(QStringLiteral("width")).toInt(320), kMaxWidth);
        sheet.height
            = qBound(kMinSize, obj.value(QStringLiteral("height")).toInt(200), kMaxHeight);
        sheets.append(sheet);
    }

    const QJsonArray phaseArray = root.value(QStringLiteral("phases")).toArray();
    if (phaseArray.size() > kMaxPhases)
        return reject(QStringLiteral(".pim declares too many phases (%1)").arg(phaseArray.size()));
    QVector<ImagePhase> phases;
    for (const QJsonValue &value : phaseArray) {
        const QJsonObject obj = value.toObject();
        ImagePhase phase;
        phase.name = obj.value(QStringLiteral("name")).toString(QStringLiteral("Phase"));
        phase.cellW = obj.value(QStringLiteral("cellW")).toInt(32);
        phase.cellH = obj.value(QStringLiteral("cellH")).toInt(32);
        if (phase.cellW < kMinSize || phase.cellW > kMaxWidth || phase.cellH < kMinSize
            || phase.cellH > kMaxHeight)
            return reject(QStringLiteral("phase %1 has an out-of-range cell size").arg(phase.name));
        // The sheet reference comes straight from the file, but currentSheetIndex()
        // and sheets().at() assume it is in range. -1 means "no sheet"; clamp an
        // out-of-range value to it rather than let it index past the end.
        phase.sheet = obj.value(QStringLiteral("sheet")).toInt(-1);
        if (phase.sheet >= sheets.size())
            phase.sheet = -1;
        phase.x = obj.value(QStringLiteral("x")).toInt(0);
        phase.y = obj.value(QStringLiteral("y")).toInt(0);
        const QJsonArray frameArray = obj.value(QStringLiteral("frames")).toArray();
        if (frameArray.size() > kMaxFramesPerPhase) {
            return reject(QStringLiteral("phase %1 declares too many frames (%2)")
                              .arg(phase.name)
                              .arg(frameArray.size()));
        }
        for (const QJsonValue &frameValue : frameArray) {
            phase.frames.append(
                frameFromJson(frameValue.toObject(), phase.cellW, phase.cellH, pixelBudget));
            if (pixelBudget < 0)
                return reject(QStringLiteral(".pim declares more pixel data than the file holds"));
        }
        if (phase.frames.isEmpty()) {
            // A frame-less phase still needs its composite sized to THIS phase's
            // cell. blankFrame() reads pixelCount() -> phase() -> m_phases, which
            // is still the previous document (the constructor's 32x32) until
            // m_phases is reassigned at the end of fromJson — so it would build a
            // 1024-int buffer for a 320x200 phase, and ImageCanvas::rebuildImage
            // then indexes pixels.at(y*width()+x) up to 63999 on it: OOB in
            // Release. An empty object yields an all-transparent cellW*cellH
            // composite plus the v1 single layer, and it draws on the budget.
            phase.frames.append(
                frameFromJson(QJsonObject(), phase.cellW, phase.cellH, pixelBudget));
            if (pixelBudget < 0)
                return reject(QStringLiteral(".pim declares more pixel data than the file holds"));
        }
        phases.append(phase);
    }
    if (phases.isEmpty())
        return reject(QStringLiteral(".pim file has no phases"));

    QVector<int> active;
    const QJsonArray activeArray = root.value(QStringLiteral("active")).toArray();
    for (const QJsonValue &value : activeArray)
        active.append(value.toInt());

    m_kind = kind;
    m_active = clampActive(active, kind);
    m_background = root.value(QStringLiteral("background")).toInt(0);
    m_sheets = sheets;
    m_phases = phases;
    m_currentPhase = 0;
    m_currentFrame = 0;
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
