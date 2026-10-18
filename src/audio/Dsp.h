#pragma once
// Small DSP building blocks for the instruments and effects. Header-only and
// allocation-free once constructed, so everything here is safe on the audio thread.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace sessions::dsp {

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;

inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float gainToDb(float g)  { return g > 1e-9f ? 20.0f * std::log10(g) : -180.0f; }
inline float faderGain(double v) { double g = v / 0.75; return float(g * g); }   // 0.75 = 0 dB, matches dbText()
inline float mtof(float note)   { return 440.0f * std::exp2((note - 69.0f) / 12.0f); }

// Coefficient for a one-pole that covers ~63% of the way in `seconds`.
inline float onePoleCoef(float seconds, float sr) { return std::exp(-1.0f / std::max(1.0f, seconds * sr)); }

struct Noise {
    uint32_t s = 0x9e3779b9u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return float(int32_t(s)) * (1.0f / 2147483648.0f); }
    float uniform() { return 0.5f * (next() + 1.0f); }   // 0..1
};

inline float polyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

// sin(2π·phase) for a phase in 0..1, from a table (organ drawbars, many partials per voice).
// The table is built on first use: call it once on the UI thread before the audio thread does.
inline float sinTable(float phase) {
    static const std::vector<float> t = [] {
        std::vector<float> v(4097);
        for (int i = 0; i <= 4096; ++i) v[size_t(i)] = std::sin(kTwoPi * float(i) / 4096.0f);
        return v;
    }();
    const float x = phase * 4096.0f;
    const int i = std::clamp(int(x), 0, 4095);
    return t[size_t(i)] + (t[size_t(i) + 1] - t[size_t(i)]) * (x - float(i));
}

// Phase-accumulator oscillator; each call returns a sample and advances.
struct Osc {
    float phase = 0.0f;
    void  advance(float dt) { phase += dt; if (phase >= 1.0f) phase -= 1.0f; }
    float sine(float dt)    { float v = std::sin(kTwoPi * phase); advance(dt); return v; }
    float saw(float dt)     { float v = 2.0f * phase - 1.0f - polyBlep(phase, dt); advance(dt); return v; }
    // Shapes at the current phase without advancing (used by the wavetable morph).
    float sineAt() const    { return std::sin(kTwoPi * phase); }
    float triAt() const     { return 4.0f * std::fabs(phase - 0.5f) - 1.0f; }
    float sawAt(float dt) const { return 2.0f * phase - 1.0f - polyBlep(phase, dt); }
    float squareAt(float dt) const {
        float p2 = phase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
        return (phase < 0.5f ? 1.0f : -1.0f) + polyBlep(phase, dt) - polyBlep(p2, dt);
    }
};

// Linear attack, exponential decay / release.
struct Adsr {
    enum Stage { Idle, Attack, Decay, Sustain, Release };
    Stage stage = Idle;
    float level = 0, attackInc = 1, decayCoef = 0, releaseCoef = 0, sustain = 1;

    void set(float a, float d, float s, float r, float sr) {
        attackInc = 1.0f / std::max(1.0f, a * sr);
        decayCoef = onePoleCoef(d / 4.6f, sr);     // ~-40 dB after d seconds
        releaseCoef = onePoleCoef(r / 4.6f, sr);
        sustain = s;
    }
    void on()  { stage = Attack; }
    void off() { if (stage != Idle) stage = Release; }
    void kill() { stage = Idle; level = 0; }
    bool active() const { return stage != Idle; }
    float next() {
        switch (stage) {
        case Attack:
            level += attackInc;
            if (level >= 1.0f) { level = 1.0f; stage = Decay; }
            break;
        case Decay:
            level = sustain + (level - sustain) * decayCoef;
            if (level - sustain < 1e-4f) { level = sustain; stage = sustain > 1e-4f ? Sustain : Idle; }
            break;
        case Release:
            level *= releaseCoef;
            if (level < 1e-4f) { level = 0; stage = Idle; }
            break;
        default: break;
        }
        return level;
    }
};

// Topology-preserving state-variable filter (Cytomic / Simper).
struct Svf {
    float ic1 = 0, ic2 = 0, a1 = 1, a2 = 0, a3 = 0, k = 1.4f;
    void set(float cutoff, float q, float sr) {
        float g = std::tan(kPi * std::clamp(cutoff, 10.0f, sr * 0.45f) / sr);
        k = 1.0f / std::max(0.3f, q);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void reset() { ic1 = ic2 = 0; }
    void tick(float v0, float& lp, float& bp, float& hp) {
        float v3 = v0 - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = v0 - k * v1 - v2;
    }
    float lp(float v0) { float l, b, h; tick(v0, l, b, h); return l; }
    float bp(float v0) { float l, b, h; tick(v0, l, b, h); return b; }
    float hp(float v0) { float l, b, h; tick(v0, l, b, h); return h; }
    float notch(float v0) { float l, b, h; tick(v0, l, b, h); return l + h; }
    // 0 low-pass, 1 high-pass, 2 band-pass, 3 notch
    float run(int type, float v0) {
        float l, b, h; tick(v0, l, b, h);
        switch (type) { case 1: return h; case 2: return b; case 3: return l + h; default: return l; }
    }
    // The response of `type` at `f` for a cutoff `fc` and resonance `q` (for the displays).
    static float magnitude(int type, float f, float fc, float q) {
        const float w = f / std::max(1.0f, fc), w2 = w * w, iq = 1.0f / std::max(0.3f, q);
        const float den = std::sqrt((1 - w2) * (1 - w2) + w2 * iq * iq);
        switch (type) {
        case 1:  return w2 / den;
        case 2:  return w * iq / den;
        case 3:  return std::fabs(1 - w2) / den;
        default: return 1.0f / den;
        }
    }
};

// First-order all-pass (phaser stages): coefficient from allpassCoef().
struct Allpass1 {
    float z = 0;
    float process(float x, float a) { float y = a * x + z; z = x - a * y; return y; }
};
inline float allpassCoef(float f, float sr) {
    const float t = std::tan(kPi * std::clamp(f, 10.0f, sr * 0.45f) / sr);
    return (t - 1.0f) / (t + 1.0f);
}

// LFO shapes (Auto Filter, Auto Pan) at `phase` cycles: 0 sine, 1 triangle, 2 saw up,
// 3 saw down, 4 square, 5 random (a new value every cycle). -1..1.
inline float lfoShape(int shape, double phase) {
    const double cycle = std::floor(phase);
    const float ph = float(phase - cycle);
    switch (shape) {
    case 1:  return ph < 0.25f ? 4 * ph : ph < 0.75f ? 2 - 4 * ph : 4 * ph - 4;
    case 2:  return 2 * ph - 1;
    case 3:  return 1 - 2 * ph;
    case 4:  return ph < 0.5f ? 1.0f : -1.0f;
    case 5: {
        uint32_t h = uint32_t(int64_t(cycle)) * 2654435761u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        return float(h & 0xffff) / 32767.5f - 1.0f;
    }
    default: return std::sin(kTwoPi * ph);
    }
}

// Waveshapers (Saturator): 0 analog (tanh), 1 soft sine, 2 hard clip, 3 fold, 4 digital.
inline float saturate(int type, float x) {
    switch (type) {
    case 1:  return std::fabs(x) < kPi * 0.5f ? std::sin(x) : (x > 0 ? 1.0f : -1.0f);
    case 2:  return std::clamp(x, -1.0f, 1.0f);
    case 3:  return std::sin(x);
    case 4:  return std::round(std::clamp(x, -1.0f, 1.0f) * 12.0f) / 12.0f;
    default: return std::tanh(x);
    }
}

// Compressor gain computer: how many dB to take off a signal `over` dB above the threshold.
inline float compReduction(float over, float ratio, float knee) {
    const float slope = 1.0f / std::max(1.0f, ratio) - 1.0f;
    if (knee < 0.01f) return over > 0 ? slope * over : 0.0f;
    if (2 * over >= knee) return slope * over;
    if (2 * over > -knee) return slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
    return 0.0f;
}

// RBJ cookbook biquad, transposed direct form II.
struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float process(float x) {
        float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void normalize(float a0, float nb0, float nb1, float nb2, float na1, float na2) {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }
    void peak(float f, float q, float gainDb, float sr) {
        float A = std::pow(10.0f, gainDb / 40.0f), w = kTwoPi * f / sr, al = std::sin(w) / (2 * q), c = std::cos(w);
        normalize(1 + al / A, 1 + al * A, -2 * c, 1 - al * A, -2 * c, 1 - al / A);
    }
    void lowShelf(float f, float gainDb, float sr) {
        float A = std::pow(10.0f, gainDb / 40.0f), w = kTwoPi * f / sr, c = std::cos(w), s = std::sin(w);
        float al = s / 2 * std::sqrt(2.0f), sq = 2 * std::sqrt(A) * al;
        normalize((A + 1) + (A - 1) * c + sq, A * ((A + 1) - (A - 1) * c + sq), 2 * A * ((A - 1) - (A + 1) * c),
                  A * ((A + 1) - (A - 1) * c - sq), -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sq);
    }
    void lowpass(float f, float q, float sr) {
        float w = kTwoPi * std::min(f, sr * 0.49f) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        normalize(1 + al, (1 - c) / 2, 1 - c, (1 - c) / 2, -2 * c, 1 - al);
    }
    void highpass(float f, float q, float sr) {
        float w = kTwoPi * std::min(f, sr * 0.49f) / sr, c = std::cos(w), al = std::sin(w) / (2 * q);
        normalize(1 + al, (1 + c) / 2, -(1 + c), (1 + c) / 2, -2 * c, 1 - al);
    }
    void reset() { z1 = z2 = 0; }
    float magnitude(float f, float sr) const {
        const std::complex<float> z1c = std::polar(1.0f, -kTwoPi * f / sr), z2c = z1c * z1c;
        return std::abs((b0 + b1 * z1c + b2 * z2c) / (1.0f + a1 * z1c + a2 * z2c));
    }
    void highShelf(float f, float gainDb, float sr) {
        float A = std::pow(10.0f, gainDb / 40.0f), w = kTwoPi * f / sr, c = std::cos(w), s = std::sin(w);
        float al = s / 2 * std::sqrt(2.0f), sq = 2 * std::sqrt(A) * al;
        normalize((A + 1) - (A - 1) * c + sq, A * ((A + 1) + (A - 1) * c + sq), -2 * A * ((A - 1) + (A + 1) * c),
                  A * ((A + 1) + (A - 1) * c - sq), 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sq);
    }
};

// One-pole low-pass (and the matching high-pass) for tone controls and feedback loops.
struct OnePole {
    float z = 0, a = 0;
    void setLowpass(float f, float sr) { a = std::exp(-kTwoPi * std::min(f, sr * 0.49f) / sr); }
    float lp(float x) { z = x + (z - x) * a; return z; }
    float hp(float x) { return x - lp(x); }
};

// Circular delay line with linear-interpolated reads.
struct DelayLine {
    std::vector<float> buf;
    int w = 0;
    void init(int n) { buf.assign(std::max(4, n), 0.0f); w = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.0f); }
    void push(float x) { buf[w] = x; if (++w >= int(buf.size())) w = 0; }
    float read(float delay) const {   // samples behind the last push, >= 1
        int n = int(buf.size());
        delay = std::clamp(delay, 1.0f, float(n - 2));
        float pos = float(w) - delay;
        while (pos < 0) pos += float(n);
        int i0 = int(pos);
        const float f = pos - float(i0);
        if (i0 >= n) i0 -= n;   // a tiny negative pos plus n can round up to exactly n
        const int i1 = i0 + 1 >= n ? 0 : i0 + 1;
        return buf[size_t(i0)] + (buf[size_t(i1)] - buf[size_t(i0)]) * f;
    }
};

// Freeverb building blocks.
struct Comb {
    std::vector<float> buf;
    int len = 1, idx = 0;
    float store = 0;
    void init(int capacity) { buf.assign(std::max(1, capacity), 0.0f); idx = 0; store = 0; }
    void setLength(int n) { len = std::clamp(n, 1, int(buf.size())); if (idx >= len) idx = 0; }
    float process(float in, float feedback, float damp) {
        float out = buf[idx];
        store = out * (1.0f - damp) + store * damp;
        buf[idx] = in + store * feedback;
        if (++idx >= len) idx = 0;
        return out;
    }
};

struct Allpass {
    std::vector<float> buf;
    int idx = 0;
    void init(int n) { buf.assign(std::max(1, n), 0.0f); idx = 0; }
    float process(float in) {
        float b = buf[idx];
        buf[idx] = in + b * 0.5f;
        if (++idx >= int(buf.size())) idx = 0;
        return b - in;
    }
};

} // namespace sessions::dsp
