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
    void fillAndLineIndices();
    void pi1RoundTrip();
    void neoRoundTrip();
    void stosMbkHeader();
    void iffRoundTrip();
    void assemblerIncludeHasDcW();
    void pngRoundTripOpaque();
    void flipAndShift();
    void rotateNinetyAndBake();
    void layersOccludeAndRoundTrip();
    void onionAndPreviewIndex();
    void spriteSafeDocumentReservesColourZero();
    void phasesOwnTheirFrames();
    void v2PersistsPlacementAndSheets();
    void sheetComposeAndSlice();
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

QTEST_MAIN(TstImage)
#include "tst_image.moc"
