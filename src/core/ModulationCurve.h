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
    if (phase <= points[0].x) return points[0].y;
    for (int i = 1; i < count; ++i) if (phase <= points[i].x) {
        const double span = points[i].x - points[i - 1].x;
        return segment(points[i - 1], points[i], span > 1e-9 ? (phase - points[i - 1].x) / span : 1);
    }
    return points[count - 1].y;
}
}
