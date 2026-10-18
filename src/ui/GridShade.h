#pragma once
// Alternating beat shading for the grid editors (0.21): every second beat is a shade lighter,
// so the beats read at a glance grids; as you zoom out it steps up to every second
// bar, then every second group of four bars, then sixteen, so a long clip isn't a picket fence
// (a band is at least kMinBand pixels wide). Painted over the rows' background, under the grid
// lines, faintly (theme::beatAlt).

#include "core/Theme.h"
#include <QPainter>
#include <QRectF>
#include <cmath>
#include <functional>

namespace sessions::gridshade {

// `pxPerBeat` picks the unit, `beatToX` maps beats to x, [first, last) are the beats on screen,
// `area` the part of the widget to shade (the shading is clipped to it).
inline void paint(QPainter& p, double pxPerBeat, const std::function<double(double)>& beatToX, double first, double last, const QRectF& area) {
    if (!(last > first) || area.isEmpty() || pxPerBeat <= 0) return;
    constexpr double kMinBand = 28;
    double unit = 64;
    for (double u : {1.0, 4.0, 16.0}) if (u * pxPerBeat >= kMinBand) { unit = u; break; }
    const double b0 = std::floor(first / unit) * unit;
    for (double b = b0; b < last; b += unit) {
        if (!(std::lround(b / unit) & 1)) continue;
        const double x0 = std::max(area.left(), beatToX(b)), x1 = std::min(area.right(), beatToX(b + unit));
        if (x1 > x0) p.fillRect(QRectF(x0, area.top(), x1 - x0, area.height()), theme::beatAlt);
    }
}

} // namespace sessions::gridshade
