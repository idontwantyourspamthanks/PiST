// SPDX-License-Identifier: GPL-2.0-or-later
//
// Unit tests for the sprite document, ST palettes, paint tools and ST file
// codecs. These always run; they do not need a display or an emulator.

#include "image/ImageDocument.h"
#include "image/Palette.h"
#include "image/StFormats.h"
#include "image/Tools.h"

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
    QVERIFY(!doc.fromJson(QByteArray("{\"format\":\"pist.image\",\"width\":0,\"height\":8,"
                                     "\"palette\":\"ste\",\"active\":[0],\"frames\":[{\"pixels\":[]}]}"),
                          &error));
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

QTEST_MAIN(TstImage)
#include "tst_image.moc"
