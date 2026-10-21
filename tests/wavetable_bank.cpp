// Additive-bank audit: legacy samples, exact band limits, morphs and playable mips.
#include "audio/Wavetables.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace sessions::audio;
constexpr double pi = 3.14159265358979323846;
int checks = 0, failures = 0;
void check(bool ok, const std::string& message) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", message.c_str()); }
}
uint64_t hash(const Wavetable& w) {
    uint64_t result = 1469598103934665603ULL;
    for (float sample : w.data) {
        uint32_t bits; std::memcpy(&bits, &sample, sizeof bits);
        for (int byte = 0; byte < 4; ++byte) { result ^= (bits >> (byte * 8)) & 255; result *= 1099511628211ULL; }
    }
    return result;
}
void forward(std::vector<std::complex<double>>& values) {
    const size_t n = values.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }
    for (size_t length = 2; length <= n; length <<= 1) {
        const auto step = std::polar(1.0, -2 * pi / double(length));
        for (size_t start = 0; start < n; start += length) {
            std::complex<double> phase = 1;
            for (size_t bin = 0; bin < length / 2; ++bin) {
                const auto a = values[start + bin], b = values[start + bin + length / 2] * phase;
                values[start + bin] = a + b; values[start + bin + length / 2] = a - b; phase *= step;
            }
        }
    }
}
double distance(const float* a, const float* b) {
    double sum = 0;
    for (int i = 0; i < kWtSize; ++i) { const double d = a[i] - b[i]; sum += d * d; }
    return std::sqrt(sum / kWtSize);
}
}

int main() {
    using namespace sessions::audio;
    check(wavetableCount() == 48, "48 indexed tables: eight classic and forty new");
    // Captured from the original eight-table builder before expanding the bank. Keep
    // these full float-data fingerprints: old projects must retain their exact waves.
    const uint64_t legacy[] = {0x176573ff80ad737fULL, 0xfad9f9dea9d1b5f3ULL, 0xcf85c0bde1b024d6ULL,
        0x5e0288ab9d12f074ULL, 0x728030c340c15267ULL, 0x10e24f962c948680ULL,
        0x949f6652a759d28fULL, 0x0d235149dc5a1dcfULL};
    for (int table = 0; table < 8; ++table)
        check(hash(wavetable(table)) == legacy[table], std::string(wavetable(table).name) + ": legacy frames remain bit-identical");

    std::set<std::string> names, categories;
    std::vector<std::complex<double>> spectrum(kWtSize);
    double minNewRms = 1, maxLeak = 0, minMorph = 1, minTableDifference = 1;
    for (int index = 0; index < wavetableCount(); ++index) {
        const auto& table = wavetable(index); const std::string name(table.name);
        check(names.insert(name).second && !name.empty(), name + ": unique descriptive name");
        categories.insert(wavetableCategory(index));
        check(table.category == wavetableCategory(index), name + ": category accessor agrees with table metadata");
        check(table.data.size() == size_t(kWtLevels) * kWtFrames * (kWtSize + 1), name + ": all eight frames and ten mips are present");
        bool finite = true, dcFree = true, seams = true, bandlimited = true, audible = true, bounded = true, morphs = true, slopes = true;
        for (int level = 0; level < kWtLevels; ++level) {
            for (int frame = 0; frame < kWtFrames; ++frame) {
                const float* wave = table.frame(level, frame); double mean = 0, energy = 0, peak = 0;
                for (int i = 0; i < kWtSize; ++i) {
                    finite &= std::isfinite(wave[i]); mean += wave[i]; energy += double(wave[i]) * wave[i];
                    peak = std::max(peak, std::abs(double(wave[i]))); spectrum[size_t(i)] = wave[i];
                    slopes &= std::isfinite(table.slope[level][frame]) && table.slope[level][frame] >=
                              std::abs(wave[i + 1] - wave[i]) * kWtSize;
                }
                const double rms = std::sqrt(energy / kWtSize); mean /= kWtSize;
                dcFree &= std::abs(mean) < 2e-7; seams &= wave[0] == wave[kWtSize]; bounded &= peak <= 1.000001;
                if (index >= 8) { audible &= rms > 0.025; minNewRms = std::min(minNewRms, rms); }
                forward(spectrum);
                const int maxHarmonic = 512 >> level;
                for (int bin = maxHarmonic + 1; bin <= kWtSize / 2; ++bin) {
                    const double leak = std::abs(spectrum[size_t(bin)]) / kWtSize;
                    maxLeak = std::max(maxLeak, leak); bandlimited &= leak < 2e-7;
                }
                if (index >= 8 && level == 0 && frame) {
                    const double d = distance(wave, table.frame(0, frame - 1)); minMorph = std::min(minMorph, d); morphs &= d > 0.0001;
                }
            }
        }
        check(finite && (index < 8 || bounded), name + ": every sample is finite; new tables peak within unity");
        check(dcFree && seams, name + ": every frame is DC-free with an exact periodic seam");
        check(bandlimited, name + ": all ten mips contain no partials above their intended band limit");
        check(slopes, name + ": phase-slope metadata bounds every linearly read sample, including the seam");
        if (index >= 8) {
            check(audible, name + ": all mips retain a usable fundamental, including the highest notes");
            check(morphs, name + ": every adjacent pair of morph frames changes the wave");
        }
        bool interpolation = true;
        for (int level : {0, 4, 9}) for (int frame = 0; frame < kWtFrames - 1; ++frame) {
            const float position = (float(frame) + 0.5f) / float(kWtFrames - 1);
            for (int sample : {0, 37, 513, 1024, 1800}) {
                const float phase = float(sample) / kWtSize;
                const float actual = wavetableRead(table, level, position, phase);
                const float expected = (table.frame(level, frame)[sample] + table.frame(level, frame + 1)[sample]) * 0.5f;
                interpolation &= std::isfinite(actual) && std::abs(actual - expected) < 1e-6;
            }
        }
        check(interpolation, name + ": Position linearly interpolates the actual adjacent frames");
        bool morphSlope = true;
        for (int level = 0; level < kWtLevels; ++level) for (float position : {0.f, .137f, .5f, .893f, 1.f}) {
            const float frame = position * (kWtFrames - 1);
            const int first = int(frame), second = std::min(first + 1, kWtFrames - 1);
            const float fraction = frame - first;
            const float bound = wavetableSlope(table, level, position);
            for (int i = 0; i < kWtSize; ++i) {
                const float a = table.frame(level, first)[i + 1] - table.frame(level, first)[i];
                const float b = table.frame(level, second)[i + 1] - table.frame(level, second)[i];
                const float slope = std::abs(a * (1 - fraction) + b * fraction) * kWtSize;
                morphSlope &= std::isfinite(bound) && bound >= slope * (1 - 1e-6f);
            }
        }
        check(morphSlope, name + ": blended-frame slope bounds safely cover continuously scanned FM sources");
        check(std::abs(wavetableRead(table, 9, 1, 1) - table.frame(9, 7)[0]) < 1e-6,
              name + ": exact phase/position endpoints safely return the periodic seam");
        for (int other = 0; other < index; ++other) {
            double sum = 0;
            for (int frame = 0; frame < kWtFrames; ++frame) sum += distance(table.frame(0, frame), wavetable(other).frame(0, frame));
            minTableDifference = std::min(minTableDifference, sum / kWtFrames);
            check(sum / kWtFrames > 0.01, name + ": distinct morph sequence from " + wavetable(other).name);
        }
    }
    check(categories.size() == 8 && categories.count("Classic") && categories.count("Analog") && categories.count("Spectral") &&
          categories.count("Vocal") && categories.count("Metal") && categories.count("Digital") && categories.count("Bass") && categories.count("Organic"),
          "Eight complete wavetable families are available to the grouped picker");
    check(&wavetable(-1) == &wavetable(0) && &wavetable(500) == &wavetable(47), "Out-of-range table indices clamp safely");
    bool mipSelection = true;
    for (float rate : {44100.f, 48000.f, 96000.f}) for (float frequency : {20.f, 55.f, 110.f, 440.f, 1760.f, 7040.f, 14000.f}) {
        const int level = wavetableLevel(frequency, rate);
        mipSelection &= level >= 0 && level < kWtLevels && float(512 >> level) * frequency <= rate * 0.45f;
        if (level) mipSelection &= float(512 >> (level - 1)) * frequency > rate * 0.45f;
    }
    check(mipSelection, "Mip selection chooses the richest safe spectrum across three sample rates");
    std::printf("New-table minimum RMS %.6f; maximum out-of-band amplitude %.3g; minimum frame motion %.6f; minimum bank difference %.6f\n",
                minNewRms, maxLeak, minMorph, minTableDifference);
    std::printf("wavetable_bank: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
