#pragma once
// The transport glyphs, drawn rather than typed (0.21): a font's ▶ ■ ● come out uneven and
// smeared at the sizes the slots and buttons use. These paint crisp, antialiased shapes fitted
// to a box, and make QIcons of them for tool buttons (the On state in the colour a checked
// button's text has, so the stylesheet's accent background keeps its dark glyph).

#include "core/Theme.h"
#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRectF>
#include <QGuiApplication>

namespace sessions::icons {

enum class Glyph { Play, Stop, Record };

// Paints `g` in `colour`, centred in `box` (a square the glyph fills to about 70 %).
inline void paint(QPainter& p, Glyph g, const QRectF& box, const QColor& colour) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    const double s = std::min(box.width(), box.height());
    const QPointF c = box.center();
    switch (g) {
    case Glyph::Play: {   // an equilateral-ish triangle, its centroid a little right of centre so it reads as centred
        const double h = s * 0.74, w = h * 0.86;
        QPainterPath path;
        path.moveTo(c.x() - w * 0.42, c.y() - h / 2);
        path.lineTo(c.x() + w * 0.58, c.y());
        path.lineTo(c.x() - w * 0.42, c.y() + h / 2);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case Glyph::Stop: {
        const double w = s * 0.58;
        p.drawRoundedRect(QRectF(c.x() - w / 2, c.y() - w / 2, w, w), 0.8, 0.8);
        break;
    }
    case Glyph::Record: {
        const double r = s * 0.34;
        p.drawEllipse(c, r, r);
        break;
    }
    }
    p.restore();
}

// A pixmap of the glyph at the screen's pixel ratio, for QIcons.
inline QPixmap pixmap(Glyph g, const QColor& colour, int size) {
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    paint(p, g, QRectF(0, 0, size, size), colour);
    return pm;
}

// An icon for a tool button: `off` when it's up, `on` when it's checked (the stylesheet turns the
// background to the accent then, so the glyph goes dark like the text would).
inline QIcon icon(Glyph g, const QColor& off, const QColor& on, int size = 12) {
    QIcon i;
    i.addPixmap(pixmap(g, off, size), QIcon::Normal, QIcon::Off);
    i.addPixmap(pixmap(g, off, size), QIcon::Active, QIcon::Off);
    i.addPixmap(pixmap(g, on, size), QIcon::Normal, QIcon::On);
    i.addPixmap(pixmap(g, on, size), QIcon::Active, QIcon::On);
    return i;
}

} // namespace sessions::icons
