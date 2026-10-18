#include "Wavetables.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace sessions::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxHarmonics = 512;

// In-place radix-2 inverse FFT (unnormalized).
void ifft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> w = std::polar(1.0, 2 * kPi / double(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wk = 1;
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * wk;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wk *= w;
            }
        }
    }
}

struct Partial { double cosAmp = 0, sinAmp = 0; };

// Basic: sine -> triangle -> saw -> square.
Partial shape(int which, int h) {
    switch (which) {
    case 0: return {0, h == 1 ? 1.0 : 0.0};
    case 1: return {0, h % 2 ? 8 / (kPi * kPi) * (((h - 1) / 2) % 2 ? -1 : 1) / (double(h) * h) : 0.0};
    case 2: return {0, 2 / (kPi * h) * (h % 2 ? 1 : -1)};
    default: return {0, h % 2 ? 4 / (kPi * h) : 0.0};
    }
}

Partial partial(int table, double t, int h) {
    switch (table) {
    case 0: {
        const double x = t * 3;
        const int a = std::min(2, int(x));
        const double f = x - a;
        const Partial p = shape(a, h), q = shape(a + 1, h);
        return {0, p.sinAmp * (1 - f) + q.sinAmp * f};
    }
    case 1: {   // Harmonics: from a soft, nearly pure tone to a bright stack
        const double limit = 3 + t * 90;
        if (h > limit) return {};
        return {0, std::pow(double(h), -(2.2 - 1.8 * t))};
    }
    case 2: {   // Formant: a voice moving through the vowels a, e, i, o, u
        static const double f1[] = {7.3, 5.3, 2.7, 5.7, 3.0}, f2[] = {11.0, 18.4, 22.9, 8.7, 8.7};
        const double x = t * 4;
        const int a = std::min(3, int(x));
        const double fr = x - a;
        const double F1 = f1[a] + (f1[a + 1] - f1[a]) * fr, F2 = f2[a] + (f2[a + 1] - f2[a]) * fr;
        if (h > 64) return {};
        const double g = std::exp(-std::pow((h - F1) / 1.6, 2)) + 0.6 * std::exp(-std::pow((h - F2) / 2.6, 2)) + 0.25 / (double(h) * h);
        return {0, g / std::sqrt(double(h))};
    }
    case 3: {   // Pulse: from a square to a thin pulse
        const double d = 0.5 - 0.45 * t;
        return {2 / (kPi * h) * std::sin(kPi * h * d), 0};
    }
    case 4: {   // Sub: a sine that grows a few warm harmonics
        if (h > 1 + 7 * t + 1e-9) return {};
        return {0, std::pow(double(h), -1.7)};
    }
    case 5: {   // Growl: a saw with a resonant bump that sweeps up through the harmonics
        if (h > 64) return {};
        const double c = 2 + 14 * t, w = 1.5 + 4 * t;
        const double bump = 0.25 + 1.2 * std::exp(-std::pow((h - c) / w, 2));
        return {0, (h % 2 ? 1.0 : -1.0) * bump / h};
    }
    case 6: {   // Metallic: sparse, clangorous harmonics, more of them to the right
        if (h > 8 + 56 * t) return {};
        if (h != 1 && h % 3 != 1 && h % 7 != 0) return {};
        return {0, std::pow(double(h), -(1.1 - 0.5 * t))};
    }
    default: {  // Digital: stepped, bitty partials that brighten
        if (h > 4 + 28 * t) return {};
        return {0, (h % 2 ? 1.0 : 0.6) * (h % 4 == 0 ? -1.0 : 1.0) / std::sqrt(double(h))};
    }
    }
}

std::vector<Wavetable> build() {
    std::vector<Wavetable> tables = {{"Basic", {}}, {"Harmonics", {}}, {"Formant", {}}, {"Pulse", {}},
                                     {"Sub", {}}, {"Growl", {}}, {"Metallic", {}}, {"Digital", {}}};
    std::vector<std::complex<double>> buf(kWtSize);
    for (int t = 0; t < int(tables.size()); ++t) {
        Wavetable& w = tables[size_t(t)];
        w.data.assign(size_t(kWtLevels) * kWtFrames * (kWtSize + 1), 0.0f);
        for (int f = 0; f < kWtFrames; ++f) {
            const double pos = double(f) / (kWtFrames - 1);
            double norm = 1;
            for (int level = 0; level < kWtLevels; ++level) {
                const int maxH = kMaxHarmonics >> level;
                std::fill(buf.begin(), buf.end(), 0.0);
                for (int h = 1; h <= maxH; ++h) {
                    const Partial p = partial(t, pos, h);
                    buf[size_t(h)] = {p.cosAmp, -p.sinAmp};
                }
                ifft(buf);
                if (level == 0) {
                    double peak = 1e-9;
                    for (const auto& c : buf) peak = std::max(peak, std::abs(c.real()));
                    norm = 1 / peak;
                }
                float* out = const_cast<float*>(w.frame(level, f));
                for (int i = 0; i < kWtSize; ++i) out[i] = float(buf[size_t(i)].real() * norm);
                out[kWtSize] = out[0];
            }
        }
    }
    return tables;
}

const std::vector<Wavetable>& tables() {
    static const std::vector<Wavetable> t = build();
    return t;
}

} // namespace

int wavetableCount() { return int(tables().size()); }
const Wavetable& wavetable(int index) { return tables()[size_t(std::clamp(index, 0, wavetableCount() - 1))]; }

int wavetableLevel(float freq, float sampleRate) {
    const float limit = sampleRate * 0.45f;
    for (int level = 0; level < kWtLevels; ++level)
        if (float(kMaxHarmonics >> level) * freq <= limit) return level;
    return kWtLevels - 1;
}

} // namespace sessions::audio
