#include "Wavetables.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

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

Partial legacyPartial(int table, double t, int h) {
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

double bell(double h, double center, double width) {
    const double d = (h - center) / width;
    return std::exp(-0.5 * d * d);
}

Partial polarPartial(double amplitude, double phase = 0) {
    return {amplitude * std::sin(phase), amplitude * std::cos(phase)};
}

// The new material is additive, rather than sampled from a discontinuous time-domain
// waveform. Every morph frame is periodic, DC-free and can be rebuilt at an exact band
// limit. Fundamental anchors preserve pitch and keep sparse/formant tables useful high
// on the keyboard. Phase offsets change smoothly across frames, avoiding unrelated
// noise frames that collapse while the Position control interpolates between them.
Partial expandedPartial(int table, double t, int h) {
    const double x = double(h), odd = h % 2 ? 1.0 : 0.0;
    double amplitude = 0, phase = 0;
    switch (table - 8) {
    case 0: // Velvet Saw: rounded ramp opening into a bright, gently tilted saw.
        amplitude = std::pow(x, -(2.2 - 1.25 * t)) * std::exp(-x / (14 + 100 * t));
        phase = (h % 2 ? 0 : kPi) + 0.15 * t * std::sin(x * 0.7);
        break;
    case 1: // Vintage Stack: phase-aligned voices form a moving spectral comb.
        amplitude = (0.6 + 0.4 * std::cos(x * (0.08 + 0.55 * t))) / std::pow(x, 1.15);
        phase = (h % 2 ? 0 : kPi) + 0.35 * t * std::sin(x * 1.3);
        break;
    case 2: // PWM Drift: wide-to-narrow pulse, without its time-domain DC component.
        amplitude = std::sin(kPi * x * (0.08 + 0.39 * t)) / x;
        phase = kPi * 0.5;
        break;
    case 3: // Triangle Bloom: odd triangle harmonics acquire a bright even sheen.
        amplitude = odd ? (h % 4 == 1 ? 1 : -1) / std::pow(x, 2 - 0.65 * t)
                        : 0.42 * t / std::pow(x, 1.6);
        break;
    case 4: // Soft Fold: rounded sine folds reveal successive odd harmonics.
        amplitude = odd ? (h % 4 == 1 ? 1 : -1) * std::pow(0.16 + 0.8 * t, (x - 1) * 0.5) / std::sqrt(x) : 0;
        break;
    case 5: // Sync Sweep: harmonic peaks rise through a hard-sync-like spectrum.
        amplitude = (0.12 + bell(x, 2 + 22 * t, 0.7 + 2 * t) +
                     0.48 * bell(x, 4 + 44 * t, 1.2 + 3 * t)) / std::pow(x, 0.8);
        phase = 0.2 * t * x;
        break;
    case 6: // Odd Ensemble: hollow odd harmonics with a moving shoulder.
        amplitude = odd * (0.3 + 0.9 * bell(x, 3 + 18 * t, 3.2)) / std::pow(x, 0.9);
        break;
    case 7: // Even Bloom: a strong fundamental supporting octave/even partials.
        amplitude = h % 2 == 0 ? (0.2 + 1.5 * t) * std::exp(-x / (5 + 32 * t)) / std::sqrt(x) : 0;
        break;
    case 8: // Harmonic Prism: three independently moving bands.
        amplitude = (0.7 * bell(x, 3 + 11 * t, 1.4) + bell(x, 8 + 27 * t, 2.5) +
                     0.5 * bell(x, 17 + 46 * t, 3.3)) / std::sqrt(x);
        phase = 0.16 * std::sin(x * (0.3 + t));
        break;
    case 9: // Spectral Comb: teeth become dense as the frame advances.
        amplitude = std::pow(0.5 + 0.5 * std::cos(x * (2.8 - 1.9 * t)), 4) / std::pow(x, 0.9);
        phase = 0.22 * x * t;
        break;
    case 10: // Hollow Sweep: a moving notch through an odd/even reed spectrum.
        amplitude = (odd ? 1.0 : 0.22) * (1 - 0.98 * bell(x, 2 + 27 * t, 1.3 + 5 * t)) / std::pow(x, 1.1);
        break;
    case 11: // Glass Ribbon: a luminous upper band above a soft root.
        amplitude = (0.3 * bell(x, 2 + 5 * t, 1.4) + bell(x, 12 + 32 * t, 2 + 5 * t)) / std::pow(x, 0.7);
        phase = 0.4 * std::sin(x * 0.9) * t;
        break;
    case 12: { // Vowel Sweep: three broad resonances traverse ah / oh / ee regions.
        const double f1 = 3.5 + 5.5 * std::sin(kPi * t), f2 = 9 + 15 * t, f3 = 24 + 11 * t;
        amplitude = (bell(x, f1, 1.1) + 0.75 * bell(x, f2, 1.8) + 0.35 * bell(x, f3, 2.8)) / std::sqrt(x);
        break;
    }
    case 13: // Throat: low growled resonances with asymmetric harmonic emphasis.
        amplitude = ((odd ? 0.5 : 0.14) + 1.6 * bell(x, 2 + 6 * t, 0.8) +
                     0.65 * bell(x, 8 + 10 * t, 1.6)) / std::pow(x, 1.05);
        phase = (h % 3 == 0 ? 0.75 : -0.15) * t;
        break;
    case 14: // Whisper Formant: breath-like, deterministic upper harmonic clusters.
        amplitude = (0.3 * bell(x, 6 + 7 * t, 2) + bell(x, 20 + 28 * t, 7) +
                     0.5 * bell(x, 58 - 20 * t, 8)) * (0.6 + 0.4 * std::sin(x * 2.39996)) / std::sqrt(x);
        phase = 1.6 * std::sin(x * 1.618) * (0.5 + 0.5 * t);
        break;
    case 15: // Choir Air: soft vowel bands and a high choral halo.
        amplitude = (0.8 * bell(x, 4 + 3 * t, 1.5) + 0.55 * bell(x, 12 + 6 * t, 2.4) +
                     0.35 * bell(x, 28 + 14 * t, 5)) / std::pow(x, 0.75);
        phase = 0.32 * std::sin(x * 1.1) * t;
        break;
    case 16: // Talkbox: narrow, bright double formants with a deep moving valley.
        amplitude = (1.3 * bell(x, 3 + 12 * t, 0.7 + t) + bell(x, 7 + 23 * t, 1.2 + t)) / std::sqrt(x);
        phase = (h % 2 ? 0.3 : -0.6) * t;
        break;
    case 17: // Nasal Reed: odd harmonics under a narrow nasal resonance.
        amplitude = (odd ? 1.0 : 0.09) * (0.22 + 1.8 * bell(x, 5 + 13 * t, 1.4)) / std::pow(x, 1.05);
        break;
    case 18: // Iron Bell: tuned, sparse bell modes bloom above the fundamental.
        amplitude = (bell(x, 2, 0.12) + 0.8 * bell(x, 5, 0.2) + 0.65 * bell(x, 9 + 3 * t, 0.6) +
                     0.5 * bell(x, 15 + 12 * t, 1.1) + 0.35 * bell(x, 31, 0.3)) * (0.25 + 0.75 * t);
        phase = 0.35 * x * t;
        break;
    case 19: // Copper Gong: broader, darker resonant clusters.
        amplitude = (0.8 * bell(x, 3, 0.3) + bell(x, 7 + 4 * t, 1.2) +
                     0.6 * bell(x, 17 + 14 * t, 2.4) + 0.35 * bell(x, 39 - 9 * t, 3)) / std::pow(x, 0.35);
        phase = 1.2 * std::sin(x * 2.1) * t;
        break;
    case 20: // Steel Wire: struck-string overtones with a moving bridge comb.
        amplitude = (0.15 + std::pow(std::sin(x * (0.2 + 0.5 * t)), 2)) / std::pow(x, 0.9);
        phase = 0.38 * std::sin(x * x * 0.11) * t;
        break;
    case 21: // Alloy Cluster: adjacent partial pairs at increasingly high modes.
        amplitude = (bell(x, 4 + 2 * t, 0.55) + 0.85 * bell(x, 11 + 7 * t, 0.8) +
                     0.7 * bell(x, 24 + 13 * t, 1.2) + 0.5 * bell(x, 47 - 8 * t, 1.4)) / std::pow(x, 0.25);
        phase = (h % 2 ? 0 : 1.1) * t;
        break;
    case 22: // Bowed Plate: sustained comb clusters and less percussive brightness.
        amplitude = (0.12 + bell(x, 6 + 16 * t, 4.5) + 0.5 * bell(x, 35 + 9 * t, 7)) *
                    std::pow(0.5 + 0.5 * std::cos(x * 1.9), 2) / std::pow(x, 0.65);
        phase = 0.7 * std::sin(x * 0.8 + t);
        break;
    case 23: // Chrome Chime: bright octave/fifth modes with a glassy top.
        amplitude = (bell(x, 2, 0.15) + 0.75 * bell(x, 3, 0.2) + 0.55 * bell(x, 8, 0.3) +
                     (0.25 + t) * bell(x, 16 + 12 * t, 0.9) + 0.6 * t * bell(x, 48, 1.5)) / std::pow(x, 0.25);
        phase = 0.1 * x * t;
        break;
    case 24: // Bit Fold: alternating, stepped harmonic groups.
        amplitude = h <= 8 + 88 * t ? (h % 4 == 0 ? -0.8 : h % 4 == 2 ? 0.3 : 1.0) / std::pow(x, 0.75) : 0;
        phase = 0.2 * std::sin(x * 1.5) * t;
        break;
    case 25: // Phase Lattice: a dispersive phase spread through a smooth spectrum.
        amplitude = std::exp(-x / (10 + 70 * t)) / std::pow(x, 1.0);
        phase = 0.025 * x * x * t + 0.6 * std::sin(x * 0.6) * t;
        break;
    case 26: // Binary Stair: different harmonic octaves reveal nested steps.
        amplitude = (odd ? 0.8 : (h % 8 == 0 ? 1.6 : 0.18)) * std::exp(-x / (6 + 110 * t)) / std::pow(x, 0.8);
        phase = (h % 8 >= 4 ? kPi : 0) + 0.3 * t;
        break;
    case 27: // Pixel Pulse: narrow pulse clusters at smoothly moving bin centers.
        amplitude = (0.1 + bell(x, 3 + 19 * t, 0.9) + 0.7 * bell(x, 9 + 38 * t, 1.3)) *
                    (h % 3 == 0 ? -0.6 : 1) / std::pow(x, 0.7);
        phase = 0.5 * std::sin(x * 2.3) * t;
        break;
    case 28: // FM Prism: analytical FM sidebands, spectrally truncated at each mip.
        amplitude = h <= 48 ? std::cyl_bessel_j(std::abs(h - 3), 0.5 + 13 * t) : 0;
        phase = (h % 4) * kPi * 0.5;
        break;
    case 29: // Shard Stack: sparse prime-like partials with animated phase.
        amplitude = (h == 2 || h == 3 || h == 5 || h == 7 || h == 11 || h == 13 || h == 17 ||
                     h == 19 || h == 23 || h == 31 || h == 43) ? (0.2 + bell(x, 2 + 39 * t, 8)) / std::pow(x, 0.55) : 0;
        phase = 0.9 * std::sin(x * 1.7) * t;
        break;
    case 30: // Reese Core: dense low harmonics and a moving phase-cancellation comb.
        amplitude = (0.45 + 0.55 * std::cos(x * (0.03 + 0.4 * t))) / std::pow(x, 1.2);
        phase = (h % 2 ? 0 : kPi) + 0.55 * std::sin(x * (0.15 + 0.4 * t));
        break;
    case 31: // Rubber Growl: low, narrow resonances bend into a rounded bright tone.
        amplitude = (0.15 + 1.8 * bell(x, 2 + 8 * t, 0.7 + 1.8 * t) +
                     0.6 * bell(x, 5 + 13 * t, 1.1)) / std::pow(x, 1.1);
        phase = 0.45 * std::sin(x * 0.9) * t;
        break;
    case 32: // Acid Hollow: square-like hollowness under a rising narrow peak.
        amplitude = (odd ? 1.0 : 0.1) * (0.25 + 2.3 * bell(x, 3 + 17 * t, 1 + t)) / x;
        break;
    case 33: // Rounded Sub: fundamental-led, with a few quiet low harmonics.
        amplitude = h <= 7 ? std::pow(0.12 + 0.58 * t, x - 1) / std::pow(x, 1.8) : 0;
        phase = (h % 2 == 0 ? 0.3 : 0) * t;
        break;
    case 34: // Teeth Sweep: bright, uneven teeth move over a stable deep root.
        amplitude = (0.1 + bell(x, 4 + 21 * t, 1.6) + 0.6 * bell(x, 11 + 31 * t, 2)) *
                    (h % 3 == 1 ? 1.0 : 0.18) / std::pow(x, 0.85);
        phase = 0.32 * x * t;
        break;
    case 35: // Neuro Edge: animated notches, odd/even asymmetry and a broad bite band.
        amplitude = (odd ? 1.0 : 0.45) * (0.15 + 1.5 * bell(x, 5 + 28 * t, 3 + 4 * t)) *
                    (0.35 + 0.65 * std::pow(std::cos(x * (0.15 + 0.3 * t)), 2)) / std::pow(x, 0.8);
        phase = 0.8 * std::sin(x * 0.7 + 2 * t) * t;
        break;
    case 36: // Reed Cloud: airy reed harmonics and a soft upper resonance.
        amplitude = (odd ? 1.0 : 0.12) * (0.6 + 0.8 * bell(x, 8 + 16 * t, 4)) / std::pow(x, 1.25 - 0.25 * t);
        phase = 0.2 * std::sin(x * 1.2) * t;
        break;
    case 37: // Hollow Wood: a small set of warm, uneven pipe modes.
        amplitude = (bell(x, 2, 0.3) + 0.9 * bell(x, 3 + 2 * t, 0.5) + 0.6 * bell(x, 7 + 3 * t, 0.8) +
                     0.25 * bell(x, 13 + 3 * t, 1.1)) / std::pow(x, 0.9);
        phase = 0.4 * std::sin(x * 1.4) * t;
        break;
    case 38: // Organ Bloom: additive drawbars open from flute into a bright registration.
        amplitude = h == 2 ? 0.35 + 0.3 * t : h == 3 ? 0.15 + 0.5 * t : h == 4 ? 0.12 + 0.65 * t
                  : h == 6 ? 0.05 + 0.5 * t : h == 8 ? 0.35 * t : h == 12 ? 0.25 * t * t : h == 16 ? 0.15 * t * t : 0;
        break;
    case 39: // Air String: bow-position notches and a soft moving high-frequency sheen.
        amplitude = (0.18 + std::abs(std::sin(kPi * x * (0.07 + 0.13 * t)))) *
                    (0.65 + 0.45 * bell(x, 18 + 15 * t, 10)) / std::pow(x, 1.15);
        phase = 0.3 * std::sin(std::log(x + 1) * 2) * t;
        break;
    default: break;
    }
    Partial p = polarPartial(amplitude, phase);
    if (h == 1) p.sinAmp += 0.7;
    return p;
}

std::vector<Wavetable> build() {
    std::vector<Wavetable> tables = {{"Basic", {}}, {"Harmonics", {}}, {"Formant", {}}, {"Pulse", {}},
                                     {"Sub", {}}, {"Growl", {}}, {"Metallic", {}}, {"Digital", {}},
        {"Velvet Saw", {}, "Analog"}, {"Vintage Stack", {}, "Analog"}, {"PWM Drift", {}, "Analog"},
        {"Triangle Bloom", {}, "Analog"}, {"Soft Fold", {}, "Analog"}, {"Sync Sweep", {}, "Analog"},
        {"Odd Ensemble", {}, "Spectral"}, {"Even Bloom", {}, "Spectral"}, {"Harmonic Prism", {}, "Spectral"},
        {"Spectral Comb", {}, "Spectral"}, {"Hollow Sweep", {}, "Spectral"}, {"Glass Ribbon", {}, "Spectral"},
        {"Vowel Sweep", {}, "Vocal"}, {"Throat", {}, "Vocal"}, {"Whisper Formant", {}, "Vocal"},
        {"Choir Air", {}, "Vocal"}, {"Talkbox", {}, "Vocal"}, {"Nasal Reed", {}, "Vocal"},
        {"Iron Bell", {}, "Metal"}, {"Copper Gong", {}, "Metal"}, {"Steel Wire", {}, "Metal"},
        {"Alloy Cluster", {}, "Metal"}, {"Bowed Plate", {}, "Metal"}, {"Chrome Chime", {}, "Metal"},
        {"Bit Fold", {}, "Digital"}, {"Phase Lattice", {}, "Digital"}, {"Binary Stair", {}, "Digital"},
        {"Pixel Pulse", {}, "Digital"}, {"FM Prism", {}, "Digital"}, {"Shard Stack", {}, "Digital"},
        {"Reese Core", {}, "Bass"}, {"Rubber Growl", {}, "Bass"}, {"Acid Hollow", {}, "Bass"},
        {"Rounded Sub", {}, "Bass"}, {"Teeth Sweep", {}, "Bass"}, {"Neuro Edge", {}, "Bass"},
        {"Reed Cloud", {}, "Organic"}, {"Hollow Wood", {}, "Organic"}, {"Organ Bloom", {}, "Organic"},
        {"Air String", {}, "Organic"}};
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
                    const Partial p = t < 8 ? legacyPartial(t, pos, h) : expandedPartial(t, pos, h);
                    buf[size_t(h)] = {p.cosAmp, -p.sinAmp};
                }
                ifft(buf);
                if (level == 0) {
                    double peak = 1e-9;
                    for (const auto& c : buf) peak = std::max(peak, std::abs(c.real()));
                    norm = 1 / peak;
                }
                double levelNorm = norm;
                if (t >= 8 && level > 0) {
                    // Removing high partials can otherwise bury a sparse bell/formant
                    // table. Compensate at most 6 dB, without boosting any mip above its
                    // unit peak or changing the original eight tables' level law.
                    double peak = 1e-9;
                    for (const auto& c : buf) peak = std::max(peak, std::abs(c.real()));
                    levelNorm = std::min(1 / peak, norm * 2);
                }
                float* out = const_cast<float*>(w.frame(level, f));
                for (int i = 0; i < kWtSize; ++i) out[i] = float(buf[size_t(i)].real() * levelNorm);
                out[kWtSize] = out[0];
                float maxSlope = 0;
                for (int i = 0; i < kWtSize; ++i)
                    maxSlope = std::max(maxSlope, std::abs(out[i + 1] - out[i]) * kWtSize);
                w.slope[level][f] = std::nextafter(maxSlope, std::numeric_limits<float>::infinity());
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
const char* wavetableCategory(int index) { return wavetable(index).category; }
float wavetableSlope(const Wavetable& table, int level, float position) {
    level = std::clamp(level, 0, kWtLevels - 1);
    const float frame = std::clamp(position, 0.0f, 1.0f) * (kWtFrames - 1);
    const int first = int(frame), second = std::min(first + 1, kWtFrames - 1);
    const float fraction = frame - first;
    // A convex bound remains valid even if adjacent frames' steepest slopes occur
    // at different phases. It is considerably tighter than a harmonic-count bound.
    return table.slope[level][first] * (1 - fraction) + table.slope[level][second] * fraction;
}

int wavetableLevel(float freq, float sampleRate) {
    const float limit = sampleRate * 0.45f;
    for (int level = 0; level < kWtLevels; ++level)
        if (float(kMaxHarmonics >> level) * freq <= limit) return level;
    return kWtLevels - 1;
}

} // namespace sessions::audio
