// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/EmulatorDisplayWidget.h"

#include <QApplication>
#include <QImage>

#include <QtTest>

class TstDisplay : public QObject
{
    Q_OBJECT

private slots:
    void letterboxesAFrame();
};

void TstDisplay::letterboxesAFrame()
{
    pist::EmulatorDisplayWidget widget;
    widget.resize(200, 100);

    QImage image(20, 20, QImage::Format_RGB32);
    image.fill(qRgb(255, 0, 0));
    widget.setFrame(image);
    widget.show();

    const QImage shot = widget.grab().toImage();
    QVERIFY(!shot.isNull());
    const QColor centre = shot.pixelColor(shot.width() / 2, shot.height() / 2);
    QCOMPARE(centre.red(), 255);
    QCOMPARE(centre.green(), 0);
    QCOMPARE(centre.blue(), 0);
    // The frame is square, so a wide panel leaves black bars on the sides.
    const QColor bar = shot.pixelColor(0, shot.height() / 2);
    QCOMPARE(bar.red(), 0);
    QCOMPARE(bar.green(), 0);
    QCOMPARE(bar.blue(), 0);

    widget.setPaused(true);
    const QImage paused = widget.grab().toImage();
    const QColor still = paused.pixelColor(paused.width() / 2, paused.height() / 2);
    QCOMPARE(still.red(), 255);
    QCOMPARE(still.green(), 0);

    widget.clearEmbedded();
    const QImage empty = widget.grab().toImage();
    QVERIFY(empty.pixelColor(empty.width() / 2, empty.height() / 2).red() != 255);
}

QTEST_MAIN(TstDisplay)
#include "tst_display.moc"
