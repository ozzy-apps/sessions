#include "Resample.h"
#include <algorithm>
#include <mutex>

namespace sessions::audio::resample {
namespace {
constexpr int taps = 32, phases = 128, cutoffs = 64;
constexpr double pi = 3.14159265358979323846;
// One extra phase permits interpolation between phase kernels, avoiding the
// phase quantization noise of rounding every source position to a table cell.
alignas(64) float kernels[cutoffs * (phases + 1) * taps] = {};
std::once_flag prepared;
}

void prepare() {
    std::call_once(prepared, [] {
        for (int c = 0; c < cutoffs; ++c) {
            const double cutoff = double(c + 1) / cutoffs;
            for (int p = 0; p <= phases; ++p) {
                const double fraction = double(p) / phases;
                float* kernel = kernels + (c * (phases + 1) + p) * taps;
                double gain = 0;
                for (int k = 0; k < taps; ++k) {
                    const double x = k - (taps / 2 - 1) - fraction;
                    const double window = std::abs(x) >= taps / 2 ? 0.0
                        : 0.42 + 0.5 * std::cos(2 * pi * x / taps) + 0.08 * std::cos(4 * pi * x / taps);
                    const double sinc = std::abs(x) < 1e-12 ? cutoff : std::sin(pi * cutoff * x) / (pi * x);
                    kernel[k] = float(window * sinc);
                    gain += window * sinc;
                }
                // Every fractional phase retains unity DC gain, including the
                // small-cutoff filters used by large playback-rate changes.
                for (int k = 0; k < taps; ++k) kernel[k] = float(kernel[k] / gain);
            }
        }
    });
}

Plan plan(double frameStep) {
    const double speed = std::abs(frameStep);
    if (!std::isfinite(speed) || speed <= 1.0 + 1e-9) return {};
    // Round the cutoff down, so table quantization can never admit frequencies
    // above the actual output Nyquist limit. No log or trigonometry per read.
    const int c = std::clamp(int(std::min(double(cutoffs), cutoffs / speed)), 1, cutoffs) - 1;
    return {kernels + c * (phases + 1) * taps};
}

float filtered(const std::vector<float>& x, double position, const Plan& p) {
    const auto n = std::ptrdiff_t(x.size());
    if (!p.phases) return cubic(x, position);
    if (!std::isfinite(position) || n == 0 || position < -taps || position > double(n) + taps) return 0;
    const auto center = std::ptrdiff_t(std::floor(position));
    const double phase = (position - double(center)) * phases;
    const int index = std::min(phases - 1, int(phase));
    const float fraction = float(phase - index);
    const float* a = p.phases + index * taps;
    const float* b = a + taps;
    const auto start = center - (taps / 2 - 1);
    float sum = 0, difference = 0;
    if (start >= 0 && start + taps <= n) {
        const float* input = x.data() + start;
        for (int k = 0; k < taps; ++k) {
            sum += input[k] * a[k];
            difference += input[k] * (b[k] - a[k]);
        }
    } else {
        // Zero padding at file edges; no clamped endpoint is repeated.
        for (int k = 0; k < taps; ++k) {
            const auto at = start + k;
            const float value = at >= 0 && at < n ? x[size_t(at)] : 0;
            sum += value * a[k];
            difference += value * (b[k] - a[k]);
        }
    }
    return sum + fraction * difference;
}

} // namespace sessions::audio::resample
