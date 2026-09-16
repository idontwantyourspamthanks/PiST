// SPDX-License-Identifier: GPL-2.0-or-later
//
// PiST - an IDE for Atari ST assembly development

#include "ui/Appearance.h"

#include <QApplication>
#include <QCursor>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QTransform>
#include <QColor>

#include <cmath>

namespace pist {
namespace appearance {

namespace {

qreal dpr()
{
    return qApp ? qApp->devicePixelRatio() : 1.0;
}

QColor ink()
{
    return QApplication::palette().color(QPalette::WindowText);
}

QColor accent()
{
    return QApplication::palette().color(QPalette::Highlight);
}

QPen stroke(const QColor &color, qreal width)
{
    QPen p(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    return p;
}

void paintOpen(QPainter &p, const QRectF &r, qreal w)
{
    QPainterPath path;
    const qreal x = r.left() + r.width() * 0.12;
    const qreal y = r.top() + r.height() * 0.28;
    const qreal fw = r.width() * 0.76;
    const qreal fh = r.height() * 0.52;
    path.moveTo(x, y + fh * 0.15);
    path.lineTo(x, y + fh);
    path.lineTo(x + fw, y + fh);
    path.lineTo(x + fw, y + fh * 0.22);
    path.lineTo(x + fw * 0.55, y + fh * 0.22);
    path.lineTo(x + fw * 0.42, y);
    path.lineTo(x, y);
    path.closeSubpath();
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void paintSave(QPainter &p, const QRectF &r, qreal w)
{
    const QRectF tray(r.left() + r.width() * 0.18, r.top() + r.height() * 0.62,
                      r.width() * 0.64, r.height() * 0.18);
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(tray.left(), tray.bottom()), QPointF(tray.right(), tray.bottom()));
    p.drawLine(QPointF(tray.left(), tray.top()), QPointF(tray.left(), tray.bottom()));
    p.drawLine(QPointF(tray.right(), tray.top()), QPointF(tray.right(), tray.bottom()));
    const qreal cx = r.center().x();
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.16),
               QPointF(cx, r.top() + r.height() * 0.58));
    const qreal ah = r.width() * 0.16;
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.58),
               QPointF(cx - ah, r.top() + r.height() * 0.58 - ah));
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.58),
               QPointF(cx + ah, r.top() + r.height() * 0.58 - ah));
}

void paintSettings(QPainter &p, const QRectF &r, qreal w)
{
    const QPointF c = r.center();
    const qreal outer = qMin(r.width(), r.height()) * 0.48;
    const qreal hub = outer * 0.64;
    const qreal hole = outer * 0.26;
    constexpr int teeth = 8;
    constexpr qreal pi = 3.14159265358979323846;

    QPainterPath cog;
    cog.setFillRule(Qt::OddEvenFill);
    for (int i = 0; i < teeth; ++i) {
        const qreal mid = (i / qreal(teeth)) * 2 * pi - pi / 2;
        const qreal step = pi / teeth;
        const qreal tip = step * 0.40;
        auto pt = [&](qreal a, qreal rad) {
            return QPointF(c.x() + std::cos(a) * rad, c.y() + std::sin(a) * rad);
        };
        const QPointF p0 = pt(mid - step, hub);
        if (i == 0)
            cog.moveTo(p0);
        else
            cog.lineTo(p0);
        cog.lineTo(pt(mid - tip, outer));
        cog.lineTo(pt(mid + tip, outer));
        cog.lineTo(pt(mid + step, hub));
    }
    cog.closeSubpath();
    cog.addEllipse(c, hole, hole);

    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(cog);
    Q_UNUSED(w);
}

void paintBuild(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF brick1(r.left() + r.width() * 0.18, r.top() + r.height() * 0.52,
                        r.width() * 0.38, r.height() * 0.22);
    const QRectF brick2(r.left() + r.width() * 0.44, r.top() + r.height() * 0.28,
                        r.width() * 0.38, r.height() * 0.22);
    p.drawRoundedRect(brick1, 1, 1);
    p.drawRoundedRect(brick2, 1, 1);
    p.drawLine(brick2.center(), QPointF(brick2.center().x(), r.top() + r.height() * 0.16));
}

void paintRun(QPainter &p, const QRectF &r, qreal w)
{
    QPainterPath tri;
    tri.moveTo(r.left() + r.width() * 0.30, r.top() + r.height() * 0.20);
    tri.lineTo(r.left() + r.width() * 0.30, r.top() + r.height() * 0.80);
    tri.lineTo(r.left() + r.width() * 0.80, r.center().y());
    tri.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(accent());
    p.drawPath(tri);
    Q_UNUSED(w);
}

void paintStop(QPainter &p, const QRectF &r, qreal w)
{
    const QRectF sq(r.left() + r.width() * 0.26, r.top() + r.height() * 0.26,
                    r.width() * 0.48, r.height() * 0.48);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xe0, 0x5a, 0x4a));
    p.drawRoundedRect(sq, w, w);
}

void paintPause(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    const qreal bw = r.width() * 0.14;
    const qreal h = r.height() * 0.52;
    const qreal y = r.top() + r.height() * 0.24;
    p.drawRoundedRect(QRectF(r.left() + r.width() * 0.30, y, bw, h), 1, 1);
    p.drawRoundedRect(QRectF(r.left() + r.width() * 0.56, y, bw, h), 1, 1);
    Q_UNUSED(w);
}

void paintContinue(QPainter &p, const QRectF &r, qreal w)
{
    paintRun(p, r, w);
}

void paintStep(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const qreal cx = r.center().x();
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.18),
               QPointF(cx, r.top() + r.height() * 0.72));
    const qreal ah = r.width() * 0.18;
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.72),
               QPointF(cx - ah, r.top() + r.height() * 0.72 - ah));
    p.drawLine(QPointF(cx, r.top() + r.height() * 0.72),
               QPointF(cx + ah, r.top() + r.height() * 0.72 - ah));
}

void paintStepOver(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    QPainterPath arc;
    const QRectF box(r.left() + r.width() * 0.18, r.top() + r.height() * 0.22,
                     r.width() * 0.64, r.height() * 0.50);
    arc.moveTo(box.left(), box.bottom());
    arc.quadTo(QPointF(box.center().x(), box.top() - r.height() * 0.08),
               QPointF(box.right(), box.bottom()));
    p.drawPath(arc);
    const qreal ah = r.width() * 0.14;
    p.drawLine(QPointF(box.right(), box.bottom()),
               QPointF(box.right() - ah, box.bottom() - ah * 0.2));
    p.drawLine(QPointF(box.right(), box.bottom()),
               QPointF(box.right() - ah * 0.2, box.bottom() - ah));
}

void paintClearBreakpoints(QPainter &p, const QRectF &r, qreal w)
{
    const QPointF c = r.center();
    const qreal rad = r.width() * 0.28;
    p.setPen(stroke(QColor(0xe0, 0x5a, 0x4a), w));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, rad, rad);
    const qreal o = rad * 0.55;
    p.drawLine(QPointF(c.x() - o, c.y() - o), QPointF(c.x() + o, c.y() + o));
}

/// Outline brush from docs/paint-brush-svgrepo-com.svg (viewBox 24²).
QPainterPath paintBrushGlyph()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.moveTo(22.016800, 3.873910);
    p.lineTo(21.427416, 5.455402);
    p.lineTo(20.694500, 7.152280);
    p.cubicTo(20.016400, 8.607180, 19.083300, 10.308100, 17.967300, 11.481900);
    p.cubicTo(16.901800, 12.602500, 15.293900, 13.634900, 14.006100, 14.368200);
    p.cubicTo(14.337100, 15.962600, 13.883800, 17.687200, 12.646400, 18.924600);
    p.cubicTo(10.376100, 21.195000, 7.840190, 21.195400, 5.953300, 20.634500);
    p.cubicTo(4.944112, 20.334440, 3.937735, 19.849214, 3.088665, 19.204629);
    p.lineTo(2.224130, 18.486800);
    p.cubicTo(2.026030, 18.319100, 2.038560, 18.011700, 2.250940, 17.862500);
    p.lineTo(2.583069, 17.625774);
    p.cubicTo(3.310915, 17.095233, 4.061794, 16.443673, 4.241220, 15.546500);
    p.cubicTo(4.355642, 14.620883, 4.421192, 14.116383, 4.437870, 14.033000);
    p.cubicTo(4.575670, 13.344000, 4.867270, 12.561600, 5.575360, 11.853500);
    p.cubicTo(6.812710, 10.616200, 8.537240, 10.162900, 10.131500, 10.493800);
    p.cubicTo(10.864900, 9.205990, 11.897300, 7.598030, 13.018000, 6.532540);
    p.cubicTo(14.191700, 5.416560, 15.892600, 4.483460, 17.347500, 3.805300);
    p.lineTo(19.044406, 3.072383);
    p.lineTo(20.625900, 2.483010);
    p.cubicTo(21.489400, 2.188070, 22.311700, 3.010440, 22.016800, 3.873910);
    p.closeSubpath();
    p.moveTo(6.989570, 13.267800);
    p.cubicTo(6.637000, 13.620300, 6.483690, 14.001900, 6.399030, 14.425200);
    p.cubicTo(6.284597, 15.350950, 6.219050, 15.855483, 6.202390, 15.938800);
    p.cubicTo(6.021600, 16.842700, 5.493240, 17.588600, 5.052840, 18.087000);
    p.cubicTo(5.457130, 18.313700, 5.960350, 18.550000, 6.523240, 18.717400);
    p.cubicTo(7.906720, 19.128700, 9.613470, 19.129100, 11.232200, 17.510400);
    p.cubicTo(12.403800, 16.338800, 12.403800, 14.439300, 11.232200, 13.267800);
    p.cubicTo(10.060600, 12.096200, 8.161150, 12.096200, 6.989570, 13.267800);
    p.closeSubpath();
    p.moveTo(11.979200, 11.292600);
    p.cubicTo(12.213600, 11.457100, 12.437000, 11.644100, 12.646400, 11.853500);
    p.cubicTo(12.855800, 12.062900, 13.042700, 12.286300, 13.207200, 12.520600);
    p.cubicTo(13.598500, 12.293800, 14.002200, 12.047700, 14.397000, 11.788900);
    p.lineTo(14.366500, 11.721600);
    p.cubicTo(14.250600, 11.474300, 14.047700, 11.133500, 13.707000, 10.792800);
    p.cubicTo(13.415057, 10.500857, 13.122967, 10.310082, 12.889737, 10.188546);
    p.lineTo(12.710900, 10.102900);
    p.cubicTo(12.452100, 10.497600, 12.206000, 10.901300, 11.979200, 11.292600);
    p.closeSubpath();
    p.moveTo(19.421500, 5.078270);
    p.cubicTo(19.038400, 5.236980, 18.622100, 5.417790, 18.192500, 5.618050);
    p.cubicTo(16.783700, 6.274710, 15.326300, 7.097520, 14.396100, 7.981980);
    p.cubicTo(14.239000, 8.131310, 14.081700, 8.296780, 13.925400, 8.474750);
    p.cubicTo(14.288200, 8.675470, 14.707900, 8.965270, 15.121300, 9.378640);
    p.cubicTo(15.534600, 9.791970, 15.824400, 10.211700, 16.025100, 10.574400);
    p.cubicTo(16.203000, 10.418100, 16.368500, 10.260800, 16.517800, 10.103800);
    p.cubicTo(17.402300, 9.173520, 18.225100, 7.716120, 18.881800, 6.307320);
    p.cubicTo(19.082000, 5.877690, 19.262800, 5.461440, 19.421500, 5.078270);
    p.closeSubpath();
    return p;
}

QPainterPath paintBrushHeadGlyph()
{
    QPainterPath p;
    p.moveTo(6.989570, 13.267800);
    p.cubicTo(6.637000, 13.620300, 6.483690, 14.001900, 6.399030, 14.425200);
    p.cubicTo(6.284597, 15.350950, 6.219050, 15.855483, 6.202390, 15.938800);
    p.cubicTo(6.021600, 16.842700, 5.493240, 17.588600, 5.052840, 18.087000);
    p.cubicTo(5.457130, 18.313700, 5.960350, 18.550000, 6.523240, 18.717400);
    p.cubicTo(7.906720, 19.128700, 9.613470, 19.129100, 11.232200, 17.510400);
    p.cubicTo(12.403800, 16.338800, 12.403800, 14.439300, 11.232200, 13.267800);
    p.cubicTo(10.060600, 12.096200, 8.161150, 12.096200, 6.989570, 13.267800);
    p.closeSubpath();
    return p;
}

void paintBrushTinted(QPainter &p, const QRectF &r, qreal w, const QColor &paint)
{
    Q_UNUSED(w);
    const qreal s = qMin(r.width(), r.height()) / 24.0;
    QTransform xf;
    xf.translate(r.center().x() - 12.0 * s, r.center().y() - 12.0 * s);
    xf.scale(s, s);
    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(xf.map(paintBrushGlyph()));
    if (paint.alpha() > 0) {
        p.setBrush(paint);
        p.drawPath(xf.map(paintBrushHeadGlyph()));
    }
}

void paintBrush(QPainter &p, const QRectF &r, qreal w)
{
    paintBrushTinted(p, r, w, QColor());
}

void paintLine(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QPointF(r.left() + r.width() * 0.22, r.top() + r.height() * 0.78),
               QPointF(r.left() + r.width() * 0.78, r.top() + r.height() * 0.22));
}

void paintRect(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(r.width() * 0.18, r.height() * 0.22, -r.width() * 0.18,
                          -r.height() * 0.22));
}

void paintRoundRect(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF box = r.adjusted(r.width() * 0.18, r.height() * 0.22, -r.width() * 0.18,
                                  -r.height() * 0.22);
    p.drawRoundedRect(box, box.width() * 0.28, box.height() * 0.28);
}

void paintEllipse(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(r.adjusted(r.width() * 0.18, r.height() * 0.22, -r.width() * 0.18,
                             -r.height() * 0.22));
}

/// Phosphor-style bucket from docs/paint-bucket-fill-svgrepo-com.svg (viewBox 256²).
QPainterPath paintBucketGlyph()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.moveTo(115.5147, 124.4917);
    p.cubicTo(112.0892, 121.0667, 111.0591, 115.8891, 112.9126, 111.4137);
    p.cubicTo(114.7661, 106.9384, 119.1554, 104.0053, 123.9994, 104.0052);
    p.cubicTo(128.8434, 104.0050, 133.2329, 106.9378, 135.0866, 111.4131);
    p.cubicTo(136.9404, 115.8883, 135.9106, 121.0660, 132.4854, 124.4912);
    p.cubicTo(127.8271, 129.1386, 120.1732, 129.1388, 115.5147, 124.4917);
    p.closeSubpath();
    p.moveTo(43.5147, 24.2070);
    p.cubicTo(41.4962, 22.1884, 38.5446, 21.3974, 35.7872, 22.1361);
    p.cubicTo(33.0298, 22.8748, 30.8689, 25.0355, 30.1299, 27.7928);
    p.cubicTo(29.3910, 30.5502, 30.1818, 33.5019, 32.2002, 35.5205);
    p.lineTo(58.4228, 61.7432);
    p.lineTo(69.7369, 50.4292);
    p.closeSubpath();
    p.moveTo(233.6572, 158.3438);
    p.cubicTo(232.1576, 156.8437, 230.1211, 156.0000, 228.0000, 156.0000);
    p.cubicTo(225.8789, 156.0000, 223.8424, 156.8437, 222.3428, 158.3438);
    p.cubicTo(221.4297, 159.2563, 200.0000, 180.9580, 200.0000, 204.0000);
    p.cubicTo(200.0000, 219.3603, 212.6397, 232.0000, 228.0000, 232.0000);
    p.cubicTo(243.3603, 232.0000, 256.0000, 219.3603, 256.0000, 204.0000);
    p.cubicTo(256.0000, 180.9580, 234.5703, 159.2563, 233.6572, 158.3438);
    p.closeSubpath();
    p.moveTo(230.9648, 123.4844);
    p.cubicTo(230.9649, 121.3636, 230.1216, 119.3274, 228.6221, 117.8276);
    p.lineTo(121.1377, 10.3433);
    p.cubicTo(118.0341, 7.2402, 112.9269, 7.2402, 109.8232, 10.3433);
    p.lineTo(69.7368, 50.4292);
    p.lineTo(110.6858, 91.3782);
    p.cubicTo(122.8608, 84.7962, 138.1808, 88.1794, 146.4516, 99.2765);
    p.cubicTo(154.7224, 110.3735, 153.5866, 126.0215, 143.7998, 135.8079);
    p.cubicTo(134.0131, 145.5942, 118.3650, 146.7294, 107.2683, 138.4581);
    p.cubicTo(96.1716, 130.1868, 92.7891, 114.8666, 99.3716, 102.6919);
    p.lineTo(58.4228, 61.7432);
    p.lineTo(13.6562, 106.5098);
    p.cubicTo(4.3621, 115.8265, 4.3621, 131.1340, 13.6562, 140.4507);
    p.lineTo(98.5137, 225.3076);
    p.cubicTo(103.0128, 229.8070, 109.1215, 232.3374, 115.4844, 232.3374);
    p.cubicTo(121.8472, 232.3374, 127.9560, 229.8070, 132.4551, 225.3076);
    p.lineTo(228.6221, 129.1411);
    p.cubicTo(230.1216, 127.6413, 230.9649, 125.6052, 230.9648, 123.4844);
    p.closeSubpath();
    return p;
}

void paintFill(QPainter &p, const QRectF &r, qreal w)
{
    Q_UNUSED(w);
    const qreal s = qMin(r.width(), r.height()) / 256.0;
    QTransform xf;
    xf.translate(r.center().x() - 128.0 * s, r.center().y() - 128.0 * s);
    xf.scale(s, s);
    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(xf.map(paintBucketGlyph()));
}

/// Solid bulb, outlined tube and drop from docs/dropper-2-svgrepo-com.svg (viewBox 512²).
QPainterPath dropperGlyph()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.moveTo(224.658, 54.926);
    p.cubicTo(213.618, 46.916, 198.162, 49.38, 190.159, 60.42);
    p.cubicTo(184.361, 68.408, 188.295, 62.981, 184.813, 67.777);
    p.cubicTo(172.869, 84.243, 154.464, 79.039, 147.657, 58.905);
    p.cubicTo(143.492, 46.551, 133.543, 25.185, 118.057, 13.945);
    p.cubicTo(85.339, -9.781, 39.585, -2.498, 15.857, 30.212);
    p.cubicTo(-7.87, 62.914, -0.586, 108.677, 32.124, 132.403);
    p.cubicTo(47.61, 143.644, 71.01, 146.457, 84.048, 146.591);
    p.cubicTo(105.303, 146.814, 115.964, 162.693, 104.018, 179.159);
    p.cubicTo(100.536, 183.955, 104.471, 178.528, 98.68, 186.517);
    p.cubicTo(90.67, 197.557, 93.127, 213.021, 104.174, 221.024);
    p.cubicTo(115.206, 229.034, 130.663, 226.576, 138.681, 215.53);
    p.lineTo(230.152, 89.434);
    p.cubicTo(238.161, 78.394, 235.697, 62.937, 224.658, 54.926);
    p.closeSubpath();
    p.moveTo(451.082, 365.643);
    p.cubicTo(451.068, 365.643, 448.773, 363.973, 443.889, 360.432);
    p.cubicTo(439.011, 356.89, 431.557, 351.486, 421.26, 344.01);
    p.cubicTo(393.524, 323.772, 369.916, 318.108, 350.472, 314.522);
    p.cubicTo(340.739, 312.696, 332.082, 311.27, 323.976, 308.812);
    p.cubicTo(315.847, 306.348, 308.186, 302.97, 299.796, 296.904);
    p.cubicTo(280.858, 283.17, 248.816, 259.918, 221.495, 240.096);
    p.cubicTo(207.842, 230.192, 195.37, 221.142, 186.313, 214.572);
    p.cubicTo(177.248, 208.002, 171.629, 203.919, 171.613, 203.904);
    p.lineTo(158.22, 222.36);
    p.cubicTo(158.287, 222.419, 248.541, 287.891, 286.402, 315.36);
    p.cubicTo(296.959, 323.043, 307.382, 327.646, 317.39, 330.653);
    p.cubicTo(332.461, 335.137, 346.196, 336.355, 360.3, 339.925);
    p.cubicTo(374.443, 343.488, 389.343, 349.079, 407.858, 362.471);
    p.cubicTo(428.452, 377.415, 437.68, 384.105, 437.68, 384.105);
    p.lineTo(451.082, 365.643);
    p.closeSubpath();
    p.moveTo(244.027, 135.009);
    p.cubicTo(271.199, 154.727, 329.106, 196.725, 357.51, 217.333);
    p.cubicTo(365.884, 223.429, 371.482, 229.657, 376.353, 236.621);
    p.cubicTo(383.643, 247, 389.048, 259.472, 397.882, 273.63);
    p.cubicTo(406.701, 287.743, 419.144, 303.096, 439.998, 318.181);
    p.cubicTo(450.295, 325.643, 457.757, 331.047, 462.634, 334.595);
    p.cubicTo(467.518, 338.129, 469.813, 339.807, 469.82, 339.807);
    p.lineTo(483.22, 321.351);
    p.cubicTo(483.22, 321.344, 473.984, 314.647, 453.398, 299.711);
    p.cubicTo(441.067, 290.765, 432.559, 282.22, 425.936, 273.905);
    p.cubicTo(416.011, 261.448, 410.28, 249.251, 403.048, 236.429);
    p.cubicTo(395.868, 223.69, 386.804, 210.357, 370.902, 198.879);
    p.cubicTo(333.024, 171.395, 242.712, 105.886, 242.712, 105.886);
    p.lineTo(229.326, 124.357);
    p.cubicTo(229.394, 124.4, 234.999, 128.461, 244.027, 135.009);
    p.closeSubpath();
    p.moveTo(472.389, 399.748);
    p.cubicTo(472.181, 399.748, 434.697, 453.483, 434.697, 474.308);
    p.cubicTo(434.697, 495.124, 451.572, 512, 472.389, 512);
    p.cubicTo(493.206, 512, 510.088, 495.125, 510.088, 474.308);
    p.cubicTo(510.088, 453.484, 472.604, 399.748, 472.389, 399.748);
    p.closeSubpath();
    return p;
}

void paintEyedropper(QPainter &p, const QRectF &r, qreal w)
{
    Q_UNUSED(w);
    const qreal s = qMin(r.width(), r.height()) / 512.0;
    QTransform xf;
    xf.translate(r.center().x() - 256.0 * s, r.center().y() - 256.0 * s);
    xf.scale(s, s);
    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(xf.map(dropperGlyph()));
}

/// Rounded undo arrow from docs/undo-left-round-svgrepo-com.svg (viewBox 24²).
QPainterPath undoGlyph()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.moveTo(7.5303, 3.4697);
    p.cubicTo(7.8232, 3.7626, 7.8232, 4.2374, 7.5303, 4.5303);
    p.lineTo(5.8107, 6.25);
    p.lineTo(15, 6.25);
    p.cubicTo(18.1756, 6.25, 20.75, 8.8244, 20.75, 12);
    p.cubicTo(20.75, 15.1756, 18.1756, 17.75, 15, 17.75);
    p.lineTo(8, 17.75);
    p.cubicTo(7.5858, 17.75, 7.25, 17.4142, 7.25, 17);
    p.cubicTo(7.25, 16.5858, 7.5858, 16.25, 8, 16.25);
    p.lineTo(15, 16.25);
    p.cubicTo(17.3472, 16.25, 19.25, 14.3472, 19.25, 12);
    p.cubicTo(19.25, 9.6528, 17.3472, 7.75, 15, 7.75);
    p.lineTo(5.8107, 7.75);
    p.lineTo(7.5303, 9.4697);
    p.cubicTo(7.8232, 9.7626, 7.8232, 10.2374, 7.5303, 10.5303);
    p.cubicTo(7.2374, 10.8232, 6.7626, 10.8232, 6.4697, 10.5303);
    p.lineTo(3.4697, 7.5303);
    p.cubicTo(3.1768, 7.2374, 3.1768, 6.7626, 3.4697, 6.4697);
    p.lineTo(6.4697, 3.4697);
    p.cubicTo(6.7626, 3.1768, 7.2374, 3.1768, 7.5303, 3.4697);
    p.closeSubpath();
    return p;
}

void paintUndo(QPainter &p, const QRectF &r, qreal w)
{
    Q_UNUSED(w);
    const qreal s = qMin(r.width(), r.height()) / 24.0;
    QTransform xf;
    xf.translate(r.center().x() - 12.0 * s, r.center().y() - 12.0 * s);
    xf.scale(s, s);
    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(xf.map(undoGlyph()));
}

void paintRedo(QPainter &p, const QRectF &r, qreal w)
{
    Q_UNUSED(w);
    // The undo arrow mirrored horizontally.
    const qreal s = qMin(r.width(), r.height()) / 24.0;
    QTransform xf;
    xf.translate(r.center().x() + 12.0 * s, r.center().y() - 12.0 * s);
    xf.scale(-s, s);
    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(xf.map(undoGlyph()));
}

void paintGrid(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF box = r.adjusted(r.width() * 0.18, r.height() * 0.18, -r.width() * 0.18,
                                  -r.height() * 0.18);
    p.drawRect(box);
    p.drawLine(QPointF(box.center().x(), box.top()), QPointF(box.center().x(), box.bottom()));
    p.drawLine(QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y()));
}

void paintFit(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const qreal m = r.width() * 0.18;
    const qreal L = r.width() * 0.22;
    const qreal left = r.left() + m;
    const qreal right = r.right() - m;
    const qreal top = r.top() + m;
    const qreal bottom = r.bottom() - m;
    p.drawLine(QPointF(left, top), QPointF(left + L, top));
    p.drawLine(QPointF(left, top), QPointF(left, top + L));
    p.drawLine(QPointF(right, top), QPointF(right - L, top));
    p.drawLine(QPointF(right, top), QPointF(right, top + L));
    p.drawLine(QPointF(left, bottom), QPointF(left + L, bottom));
    p.drawLine(QPointF(left, bottom), QPointF(left, bottom - L));
    p.drawLine(QPointF(right, bottom), QPointF(right - L, bottom));
    p.drawLine(QPointF(right, bottom), QPointF(right, bottom - L));
}

void paintZoomBadge(QPainter &p, const QRectF &r, qreal w, bool plus)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QPointF c(r.left() + r.width() * 0.42, r.top() + r.height() * 0.42);
    const qreal rad = r.width() * 0.28;
    p.drawEllipse(c, rad, rad);
    p.drawLine(QPointF(c.x() + rad * 0.72, c.y() + rad * 0.72),
               QPointF(r.right() - r.width() * 0.14, r.bottom() - r.height() * 0.14));
    const qreal arm = rad * 0.45;
    p.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
    if (plus)
        p.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));
}

void paintZoomIn(QPainter &p, const QRectF &r, qreal w)
{
    paintZoomBadge(p, r, w, true);
}

void paintZoomOut(QPainter &p, const QRectF &r, qreal w)
{
    paintZoomBadge(p, r, w, false);
}

/// Palette + brush from docs/paint-palette-svgrepo-com.svg (viewBox 490²).
QPainterPath paletteBoardGlyph()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.moveTo(416.848, 73.562);
    p.cubicTo(369.658, 26.126, 306.918, 0.000, 240.185, 0.000);
    p.cubicTo(173.452, 0.000, 110.711, 26.126, 63.521, 73.562);
    p.cubicTo(44.070, 93.111, 28.065, 115.563, 15.954, 140.273);
    p.cubicTo(3.222, 166.257, 4.566, 196.811, 19.553, 222.022);
    p.cubicTo(34.937, 247.884, 61.922, 264.082, 91.742, 265.335);
    p.lineTo(136.568, 267.223);
    p.cubicTo(129.432, 280.280, 124.884, 294.809, 123.213, 310.319);
    p.cubicTo(122.876, 313.444, 123.966, 316.560, 126.186, 318.790);
    p.lineTo(170.480, 363.314);
    p.cubicTo(172.481, 365.320, 175.153, 366.389, 177.876, 366.389);
    p.cubicTo(179.281, 366.389, 180.706, 366.103, 182.050, 365.513);
    p.cubicTo(198.729, 358.214, 213.199, 345.070, 222.943, 329.707);
    p.cubicTo(223.018, 330.620, 223.078, 331.534, 223.116, 332.453);
    p.lineTo(225.941, 400.293);
    p.cubicTo(227.891, 447.118, 267.048, 485.218, 313.229, 485.218);
    p.cubicTo(326.139, 485.218, 338.667, 482.256, 350.467, 476.422);
    p.cubicTo(375.060, 464.233, 397.392, 448.147, 416.848, 428.588);
    p.cubicTo(464.023, 381.171, 490.000, 318.126, 490.000, 251.069);
    p.cubicTo(490.000, 184.023, 464.023, 120.979, 416.848, 73.562);
    p.closeSubpath();
    p.moveTo(215.119, 288.731);
    p.cubicTo(214.992, 308.565, 199.695, 331.688, 179.917, 343.234);
    p.lineTo(144.584, 307.720);
    p.cubicTo(147.664, 288.090, 156.404, 270.599, 170.037, 256.884);
    p.cubicTo(184.886, 241.957, 218.489, 237.152, 241.743, 235.654);
    p.cubicTo(228.889, 249.084, 215.246, 268.164, 215.119, 288.731);
    p.closeSubpath();
    p.moveTo(402.065, 413.884);
    p.cubicTo(384.228, 431.814, 363.753, 446.568, 341.212, 457.726);
    p.cubicTo(332.319, 462.136, 322.901, 464.364, 313.229, 464.364);
    p.cubicTo(278.079, 464.364, 248.269, 435.235, 246.772, 399.416);
    p.lineTo(243.947, 331.575);
    p.cubicTo(243.449, 319.582, 240.557, 308.026, 235.354, 297.182);
    p.cubicTo(235.376, 297.021, 235.390, 296.863, 235.411, 296.703);
    p.cubicTo(235.591, 295.330, 235.726, 293.960, 235.818, 292.592);
    p.cubicTo(235.846, 292.251, 235.845, 292.252, 235.844, 292.252);
    p.cubicTo(235.914, 291.120, 235.963, 289.989, 235.970, 288.864);
    p.cubicTo(236.113, 266.159, 266.150, 242.541, 276.007, 233.261);
    p.cubicTo(282.198, 227.432, 287.060, 214.150, 261.489, 214.150);
    p.cubicTo(232.228, 214.150, 179.515, 217.786, 155.253, 242.181);
    p.cubicTo(153.713, 243.729, 152.244, 245.330, 150.807, 246.954);
    p.lineTo(92.619, 244.502);
    p.cubicTo(69.859, 243.544, 49.247, 231.153, 37.477, 211.360);
    p.cubicTo(25.932, 191.944, 24.909, 169.381, 34.678, 149.455);
    p.cubicTo(45.785, 126.781, 60.461, 106.194, 78.305, 88.264);
    p.cubicTo(121.549, 44.788, 179.043, 20.852, 240.185, 20.852);
    p.cubicTo(301.328, 20.852, 358.821, 44.789, 402.065, 88.264);
    p.cubicTo(445.326, 131.750, 469.149, 189.572, 469.149, 251.068);
    p.cubicTo(469.149, 312.577, 445.326, 370.397, 402.065, 413.884);
    p.closeSubpath();
    return p;
}

QPainterPath paletteBrushGlyph()
{
    QPainterPath p;
    p.setFillRule(Qt::OddEvenFill);
    p.moveTo(114.808, 322.637);
    p.cubicTo(110.898, 318.706, 103.935, 318.706, 100.026, 322.637);
    p.lineTo(2.607, 428.892);
    p.cubicTo(-1.027, 433.015, -0.840, 439.247, 3.034, 443.135);
    p.lineTo(46.615, 486.948);
    p.cubicTo(50.821, 490.941, 57.216, 490.941, 60.940, 487.386);
    p.lineTo(166.641, 389.448);
    p.cubicTo(170.688, 385.385, 170.688, 378.808, 166.641, 374.745);
    p.lineTo(114.808, 322.637);
    p.closeSubpath();
    p.moveTo(54.439, 465.250);
    p.lineTo(24.704, 435.357);
    p.lineTo(75.101, 378.177);
    p.lineTo(111.316, 414.587);
    p.lineTo(54.439, 465.250);
    p.closeSubpath();
    p.moveTo(126.477, 400.261);
    p.lineTo(89.350, 362.935);
    p.lineTo(107.416, 344.771);
    p.lineTo(144.543, 382.097);
    p.lineTo(126.477, 400.261);
    p.closeSubpath();
    return p;
}

void addPaletteWell(QPainterPath *path, const QPointF &c)
{
    path->addEllipse(c, 52.537, 52.537);
    path->addEllipse(c, 31.769, 31.769);
}

/// The pointed lobe of the palette that reads as the brush head.
QPainterPath paletteHeadClip()
{
    QPainterPath p;
    p.moveTo(122, 248);
    p.lineTo(205, 215);
    p.lineTo(280, 225);
    p.lineTo(278, 260);
    p.lineTo(228, 335);
    p.lineTo(168, 375);
    p.lineTo(112, 322);
    p.closeSubpath();
    return p;
}

void paintPalette(QPainter &p, const QRectF &r, qreal w)
{
    Q_UNUSED(w);
    const qreal s = qMin(r.width(), r.height()) / 490.0;
    QTransform xf;
    xf.translate(r.center().x() - 245.0 * s, r.center().y() - 245.0 * s);
    xf.scale(s, s);

    QPainterPath wells;
    wells.setFillRule(Qt::OddEvenFill);
    addPaletteWell(&wells, QPointF(351.470, 357.244));
    addPaletteWell(&wells, QPointF(378.771, 209.590));
    addPaletteWell(&wells, QPointF(134.568, 139.204));
    addPaletteWell(&wells, QPointF(281.444, 111.759));

    QPainterPath tint;
    tint.addEllipse(QPointF(134.568, 139.204), 31.769, 31.769);

    const QPainterPath board = xf.map(paletteBoardGlyph());

    p.setPen(Qt::NoPen);
    p.setBrush(ink());
    p.drawPath(board);
    p.drawPath(xf.map(wells));
    p.drawPath(xf.map(paletteBrushGlyph()));

    p.save();
    p.setClipPath(xf.map(paletteHeadClip()), Qt::IntersectClip);
    p.setBrush(accent());
    p.drawPath(board);
    p.restore();

    p.setBrush(accent());
    p.drawPath(xf.map(tint));
}

void paintAddFrame(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    const qreal arm = r.width() * 0.28;
    p.drawLine(QPointF(c.x() - arm, c.y()), QPointF(c.x() + arm, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - arm), QPointF(c.x(), c.y() + arm));
}

void paintDuplicateFrame(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF back(r.left() + r.width() * 0.22, r.top() + r.height() * 0.18,
                      r.width() * 0.46, r.height() * 0.46);
    const QRectF front = back.translated(r.width() * 0.16, r.height() * 0.16);
    p.drawRoundedRect(back, 1, 1);
    p.drawRoundedRect(front, 1, 1);
}

void paintRemoveFrame(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    p.drawLine(QPointF(c.x() - r.width() * 0.28, c.y()),
               QPointF(c.x() + r.width() * 0.28, c.y()));
}

void paintFlipHorizontal(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    p.drawLine(QPointF(c.x(), r.top() + r.height() * 0.12),
               QPointF(c.x(), r.bottom() - r.height() * 0.12));
    QPolygonF left;
    left << QPointF(c.x() - r.width() * 0.08, c.y())
         << QPointF(r.left() + r.width() * 0.12, r.top() + r.height() * 0.28)
         << QPointF(r.left() + r.width() * 0.12, r.bottom() - r.height() * 0.28);
    QPolygonF right;
    right << QPointF(c.x() + r.width() * 0.08, c.y())
          << QPointF(r.right() - r.width() * 0.12, r.top() + r.height() * 0.28)
          << QPointF(r.right() - r.width() * 0.12, r.bottom() - r.height() * 0.28);
    p.setBrush(ink());
    p.drawPolygon(left);
    p.drawPolygon(right);
}

void paintFlipVertical(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QPointF c = r.center();
    p.drawLine(QPointF(r.left() + r.width() * 0.12, c.y()),
               QPointF(r.right() - r.width() * 0.12, c.y()));
    QPolygonF top;
    top << QPointF(c.x(), c.y() - r.height() * 0.08)
        << QPointF(r.left() + r.width() * 0.28, r.top() + r.height() * 0.12)
        << QPointF(r.right() - r.width() * 0.28, r.top() + r.height() * 0.12);
    QPolygonF bottom;
    bottom << QPointF(c.x(), c.y() + r.height() * 0.08)
           << QPointF(r.left() + r.width() * 0.28, r.bottom() - r.height() * 0.12)
           << QPointF(r.right() - r.width() * 0.28, r.bottom() - r.height() * 0.12);
    p.setBrush(ink());
    p.drawPolygon(top);
    p.drawPolygon(bottom);
}

void paintRotate(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF arc = r.adjusted(r.width() * 0.18, r.height() * 0.18, -r.width() * 0.18,
                                  -r.height() * 0.18);
    p.drawArc(arc, 40 * 16, 240 * 16);
    const QPointF tip(arc.right(), arc.center().y());
    QPolygonF head;
    head << tip << QPointF(tip.x() - r.width() * 0.18, tip.y() - r.height() * 0.08)
         << QPointF(tip.x() - r.width() * 0.08, tip.y() + r.height() * 0.16);
    p.setBrush(ink());
    p.drawPolygon(head);
}

void paintOnion(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF back = r.adjusted(r.width() * 0.08, r.height() * 0.22, -r.width() * 0.28,
                                   -r.height() * 0.18);
    const QRectF front = back.translated(r.width() * 0.2, -r.height() * 0.1);
    p.drawRoundedRect(back, 2, 2);
    p.drawRoundedRect(front, 2, 2);
}

void paintShiftArrow(QPainter &p, const QRectF &r, qreal w, int dx, int dy)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(ink());
    const QPointF c = r.center();
    const qreal s = r.width() * 0.32;
    const QPointF tip(c.x() + dx * s, c.y() + dy * s);
    const QPointF back(c.x() - dx * s * 0.15, c.y() - dy * s * 0.15);
    QPointF perp(-dy * s * 0.55, dx * s * 0.55);
    QPolygonF head;
    head << tip << (back + perp) << (back - perp);
    p.setPen(Qt::NoPen);
    p.drawPolygon(head);
    p.setPen(stroke(ink(), w));
    const QPointF shaftStart(c.x() - dx * s * 0.55, c.y() - dy * s * 0.55);
    const QPointF shaftEnd(c.x() + dx * s * 0.05, c.y() + dy * s * 0.05);
    p.drawLine(shaftStart, shaftEnd);
}

void paintShiftLeft(QPainter &p, const QRectF &r, qreal w) { paintShiftArrow(p, r, w, -1, 0); }
void paintShiftRight(QPainter &p, const QRectF &r, qreal w) { paintShiftArrow(p, r, w, 1, 0); }
void paintShiftUp(QPainter &p, const QRectF &r, qreal w) { paintShiftArrow(p, r, w, 0, -1); }
void paintShiftDown(QPainter &p, const QRectF &r, qreal w) { paintShiftArrow(p, r, w, 0, 1); }

void paintSelect(QPainter &p, const QRectF &r, qreal w)
{
    QPen dash(ink(), w, Qt::DashLine, Qt::FlatCap, Qt::RoundJoin);
    dash.setDashPattern({2.4, 2.4});
    p.setPen(dash);
    p.setBrush(Qt::NoBrush);
    p.drawRect(r.adjusted(r.width() * 0.16, r.height() * 0.20, -r.width() * 0.16,
                          -r.height() * 0.20));
}

void paintCopy(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF back(r.left() + r.width() * 0.24, r.top() + r.height() * 0.18,
                      r.width() * 0.52, r.height() * 0.52);
    const QRectF front = back.translated(r.width() * 0.14, r.height() * 0.16);
    p.drawRect(back);
    p.drawRect(front);
}

void paintCut(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const qreal rad = r.width() * 0.11;
    const QPointF left(r.left() + r.width() * 0.26, r.bottom() - r.height() * 0.24);
    const QPointF right(r.right() - r.width() * 0.26, r.bottom() - r.height() * 0.24);
    p.drawEllipse(left, rad, rad);
    p.drawEllipse(right, rad, rad);
    // Blades cross above the handles; the tips reach the icon's top corners.
    p.drawLine(QPointF(left.x() + rad * 0.7, left.y() - rad * 0.7),
               QPointF(r.right() - r.width() * 0.12, r.top() + r.height() * 0.14));
    p.drawLine(QPointF(right.x() - rad * 0.7, right.y() - rad * 0.7),
               QPointF(r.left() + r.width() * 0.12, r.top() + r.height() * 0.14));
}

void paintPaste(QPainter &p, const QRectF &r, qreal w)
{
    p.setPen(stroke(ink(), w));
    p.setBrush(Qt::NoBrush);
    const QRectF board(r.left() + r.width() * 0.20, r.top() + r.height() * 0.20,
                       r.width() * 0.60, r.height() * 0.66);
    p.drawRoundedRect(board, 1, 1);
    // Clip tab across the top edge.
    const QRectF tab(r.center().x() - r.width() * 0.13, r.top() + r.height() * 0.10,
                     r.width() * 0.26, r.height() * 0.16);
    p.setBrush(ink());
    p.drawRoundedRect(tab, 1, 1);
}

using PaintFn = void (*)(QPainter &, const QRectF &, qreal);

PaintFn painterFor(Icon id)
{
    switch (id) {
    case Icon::Open: return paintOpen;
    case Icon::Save: return paintSave;
    case Icon::Settings: return paintSettings;
    case Icon::Build: return paintBuild;
    case Icon::Run: return paintRun;
    case Icon::Stop: return paintStop;
    case Icon::Pause: return paintPause;
    case Icon::Continue: return paintContinue;
    case Icon::Step: return paintStep;
    case Icon::StepOver: return paintStepOver;
    case Icon::ClearBreakpoints: return paintClearBreakpoints;
    case Icon::Brush: return paintBrush;
    case Icon::Line: return paintLine;
    case Icon::Rect: return paintRect;
    case Icon::RoundRect: return paintRoundRect;
    case Icon::Ellipse: return paintEllipse;
    case Icon::Fill: return paintFill;
    case Icon::Eyedropper: return paintEyedropper;
    case Icon::Undo: return paintUndo;
    case Icon::Redo: return paintRedo;
    case Icon::Grid: return paintGrid;
    case Icon::Fit: return paintFit;
    case Icon::ZoomIn: return paintZoomIn;
    case Icon::ZoomOut: return paintZoomOut;
    case Icon::Palette: return paintPalette;
    case Icon::AddFrame: return paintAddFrame;
    case Icon::DuplicateFrame: return paintDuplicateFrame;
    case Icon::RemoveFrame: return paintRemoveFrame;
    case Icon::FlipHorizontal: return paintFlipHorizontal;
    case Icon::FlipVertical: return paintFlipVertical;
    case Icon::Rotate: return paintRotate;
    case Icon::Onion: return paintOnion;
    case Icon::ShiftLeft: return paintShiftLeft;
    case Icon::ShiftRight: return paintShiftRight;
    case Icon::ShiftUp: return paintShiftUp;
    case Icon::ShiftDown: return paintShiftDown;
    case Icon::Select: return paintSelect;
    case Icon::Copy: return paintCopy;
    case Icon::Cut: return paintCut;
    case Icon::Paste: return paintPaste;
    }
    return paintOpen;
}

void paintWindowGlyph(QPainter &p, int s)
{
    const qreal k = s / 64.0;
    auto sc = [&](qreal v) { return v * k; };

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x2b, 0x2b, 0x30));
    p.drawRoundedRect(QRectF(sc(2), sc(4), sc(60), sc(40)), sc(3), sc(3));
    p.setBrush(QColor(0x10, 0x10, 0x14));
    p.drawRoundedRect(QRectF(sc(6), sc(8), sc(52), sc(32)), sc(2), sc(2));
    // GEM desktop green, matching the theme accent rather than TOS blue.
    p.setBrush(QColor(0x0a, 0x5a, 0x18));
    p.drawRect(QRectF(sc(9), sc(11), sc(46), sc(26)));
    p.setBrush(QColor(0x1a, 0x8a, 0x30));
    p.drawRect(QRectF(sc(9), sc(11), sc(46), sc(5)));
    if (s >= 32) {
        p.setPen(QColor(0xe8, 0xe8, 0xee));
        QFont f(QStringLiteral("monospace"));
        f.setBold(true);
        f.setPixelSize(int(std::max(8.0, sc(11))));
        p.setFont(f);
        p.drawText(QRectF(sc(9), sc(16), sc(46), sc(16)), Qt::AlignCenter,
                   QStringLiteral("PiST"));
        p.setPen(Qt::NoPen);
    }
    p.setBrush(QColor(0x2b, 0x2b, 0x30));
    p.drawRoundedRect(QRectF(sc(20), sc(46), sc(24), sc(4)), sc(1), sc(1));
    p.setBrush(QColor(0x3a, 0x3a, 0x42));
    p.drawRoundedRect(QRectF(sc(12), sc(52), sc(40), sc(9)), sc(2), sc(2));
    p.setBrush(QColor(0x6d, 0x6d, 0x78));
    for (int i = 0; i < 6; ++i)
        p.drawRect(QRectF(sc(16 + i * 6), sc(55), sc(4), sc(3)));
}

} // namespace

QIcon icon(Icon id, const QColor &paint)
{
    QIcon ic;
    const PaintFn fn = painterFor(id);
    const qreal ratio = dpr();
    for (int logical : {16, 20, 24, 32}) {
        const int px = int(std::lround(logical * ratio));
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(ratio);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal pad = logical * 0.08;
        const QRectF r(pad, pad, logical - 2 * pad, logical - 2 * pad);
        const qreal w = std::max(1.2, logical / 16.0 * 1.6);
        if (id == Icon::Brush)
            paintBrushTinted(p, r, w, paint);
        else
            fn(p, r, w);
        p.end();
        ic.addPixmap(pm);
    }
    return ic;
}

QIcon windowIcon()
{
    QIcon ic;
    const qreal ratio = dpr();
    for (int logical : {16, 32, 64, 128}) {
        const int px = int(std::lround(logical * ratio));
        QPixmap pm(px, px);
        pm.setDevicePixelRatio(ratio);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        paintWindowGlyph(p, logical);
        p.end();
        ic.addPixmap(pm);
    }
    return ic;
}

namespace {

void outlinePath(QPainter &p, const QPainterPath &path, qreal width)
{
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::black, width + 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
    p.setPen(QPen(Qt::white, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(path);
}

void paintBrushCursor(QPainter &p, const QColor &paint)
{
    // Tip / hotspot at (3, 25) in 32×32 logical pixels (SVG bristle tip).
    QTransform xf;
    xf.scale(32.0 / 24.0, 32.0 / 24.0);
    p.setPen(QPen(Qt::black, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::white);
    p.drawPath(xf.map(paintBrushGlyph()));
    if (paint.alpha() > 0) {
        p.setPen(Qt::NoPen);
        p.setBrush(paint);
        p.drawPath(xf.map(paintBrushHeadGlyph()));
    }
}

void paintCrosshairCursor(QPainter &p)
{
    QPainterPath path;
    path.moveTo(16, 3);
    path.lineTo(16, 12);
    path.moveTo(16, 20);
    path.lineTo(16, 29);
    path.moveTo(3, 16);
    path.lineTo(12, 16);
    path.moveTo(20, 16);
    path.lineTo(29, 16);
    outlinePath(p, path, 1.6);
}

void paintFillCursor(QPainter &p)
{
    QTransform xf;
    xf.scale(32.0 / 256.0, 32.0 / 256.0);
    p.setPen(QPen(Qt::black, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::white);
    p.drawPath(xf.map(paintBucketGlyph()));
}

void paintEyedropperCursor(QPainter &p)
{
    // Tip / hotspot at (5, 27).
    QPainterPath tube;
    tube.moveTo(5, 27);
    tube.lineTo(18, 12);
    outlinePath(p, tube, 2.2);

    p.setPen(QPen(Qt::black, 1.6));
    p.setBrush(Qt::white);
    p.drawEllipse(QPointF(22, 8), 4.2, 4.2);
}

} // namespace

QCursor canvasCursor(CanvasCursor id, const QColor &paint)
{
    const qreal ratio = dpr();
    const int logical = 32;
    const int px = qMax(1, int(std::lround(logical * ratio)));
    QPixmap pm(px, px);
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(ratio, ratio);

    QPoint hotspot(16, 16);
    switch (id) {
    case CanvasCursor::Brush:
        paintBrushCursor(p, paint);
        hotspot = QPoint(3, 25);
        break;
    case CanvasCursor::Fill:
        paintFillCursor(p);
        hotspot = QPoint(29, 29);
        break;
    case CanvasCursor::Eyedropper:
        paintEyedropperCursor(p);
        hotspot = QPoint(5, 27);
        break;
    case CanvasCursor::Crosshair:
        paintCrosshairCursor(p);
        hotspot = QPoint(16, 16);
        break;
    }
    p.end();
    return QCursor(pm, hotspot.x(), hotspot.y());
}

} // namespace appearance
} // namespace pist
