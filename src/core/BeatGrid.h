#pragma once

#include "Sample.h"
#include <QVector>

namespace sessions::tempo {

struct GridPoint { double beat = 0; double seconds = 0; };
struct TimingGrid {
    QVector<GridPoint> points;
    double confidence = 0;
    bool variable = false;
};

// An independently accepted source tempo supplies the metrical interpretation. Track slow
// tempo changes only when several windows of rhythmic audio corroborate them. The origin is
// always (0, 0): intros and the relative offsets of aligned stems must remain intact.
// Uncertain, short and steady recordings retain the exact two-point source-tempo map.
TimingGrid trackGrid(const SampleData& sample, double acceptedBpm);

} // namespace sessions::tempo
