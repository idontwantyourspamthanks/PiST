// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the sprite document, ST palettes, paint tools and ST file
// codecs. These always run; they do not need a display or an emulator.

#include "image/ImageDocument.h"
#include "image/Palette.h"
#include "image/StFormats.h"
#include "image/Tools.h"
#include "image/Transform.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

using namespace pist;

class TstImage : public QObject
{
    Q_OBJECT

private slots:
    void cubeSizes();
    void colourWordRoundTrip();
    void stfmVsSteEncoding();
    void defaultActiveHasSixteen();
    void pimRoundTrip();
    void pimRejectsBadSize();
    void pimRejectsPixelAmplification();
    void pimClampsSheetReference();
    void pimEmptyFramesSizedToPhase();
    void removePhaseAdjustsCurrentIndex();
    void activePaletteDeduped();
    void frameIndexClampsToRange();
    void iffRejectsBadPlaneCount();
    void fillAndLineIndices();
    void pi1RoundTrip();
    void neoRoundTrip();
    void stosMbkHeader();
    void iffRoundTrip();
    void assemblerIncludeHasDcW();
    void pngRoundTripOpaque();
    void flipAndShift();
    void regionClearStampMove();
    void rotateNinetyAndBake();
    void layersOccludeAndRoundTrip();
    void onionAndPreviewIndex();
    void spriteSafeDocumentReservesColourZero();
    void phaseCellSizeResizesFrames();
    void phasesOwnTheirFrames();
    void v2PersistsPlacementAndSheets();
    void sheetComposeAndSlice();
    void bitplaneDataLayout();
    void bitplaneDataPlanesAndMask();
    void bitplaneDataPreShift();
    void bitplaneScrollDemo();
    void bitplaneScrollDemoAnimatesFrames();
    void bitplaneDataExportsTheChosenPhase();
    void bitplaneScrollDemoAssembles();
    void scrollDemoHandlesAnOversizeFrameStride();
};

void TstImage::cubeSizes()
{
    QCOMPARE(cubeSize(PaletteKind::Stfm), 512);
    QCOMPARE(cubeSize(PaletteKind::Ste), 4096);
    QCOMPARE(channelLevels(PaletteKind::Stfm).size(), 8);
    QCOMPARE(channelLevels(PaletteKind::Ste).size(), 16);
    QCOMPARE(channelLevels(PaletteKind::Ste).last(), 255);
}

void TstImage::colourWordRoundTrip()
{
    const Rgb white{255, 255, 255};
    QCOMPARE(steColourWord(white), quint16(0x0FFF));
    const Rgb decoded = rgbFromSteWord(0x0FFF);
    QCOMPARE(decoded.r, uchar(255));
    QCOMPARE(decoded.g, uchar(255));
    QCOMPARE(decoded.b, uchar(255));
}

void TstImage::stfmVsSteEncoding()
{
    // #b6b6b6 rounds to an odd 4-bit STe value (extension bits set) and a
    // plain 3-bit STfm word — the same fixture LemonAndLime pins.
    const Rgb grey{0xb6, 0xb6, 0xb6};
    QCOMPARE(stfmColourWord(grey), quint16(0x0555));
    QCOMPARE(steColourWord(grey), quint16(0x0DDD));
}

void TstImage::defaultActiveHasSixteen()
{
    const QVector<int> stfm = defaultActiveIndices(PaletteKind::Stfm);
    const QVector<int> ste = defaultActiveIndices(PaletteKind::Ste);
    QCOMPARE(stfm.size(), 16);
    QCOMPARE(ste.size(), 16);
    QVERIFY(stfm.contains(0));
    QVERIFY(ste.contains(0));
}

void TstImage::pimRoundTrip()
{
    ImageDocument doc = ImageDocument::create(32, 16, PaletteKind::Ste);
    QCOMPARE(doc.width(), 32);
    QCOMPARE(doc.height(), 16);
    QCOMPARE(doc.frameCount(), 1);
    doc.setPixel(0, doc.active().at(1));
    doc.setPixel(1, kTransparent);
    doc.addFrame();
    doc.setPixel(2, doc.active().at(2));
    QCOMPARE(doc.frameCount(), 2);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sprite.pim"));
    QString error;
    QVERIFY2(doc.save(path, &error), qPrintable(error));

    ImageDocument loaded;
    QVERIFY2(loaded.load(path, &error), qPrintable(error));
    QCOMPARE(loaded.width(), 32);
    QCOMPARE(loaded.height(), 16);
    QCOMPARE(loaded.paletteKind(), PaletteKind::Ste);
    QCOMPARE(loaded.frameCount(), 2);
    QCOMPARE(loaded.frame(0).at(0), doc.frame(0).at(0));
    QCOMPARE(loaded.frame(0).at(1), kTransparent);
    QCOMPARE(loaded.frame(1).at(2), doc.frame(1).at(2));
    QVERIFY(loaded.toJson().contains("\"format\":\"pist.image\""));
}

void TstImage::pimRejectsBadSize()
{
    ImageDocument doc = ImageDocument::create(999, 999, PaletteKind::Stfm);
    QCOMPARE(doc.width(), 32);
    QCOMPARE(doc.height(), 32);

    QString error;
    // v2 with an out-of-range cell size is rejected.
    QVERIFY(!doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\","
        "\"active\":[0],\"phases\":[{\"name\":\"p\",\"cellW\":0,\"cellH\":8,"
        "\"frames\":[{\"pixels\":[]}]}]}"), &error));
    QVERIFY(!error.isEmpty());

    // So is the v1 shape: there is no upgrade path.
    QVERIFY(!doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":1,\"width\":8,\"height\":8,"
        "\"palette\":\"ste\",\"active\":[0],\"frames\":[{\"pixels\":[]}]}"), &error));
    QVERIFY(!error.isEmpty());
}

void TstImage::pimRejectsPixelAmplification()
{
    ImageDocument doc;
    QString error;

    // A ~130-byte file declaring one 320x200 frame (64,000 pixels) with an empty
    // pixels array. pixelsFromJson allocates from the *declared* cell size, so
    // this is ~500x amplification; the total-pixel budget (the file's own byte
    // size) rejects it, because a real file spends at least a byte per pixel.
    QVERIFY(!doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[0],"
        "\"phases\":[{\"cellW\":320,\"cellH\":200,\"frames\":[{\"pixels\":[]}]}]}"),
        &error));
    QVERIFY(!error.isEmpty());

    // A structure whose declared pixels fit inside the file's own size still
    // loads: the budget is not a blanket cap on cell size or a small frame count.
    QVERIFY2(doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[0],"
        "\"phases\":[{\"cellW\":4,\"cellH\":4,\"frames\":[{\"pixels\":[]}]}]}"),
        &error),
        qPrintable(error));
    QCOMPARE(doc.frameCount(), 1);

    // Many 1x1 frames pass the budget individually (one int each) but are bounded
    // by the per-phase frame cap, so the count itself is capped, not just volume.
    QByteArray many = QStringLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[0],"
        "\"phases\":[{\"cellW\":1,\"cellH\":1,\"frames\":[")
        .toUtf8();
    for (int i = 0; i <= ImageDocument::kMaxFramesPerPhase; ++i) {
        if (i)
            many += ',';
        many += "{\"pixels\":[]}";
    }
    many += "]}]}";
    QVERIFY(!doc.fromJson(many, &error));
    QVERIFY(!error.isEmpty());
}

void TstImage::pimClampsSheetReference()
{
    ImageDocument doc;
    QString error;
    // A phase may name a sheet index that does not exist, and a sheet may declare
    // a size past the ST screen. Both are read straight from the file; load must
    // clamp them so currentSheetIndex()/sheets().at() can never index out of range
    // (an unchecked .at() is assert-only, so it is UB in Release).
    QVERIFY2(doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[0],"
        "\"sheets\":[{\"path\":\"a.pi1\",\"width\":99999,\"height\":99999}],"
        "\"phases\":[{\"cellW\":4,\"cellH\":4,\"sheet\":99999,\"frames\":[{\"pixels\":[]}]}]}"),
        &error),
        qPrintable(error));
    QCOMPARE(doc.sheets().size(), 1);
    QCOMPARE(doc.phases().at(0).sheet, -1); // out-of-range -> "no sheet"
    QCOMPARE(doc.sheets().at(0).width, 320); // clamped to the ST screen width
    QCOMPARE(doc.sheets().at(0).height, 200); // clamped to the ST screen height

    // A valid sheet reference is left alone.
    QVERIFY2(doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[0],"
        "\"sheets\":[{\"path\":\"a.pi1\",\"width\":16,\"height\":16}],"
        "\"phases\":[{\"cellW\":4,\"cellH\":4,\"sheet\":0,\"frames\":[{\"pixels\":[]}]}]}"),
        &error),
        qPrintable(error));
    QCOMPARE(doc.phases().at(0).sheet, 0);
    QCOMPARE(doc.sheets().at(0).width, 16);
}

void TstImage::pimEmptyFramesSizedToPhase()
{
    // A phase with an empty "frames" array still needs a composite sized to its
    // own cell. The old fallback used blankFrame(), which reads the *previous*
    // document's cell (still the 32x32 constructor default mid-load), so a larger
    // phase got an undersized buffer and ImageCanvas::rebuildImage indexed past it
    // with pixels.at(y*width()+x) — OOB in Release. The invariant rebuildImage
    // relies on is frame size == width()*height(); assert it for a phase bigger
    // than the stale default. The padded name keeps the file over the pixel budget.
    ImageDocument doc;
    QString error;
    QByteArray json = QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[0],"
        "\"phases\":[{\"name\":\"");
    json += QByteArray(1600, 'x');
    json += QByteArrayLiteral("\",\"cellW\":40,\"cellH\":40,\"frames\":[]}]}");
    QVERIFY2(doc.fromJson(json, &error), qPrintable(error));
    QCOMPARE(doc.width(), 40);
    QCOMPARE(doc.height(), 40);
    QCOMPARE(doc.frameCount(), 1);
    QCOMPARE(doc.frame(0).size(), 40 * 40); // not the stale 32*32 = 1024
}

void TstImage::removePhaseAdjustsCurrentIndex()
{
    ImageDocument doc = ImageDocument::create(8, 8, PaletteKind::Ste); // A
    const int b = doc.addPhase(QStringLiteral("B"), 8, 8);
    doc.addPhase(QStringLiteral("C"), 8, 8);
    QCOMPARE(doc.phaseCount(), 3);
    QVERIFY(doc.setCurrentPhase(b)); // current = B (index 1)

    // Remove a phase *before* the current one. The current index must shift down
    // so it still names B. removeFrame() does this; removePhase() used to only
    // clamp the upper bound, so the current selection silently retargeted to the
    // phase that slid into the old index (C here).
    QVERIFY(doc.removePhase(0)); // remove A -> [B, C]
    QCOMPARE(doc.phaseCount(), 2);
    QCOMPARE(doc.currentPhase(), 0);
    QCOMPARE(doc.phases().at(doc.currentPhase()).name, QStringLiteral("B"));
}

void TstImage::activePaletteDeduped()
{
    ImageDocument doc;
    QString error;
    // clampActive must collapse duplicate registers: "active":[5,5,5,7] is two
    // colours, not four, or a crafted file burns palette registers.
    QVERIFY2(doc.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":2,\"palette\":\"ste\",\"active\":[5,5,5,7],"
        "\"phases\":[{\"cellW\":4,\"cellH\":4,\"frames\":[{\"pixels\":[]}]}]}"),
        &error),
        qPrintable(error));
    QCOMPARE(doc.active(), (QVector<int>{5, 7}));
}

void TstImage::frameIndexClampsToRange()
{
    ImageDocument doc = ImageDocument::create(8, 8, PaletteKind::Ste);
    doc.addFrame(); // two frames, the second current
    doc.setPixel(0, doc.active().at(1)); // paint frame 1 only
    QVERIFY(doc.frame(0).at(0) == kTransparent);
    QVERIFY(doc.frame(1).at(0) != kTransparent);
    // Out-of-range indices clamp to a real composite instead of .at() OOB (UB in
    // Release); every single-frame exporter routes through frame().
    QCOMPARE(doc.frame(99), doc.frame(1));
    QCOMPARE(doc.frame(-5), doc.frame(0));
}

void TstImage::iffRejectsBadPlaneCount()
{
    ImageDocument doc = ImageDocument::create(16, 8, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QString error;
    const QByteArray bytes = exportIff(doc, 0, &error);
    QVERIFY(!bytes.isEmpty());
    // BMHD chunk sits at offset 12; nPlanes is BMHD data byte 8 -> file offset 28.
    QVERIFY(bytes.mid(12, 4) == QByteArray("BMHD"));
    const int planesByte = 28;
    QVERIFY(bytes.at(planesByte) >= 1 && bytes.at(planesByte) <= 4); // valid as written
    ImportedSheet sheet;
    for (char bad : {char(0), char(5), char(31)}) {
        QByteArray corrupt = bytes;
        corrupt[planesByte] = bad;
        QVERIFY2(!importIff(corrupt, PaletteKind::Ste, &sheet, &error),
                 qPrintable(QStringLiteral("nPlanes=%1 must be rejected").arg(int(bad))));
        QVERIFY(!error.isEmpty());
    }
    // The untouched 4-plane file still imports.
    QVERIFY2(importIff(bytes, PaletteKind::Ste, &sheet, &error), qPrintable(error));
}

void TstImage::fillAndLineIndices()
{
    Grid grid{4, 4};
    QVector<int> data(16, 0);
    data[5] = 1;
    data[6] = 1;
    data[9] = 1;
    data[10] = 1;
    const QVector<int> filled = fillIndices(5, data, grid);
    QCOMPARE(filled.size(), 4);
    QVERIFY(filled.contains(5));
    QVERIFY(filled.contains(10));

    const QVector<int> line = lineIndices(0, 3, 1, grid);
    QCOMPARE(line, (QVector<int>{0, 1, 2, 3}));

    const QVector<int> brush = brushIndices(5, 1, grid);
    QCOMPARE(brush, (QVector<int>{5}));
}

void TstImage::pi1RoundTrip()
{
    ImageDocument doc = ImageDocument::create(16, 8, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    doc.setPixel(1, doc.active().at(2));
    QString error;
    const QByteArray bytes = exportPi1(doc, 0, &error);
    QCOMPARE(bytes.size(), 32034);

    ImportedSheet sheet;
    QVERIFY2(importPi1(bytes, PaletteKind::Ste, &sheet, &error), qPrintable(error));
    QCOMPARE(sheet.width, 320);
    QCOMPARE(sheet.height, 200);
    QCOMPARE(sheet.pixels.at(0), doc.active().at(1));
    QCOMPARE(sheet.pixels.at(1), doc.active().at(2));
}

void TstImage::neoRoundTrip()
{
    ImageDocument doc = ImageDocument::create(8, 8, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(3));
    QString error;
    const QByteArray bytes = exportNeo(doc, 0, QStringLiteral("SPRITE"), &error);
    QCOMPARE(bytes.size(), 32128);
    QCOMPARE(bytes.mid(36, 6), QByteArray("SPRITE"));

    ImportedSheet sheet;
    QVERIFY2(importNeo(bytes, PaletteKind::Ste, &sheet, &error), qPrintable(error));
    QCOMPARE(sheet.width, 320);
    QCOMPARE(sheet.pixels.at(0), doc.active().at(3));
}

void TstImage::stosMbkHeader()
{
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.addFrame();
    QString error;
    const QByteArray bytes = exportStosMbk(doc, 0, 1, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));
    QVERIFY(bytes.startsWith("Lionpoubnk"));
    QCOMPARE(int(uchar(bytes.at(18))), 0x19);
    QCOMPARE(int(uchar(bytes.at(19))), 0x86);
    // sprite count at body+0x10 = file offset 0x12+0x10 = 0x22
    QCOMPARE(quint16((uchar(bytes.at(0x22)) << 8) | uchar(bytes.at(0x23))), quint16(2));

    QVERIFY(exportStosMbk(ImageDocument::create(20, 16, PaletteKind::Ste), 0, 1, &error).isEmpty());
    QVERIFY(error.contains(QLatin1String("width")));
}

void TstImage::iffRoundTrip()
{
    ImageDocument doc = ImageDocument::create(16, 8, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QString error;
    const QByteArray bytes = exportIff(doc, 0, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));
    QVERIFY(bytes.startsWith("FORM"));
    QVERIFY(bytes.mid(8, 4) == QByteArray("ILBM"));

    ImportedSheet sheet;
    QVERIFY2(importIff(bytes, PaletteKind::Ste, &sheet, &error), qPrintable(error));
    QCOMPARE(sheet.width, 320);
    QCOMPARE(sheet.height, 200);
    QCOMPARE(sheet.pixels.at(0), doc.active().at(1));
}

void TstImage::assemblerIncludeHasDcW()
{
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(2));
    QString error;
    const QByteArray text = exportAssembler(doc, 0, &error);
    QVERIFY2(!text.isEmpty(), qPrintable(error));
    QVERIFY(text.contains("dc.w"));
    QVERIFY(text.contains("palette:"));
    QVERIFY(text.contains("sprite:"));
}

void TstImage::pngRoundTripOpaque()
{
    ImageDocument doc = ImageDocument::create(4, 4, PaletteKind::Ste);
    const int colour = doc.active().at(1);
    doc.fillIndices({0, 1, 2, 3}, colour);
    QString error;
    const QByteArray png = exportPng(doc, 0, &error);
    QVERIFY2(!png.isEmpty(), qPrintable(error));

    ImportedSheet sheet;
    QVERIFY2(importPng(png, PaletteKind::Ste, &sheet, &error), qPrintable(error));
    QCOMPARE(sheet.width, 4);
    QCOMPARE(sheet.height, 4);
    QCOMPARE(sheet.pixels.at(0), colour);
}

void TstImage::flipAndShift()
{
    const QVector<int> data{1, 2, 3, 4, 5, 6};
    QCOMPARE(flipData(data, 3, 2, FlipDirection::Horizontal), (QVector<int>{3, 2, 1, 6, 5, 4}));
    QCOMPARE(flipData(data, 3, 2, FlipDirection::Vertical), (QVector<int>{4, 5, 6, 1, 2, 3}));
    QCOMPARE(shiftData(QVector<int>{1, 2, 3, 4}, 2, 2, ShiftDirection::Left),
             (QVector<int>{2, 1, 4, 3}));
}

void TstImage::regionClearStampMove()
{
    // 3×2 grid, transparent (−1) background.
    const QVector<int> data{1, -1, 3,
                            4, 5, -1};

    // Region extraction is row-major and clips to the grid.
    QCOMPARE(regionData(data, 3, 2, QRect(0, 0, 2, 2)), (QVector<int>{1, -1, 4, 5}));
    QVERIFY(regionData(data, 3, 2, QRect(5, 5, 2, 2)).isEmpty());
    QCOMPARE(regionData(data, 3, 2, QRect(1, 0, 9, 9)), (QVector<int>{-1, 3, 5, -1}));

    // Clearing wipes only the rectangle.
    QCOMPARE(clearRegion(data, 3, 2, QRect(0, 0, 2, 1), 7), (QVector<int>{7, 7, 3, 4, 5, -1}));
    QCOMPARE(clearRegion(data, 3, 2, QRect(0, 0, 3, 2), kTransparent),
             (QVector<int>{-1, -1, -1, -1, -1, -1}));

    // Stamping is transparency-aware: holes keep the destination…
    QCOMPARE(stampRegion(data, 3, 2, QVector<int>{9, 9, 9, kTransparent}, 2, QPoint(1, 0)),
             (QVector<int>{1, 9, 9, 4, 9, -1}));
    // …and clips at the grid edge.
    QCOMPARE(stampRegion(data, 3, 2, QVector<int>{8, 8, 8, 8}, 2, QPoint(2, 1)),
             (QVector<int>{1, -1, 3, 4, 5, 8}));

    // Move = clear the source, then stamp at the offset; untouched pixels stay.
    QCOMPARE(moveRegion(data, 3, 2, QRect(0, 0, 2, 2), QPoint(1, 0)),
             (QVector<int>{-1, 1, 3, -1, 4, 5}));
    // Pixels pushed off the grid are dropped.
    QCOMPARE(moveRegion(data, 3, 2, QRect(0, 0, 2, 2), QPoint(2, 1)),
             (QVector<int>{-1, -1, 3, -1, -1, 1}));
    // A zero delta is a no-op.
    QCOMPARE(moveRegion(data, 3, 2, QRect(0, 0, 2, 2), QPoint(0, 0)), data);
}

void TstImage::rotateNinetyAndBake()
{
    const QVector<int> p3{0, 0, 1, 0, 2, 0, 3, 0, 0};
    QCOMPARE(rotateIndexed(p3, 3, 90), (QVector<int>{3, 0, 0, 0, 2, 0, 0, 0, 1}));
    QCOMPARE(rotateIndexed(QVector<int>{1, 2, 3, 4}, 2, 90), (QVector<int>{3, 1, 4, 2}));
    int outW = 0;
    int outH = 0;
    QCOMPARE(rotate90Cw(QVector<int>{1, 2, 3, 4}, 2, 2, &outW, &outH),
             (QVector<int>{3, 1, 4, 2}));
    QCOMPARE(outW, 2);
    QCOMPARE(outH, 2);

    ImageDocument doc = ImageDocument::create(2, 2, PaletteKind::Ste);
    doc.replaceActiveLayer({1, 2, 3, 4});
    QCOMPARE(doc.generateRotations(4), 3);
    QCOMPARE(doc.frameCount(), 4);
    QCOMPARE(doc.frame(1), (QVector<int>{3, 1, 4, 2}));
}

void TstImage::layersOccludeAndRoundTrip()
{
    ImageDocument doc = ImageDocument::create(2, 1, PaletteKind::Ste);
    QCOMPARE(doc.layerCount(), 1);
    doc.setPixel(0, 1);
    QCOMPARE(doc.addLayer(), 1);
    doc.setPixel(0, 2);
    QCOMPARE(doc.pixels().at(0), 2);
    QVERIFY(doc.setLayerVisible(1, false));
    QCOMPARE(doc.pixels().at(0), 1);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("layers.pim"));
    QString error;
    QVERIFY2(doc.save(path, &error), qPrintable(error));

    ImageDocument loaded;
    QVERIFY2(loaded.load(path, &error), qPrintable(error));
    QCOMPARE(loaded.layerCount(), 2);
    QCOMPARE(loaded.layers().at(1).visible, false);
    QCOMPARE(loaded.pixels().at(0), 1);
    QVERIFY(loaded.toJson().contains("\"layers\""));
}

void TstImage::phaseCellSizeResizesFrames()
{
    ImageDocument doc = ImageDocument::create(4, 4, PaletteKind::Ste);
    doc.replaceActiveLayer({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    doc.addLayer();
    doc.setPixel(0, doc.active().at(3));   // second layer diverges at (0, 0)

    // Grow: new area is transparent on every layer, old content survives.
    QVERIFY(doc.setPhaseCellSize(0, 6, 5));
    QCOMPARE(doc.width(), 6);
    QCOMPARE(doc.height(), 5);
    QCOMPARE(doc.pixels().at(0 * 6 + 0), doc.active().at(3));   // top layer wins
    QCOMPARE(doc.pixels().at(1 * 6 + 1), 6);
    QCOMPARE(doc.pixels().at(4 * 6 + 5), kTransparent);
    QCOMPARE(doc.layerCount(), 2);
    QCOMPARE(doc.layers().at(0).pixels.at(3), 4);
    QCOMPARE(doc.layers().at(0).pixels.at(4 * 6 + 5), kTransparent);

    // Shrink: content outside the new cell is cropped.
    QVERIFY(doc.setPhaseCellSize(0, 2, 2));
    QCOMPARE(doc.pixels(), (QVector<int>{doc.active().at(3), 2, 5, 6}));
    QCOMPARE(doc.layers().at(0).pixels, (QVector<int>{1, 2, 5, 6}));

    // A second phase is untouched.
    doc.addPhase(QStringLiteral("other"), 8, 8);
    QVERIFY(doc.setPhaseCellSize(0, 2, 2));
    QVERIFY(doc.setCurrentPhase(1));
    QCOMPARE(doc.width(), 8);
}

void TstImage::phasesOwnTheirFrames()
{
    ImageDocument doc = ImageDocument::create(8, 8, PaletteKind::Ste);
    doc.addFrame();
    doc.addFrame();
    QCOMPARE(doc.phaseCount(), 1);
    QCOMPARE(doc.frameCount(), 3);

    // A new phase owns its own frames at its own cell size; the two phases
    // are fully independent.
    QCOMPARE(doc.addPhase(QStringLiteral("dragon"), 16, 16), 1);
    QCOMPARE(doc.currentPhase(), 1);
    QCOMPARE(doc.frameCount(), 1);
    QCOMPARE(doc.width(), 16);
    doc.setPixel(0, doc.active().at(1));
    doc.addFrame();
    QCOMPARE(doc.frameCount(), 2);

    // Editing the first phase does not touch the second.
    QVERIFY(doc.setCurrentPhase(0));
    QCOMPARE(doc.frameCount(), 3);
    QCOMPARE(doc.width(), 8);
    QCOMPARE(doc.pixels().at(0), kTransparent);

    // Placement round-trips.
    QCOMPARE(doc.addSheet(QStringLiteral("sheets/chars.pi1"), 320, 200), 0);
    QVERIFY(doc.setPhasePlacement(1, 0, 42, 50));
    QCOMPARE(doc.phases().at(1).sheet, 0);
    QCOMPARE(doc.phases().at(1).x, 42);

    // The last remaining phase cannot be removed.
    QVERIFY(doc.removePhase(1));
    QVERIFY(!doc.removePhase(0));
    QCOMPARE(doc.phaseCount(), 1);
}

void TstImage::onionAndPreviewIndex()
{
    QCOMPARE(neighbourFrames(2, 5).first().index, 1);
    QVERIFY(neighbourFrames(0, 3, -1).isEmpty());
    QCOMPARE(nextPreviewFrame(3, 4, 0, 3), 0);
    QCOMPARE(nextPreviewFrame(5, 8, 2, 5), 2);
    QCOMPARE(nextPreviewFrame(3, 8, 3, 3), 3);
}

void TstImage::spriteSafeDocumentReservesColourZero()
{
    ImageDocument doc = ImageDocument::create(16, 8, PaletteKind::Ste);
    const int red = doc.active().at(0);   // the problem colour: index 0
    const int green = doc.active().at(1);
    doc.setPixel(0, red);
    doc.setPixel(1, green);

    QString error;
    ImageDocument safe = spriteSafeDocument(doc, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(safe.active().first(), 0);      // reserved slot
    QCOMPARE(safe.active().at(1), red);
    QCOMPARE(safe.active().at(2), green);
    QCOMPARE(safe.pixels().at(0), red);      // pixels keep their colour words
    QCOMPARE(safe.pixels().at(1), green);

    // Encoding the safe sheet puts the sprite on indices >= 1, so the file
    // re-imports losslessly with colour 0 read as the background.
    const QByteArray bytes = exportPi1(safe, 0, &error);
    QVERIFY2(!bytes.isEmpty(), qPrintable(error));
    ImportedSheet sheet;
    QVERIFY2(importPi1(bytes, PaletteKind::Ste, &sheet, &error), qPrintable(error));
    QCOMPARE(sheet.active.at(0), 0);
    QCOMPARE(sheet.pixels.at(0), red);
    QCOMPARE(sheet.pixels.at(1), green);

    // A sheet whose first colour paints nothing is already safe.
    ImageDocument plain = ImageDocument::create(8, 8, PaletteKind::Ste);
    plain.setPixel(0, plain.active().at(2));
    ImageDocument untouched = spriteSafeDocument(plain, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(untouched.active(), plain.active());

    // All 16 colours painted: there is no slot to reserve.
    ImageDocument full = ImageDocument::create(16, 1, PaletteKind::Ste);
    for (int i = 0; i < 16; ++i)
        full.setPixel(i, full.active().at(i));
    error.clear();
    spriteSafeDocument(full, &error);
    QVERIFY(!error.isEmpty());
}


void TstImage::v2PersistsPlacementAndSheets()
{
    QString error;
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QCOMPARE(doc.addPhase(QStringLiteral("dragon"), 32, 32), 1);
    doc.setPixel(3, doc.active().at(2));
    QCOMPARE(doc.addSheet(QStringLiteral("sheets/chars.pi1"), 320, 200), 0);
    QVERIFY(doc.setPhasePlacement(1, 0, 42, 50));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("set.pim"));
    QVERIFY2(doc.save(path, &error), qPrintable(error));

    ImageDocument loaded;
    QVERIFY2(loaded.load(path, &error), qPrintable(error));
    QCOMPARE(loaded.phaseCount(), 2);
    QCOMPARE(loaded.sheets().size(), 1);
    QCOMPARE(loaded.sheets().at(0).path, QStringLiteral("sheets/chars.pi1"));
    QCOMPARE(loaded.sheets().at(0).width, 320);
    QVERIFY(loaded.setCurrentPhase(1));
    QCOMPARE(loaded.width(), 32);
    QCOMPARE(loaded.phases().at(1).x, 42);
    QCOMPARE(loaded.phases().at(1).sheet, 0);
    QCOMPARE(loaded.pixels().at(3), doc.active().at(2));

    // The v1 shape is rejected outright: there is no upgrade path.
    error.clear();
    QVERIFY(!loaded.fromJson(QByteArrayLiteral(
        "{\"format\":\"pist.image\",\"version\":1,\"width\":8,\"height\":8,"
        "\"palette\":\"ste\",\"active\":[0],\"frames\":[{\"pixels\":[]}]}"), &error));
    QVERIFY(!error.isEmpty());
}

void TstImage::sheetComposeAndSlice()
{
    ImageDocument doc = ImageDocument::create(4, 4, PaletteKind::Ste);
    QCOMPARE(doc.addSheet(QString(), 8, 4), 0);
    // Phase 0: two 4x4 cells at [0, 0], painted with distinct colours.
    doc.setPixel(0, doc.active().at(1));
    QCOMPARE(doc.addPhase(QStringLiteral("second"), 4, 4), 1);
    doc.setPixel(5, doc.active().at(2));   // top-right pixel of its single cell
    QVERIFY(doc.setPhasePlacement(0, 0, 0, 0));
    QVERIFY(doc.setPhasePlacement(1, 0, 4, 0));

    QString error;
    ImageDocument composed = composeSheet(doc, 0, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(composed.width(), 8);
    QCOMPARE(composed.height(), 4);
    QCOMPARE(composed.pixels().at(0), doc.active().at(1));
    // Phase 1's pixel 5 sits at row 1, col 1 of its cell: [4+1, 0+1] → index 13.
    QCOMPARE(composed.pixels().at(13), doc.active().at(2));
    QCOMPARE(composed.pixels().at(1), kTransparent);

    // Slicing is the inverse: cut [4, 0] 4x4 back out of the composed sheet.
    ImportedSheet sheet;
    sheet.width = 8;
    sheet.height = 4;
    sheet.pixels = composed.pixels();
    const QVector<QVector<int>> cells = sliceSheetCells(sheet, 4, 0, 4, 4, 1);
    QCOMPARE(cells.size(), 1);
    QCOMPARE(cells.at(0).at(5), doc.active().at(2));
    QCOMPARE(cells.at(0).at(0), kTransparent);

    // Unknown sheet index is an error.
    error.clear();
    composeSheet(doc, 3, &error);
    QVERIFY(!error.isEmpty());
}

void TstImage::bitplaneDataLayout()
{
    // Every block on, four pre-shifts: the map is the file, in order.
    BitplaneDataOptions opt;
    opt.masked = true;
    opt.shifted = true;
    opt.shiftedMasked = true;
    const QVector<BitplaneBlock> blocks = bitplaneLayout(16, 16, 1, opt);
    QCOMPARE(blocks.size(), 11);   // palette + sprite + masked + 4 + 4
    QCOMPARE(blocks.at(0).name, QStringLiteral("palette"));
    QCOMPARE(blocks.at(0).offset, 0);
    QCOMPARE(blocks.at(0).bytes, 32);
    QCOMPARE(blocks.at(1).name, QStringLiteral("sprite_f0"));
    QCOMPARE(blocks.at(1).offset, 32);
    QCOMPARE(blocks.at(1).bytes, 16 * 8);        // one 16-pixel group per row
    QCOMPARE(blocks.at(2).name, QStringLiteral("sprite_masked_f0"));
    QCOMPARE(blocks.at(2).offset, 32 + 16 * 8);
    QCOMPARE(blocks.at(2).bytes, 16 * 10);       // mask word per group
    // Pre-shifted copies are one group wider, and all have the same stride.
    QCOMPARE(blocks.at(3).name, QStringLiteral("sprite_shift0_f0"));
    QCOMPARE(blocks.at(3).bytes, 16 * 16);       // two groups per row
    QCOMPARE(blocks.at(4).offset, blocks.at(3).offset + blocks.at(3).bytes);
    QCOMPARE(blocks.at(7).name, QStringLiteral("sprite_masked_shift0_f0"));
    QCOMPARE(blocks.at(7).bytes, 16 * 20);

    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    const QVector<quint16> table = stColourTable(doc.paletteKind(), doc.active());
    QString error;
    const QByteArray data = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(data.size(), blocks.last().offset + blocks.last().bytes);
    for (int i = 0; i < 16; ++i) {
        const quint16 word = quint16((uchar(data.at(i * 2)) << 8) | uchar(data.at(i * 2 + 1)));
        QCOMPARE(word, table.at(i));
    }

    // A width that is not a whole number of groups is padded, not wrapped.
    const QVector<BitplaneBlock> padded = bitplaneLayout(20, 4, 1, opt);
    QCOMPARE(padded.at(1).bytes, 4 * 16);        // 32 px wide → two groups a row

    // Nothing selected, or a pre-shift count the format does not offer, is
    // refused instead of written as an empty or ragged blob.
    BitplaneDataOptions none;
    none.palette = none.sprite = none.masked = false;
    QVERIFY(exportBitplaneData(doc, 0, none, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    BitplaneDataOptions bad = opt;
    bad.preShifts = 3;
    QVERIFY(exportBitplaneData(doc, 0, bad, &error).isEmpty());
    QVERIFY(error.contains(QLatin1String("pre-shift")));
}

void TstImage::bitplaneDataPlanesAndMask()
{
    const auto wordAt = [](const QByteArray &bytes, int offset) {
        return quint16((uchar(bytes.at(offset)) << 8) | uchar(bytes.at(offset + 1)));
    };

    // Colour 1 on the top-left pixel: screen format, plane 0 first, column 0
    // in the top bit.
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    BitplaneDataOptions opt;
    opt.palette = false;
    opt.masked = true;
    QString error;
    const QByteArray data = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(wordAt(data, 0), quint16(0x8000));            // plane 0
    QCOMPARE(wordAt(data, 2), quint16(0));                 // plane 1
    QCOMPARE(wordAt(data, 4), quint16(0));
    QCOMPARE(wordAt(data, 6), quint16(0));
    const int maskBlock = 16 * 8;                          // after the sprite block
    QCOMPARE(wordAt(data, maskBlock), quint16(0x7FFF));    // 15 pixels kept
    QCOMPARE(wordAt(data, maskBlock + 2), quint16(0x8000));

    // The mask keeps the screen where nothing is drawn: an unpainted pixel, or
    // the palette colour the export nominates as the background.
    ImageDocument art = ImageDocument::create(16, 16, PaletteKind::Ste);
    art.setPixel(1, art.active().at(2));    // ST colour 2 → plane 1
    art.setPixel(2, art.active().at(0));    // ST colour 0 → not drawn
    BitplaneDataOptions maskOnly;
    maskOnly.palette = false;
    maskOnly.sprite = false;
    maskOnly.masked = true;
    const QByteArray masked = exportBitplaneData(art, 0, maskOnly, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(wordAt(masked, 0), quint16(0xBFFF));          // only column 1 opaque
    QCOMPARE(wordAt(masked, 2), quint16(0));               // plane 0
    QCOMPARE(wordAt(masked, 4), quint16(0x4000));          // plane 1, column 1

    // Nominating a different colour moves what is left out: with colour 2 as the
    // background, the column painted in it is masked and colour 0 becomes opaque.
    BitplaneDataOptions other = maskOnly;
    other.transparent = 2;
    const QByteArray recoloured = exportBitplaneData(art, 0, other, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(wordAt(recoloured, 0), quint16(0xDFFF));      // columns 0 and 2 opaque
    // A colour left out is absent from the planes as well as from the mask: the
    // blit keeps the screen there and has nothing to OR over it.
    QCOMPARE(wordAt(recoloured, 4), quint16(0));

    // "None" leaves only the unpainted pixels out: every painted colour, colour
    // 0 included, is opaque.
    BitplaneDataOptions opaque = maskOnly;
    opaque.transparent = -1;
    const QByteArray none = exportBitplaneData(art, 0, opaque, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(wordAt(none, 0), quint16(0x9FFF));            // columns 1 and 2 opaque
    QCOMPARE(wordAt(none, 2), quint16(0));                 // colour 0 draws nothing
}

void TstImage::bitplaneDataPreShift()
{
    const auto wordAt = [](const QByteArray &bytes, int offset) {
        return quint16((uchar(bytes.at(offset)) << 8) | uchar(bytes.at(offset + 1)));
    };

    // Two pixels: the leftmost, and the rightmost — the second one only fits
    // in the spare group a pre-shifted copy carries.
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    doc.setPixel(15, doc.active().at(1));
    BitplaneDataOptions opt;
    opt.palette = false;
    opt.sprite = false;
    opt.shifted = true;
    opt.shiftedMasked = true;
    opt.preShifts = 2;                      // two copies, 8 px apart
    QString error;
    const QByteArray data = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));

    const QVector<BitplaneBlock> blocks = bitplaneLayout(16, 16, 1, opt);
    QCOMPARE(blocks.size(), 4);
    QCOMPARE(blocks.at(0).name, QStringLiteral("sprite_shift0_f0"));
    QCOMPARE(blocks.at(0).bytes, 16 * 16);
    QCOMPARE(blocks.at(1).name, QStringLiteral("sprite_shift1_f0"));
    QCOMPARE(blocks.at(2).name, QStringLiteral("sprite_masked_shift0_f0"));
    QCOMPARE(blocks.at(3).name, QStringLiteral("sprite_masked_shift1_f0"));
    QCOMPARE(data.size(), blocks.at(3).offset + blocks.at(3).bytes);

    // Copy 0 is the frame itself, one group wider (the spare group stays empty).
    QCOMPARE(wordAt(data, 0), quint16(0x8001));
    QCOMPARE(wordAt(data, 8), quint16(0));

    // Copy 1 moves both pixels 8 to the right: column 0 → 8 (bit 7 of the
    // first group), column 15 → 23 (bit 8 of the second group).
    const int shift1 = blocks.at(1).offset;
    QCOMPARE(wordAt(data, shift1), quint16(0x0080));
    QCOMPARE(wordAt(data, shift1 + 8), quint16(0x0100));

    // The mask follows the shift, so it is computed at the new position. A
    // masked group is mask,plane0..3 — five words — so the second group's mask
    // is ten bytes past the first's.
    const int masked = blocks.at(3).offset;
    QCOMPARE(wordAt(data, masked), quint16(0xFF7F));
    QCOMPARE(wordAt(data, masked + 2), quint16(0x0080));
    QCOMPARE(wordAt(data, masked + 10), quint16(0xFEFF));
    QCOMPARE(wordAt(data, masked + 12), quint16(0x0100));
}


void TstImage::bitplaneScrollDemo()
{
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    BitplaneDataOptions opt;
    opt.masked = true;
    opt.shifted = true;
    opt.shiftedMasked = true;
    opt.preShifts = 8;
    QString error;
    const QByteArray text = exportScrollDemo(doc, 0, opt, QStringLiteral("sprite.dat"), &error);
    QVERIFY2(!text.isEmpty(), qPrintable(error));

    // A GEMDOS program: supervisor mode first (the ST bus-errors user-mode
    // access to the hardware), then traps for the VBL, the resolution and the
    // keyboard.
    QVERIFY(text.contains("Super(0)"));
    QVERIFY(text.contains("\ttrap\t#14"));
    QVERIFY(text.contains("#37,-(sp)"));
    QVERIFY(text.contains("saved_ssp"));

    // The data is the .dat's, pulled in by the assembler rather than copied
    // into the source, with every block addressed by an equate into it.
    QVERIFY(text.contains("\tincbin\t\"sprite.dat\""));
    const QVector<BitplaneBlock> blocks = bitplaneLayout(16, 16, 1, opt);
    for (const BitplaneBlock &block : blocks) {
        const QByteArray equ = block.name.toLatin1().leftJustified(24) + "equ\tbitplanes+$"
            + QByteArray::number(block.offset, 16).rightJustified(4, '0').toUpper();
        QVERIFY2(text.contains(equ), equ.constData());
    }

    // Driven by the widest block selected: masked, pre-shifted, 8 copies 2 px
    // apart, so the sprite moves 2 px a frame.
    QVERIFY(text.contains("lea\tsprite_masked_shift0_f0,a0"));
    QVERIFY(text.contains("kShifts\t\tequ\t8"));
    QVERIFY(text.contains("kStep\t\tequ\t2"));
    // A 16x16 masked sprite with pre-shifts: rows are two groups wide (the
    // frame plus the spare group), so a copy is 20 bytes a row.
    QVERIFY(text.contains("kSpriteRowBytes\tequ\t20"));
    QVERIFY(text.contains("kCopyStride\tequ\t320"));
    // and says which colour the mask leaves out
    QVERIFY(text.contains("palette colour 0"));

    // The screen is 320x200 pixels of four planes — 32,000 bytes, not 64,000.
    // Clearing pixels' worth ran 32 KB past the screen.
    QVERIFY(text.contains("kScreenRowBytes\tequ\t160"));
    QVERIFY(text.contains("kScreenBytes\tequ\t32000"));

    // The keyboard check is Cconis (11), never Crawio (6): handed no character
    // argument, Crawio answered non-zero on all eight measured calls, so the
    // demo would quit on its first frame and the drain loop would never end.
    QCOMPARE(text.count("#$0b,-(sp)"), 2);   // the drain and the frame loop
    QVERIFY(!text.contains("#6,-(sp)"));

    // A 32-bit screen address built in a register that a `move.w` only half
    // fills: the upper half has to be cleared first, or the sprite is written
    // wherever that garbage points (it looked like "nothing drawn").
    QCOMPARE(text.count("moveq\t#0,d1"), 2);   // the blit and the erase
    QCOMPARE(text.count("moveq\t#0,d2"), 1);   // the frame/copy offset

    // The masked blit has to clear every plane word with the mask, not just the
    // first one, or a sprite over a non-empty background keeps stale bits.
    QCOMPARE(text.count("\tand.w\td3,(a1)"), 4);
    QCOMPARE(text.count("\tor.w\td1,(a1)+"), 4);
    QCOMPARE(text.count("lsl.w\t#3,d1"), 2);   // the blit and the erase
    QVERIFY(!text.contains("lsl.w\t#1,d1"));

    // And the palette address survives the traps in between only if it is
    // loaded again: a0 comes back pointing into the ROM, and writing the
    // colours through it bus-errors. Three loads: the save, the install after
    // the traps, and the restore on the way out.
    QCOMPARE(text.count("lea\t$ffff8240,a0"), 3);

    // A single frame is not animated, so there is no frame arithmetic and no
    // tick counter to decrement.
    QVERIFY(text.contains("kFrames\t\tequ\t1"));
    QVERIFY(!text.contains("kFrameStride"));
    QVERIFY(!text.contains("anim_frame"));

    // Without a masked or pre-shifted block there is nothing to blit: the
    // palette alone is not a sprite.
    BitplaneDataOptions paletteOnly;
    paletteOnly.sprite = false;
    error.clear();
    QVERIFY(exportScrollDemo(doc, 0, paletteOnly, QStringLiteral("d.dat"), &error).isEmpty());
    QVERIFY(error.contains(QLatin1String("sprite block")));

    // A plain sprite still scrolls, a whole group a frame, with no mask words
    // to apply and no shift table to index.
    BitplaneDataOptions plain;
    plain.masked = false;
    error.clear();
    const QByteArray simple = exportScrollDemo(doc, 0, plain, QStringLiteral("d.dat"), &error);
    QVERIFY2(!simple.isEmpty(), qPrintable(error));
    QVERIFY(!simple.contains("\tand.w\td3,(a1)"));
    QVERIFY(!simple.contains("kShifts"));
    QVERIFY(simple.contains("kStep\t\tequ\t16"));
}

void TstImage::bitplaneScrollDemoAnimatesFrames()
{
    // A three-frame phase: the data carries every frame, and the scroller steps
    // between them with a stride rather than a pointer table.
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QVERIFY(doc.addFrame() >= 0);
    QVERIFY(doc.setCurrentFrame(1));
    doc.setPixel(5, doc.active().at(2));
    QVERIFY(doc.addFrame() >= 0);
    QVERIFY(doc.setCurrentFrame(2));
    doc.setPixel(9, doc.active().at(3));
    QCOMPARE(doc.frameCount(), 3);

    BitplaneDataOptions opt;
    opt.sprite = false;
    opt.masked = true;
    opt.shiftedMasked = true;
    opt.preShifts = 4;
    QString error;
    const QVector<BitplaneBlock> blocks = bitplaneLayout(16, 16, 3, opt);
    // palette + 3 frames x (masked + 4 copies)
    QCOMPARE(blocks.size(), 1 + 3 * 5);
    QCOMPARE(blocks.at(1).name, QStringLiteral("sprite_masked_f0"));
    QCOMPARE(blocks.at(6).name, QStringLiteral("sprite_masked_f1"));
    // Every frame is the same size, so frame f is base + f * frameStride.
    const int frameStride = blocks.at(6).offset - blocks.at(1).offset;
    QCOMPARE(blocks.at(11).offset - blocks.at(6).offset, frameStride);
    // The copies of a frame are one copy's size apart; the frames are a whole
    // frame apart, which is more than the chosen block's own bytes whenever
    // another block is selected too.
    const int copyStride = blocks.at(3).offset - blocks.at(2).offset;
    QCOMPARE(blocks.at(2).bytes, copyStride);
    QCOMPARE(frameStride, blocks.at(1).bytes + 4 * copyStride);

    const QByteArray dat = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(!dat.isEmpty(), qPrintable(error));
    QCOMPARE(dat.size(), blocks.last().offset + blocks.last().bytes);
    // Frame 1's copy 0 really is frame 1's pixels: its pixel 5 (colour 2) shows
    // up in plane 1, and frame 0's pixel 5 does not.
    const int frameOne = blocks.at(6).offset;
    const quint16 group = quint16((uchar(dat.at(frameOne + 4)) << 8)
                                  | uchar(dat.at(frameOne + 5)));
    QCOMPARE(group, quint16(1u << (15 - 5)));

    const QByteArray text = exportScrollDemo(doc, 0, opt, QStringLiteral("anim.dat"), &error);
    QVERIFY2(!text.isEmpty(), qPrintable(error));
    QVERIFY(text.contains("kFrames\t\tequ\t3"));
    QVERIFY(text.contains(QStringLiteral("kFrameStride\tequ\t%1").arg(frameStride).toUtf8()));
    QVERIFY(text.contains("kFrameTicks\tequ\t4"));
    QVERIFY(text.contains("move.l\t0(a0,d2.w),d2"));
    QVERIFY(text.contains("anim_frame"));
    QVERIFY(text.contains("\tincbin\t\"anim.dat\""));
}

void TstImage::bitplaneScrollDemoAssembles()
{
    const QString vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (vasm.isEmpty())
        QSKIP("needs vasmm68k_mot");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Deliberately not a multiple of 16, and two frames, so the padding and the
    // animation path are assembled too.
    ImageDocument doc = ImageDocument::create(20, 17, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QVERIFY(doc.addFrame() >= 0);
    QVERIFY(doc.setCurrentFrame(1));
    doc.setPixel(30, doc.active().at(2));
    BitplaneDataOptions opt;
    opt.masked = true;
    opt.shiftedMasked = true;
    opt.preShifts = 4;
    QString error;
    const QByteArray data = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(!data.isEmpty(), qPrintable(error));
    const QByteArray text =
        exportScrollDemo(doc, 0, opt, QStringLiteral("scroll.dat"), &error);
    QVERIFY2(!text.isEmpty(), qPrintable(error));

    // The .s pulls the .dat in with incbin, which is what the user's F7 build
    // does — so write the pair, not just the source.
    QFile dat(dir.filePath(QStringLiteral("scroll.dat")));
    QVERIFY(dat.open(QIODevice::WriteOnly));
    QCOMPARE(dat.write(data), qint64(data.size()));
    dat.close();
    const QString source = dir.filePath(QStringLiteral("scroll.s"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(text), qint64(text.size()));
    file.close();

    QProcess assembler;
    assembler.setWorkingDirectory(dir.path());
    assembler.start(vasm, {QStringLiteral("-Ftos"), QStringLiteral("-o"),
                           dir.filePath(QStringLiteral("scroll.prg")), source});
    QVERIFY(assembler.waitForFinished(30000));
    // Warn-free, not just error-free: vasm warns about a label that shadows a
    // directive name, and a clean assemble is the point of a generated file.
    const QByteArray out = assembler.readAllStandardOutput();
    const QByteArray err = assembler.readAllStandardError();
    QVERIFY2(assembler.exitCode() == 0, err.constData());
    QVERIFY2(!err.contains("warning"), err.constData());
    QVERIFY2(!out.contains("warning"), out.constData());
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("scroll.prg"))));
    // The blob really is inside the program: it is bigger than its own code.
    QVERIFY(QFileInfo(dir.filePath(QStringLiteral("scroll.prg"))).size() > data.size());

    // And from a different working directory, which is how PiST builds: vasm
    // resolves an incbin beside the source it is assembling, so the pair
    // travels together.
    QProcess elsewhere;
    elsewhere.setWorkingDirectory(QDir::tempPath());
    elsewhere.start(vasm, {QStringLiteral("-Ftos"), QStringLiteral("-o"),
                           dir.filePath(QStringLiteral("scroll2.prg")), source});
    QVERIFY(elsewhere.waitForFinished(30000));
    QVERIFY2(elsewhere.exitCode() == 0, elsewhere.readAllStandardError().constData());
}

void TstImage::scrollDemoHandlesAnOversizeFrameStride()
{
    const QString vasm = QStandardPaths::findExecutable(QStringLiteral("vasmm68k_mot"));
    if (vasm.isEmpty())
        QSKIP("needs vasmm68k_mot");

    // A phase big enough that one frame's bytes exceed what a 16-bit `mulu`
    // immediate can hold. The codegen used to emit `mulu #kFrameStride,d2`
    // regardless, so the export reported success and handed the user a source
    // file that could not be assembled.
    ImageDocument doc = ImageDocument::create(160, 80, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QVERIFY(doc.addFrame() >= 0);
    BitplaneDataOptions opt;
    opt.masked = true;
    opt.shiftedMasked = true;
    opt.preShifts = 8;
    QString error;
    const QByteArray data = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(!data.isEmpty(), qPrintable(error));
    const QByteArray text = exportScrollDemo(doc, 0, opt, QStringLiteral("big.dat"), &error);
    QVERIFY2(!text.isEmpty(), qPrintable(error));

    // The stride really is over 16 bits, or this test proves nothing.
    const QByteArray equate = QByteArrayLiteral("kFrameStride\tequ\t");
    const int at = text.indexOf(equate);
    QVERIFY2(at >= 0, "the frame stride equate is emitted");
    const int stride = QByteArray(text.mid(at + equate.size()).split('\t').first()).toInt();
    QVERIFY2(stride > 0xffff, qPrintable(QStringLiteral("frame stride is %1").arg(stride)));
    QVERIFY(!text.contains("mulu\t#kFrameStride"));
    QVERIFY(text.contains("kFrameOffsets"));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile dat(dir.filePath(QStringLiteral("big.dat")));
    QVERIFY(dat.open(QIODevice::WriteOnly));
    QCOMPARE(dat.write(data), qint64(data.size()));
    dat.close();
    const QString source = dir.filePath(QStringLiteral("big.s"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(text), qint64(text.size()));
    file.close();

    QProcess assembler;
    assembler.setWorkingDirectory(dir.path());
    assembler.start(vasm, {QStringLiteral("-Ftos"), QStringLiteral("-o"),
                           dir.filePath(QStringLiteral("big.prg")), source});
    QVERIFY(assembler.waitForFinished(60000));
    const QByteArray err = assembler.readAllStandardError();
    QVERIFY2(assembler.exitCode() == 0, err.constData());
    QVERIFY2(!err.contains("warning"), err.constData());
}

void TstImage::bitplaneDataExportsTheChosenPhase()
{
    // Two phases of different sizes: the export follows the phase it is given,
    // not the one the editor happens to be on.
    ImageDocument doc = ImageDocument::create(16, 16, PaletteKind::Ste);
    doc.setPixel(0, doc.active().at(1));
    QCOMPARE(doc.addPhase(QStringLiteral("wide"), 32, 16), 1);
    doc.setPixel(0, doc.active().at(2));
    QCOMPARE(doc.currentPhase(), 1);

    BitplaneDataOptions opt;
    opt.masked = false;
    opt.sprite = true;
    QString error;
    const QVector<BitplaneBlock> first = bitplaneLayout(16, 16, 1, opt);
    const QVector<BitplaneBlock> second = bitplaneLayout(32, 16, 1, opt);
    QVERIFY(first.last().offset + first.last().bytes
            != second.last().offset + second.last().bytes);

    // Phase 0 is the 16x16 one even though the editor is on phase 1.
    const QByteArray a = exportBitplaneData(doc, 0, opt, &error);
    QVERIFY2(!a.isEmpty(), qPrintable(error));
    QCOMPARE(a.size(), first.last().offset + first.last().bytes);
    const QByteArray b = exportBitplaneData(doc, 1, opt, &error);
    QVERIFY2(!b.isEmpty(), qPrintable(error));
    QCOMPARE(b.size(), second.last().offset + second.last().bytes);
    // The 16x16 phase's first pixel is in the leftmost bit of plane 0 (after the
    // palette block); the 32x8 phase's is one group in...
    const auto wordAt = [](const QByteArray &bytes, int offset) {
        return quint16((uchar(bytes.at(offset)) << 8) | uchar(bytes.at(offset + 1)));
    };
    // Colour 1 is plane 0; colour 2 is plane 1, which is the group's second word.
    const int spriteAt = first.at(1).offset;
    QCOMPARE(wordAt(a, spriteAt), quint16(0x8000));
    QCOMPARE(wordAt(a, spriteAt + 2), quint16(0));
    const int wideAt = second.at(1).offset;
    QCOMPARE(wordAt(b, wideAt), quint16(0));
    QCOMPARE(wordAt(b, wideAt + 2), quint16(0x8000));

    error.clear();
    QVERIFY(exportBitplaneData(doc, 2, opt, &error).isEmpty());
    QVERIFY(!error.isEmpty());
}

QTEST_MAIN(TstImage)
#include "tst_image.moc"