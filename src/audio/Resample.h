#pragma once
// Shared fractional sample reads. Unity / upsampling keeps the existing cubic
// interpolator; downsampling uses a normalized low-pass filter to prevent source
// frequencies above the output Nyquist frequency from folding into the audio.
// Call prepare() before the audio callback starts. A Plan can be cached while
// the source-frame advance per output sample stays constant.

#include <cmath>
#include <cstddef>
#include <vector>

namespace sessions::audio::resample {

void prepare(); // one-time table generation; UI / preparation thread only

struct Plan {
    const float* phases = nullptr; // null: cubic; otherwise precomputed low-pass kernels
};
Plan plan(double frameStep);

inline float cubic(const std::vector<float>& x, double position) {
    const auto n = std::ptrdiff_t(x.size());
    if (!std::isfinite(position) || position < -1 || position >= double(n)) return 0;
    const auto i = std::ptrdiff_t(std::floor(position));
    const float t = float(position - double(i));
    auto at = [&](std::ptrdiff_t k) { return k >= 0 && k < n ? x[size_t(k)] : 0.0f; };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
}

float filtered(const std::vector<float>& x, double position, const Plan& plan);
inline float read(const std::vector<float>& x, double position, const Plan& p) {
    return p.phases ? filtered(x, position, p) : cubic(x, position);
}
inline float read(const std::vector<float>& x, double position, double frameStep = 1) {
    return read(x, position, plan(frameStep));
}

} // namespace sessions::audio::resample
