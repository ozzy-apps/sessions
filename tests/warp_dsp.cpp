#include "audio/Stretch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

using namespace sessions;
using namespace sessions::audio;

// Count only allocations inside the render callback; construction and test
// fixture vectors are intentionally outside that boundary.
static bool trackAllocations = false;
static size_t callbackAllocations = 0;
void* operator new(std::size_t n) {
    if (trackAllocations) ++callbackAllocations;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
constexpr double pi = 3.14159265358979323846;
int passed = 0, failed = 0;
void check(bool ok, const char* text) {
    std::printf("%s %s\n", ok ? "ok" : "FAIL", text);
    ok ? ++passed : ++failed;
}
struct Stereo { std::vector<float> l, r; };
SampleData tone(double rate, double hz = 220, double rightGain = 0.7) {
    SampleData s; s.rate = rate;
    s.left.resize(size_t(rate * 4)); s.right.resize(s.left.size());
    for (size_t i = 0; i < s.left.size(); ++i) {
        const double t = i / rate;
        s.left[i] = float(0.5 * std::sin(2 * pi * hz * t));
        s.right[i] = float(rightGain * s.left[i]);
    }
    return s;
}
StretchSource source(const SampleData& s, const WarpMarker* markers, StretchMode mode,
                     double tempo = 120, double pitch = 1) {
    StretchSource w; w.sample = &s; w.markers = markers; w.nMarkers = 2;
    w.secPerBeat = 60 / tempo; w.ratio = pitch; w.mode = mode; return w;
}
Stereo render(StretchSource w, int count, float rate = 48000, int block = 256,
              double startBeat = 0, int changeAt = -1, double changedTempo = 120) {
    Stereo out; out.l.resize(size_t(count)); out.r.resize(size_t(count));
    Stretcher st;
    double beat = startBeat;
    for (int begin = 0; begin < count; begin += block) {
        const int end = std::min(count, begin + block);
        for (int i = begin; i < end; ++i) {
            if (i == changeAt) w.secPerBeat = 60 / changedTempo;
            st.sample(w, beat, rate, out.l[size_t(i)], out.r[size_t(i)]);
            beat += 1.0 / (rate * w.secPerBeat);
        }
    }
    return out;
}
double rms(const std::vector<float>& v, int a, int b) {
    double e = 0;
    for (int i = a; i < b; ++i) e += double(v[size_t(i)]) * v[size_t(i)];
    return std::sqrt(e / std::max(1, b - a));
}
double hz(const std::vector<float>& v, int a, int b, double rate = 48000) {
    int count = 0;
    for (int i = a + 1; i < b; ++i) if (v[size_t(i)] >= 0 && v[size_t(i - 1)] < 0) ++count;
    return count * rate / (b - a);
}
double peak(const std::vector<float>& v) {
    double p = 0; for (float x : v) { if (!std::isfinite(x)) return 1e30; p = std::max(p, double(std::abs(x))); } return p;
}
double stereoError(const Stereo& v, double gain, int a, int b) {
    double error = 0, energy = 1e-20;
    for (int i = a; i < b; ++i) {
        const double d = v.r[size_t(i)] - gain * v.l[size_t(i)];
        error += d * d; energy += double(v.l[size_t(i)]) * v.l[size_t(i)];
    }
    return std::sqrt(error / energy);
}
const StretchMode modes[] = {StretchMode::Tones, StretchMode::Texture, StretchMode::Complex, StretchMode::ComplexPro};
}

int main() {
    // Neutral warping must preserve the exact waveform and first attack, even
    // after a seek; use asymmetric broadband stereo, not just a stationary sine.
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        SampleData s = tone(rate);
        for (size_t i = 0; i < s.left.size(); ++i) {
            s.left[i] += float(0.1 * std::sin(i * 0.119) + (i % 137 == 0 ? 0.2 : 0));
            s.right[i] += float(0.1 * std::cos(i * 0.23));
        }
        WarpMarker m[] = {{0, 0}, {8, 4}};
        for (StretchMode mode : modes) {
            StretchSource w = source(s, m, mode); w.flux = 0;
            Stereo a = render(w, 24000, float(rate), 64, 1);
            double error = 0;
            const int offset = int(rate * 0.5);
            for (int i = 0; i < 24000; ++i) error = std::max({error,
                std::abs(double(a.l[size_t(i)]) - s.left[size_t(offset + i)]),
                std::abs(double(a.r[size_t(i)]) - s.right[size_t(offset + i)])});
            check(error < 2e-5, "neutral ratio preserves both channels, including the first sample after a seek");
        }
    }

    // The source's clock must stay independent of the output device's clock.
    // This is a 176 BPM loop playing at 120, a meaningful slow-down for DnB.
    for (double sampleRate : {44100.0, 48000.0, 96000.0}) {
        SampleData s = tone(sampleRate);
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        for (StretchMode mode : modes) {
            const Stereo a = render(source(s, m, mode), 96000);
            const double f = hz(a.l, 24000, 84000);
            const double level = rms(a.l, 24000, 84000);
            check(std::abs(f - 220) < (mode == StretchMode::Texture ? 12 : 3)
                  && level > 0.20 && level < 0.45 && peak(a.l) < 0.8,
                  "176 to 120 preserves tone pitch and level across 44.1 / 48 / 96 kHz sources");
            check(stereoError(a, 0.7, 24000, 84000) < 0.002,
                  "time stretching preserves a fixed stereo balance and phase relationship");
        }
    }

    for (StretchMode mode : modes) {
        SampleData s = tone(44100, 220, -0.8);
        WarpMarker m[] = {{0, 0}, {8, 4}};
        StretchSource w = source(s, m, mode, 120, 2);
        const Stereo up = render(w, 72000);
        check(std::abs(hz(up.l, 12000, 60000) - 440) < 15 && peak(up.l) < 0.9,
              "pitch transposition is correct while duration follows the warp map");
        check(stereoError(up, -0.8, 12000, 60000) < 0.002,
              "opposite-polarity stereo does not break grain alignment or phase locking");
        const Stereo a = render(w, 24000, 48000, 64);
        const Stereo b = render(w, 24000, 48000, 1024);
        check(a.l == b.l && a.r == b.r, "audio is independent of callback block size");
    }

    // A non-periodic attack needs full overlap immediately, not a silent grain
    // ramp. Tones is the time-domain mode; spectral modes deliberately smear a
    // broadband impulse over their analysis window when time is being changed.
    {
        SampleData s = tone(48000);
        std::fill(s.left.begin(), s.left.end(), 0); std::fill(s.right.begin(), s.right.end(), 0);
        s.left[0] = s.right[0] = 0.75f;
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        const Stereo a = render(source(s, m, StretchMode::Tones), 4800);
        check(std::abs(a.l[0] - 0.75f) < 1e-6 && peak(a.l) <= 0.751,
              "Tones preserves the very first transient instead of fading it away");
    }

    // A live tempo change must advance from the beat the engine supplies. A
    // distinctive amplitude step makes an accumulated-clock jump observable.
    for (StretchMode mode : modes) {
        SampleData s = tone(48000);
        for (size_t i = 0; i < s.left.size(); ++i) {
            const float gain = i < 40000 ? 0.4f : 1.0f;
            s.left[i] *= gain; s.right[i] *= gain;
        }
        WarpMarker m[] = {{0, 0}, {8, 4}};
        StretchSource w = source(s, m, mode, 90); w.flux = 0;
        Stereo a = render(w, 72000, 48000, 256, 0, 36000, 150);
        const double beforeStep = rms(a.l, 40000, 43000);
        const double afterStep = rms(a.l, 54000, 57000);
        check(beforeStep < 0.20 && afterStep > 0.23 && peak(a.l) < 0.9,
              "changing project tempo keeps source position continuous and follows the new beat slope");
    }

    // Percussive bursts in a harmonic background exercise phase locking/onset
    // resets on actual changing spectra. Attacks must retain their beat timing;
    // their energy should stay within one analysis window of that beat.
    for (StretchMode mode : {StretchMode::Complex, StretchMode::ComplexPro}) {
        SampleData s = tone(44100);
        for (size_t i = 0; i < s.left.size(); ++i) {
            const double t = i / s.rate;
            const double beat = t * 176 / 60;
            const double within = (beat - std::floor(beat)) * 60 / 176;
            const double burst = within < 0.016 ? 0.6 * std::exp(-within * 240) * std::sin(i * 2.1367) : 0;
            s.left[i] = float(s.left[i] * 0.05 + burst);
            s.right[i] = float(s.right[i] * 0.05 + burst * 0.6);
        }
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        const Stereo a = render(source(s, m, mode), 192000);
        bool timed = true;
        for (int beat = 1; beat < 8; ++beat) {
            const int center = beat * 24000;
            double near = 0, far = 0;
            for (int i = center - 4800; i < center + 4800; ++i) {
                const double e = double(a.l[size_t(i)]) * a.l[size_t(i)];
                (std::abs(i - center) < 1440 ? near : far) += e;
            }
            timed &= near > far * 2;
        }
        check(timed && peak(a.l) < 1, "spectral modes keep percussive attack energy on the 176 to 120 beat grid");
    }

    for (StretchMode mode : {StretchMode::Complex, StretchMode::ComplexPro}) {
        SampleData s; s.rate = 48000;
        s.left.assign(24000, 0); s.right.assign(24000, 0);
        const int onset = 3840; // an isolated attack 80 ms into the source
        s.left[size_t(onset)] = s.right[size_t(onset)] = 0.75f;
        WarpMarker m[] = {{0, 0}, {176.0 / 120, 0.5}};
        const Stereo a = render(source(s, m, mode), 24000);
        const int expected = int(onset * 176.0 / 120);
        int highest = 0;
        double total = 1e-20, early = 0, centroid = 0;
        for (int i = 0; i < 24000; ++i) {
            const double e = double(a.l[size_t(i)]) * a.l[size_t(i)];
            total += e;
            centroid += i * e;
            if (i < expected - 1200) early += e;
            if (std::abs(a.l[size_t(i)]) > std::abs(a.l[size_t(highest)])) highest = i;
        }
        check(std::abs(highest - expected) <= Stretcher::kHop
              && std::abs(centroid / total - expected) < Stretcher::kHop && early / total < 0.005,
              "spectral attack energy and its peak stay within one FFT hop, without distant pre-echo");
    }

    for (StretchMode mode : modes) {
        SampleData s = tone(96000);
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        for (double pitch : {0.25, 1.0, 4.0}) {
            StretchSource w = source(s, m, mode, 120, pitch);
            w.flux = 1; w.formants = 1;
            Stretcher st;
            float l = 0, r = 0;
            bool finite = true;
            callbackAllocations = 0;
            trackAllocations = true;
            for (int i = 0; i < 24000; ++i) {
                st.sample(w, i / 24000.0, 48000, l, r);
                finite &= std::isfinite(l) && std::isfinite(r) && std::abs(l) < 10 && std::abs(r) < 10;
            }
            trackAllocations = false;
            check(finite && callbackAllocations == 0, "all modes remain finite and allocate nothing inside the audio callback");
        }
    }
    for (StretchMode mode : modes) {
        SampleData s = tone(48000);
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        StretchSource w = source(s, m, mode, 120, 0.9); w.flux = 0;
        Stretcher st;
        float last = 0, l = 0, r = 0;
        double step = 0;
        for (int i = 0; i < 48000; ++i) {
            // Change by exactly an octave inside existing grains/FFT frames.
            if (i == 20137) w.ratio = 1.8;
            st.sample(w, i / 24000.0, 48000, l, r);
            if (std::abs(i - 20137) < 32) step = std::max(step, double(std::abs(l - last)));
            last = l;
        }
        check(step < 0.08, "live octave changes advance existing grains and spectral frames without a source-position jump");
    }
    for (StretchMode mode : modes) {
        SampleData s = tone(48000);
        WarpMarker m[] = {{0, 0}, {0.713, 0.3565}, {2, 0.807}, {4, 1.807}};
        StretchSource w = source(s, m, mode); w.nMarkers = 4; w.flux = 0;
        const Stereo a = render(w, 96000);
        double jump = 0;
        for (int at : {17112, 48000})
            for (int i = at - 2; i <= at + 2; ++i)
                jump = std::max(jump, double(std::abs(a.l[size_t(i)] - a.l[size_t(i - 1)])));
        check(jump < 0.08, "piecewise warp maps enter and leave transparent regions without a click");
    }
    for (StretchMode mode : modes) {
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        const SampleData ultrasonic = tone(96000, 31000);
        const Stereo native = render(source(ultrasonic, m, mode), 24000);
        check(rms(native.l, 4800, 22000) < 0.01,
              "native sample-rate conversion removes ultrasonics before they can alias into the audio");
        const SampleData high = tone(48000, 18000);
        const Stereo shifted = render(source(high, m, mode, 120, 2), 24000);
        check(rms(shifted.l, 4800, 22000) < 0.01,
              "pitch-up removes frequencies above the new Nyquist instead of folding them into the bass");
    }
    for (StretchMode mode : modes) {
        SampleData s = tone(44100);
        WarpMarker m[] = {{0, 0}, {176.0 / 15, 4}};
        StretchSource w = source(s, m, mode);
        Stretcher ordinary, prepared, voice;
        prepared.prime(w, 0.125, 48000);
        callbackAllocations = 0; trackAllocations = true;
        voice.copyPrepared(prepared);
        trackAllocations = false;
        bool identical = callbackAllocations == 0;
        for (int i = 0; i < 12000; ++i) {
            float a, b, c, d;
            const double beat = 0.125 + i / 24000.0;
            ordinary.sample(w, beat, 48000, a, b);
            voice.sample(w, beat, 48000, c, d);
            identical &= a == c && b == d;
        }
        check(identical, "prepared voice copies allocate nothing and preserve every output sample of ordinary startup");
    }
    std::printf("warp_dsp: %d ok, %d failures\n", passed, failed);
    return failed ? 1 : 0;
}
