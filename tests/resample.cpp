// Frequency and boundary checks for the shared audio-rate converter. This test
// has no GUI, sample files, sound server or optional DSP-library dependency.
#include "audio/Resample.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace sessions::audio;
namespace {
constexpr double pi = 3.14159265358979323846;
int checks = 0, failures = 0;
#define CHECK(condition, text) do { ++checks; if (condition) std::printf("ok: %s\n", text); else { ++failures; std::printf("FAIL: %s\n", text); } } while (0)
std::vector<float> sine(double hz, double sampleRate, int frames = 100000) {
    std::vector<float> x(size_t(frames), 0);
    for (int i = 0; i < frames; ++i) x[size_t(i)] = float(std::sin(2 * pi * hz * i / sampleRate));
    return x;
}
double rms(const std::vector<float>& x, double speed, bool filtered = true) {
    const auto p = resample::plan(filtered ? speed : 1.0);
    constexpr int frames = 32768;
    double energy = 0;
    for (int i = 0; i < frames; ++i) {
        const double position = 64.173 + i * speed;
        const double value = resample::read(x, position, p);
        energy += value * value;
    }
    return std::sqrt(energy / frames);
}
}

int main() {
    resample::prepare();
    resample::prepare(); // repeated instrument / engine preparation is safe

    {
        const std::vector<float> x{0.1f, -0.2f, 0.7f, 0.3f, -0.8f, 0.4f};
        bool exact = true;
        for (size_t i = 0; i < x.size(); ++i) exact &= resample::read(x, double(i)) == x[i];
        CHECK(exact, "Unity-rate integer reads preserve every source sample exactly");
        const double t = 0.375;
        const float y0 = x[1], y1 = x[2], y2 = x[3], y3 = x[4], f = float(t);
        const float expected = y1 + 0.5f * f * (y2 - y0 + f * (2 * y0 - 5 * y1 + 4 * y2 - y3 + f * (3 * (y1 - y2) + y3 - y0)));
        CHECK(resample::read(x, 2 + t, 1) == expected && resample::read(x, 2 + t, 0.5) == expected,
              "Unity and upsampled fractional reads retain the existing cubic interpolation");
        CHECK(!resample::plan(1).phases && !resample::plan(-0.5).phases && resample::plan(2).phases,
              "Resampling plans select filtering only when the physical playback step exceeds unity");
    }
    {
        const std::vector<float> dc(1024, 0.375f);
        bool unity = true;
        double worst = 0;
        for (double speed : {1.0001, 1.088435374, 1.5, 2.0, 2.176870748, 4.0, 8.0, 32.0}) {
            const auto p = resample::plan(speed);
            for (int phase = 0; phase < 1000; ++phase) {
                const double value = resample::read(dc, 200 + phase / 1000.0, p);
                worst = std::max(worst, std::abs(value - 0.375));
                unity &= std::abs(value - 0.375) < 3e-7;
            }
        }
        std::printf("      worst DC gain error %.10f\n", worst);
        CHECK(unity, "Every tested cutoff and fractional phase preserves unity DC gain");
        CHECK(std::abs(resample::read(dc, 200.123, -2.0) - resample::read(dc, 200.123, 2.0)) < 1e-7,
              "Reverse playback receives the same anti-alias filtering as forward playback");
    }
    {
        const auto high = sine(20000, 48000), low = sine(6000, 48000);
        const double rejected = rms(high, 2), alias = rms(high, 2, false), pass = rms(low, 2);
        std::printf("      2x 20 kHz alias rejection %.1f dB, 6 kHz passband %.4f dB\n",
                    20 * std::log10(std::max(rejected, 1e-20) / alias), 20 * std::log10(pass * std::sqrt(2.0)));
        CHECK(rejected < alias * 0.005, "Two-times playback suppresses folded 20 kHz content by at least 46 dB");
        CHECK(std::abs(pass * std::sqrt(2.0) - 1) < 0.01, "Two-times playback preserves the 6 kHz source passband within one percent");
    }
    {
        const auto pass96 = sine(10000, 96000), reject96 = sine(40000, 96000);
        const double pass = rms(pass96, 2), rejected = rms(reject96, 2);
        CHECK(std::abs(pass * std::sqrt(2.0) - 1) < 0.01,
              "96 kHz to 48 kHz conversion preserves the 10 kHz passband");
        CHECK(rejected < 0.004, "96 kHz to 48 kHz conversion removes ultrasonic content before it aliases");
        const auto pass48 = sine(997, 48000);
        CHECK(std::abs(rms(pass48, 0.5) * std::sqrt(2.0) - 1) < 0.001,
              "48 kHz to 96 kHz conversion preserves a low-frequency sine at unity level");
        const auto pass44 = sine(6000, 96000), reject44 = sine(35000, 96000);
        const double step = 96000.0 / 44100.0;
        CHECK(std::abs(rms(pass44, step) * std::sqrt(2.0) - 1) < 0.01,
              "Non-integer 96 kHz to 44.1 kHz conversion preserves its passband");
        CHECK(rms(reject44, step) < 0.004,
              "Non-integer 96 kHz to 44.1 kHz conversion rejects frequencies above its output Nyquist limit");
    }
    {
        const std::vector<float> empty, shortFile{0.3f, -0.5f, 0.2f};
        bool finite = true, distantSilence = true;
        for (double speed : {0.5, 1.0, 2.0, 8.0}) {
            for (double position : {-1000.0, -16.0, -2.1, -0.5, 0.0, 0.5, 2.0, 2.9, 4.0, 18.0, 1000.0}) {
                finite &= std::isfinite(resample::read(shortFile, position, speed));
                distantSilence &= resample::read(empty, position, speed) == 0;
            }
            distantSilence &= resample::read(shortFile, -1000, speed) == 0
                && resample::read(shortFile, 1000, speed) == 0;
            distantSilence &= resample::read(shortFile, std::numeric_limits<double>::infinity(), speed) == 0
                && resample::read(shortFile, std::numeric_limits<double>::quiet_NaN(), speed) == 0;
        }
        CHECK(finite, "Short files and both sample edges are read safely with zero padding");
        CHECK(distantSilence, "Empty samples, invalid positions and reads beyond filter support are silent");
    }
    {
        // Exercise one shared plan on a callback-sized workload. Report timing
        // for regression diagnosis without imposing a machine-dependent limit.
        const auto x = sine(4000, 96000);
        const auto p = resample::plan(2.176870748);
        const auto start = std::chrono::steady_clock::now();
        volatile float checksum = 0;
        for (int repeat = 0; repeat < 16; ++repeat)
            for (int i = 0; i < 32768; ++i) checksum += resample::read(x, 64 + i * 2.176870748, p);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("      524288 filtered reads: %.3f s (%.1f million reads/s), checksum %.4f\n",
                    seconds, 0.524288 / seconds, double(checksum));
        CHECK(std::isfinite(double(checksum)), "Repeated callback-sized filtered reads remain finite");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
