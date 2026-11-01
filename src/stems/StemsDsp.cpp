#include "StemsDsp.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace sessions::stems {
void fft(std::vector<Complex>& values, bool inverse) {
    const int count = int(values.size());
    if (count < 2 || (count & (count - 1))) throw std::runtime_error("FFT length must be a power of two.");
    for (int i = 1, j = 0; i < count; ++i) {
        int bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (int length = 2; length <= count; length <<= 1) {
        const double angle = (inverse ? 2. : -2.) * 3.14159265358979323846 / length;
        const std::complex<double> rotation(std::cos(angle), std::sin(angle));
        for (int i = 0; i < count; i += length) {
            std::complex<double> weight(1, 0);
            for (int j = 0; j < length / 2; ++j) {
                const Complex a = values[i + j], b = Complex(float(weight.real()), float(weight.imag())) * values[i + j + length / 2];
                values[i + j] = a + b;
                values[i + j + length / 2] = a - b;
                weight *= rotation;
            }
        }
    }
    if (inverse) for (auto& value : values) value /= float(count);
}
std::vector<float> hannWindow() {
    std::vector<float> window(kFft);
    for (int i = 0; i < kFft; ++i) window[i] = float(.5 - .5 * std::cos(2 * 3.14159265358979323846 * i / kFft));
    return window;
}
std::array<float, kSources> ratioMasks(const std::array<float, kSources>& logits) {
    const float maximum = *std::max_element(logits.begin(), logits.end());
    std::array<float, kSources> weights{};
    float total = 0;
    for (int s = 0; s < kSources; ++s) {
        if (!std::isfinite(logits[s])) throw std::runtime_error("The model produced non-finite logits.");
        weights[s] = std::exp(2.f * (logits[s] - maximum));
        total += weights[s];
    }
    for (auto& weight : weights) weight /= total;
    return weights;
}
} // namespace sessions::stems
