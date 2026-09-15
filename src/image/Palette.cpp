// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "image/Palette.h"

#include <QtMath>

#include <cmath>
#include <limits>

namespace pist {

QString paletteKindName(PaletteKind kind)
{
    return kind == PaletteKind::Ste ? QStringLiteral("ste") : QStringLiteral("stfm");
}

bool paletteKindFromName(const QString &name, PaletteKind *kind)
{
    if (!kind)
        return false;
    if (name.compare(QLatin1String("ste"), Qt::CaseInsensitive) == 0) {
        *kind = PaletteKind::Ste;
        return true;
    }
    if (name.compare(QLatin1String("stfm"), Qt::CaseInsensitive) == 0) {
        *kind = PaletteKind::Stfm;
        return true;
    }
    return false;
}

int channelCount(PaletteKind kind)
{
    return kind == PaletteKind::Ste ? 16 : 8;
}

int cubeSize(PaletteKind kind)
{
    const int n = channelCount(kind);
    return n * n * n;
}

QVector<int> channelLevels(PaletteKind kind)
{
    const int n = channelCount(kind);
    QVector<int> levels(n);
    for (int i = 0; i < n; ++i) {
        if (kind == PaletteKind::Ste)
            levels[i] = i * 17;
        else
            levels[i] = int(std::round(i * 255.0 / 7.0));
    }
    return levels;
}

namespace {

QVector<Rgb> makeCube(PaletteKind kind)
{
    const QVector<int> levels = channelLevels(kind);
    const int n = levels.size();
    QVector<Rgb> colours;
    colours.reserve(n * n * n);
    for (int r = 0; r < n; ++r) {
        for (int g = 0; g < n; ++g) {
            for (int b = 0; b < n; ++b)
                colours.append(Rgb{quint8(levels[r]), quint8(levels[g]), quint8(levels[b])});
        }
    }
    return colours;
}

const QVector<Rgb> &cachedCube(PaletteKind kind)
{
    static const QVector<Rgb> stfm = makeCube(PaletteKind::Stfm);
    static const QVector<Rgb> ste = makeCube(PaletteKind::Ste);
    return kind == PaletteKind::Ste ? ste : stfm;
}

int nearestLevelIndex(int value, const QVector<int> &levels)
{
    int best = 0;
    int bestDist = std::abs(levels[0] - value);
    for (int i = 1; i < levels.size(); ++i) {
        const int d = std::abs(levels[i] - value);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

} // namespace

const QVector<Rgb> &cubeColours(PaletteKind kind)
{
    return cachedCube(kind);
}

Rgb cubeRgb(PaletteKind kind, int index)
{
    const QVector<Rgb> &cube = cachedCube(kind);
    if (index < 0 || index >= cube.size())
        return {};
    return cube.at(index);
}

QColor cubeColor(PaletteKind kind, int index)
{
    return cubeRgb(kind, index).toColor();
}

int snapChannel(PaletteKind kind, int value)
{
    const QVector<int> levels = channelLevels(kind);
    return levels.at(nearestLevelIndex(qBound(0, value, 255), levels));
}

int nearestCubeIndex(PaletteKind kind, Rgb rgb)
{
    const QVector<int> levels = channelLevels(kind);
    const int n = levels.size();
    const int r = nearestLevelIndex(rgb.r, levels);
    const int g = nearestLevelIndex(rgb.g, levels);
    const int b = nearestLevelIndex(rgb.b, levels);
    return r * n * n + g * n + b;
}

int nearestCubeIndex(PaletteKind kind, const QColor &color)
{
    return nearestCubeIndex(kind, Rgb{quint8(color.red()), quint8(color.green()),
                                      quint8(color.blue())});
}

QVector<int> defaultActiveIndices(PaletteKind kind)
{
    static const int kHues[6][3] = {
        {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1},
    };

    const QVector<Rgb> &cube = cachedCube(kind);
    QVector<int> picked;
    QVector<char> used(cube.size(), 0);

    auto nearest = [&](int tr, int tg, int tb) {
        int best = -1;
        qint64 bestDist = std::numeric_limits<qint64>::max();
        for (int i = 0; i < cube.size(); ++i) {
            if (used[i])
                continue;
            const qint64 dr = cube[i].r - tr;
            const qint64 dg = cube[i].g - tg;
            const qint64 db = cube[i].b - tb;
            const qint64 d = dr * dr + dg * dg + db * db;
            if (d < bestDist) {
                bestDist = d;
                best = i;
            }
        }
        return best;
    };

    const int slotCount = kMaxActive - 1;
    const int cycles = qMax(1, int(std::ceil(slotCount / 6.0)));
    for (int c = 0; c < cycles && picked.size() < slotCount; ++c) {
        const double level = double(cycles - c) / double(cycles);
        for (const auto &hue : kHues) {
            if (picked.size() >= slotCount)
                break;
            const int i = nearest(int(hue[0] * 255 * level), int(hue[1] * 255 * level),
                                  int(hue[2] * 255 * level));
            if (i >= 0) {
                picked.append(i);
                used[i] = 1;
            }
        }
    }

    const int black = nearest(0, 0, 0);
    if (black >= 0)
        picked.append(black);
    return picked;
}

int stColourIndex(int cubeIndex, const QVector<int> &active)
{
    if (cubeIndex < 0)
        return 0;
    const int pos = active.indexOf(cubeIndex);
    if (pos < 0)
        return 0;
    return pos & 0xF;
}

quint16 stfmColourWord(Rgb rgb)
{
    const int r = int(std::round(rgb.r / 255.0 * 7.0));
    const int g = int(std::round(rgb.g / 255.0 * 7.0));
    const int b = int(std::round(rgb.b / 255.0 * 7.0));
    return quint16((r << 8) | (g << 4) | b);
}

quint16 steColourWord(Rgb rgb)
{
    const int r = int(std::round(rgb.r / 255.0 * 15.0));
    const int g = int(std::round(rgb.g / 255.0 * 15.0));
    const int b = int(std::round(rgb.b / 255.0 * 15.0));
    return quint16(((r & 1) << 11) | ((r >> 1) << 8) | ((g & 1) << 7) | ((g >> 1) << 4)
                   | ((b & 1) << 3) | (b >> 1));
}

quint16 colourWord(PaletteKind kind, Rgb rgb)
{
    return kind == PaletteKind::Ste ? steColourWord(rgb) : stfmColourWord(rgb);
}

Rgb rgbFromSteWord(quint16 word)
{
    auto decode4 = [](int high3, int lsb) { return (high3 << 1) | lsb; };
    const int r4 = decode4((word >> 8) & 0x7, (word >> 11) & 0x1);
    const int g4 = decode4((word >> 4) & 0x7, (word >> 7) & 0x1);
    const int b4 = decode4((word >> 0) & 0x7, (word >> 3) & 0x1);
    auto to8 = [](int v4) { return quint8(int(std::round(v4 / 15.0 * 255.0))); };
    return Rgb{to8(r4), to8(g4), to8(b4)};
}

Rgb rgbFromStfmWord(quint16 word)
{
    const int r = (word >> 8) & 0x7;
    const int g = (word >> 4) & 0x7;
    const int b = word & 0x7;
    auto to8 = [](int v) { return quint8(int(std::round(v / 7.0 * 255.0))); };
    return Rgb{to8(r), to8(g), to8(b)};
}

QVector<quint16> stColourTable(PaletteKind kind, const QVector<int> &active)
{
    QVector<quint16> table(kMaxActive, 0);
    for (int i = 0; i < kMaxActive && i < active.size(); ++i)
        table[i] = colourWord(kind, cubeRgb(kind, active.at(i)));
    return table;
}

} // namespace pist
