#pragma once
// Generic tonal microphone models, not measured responses of branded microphones.
// Shared by the processor and the UI's static frequency-response plot.
#include <algorithm>
#include <cmath>
#include <complex>

namespace sessions::audio::drums {
struct MicColor { float cutoff, high; };
inline MicColor micColor(int model, float angle = 0) {
    constexpr MicColor colors[] = {{4500, 1}, {3600, .58f}, {6200, 1.4f}, {2800, .28f}};
    auto c = colors[std::clamp(model, 0, 3)];
    c.high *= std::exp2(-std::clamp(angle, 0.f, 90.f) / 45.f);
    return c;
}
inline float micGain(float cm, float angle) {
    return std::sqrt(.2f / (.1f + std::clamp(cm, 0.f, 100.f) * .01f))
        * (.5f + .5f * std::cos(std::clamp(angle, 0.f, 90.f) * .01745329252f));
}
inline double micResponse(double hz, int model, float angle, float cm, double rate = 48000) {
    const auto c = micColor(model, angle);
    const double a = std::exp(-6.28318530718 * std::min(double(c.cutoff), rate * .49) / rate);
    const auto z = std::polar(1., -6.28318530718 * hz / rate);
    const auto low = (1. - a) / (1. - a * z);
    return 20 * std::log10(std::max(1e-6, std::abs(low + (1. - low) * double(c.high)) * micGain(cm, angle)));
}
}
