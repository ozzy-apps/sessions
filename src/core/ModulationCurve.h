#pragma once
#include "Model.h"
#include <algorithm>
#include <cmath>

namespace sessions::modcurve {
// Shared by the editor and DSP: the displayed segment is the segment you hear.
inline double bend(double t, double amount) {
    t = std::clamp(t, 0.0, 1.0);
    amount = std::clamp(amount, -1.0, 1.0);
    if (std::abs(amount) < 1e-6) return t;
    return amount > 0 ? std::pow(t, 1 + 5 * amount) : 1 - std::pow(1 - t, 1 - 5 * amount);
}
inline double segment(const CurvePoint& a, const CurvePoint& b, double t) {
    if (a.shape == 2) return t < 1 ? a.y : b.y;
    t = bend(t, a.bend);
    if (a.shape == 1) t = t * t * (3 - 2 * t);
    return a.y + (b.y - a.y) * t;
}
inline double value(const CurvePoint* points, int count, double phase) {
    if (!points || count <= 0) return 0;
    if (count == 1 || phase <= points[0].x) return points[0].y;
    if (phase > points[count - 1].x) return points[count - 1].y;
    // Curves are time-sorted. Binary search keeps dense freehand curves cheap in the audio path.
    int lo = 1, hi = count - 1;
    while (lo < hi) { const int mid = (lo + hi) / 2; if (phase <= points[mid].x) hi = mid; else lo = mid + 1; }
    const double span = points[lo].x - points[lo - 1].x;
    return segment(points[lo - 1], points[lo], span > 1e-9 ? (phase - points[lo - 1].x) / span : 1);
}
}
