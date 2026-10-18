// Guitar and bass: the Tuner, the Guitar Amp (heads, tone stack, power amp, cabinets or a
// loaded impulse response) and the pedals. Parameter indices follow core/DeviceSpecs.cpp;
// 0 - 10 knobs arrive as 0..1 positions. Every clipping stage runs 4x oversampled so high
// gain stays smooth instead of fizzing with aliases.

#include "Convolver.h"
#include "DeviceKit.h"
#include <array>

namespace sessions::audio {

using namespace kit;

namespace {

inline float db(float d) { return dbToGain(d); }

// ============================================================================ building blocks

// A 64-tap low-pass at 4x the rate (Blackman-windowed sinc, a little under the original
// Nyquist), shared by every oversampler.
const float* osTaps() {
    static const std::array<float, 64> h = [] {
        std::array<float, 64> t{};
        const double fc = 0.11, mid = 31.5;
        double sum = 0;
        for (int i = 0; i < 64; ++i) {
            const double x = i - mid;
            const double sinc = std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / 63) + 0.08 * std::cos(4 * M_PI * i / 63);
            t[size_t(i)] = float(sinc * w);
            sum += t[size_t(i)];
        }
        for (float& v : t) v = float(v / sum);
        return t;
    }();
    return h.data();
}

// One channel through a nonlinearity at 4x the rate: up (polyphase), f(), down.
struct Oversampler4 {
    float hist[16] = {}, down[64] = {};
    int   pos = 0;
    template <class F> float run(float x, F&& f) {
        const float* h = osTaps();
        for (int j = 15; j > 0; --j) hist[j] = hist[j - 1];
        hist[0] = x;
        for (int ph = 0; ph < 4; ++ph) {
            float u = 0;
            for (int j = 0; j < 16; ++j) u += h[ph + 4 * j] * hist[j];
            down[pos] = f(u * 4.0f);
            pos = (pos + 1) & 63;
        }
        float y = 0;
        for (int i = 0; i < 64; ++i) y += h[i] * down[(pos - 1 - i) & 63];
        return y;
    }
    void reset() { std::fill(hist, hist + 16, 0.0f); std::fill(down, down + 64, 0.0f); pos = 0; }
};

// A cascade of biquads: filters, tone stacks, cabinets. magnitude() draws them.
struct BiquadChain {
    Biquad b[12];
    int n = 0;
    void clear() { n = 0; }
    Biquad& add() { b[n].reset(); return b[n++]; }
    void peak(float f, float q, float gainDb, float sr) { if (n < 12) add().peak(f, q, gainDb, sr); }
    void lowShelf(float f, float gainDb, float sr) { if (n < 12) add().lowShelf(f, gainDb, sr); }
    void highShelf(float f, float gainDb, float sr) { if (n < 12) add().highShelf(f, gainDb, sr); }
    void lowpass(float f, float q, float sr) { if (n < 12) add().lowpass(f, q, sr); }
    void highpass(float f, float q, float sr) { if (n < 12) add().highpass(f, q, sr); }
    float process(float x) { for (int i = 0; i < n; ++i) x = b[i].process(x); return x; }
    float magnitude(float f, float sr) const { float m = 1; for (int i = 0; i < n; ++i) m *= b[i].magnitude(f, sr); return m; }
    void reset() { for (int i = 0; i < n; ++i) { b[i].z1 = 0; b[i].z2 = 0; } }
    // Re-designs without clearing the filters' state (knob moves don't click).
    void copyCoefficients(const BiquadChain& o) {
        n = o.n;
        for (int i = 0; i < n; ++i) { b[i].b0 = o.b[i].b0; b[i].b1 = o.b[i].b1; b[i].b2 = o.b[i].b2; b[i].a1 = o.b[i].a1; b[i].a2 = o.b[i].a2; }
    }
};

inline float tube(float x, float bias) { return std::tanh(x + bias) - std::tanh(bias); }   // asymmetric: even harmonics
inline float hardKnee(float x) {   // a diode pair to ground: linear, a short knee, then flat
    const float a = std::fabs(x), k = 0.6f;
    const float y = a < k ? a : k + (1 - k) * std::tanh((a - k) / (1 - k));
    return x < 0 ? -y : y;
}
inline float fuzzClip(float x, float bias) {   // a starved transistor: squares up, lopsided
    const float y = x + bias;
    return y > 0 ? 1 - std::exp(-y * 1.6f) : -(1 - std::exp(y * 1.2f)) * 0.85f;
}

struct DcBlock {   // 10 Hz high-pass
    float x1 = 0, y1 = 0, r = 0.9987f;
    void set(float sr) { r = std::exp(-kTwoPi * 10.0f / sr); }
    float process(float x) { const float y = x - x1 + r * y1; x1 = x; y1 = y; return y; }
};

// A peak level for the panels, held over a block.
inline void storePeak(std::atomic<float>& m, const float* L, const float* R, int n) {
    float p = 0;
    for (int i = 0; i < n; ++i) p = std::max(p, std::max(std::fabs(L[i]), std::fabs(R[i])));
    m.store(p, std::memory_order_relaxed);
}

// ============================================================================ Guitar Amp

struct HeadVoice {
    float gainLo, gainHi;    // preamp gain at Gain 0 / 10 (dB)
    float tightHz;           // low cut before the preamp
    float brightDb;          // treble lift before the preamp (at 2.5 kHz)
    int   stages;            // clipping stages
    float stage2Db, stage3Db, bias, interLpHz;
    float bassHz, midHz, midQ, trebleHz, voiceMidDb;   // the tone stack and its built-in mid voicing
    float powerDb;           // power amp drive at Volume 5
    float trimDb;            // output level
    float cleanBlend;        // bass head: clean low end mixed back in
};

constexpr HeadVoice kHeads[guitaramp::kHeads] = {
    // Clean: lots of headroom, scooped and sparkly
    {0, 22, 60, 4, 1, 0, 0, 0.05f, 9000, 110, 420, 0.8f, 3000, -2, 0, -12, 0},
    // Crunch: the plexi, mid-forward
    {6, 32, 80, 2.5f, 2, 10, 0, 0.12f, 7500, 120, 650, 0.9f, 2800, 1, 2, -21, 0},
    // Lead: hot-rodded, more gain, tighter
    {12, 40, 110, 2, 2, 16, 0, 0.15f, 7000, 120, 700, 0.9f, 3000, 2, 3, -23, 0},
    // Metal: three stages, a tight low end, scooped by default
    {16, 46, 150, 3, 3, 14, 8, 0.2f, 6500, 100, 750, 1.0f, 3200, -3, 3, -23, 0},
    // Bass: warm, the low end kept clean underneath
    {0, 28, 30, 1, 1, 0, 0, 0.1f, 6000, 70, 500, 0.7f, 2200, 0, 0, -14, 0.4f},
};

int headOf(const float* p) { return std::clamp(choiceIndex(DeviceType::GuitarAmp, guitaramp::Head, p[guitaramp::Head]), 0, guitaramp::kHeads - 1); }
int cabOf(const float* p)  { return choiceIndex(DeviceType::GuitarAmp, guitaramp::Cab, p[guitaramp::Cab]); }
int micOf(const float* p)  { return choiceIndex(DeviceType::GuitarAmp, guitaramp::Mic, p[guitaramp::Mic]); }

// Bass / Mid / Treble around the head's voicing, then Presence (in the power amp's feedback).
void designTone(const float* p, float sr, BiquadChain& c) {
    const HeadVoice& h = kHeads[headOf(p)];
    c.clear();
    c.lowShelf(h.bassHz, (p[guitaramp::Bass] - 0.5f) * 26, sr);
    c.peak(h.midHz, h.midQ, (p[guitaramp::Mid] - 0.5f) * 24 + h.voiceMidDb, sr);
    c.highShelf(h.trebleHz, (p[guitaramp::Treble] - 0.5f) * 26, sr);
    c.highShelf(4500, (p[guitaramp::Presence] - 0.5f) * 16, sr);
}

// The built-in cabinets: a speaker's low resonance, its cone breakup bumps and dips, and the
// steep fall above 5 kHz, then the mic.
void designCab(int cab, int mic, float sr, BiquadChain& c) {
    c.clear();
    switch (cab) {
    case 1:   // 1x12 open back
        c.highpass(75, 0.7f, sr); c.peak(110, 1.2f, 3, sr); c.peak(420, 1.0f, -3, sr); c.peak(2200, 1.3f, 3, sr);
        c.lowpass(5200, 0.7f, sr); c.lowpass(6500, 0.9f, sr); break;
    case 2:   // 2x12 combo
        c.highpass(80, 0.7f, sr); c.peak(100, 1.4f, 4, sr); c.peak(500, 1.0f, -3, sr); c.peak(2500, 1.2f, 4, sr);
        c.lowpass(5000, 0.7f, sr); c.lowpass(6000, 0.8f, sr); break;
    case 3:   // 4x12 closed back, modern
        c.highpass(85, 0.8f, sr); c.peak(115, 1.8f, 5, sr); c.peak(450, 1.0f, -5, sr); c.peak(2800, 1.5f, 5, sr);
        c.lowpass(4700, 0.7f, sr); c.lowpass(5600, 1.0f, sr); break;
    case 4:   // 4x12 vintage, greenback-ish
        c.highpass(90, 0.7f, sr); c.peak(125, 1.4f, 3, sr); c.peak(600, 1.2f, -2, sr); c.peak(1900, 1.1f, 4, sr);
        c.lowpass(4200, 0.7f, sr); c.lowpass(5200, 0.8f, sr); break;
    case 5:   // 8x10 bass
        c.highpass(45, 0.7f, sr); c.peak(75, 1.2f, 3, sr); c.peak(600, 1.0f, -4, sr); c.peak(2200, 1.2f, 3, sr);
        c.lowpass(4500, 0.7f, sr); c.lowpass(6000, 0.7f, sr); break;
    default: return;
    }
    if (mic == 1) { c.lowpass(3300, 0.7f, sr); c.peak(2500, 1.0f, -2, sr); }   // off axis: darker, less bite
    else if (mic == 2) { c.lowpass(3800, 0.7f, sr); c.lowShelf(150, -2, sr); } // far: the room takes the edge and the thump
}

class GuitarAmp : public DeviceProcessor {
public:
    explicit GuitarAmp(float sr) : m_sr(sr) {
        osTaps();
        for (int c = 0; c < 2; ++c) {
            m_dc[c].set(sr); m_dc2[c].set(sr);
            m_room[c].init(int(sr * 0.012f) + 8);
        }
    }
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        storePeak(meter[0], L, R, n);
        if (!std::equal(p, p + guitaramp::DryWet + 1, m_last)) {
            std::copy(p, p + guitaramp::DryWet + 1, m_last);
            design(p);
        }
        const HeadVoice& h = kHeads[headOf(p)];
        const int cab = cabOf(p);
        const bool far = micOf(p) == 2;
        const IrKernel* ir = cab == guitaramp::kLoadedIr ? ctx.ir : nullptr;
        if (ir != m_irNow) { m_irNow = ir; m_conv[0].reset(); m_conv[1].reset(); }
        const float pre = db(h.gainLo + (h.gainHi - h.gainLo) * p[guitaramp::Gain]);
        const float s2 = db(h.stage2Db), s3 = db(h.stage3Db);
        const float drive = db((p[guitaramp::Volume] - 0.5f) * 24 + h.powerDb);
        const float makeup = db(h.trimDb) / std::pow(drive, 0.55f) * db(pv(T::GuitarAmp, p, guitaramp::Output));
        const float wet = p[guitaramp::DryWet];
        const float sagCoef = onePoleCoef(0.08f, m_sr);
        float* io[2] = {L, R};
        for (int c = 0; c < 2; ++c) {
            float* x = io[c];
            for (int i = 0; i < n; ++i) {
                const float dry = x[i];
                m_dry[i] = dry;
                float v = m_pre[c].process(m_dc[c].process(dry)) * pre;
                OnePole& inter2 = m_inter2[c];
                OnePole& inter3 = m_inter3[c];
                v = m_os[c].run(v, [&](float u) {
                    float y = tube(u, h.bias);
                    if (h.stages >= 2) y = tube(inter2.lp(y) * s2, h.bias * 0.7f);
                    if (h.stages >= 3) y = std::tanh(inter3.lp(y) * s3);
                    return y;
                });
                v = m_dc2[c].process(v);
                v = m_tone[c].process(v);
                // the power amp: soft saturation that sags a little when pushed
                m_sag[c] = std::fabs(v) + (m_sag[c] - std::fabs(v)) * sagCoef;
                const float g = drive / (1 + 0.35f * std::min(2.0f, m_sag[c] * drive));
                v = std::tanh(v * g) * makeup;
                if (h.cleanBlend > 0) v = v * (1 - h.cleanBlend) + m_cleanLow[c].lp(dry) * h.cleanBlend * 2.0f;
                if (cab > 0 && cab < guitaramp::kLoadedIr) {
                    v = m_cab[c].process(v);
                    if (far) { m_room[c].push(v); v = 0.8f * v + 0.35f * m_room[c].read(0.0019f * m_sr) + 0.2f * m_room[c].read(0.0071f * m_sr); }
                }
                m_tmp[i] = v;
            }
            // the loaded IR, in place of the built-in cabinet (one block of latency)
            if (ir) m_conv[c].process(*ir, ir->stereo ? c : 0, m_tmp, m_tmp, n);
            for (int i = 0; i < n; ++i) x[i] = m_dry[i] * (1 - wet) + m_tmp[i] * wet;
        }
        storePeak(meter[1], L, R, n);
    }
    void panic() override {
        for (int c = 0; c < 2; ++c) {
            m_os[c].reset(); m_tone[c].reset(); m_cab[c].reset(); m_pre[c].reset(); m_conv[c].reset(); m_room[c].clear();
            m_sag[c] = 0;
        }
    }
private:
    void design(const float* p) {
        const HeadVoice& h = kHeads[headOf(p)];
        BiquadChain pre, tone, cab;
        pre.highpass(h.tightHz, 0.707f, m_sr);
        pre.highShelf(2500, h.brightDb, m_sr);
        designTone(p, m_sr, tone);
        designCab(cabOf(p), micOf(p), m_sr, cab);
        for (int c = 0; c < 2; ++c) {
            m_pre[c].copyCoefficients(pre);
            m_tone[c].copyCoefficients(tone);
            m_cab[c].copyCoefficients(cab);
            m_inter2[c].setLowpass(h.interLpHz, m_sr * 4);
            m_inter3[c].setLowpass(h.interLpHz * 0.9f, m_sr * 4);
            m_cleanLow[c].setLowpass(150, m_sr);
        }
    }
    float m_sr;
    float m_last[guitaramp::DryWet + 1] = {-1};
    BiquadChain m_pre[2], m_tone[2], m_cab[2];
    Oversampler4 m_os[2];
    OnePole m_inter2[2], m_inter3[2], m_cleanLow[2];
    DcBlock m_dc[2], m_dc2[2];
    DelayLine m_room[2];
    float m_sag[2] = {0, 0};
    Convolver m_conv[2];
    const IrKernel* m_irNow = nullptr;
    float m_tmp[kMaxBlock] = {}, m_dry[kMaxBlock] = {};
};

// ============================================================================ Tuner

// Passes the sound (or mutes it) and finds its pitch: YIN on a 4x decimated copy for the
// period, refined at the full rate. meter[0] = the frequency (0: none), meter[1] = level (dBFS).
class Tuner : public DeviceProcessor {
public:
    explicit Tuner(float sr) : m_sr(sr) {
        m_full.assign(kFull, 0.0f);
        m_lin.assign(kFull, 0.0f);
        m_low.assign(kLow, 0.0f);
        m_aa1.setLowpass(2500, sr);
        m_aa2.setLowpass(2500, sr);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        const float a = onePoleCoef(0.01f, m_sr);
        const float target = pb(p, 1) ? 0.0f : 1.0f;
        for (int i = 0; i < n; ++i) {
            const float x = 0.5f * (L[i] + R[i]);
            // low-passed, so the period's peak is round enough for the parabola to land on it
            const float f = m_aa2.lp(m_aa1.lp(x));
            m_full[size_t(m_fullPos)] = f;
            m_fullPos = (m_fullPos + 1) % kFull;
            if (++m_dec == 4) {
                m_dec = 0;
                m_low[size_t(m_lowPos)] = f;
                m_lowPos = (m_lowPos + 1) % kLow;
                if (++m_sinceRun >= 384) { m_sinceRun = 0; analyse(); }
            }
            const float g = m_mute.next(target, a);
            L[i] *= g; R[i] *= g;
        }
    }
private:
    static constexpr int kFull = 4096, kLow = 1024, kMaxLag = 300, kMinLag = 6;
    float lowAt(int back) const { return m_low[size_t((m_lowPos - 1 - back + kLow * 2) % kLow)]; }
    float fullAt(int back) const { return m_full[size_t((m_fullPos - 1 - back + kFull * 2) % kFull)]; }

    void analyse() {
        const int W = kLow - kMaxLag;
        float low[kLow];
        for (int j = 0; j < kLow; ++j) low[j] = lowAt(j);   // newest first
        double energy = 0;
        for (int j = 0; j < W; ++j) energy += double(low[j]) * low[j];
        const float levelDb = gainToDb(float(std::sqrt(energy / W)));
        meter[1].store(levelDb, std::memory_order_relaxed);
        if (levelDb < -55) { meter[0].store(0.0f, std::memory_order_relaxed); return; }
        // YIN: difference function, cumulative mean normalised, first dip under the threshold
        float d[kMaxLag + 1];
        d[0] = 1;
        double running = 0;
        for (int tau = 1; tau <= kMaxLag; ++tau) {
            double s = 0;
            const float* a = low;
            const float* b = low + tau;
            for (int j = 0; j < W; ++j) { const float e = a[j] - b[j]; s += double(e * e); }
            running += s;
            d[tau] = running > 0 ? float(s * tau / running) : 1.0f;
        }
        int best = -1;
        for (int tau = kMinLag; tau < kMaxLag; ++tau)
            if (d[tau] < 0.15f) { while (tau + 1 < kMaxLag && d[tau + 1] < d[tau]) ++tau; best = tau; break; }
        if (best < 0) { meter[0].store(0.0f, std::memory_order_relaxed); return; }
        // refine at the full rate around 4 x tau with the normalised square difference (McLeod),
        // which a decaying string doesn't pull off: its peak, then a parabola through it
        const int centre = best * 4, Wf = 2048;
        for (int j = 0; j < kFull; ++j) m_lin[size_t(j)] = fullAt(j);
        double vals[11];
        int bi = -1;
        double bestVal = -2;
        for (int k = -5; k <= 5; ++k) {
            const int tau = centre + k;
            double v = -1;
            if (tau > 8 && tau + Wf < kFull) {
                double r = 0, m = 0;
                const float* x = m_lin.data();
                for (int j = 0; j < Wf; ++j) { r += double(x[j]) * x[j + tau]; m += double(x[j]) * x[j] + double(x[j + tau]) * x[j + tau]; }
                v = m > 0 ? 2 * r / m : -1;
            }
            vals[k + 5] = v;
            if (v > bestVal) { bestVal = v; bi = k + 5; }
        }
        double refined = centre + (bi - 5);
        if (bi > 0 && bi < 10) {
            const double den = vals[bi - 1] - 2 * vals[bi] + vals[bi + 1];
            if (std::fabs(den) > 1e-12) refined += 0.5 * (vals[bi - 1] - vals[bi + 1]) / den;
        }
        meter[0].store(refined > 0 ? float(m_sr / refined) : 0.0f, std::memory_order_relaxed);
    }

    float m_sr;
    std::vector<float> m_full, m_lin, m_low;
    int m_fullPos = 0, m_lowPos = 0, m_dec = 0, m_sinceRun = 0;
    OnePole m_aa1, m_aa2;
    Smoothed m_mute;
};

// ============================================================================ drive pedals
//
// Seven boxes, each its own circuit rather than one clipper behind different filters:
//   Overdrive        the green screamer: diodes in the op-amp's feedback loop, so the lows pass
//                    clean at gain 1 and only the mids clip, softly; Clip picks the 808's matched
//                    pair or a lopsided one (even harmonics)
//   Distortion       the rat: an op-amp stage whose gain leans on the treble, slew-limited, into
//                    a hard clipping pair (silicon, LEDs or germanium), then the one-pole Filter
//   Metal Distortion the mt-2: Tight high-pass and a pre-emphasis bump, an op-amp stage into hard
//                    diodes with a coupling cap between, then a three-band EQ with a sweepable mid
//   Chainsaw         the hm-2: a transistor into lopsided diodes, then the two gyrator boosts
//                    (Low at 92 Hz, High at 1.1 kHz with a shelf above): the buzzsaw
//   Metal Boost      a tight clean boost: Tight cuts the lows before an amp, barely any clipping,
//                    Tone a treble shelf, lots of level
//   Fuzz             two starved transistors: a dark input, a huge first stage, a coupling cap, a
//                    biased second stage that squares up and sputters when starved (Bias), then
//                    the muff's tone blend and a dark output
//   Crunch           a plexi in a box: two triode-like stages (lopsided, their bias sagging with
//                    the playing), Bright across the gain pot, Tone a tilt, Presence a bite on top
// Every clipping stage runs 4x oversampled.

struct DriveChannel {
    BiquadChain pre, post;
    Oversampler4 os;
    OnePole a, b, c;    // per pedal: slew, coupling caps, emphasis (at 4x inside the clipper, or at the rate after)
    DcBlock dc;
    float env = 0;      // the input's envelope: sag and gating
};

// The shapers, memoryless, so the panel draws them with the same code (pedalTransfer).
inline float odClip(float u, bool asym) { return asym ? (u > 0 ? std::tanh(u) : 0.72f * std::tanh(u / 0.72f)) : std::tanh(u); }
inline float ratClip(float u, int kind) {
    switch (kind) {
    case 1:  return 1.7f * hardKnee(u / 1.7f);       // LEDs: a higher ceiling, more open, louder
    case 2:  return 0.55f * std::tanh(u / 2.2f);     // germanium: an early, soft, compressed clip (a gentle slope through zero)
    default: return hardKnee(u);                      // silicon
    }
}
inline float metalStage(float u) { return 0.8f * hardKnee(2.5f * std::tanh(u)); }                      // op-amp soft, then hard diodes
inline float sawClip(float u) { return u > 0 ? hardKnee(u) : 0.75f * hardKnee(u / 0.75f); }             // a transistor into lopsided diodes
inline float fuzzStage(float u, float bias) { return fuzzClip(std::tanh(u) * 6.0f, bias); }              // the two transistors, memoryless
inline float crunchStages(float u, float bias) { return 0.8f * tube(2.5f * tube(u, bias), 0.32f); }      // two triodes

class DrivePedal : public DeviceProcessor {
public:
    DrivePedal(DeviceType type, float sr) : m_type(type), m_sr(sr) {
        osTaps();
        for (DriveChannel& c : m_ch) c.dc.set(sr);
        m_envRel = onePoleCoef(0.08f, sr);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        const int count = paramCount(m_type);
        if (!std::equal(p, p + count, m_last)) { std::copy(p, p + count, m_last); design(p); }
        float* io[2] = {L, R};
        const float a = onePoleCoef(0.01f, m_sr);
        for (int c = 0; c < 2; ++c) {
            DriveChannel& ch = m_ch[c];
            float* x = io[c];
            for (int i = 0; i < n; ++i) {
                const float in = std::fabs(x[i]);
                ch.env = in > ch.env ? in : in + (ch.env - in) * m_envRel;
                const float g = m_out[c].next(m_level, a);
                x[i] = ch.dc.process(clip(ch, p, x[i])) * g;
            }
        }
        storePeak(meter[0], L, R, n);
    }
    void panic() override { for (DriveChannel& c : m_ch) { c.os.reset(); c.pre.reset(); c.post.reset(); c.a.z = c.b.z = c.c.z = 0; c.env = 0; } }

    // The gain into the clipper at these knobs.
    static float gainOf(DeviceType t, const float* p) {
        switch (t) {
        case T::Overdrive:       return 1 + 119 * p[0] * p[0];                 // 1 .. 120
        case T::Distortion:      return 1 + 1500 * std::pow(p[0], 2.2f);        // 1 .. 1500
        case T::MetalDistortion: return db(20 + p[0] * 40);                     // 20 .. 60 dB
        case T::Chainsaw:        return db(24 + p[0] * 28);                     // 24 .. 52 dB
        case T::MetalBoost:      return 1 + p[0] * 8;                           // 1 .. 9
        case T::Fuzz:            return db(20 + p[0] * 30);                     // 20 .. 50 dB
        case T::Crunch:          return db(6 + p[0] * 30);                      // 6 .. 36 dB
        default: return 1;
        }
    }
    // The clipper alone (no filters, no envelope) for an input `x`: what the panel draws.
    static float transfer(DeviceType t, const float* p, float x) {
        const float g = gainOf(t, p);
        switch (t) {
        case T::Overdrive:       return (x + 0.55f * odClip(g * x, choiceIndex(t, 3, p[3]) == 1)) / 1.55f;
        case T::Distortion:      return ratClip(g * x, choiceIndex(t, 3, p[3])) / 1.7f;
        case T::MetalDistortion: return metalStage(g * x) / 0.8f;
        case T::Chainsaw:        return sawClip(g * x);
        case T::MetalBoost:      return std::tanh(g * x);
        case T::Fuzz:            return fuzzStage(g * x, -0.35f + 0.45f * p[3]);
        case T::Crunch:          return crunchStages(g * x, 0.25f) / 1.3f;
        default: return x;
        }
    }

    // the filters only (for the panel's response display)
    static void designFilters(DeviceType t, const float* p, float sr, BiquadChain& pre, BiquadChain& post) {
        pre.clear(); post.clear();
        switch (t) {
        case T::Overdrive:
            pre.highpass(720, 0.5f, sr);                                       // the clipped path: mids and highs only
            post.lowpass(600 * std::pow(10.0f, p[1] * 1.1f), 0.6f, sr);        // Tone: 600 Hz .. 7.5 kHz
            post.highpass(70, 0.7f, sr);
            break;
        case T::Distortion:
            pre.highpass(60 + 140 * p[0], 0.7f, sr);                           // the gain pot tightens the lows as it goes up
            pre.highShelf(1500, 2 + 8 * p[0], sr);                             // the stage's gain leans on the treble
            break;                                                             // the Filter is a one-pole after the clip (design)
        case T::MetalDistortion:
            pre.highpass(float(paramValue(t, 6, p[6])), 0.8f, sr);             // Tight
            pre.peak(1000, 0.8f, 6, sr);                                        // pre-emphasis
            post.lowShelf(100, (p[1] - 0.5f) * 30, sr);
            post.peak(float(paramValue(t, 3, p[3])), 1.0f, (p[2] - 0.5f) * 30, sr);
            post.highShelf(3000, (p[4] - 0.5f) * 30, sr);
            post.lowpass(7500, 0.7f, sr);
            break;
        case T::Chainsaw:
            pre.highpass(80, 0.7f, sr);
            pre.peak(900, 0.8f, 4, sr);
            post.peak(92, 1.4f, -10 + p[1] * 28, sr);                          // Low: the buzz, -10 .. +18 dB
            post.peak(1100, 1.0f, -10 + p[2] * 26, sr);                        // High: the saw, -10 .. +16 dB
            post.peak(5000, 1.5f, p[2] * 8, sr);
            post.lowpass(7500, 0.7f, sr);
            break;
        case T::MetalBoost:
            pre.highpass(float(paramValue(t, 3, p[3])), 0.8f, sr);             // Tight
            post.highShelf(2000, -8 + p[1] * 16, sr);                          // Tone: a treble shelf, -8 .. +8 dB
            post.lowpass(9000, 0.7f, sr);
            break;
        case T::Fuzz:
            pre.highpass(90, 0.7f, sr);
            pre.lowpass(4000, 0.7f, sr);                                       // a dark input
            post.lowpass(5500, 0.6f, sr);                                      // and a dark output
            break;
        case T::Crunch:
            pre.highpass(70, 0.7f, sr);
            if (p[3] >= 0.5f) pre.highShelf(2000, 7, sr);                       // Bright
            post.lowShelf(300, -(p[1] - 0.5f) * 14, sr);                       // Tone: a tilt
            post.highShelf(2000, (p[1] - 0.5f) * 14, sr);
            post.peak(3500, 1.2f, -4 + p[4] * 12, sr);                         // Presence
            post.lowpass(8000, 0.7f, sr);
            break;
        default: break;
        }
    }
    // The filter shape the panel draws (dB at f): the fuzz's tone blend and the rat's one-pole too.
    static float response(DeviceType t, const float* p, float f, float sr) {
        BiquadChain pre, post;
        designFilters(t, p, sr, pre, post);
        float m = pre.magnitude(f, sr) * post.magnitude(f, sr);
        if (t == T::Overdrive) m = (1 + 0.55f * pre.magnitude(f, sr)) * post.magnitude(f, sr) / 1.55f;   // clean lows plus the clipped mids
        if (t == T::Distortion) {
            const float fc = 20000 * std::pow(0.025f, p[1]);
            m *= 1.0f / std::sqrt(1 + (f / fc) * (f / fc));
        }
        if (t == T::Fuzz) {
            const float tone = p[1];
            const float lp = 1.0f / std::sqrt(1 + (f / 700) * (f / 700)), hp = (f / 1200) / std::sqrt(1 + (f / 1200) * (f / 1200));
            m *= std::fabs((1 - tone) * lp + tone * hp * 1.4f);
        }
        return gainToDb(std::max(1e-6f, m));
    }

private:
    void design(const float* p) {
        BiquadChain pre, post;
        designFilters(m_type, p, m_sr, pre, post);
        for (DriveChannel& c : m_ch) { c.pre.copyCoefficients(pre); c.post.copyCoefficients(post); }
        m_gain = gainOf(m_type, p);
        switch (m_type) {
        case T::Overdrive:  m_level = db(-14 + p[2] * 20); m_asym = choiceIndex(m_type, 3, p[3]) == 1; break;
        case T::Distortion:
            m_level = db(-38 + p[2] * 24);
            m_kind = choiceIndex(m_type, 3, p[3]);
            for (DriveChannel& c : m_ch) {
                c.a.setLowpass(std::clamp(1e6f / m_gain, 2500.0f, 20000.0f), m_sr * 4);   // the op-amp's slew
                c.b.setLowpass(20000 * std::pow(0.025f, p[1]), m_sr);                     // Filter: up = darker, down to 500 Hz
            }
            break;
        case T::MetalDistortion:
            m_level = db(-36 + p[5] * 24);
            for (DriveChannel& c : m_ch) { c.a.setLowpass(120, m_sr * 4); c.b.setLowpass(6000, m_sr * 4); }   // coupling cap, interstage fizz cut
            break;
        case T::Chainsaw:   m_level = db(-46 + p[3] * 24); break;
        case T::MetalBoost: m_level = db(-20 + p[2] * 30); break;   // unity near 7, +10 dB on 10
        case T::Fuzz:
            m_level = db(-30 + p[2] * 24);
            m_bias = -0.35f + 0.45f * p[3];                                                 // starved .. warm
            m_gate = 0.03f * (1 - p[3]) * (1 - p[3]);                                        // starved: sputters below this
            for (DriveChannel& c : m_ch) { c.a.setLowpass(100, m_sr * 4); c.b.setLowpass(700, m_sr); c.c.setLowpass(1200, m_sr); }
            break;
        case T::Crunch:
            m_level = db(-30 + p[2] * 24);
            for (DriveChannel& c : m_ch) c.a.setLowpass(60, m_sr * 4);                     // the coupling cap between the triodes
            break;
        default: break;
        }
    }

    float clip(DriveChannel& ch, const float* p, float x) {
        const float g = m_gain;
        switch (m_type) {
        case T::Overdrive: {   // the input plus the clipped mids (diodes in the feedback loop): the lows stay clean
            const float mids = ch.pre.process(x);
            const bool asym = m_asym;
            const float clipped = ch.os.run(mids * g, [asym](float u) { return odClip(u, asym); });
            return ch.post.process(x + 0.55f * clipped);
        }
        case T::Distortion: {
            OnePole& slew = ch.a;
            const int kind = m_kind;
            const float y = ch.os.run(ch.pre.process(x) * g, [&](float u) { return ratClip(slew.lp(u), kind); });
            return ch.b.lp(y);   // the Filter
        }
        case T::MetalDistortion: {
            OnePole& coupling = ch.a;
            OnePole& fizz = ch.b;
            const float y = ch.os.run(ch.pre.process(x) * g, [&](float u) {
                const float v = std::tanh(u);                 // the op-amp
                const float w = v - coupling.lp(v);           // the cap into the diode stage
                return fizz.lp(0.8f * hardKnee(2.5f * w));    // the diodes, the fizz cut
            });
            return ch.post.process(y);
        }
        case T::Chainsaw: {
            const float y = ch.os.run(ch.pre.process(x) * g, [](float u) { return sawClip(u); });
            return ch.post.process(y);
        }
        case T::MetalBoost: {
            const float y = ch.os.run(ch.pre.process(x) * g, [](float u) { return std::tanh(u); });
            return ch.post.process(y);
        }
        case T::Fuzz: {
            OnePole& coupling = ch.a;
            const float bias = m_bias;
            const float y = ch.os.run(ch.pre.process(x) * g, [&](float u) {
                const float v = std::tanh(u);                 // the first transistor
                const float w = v - coupling.lp(v);           // the coupling cap
                return fuzzClip(w * 6.0f, bias);              // the second, biased
            });
            // starved: the output sputters and dies away below a level
            float gate = 1;
            if (m_gate > 0) { const float t = std::clamp((ch.env - m_gate * 0.5f) / m_gate, 0.0f, 1.0f); gate = t * t * (3 - 2 * t); }
            const float lp = ch.b.lp(y), hp = y - ch.c.lp(y);
            const float tone = p[1];
            return ch.post.process(((1 - tone) * lp + tone * hp * 1.4f) * gate);
        }
        case T::Crunch: {
            OnePole& coupling = ch.a;
            const float bias = 0.22f + 0.5f * std::min(0.3f, ch.env);   // the stage's bias sags with the playing: more even harmonics on the attacks
            const float y = ch.os.run(ch.pre.process(x) * g, [&](float u) {
                const float v = tube(u, bias);                // the first triode
                const float w = v - coupling.lp(v);           // the coupling cap
                return 0.8f * tube(2.5f * w, 0.32f);          // the second
            });
            return ch.post.process(y);
        }
        default: return x;
        }
    }

    DeviceType m_type;
    float m_sr, m_gain = 1, m_level = 1, m_envRel = 0.999f, m_bias = 0, m_gate = 0;
    bool  m_asym = false;
    int   m_kind = 0;
    float m_last[16] = {-1};
    DriveChannel m_ch[2];
    Smoothed m_out[2];
};

// ============================================================================ Twang: spring reverb + clean boost
//
// Three springs per side in a tank: each a delay with a chain of all-passes (the chirp a
// spring makes), a low-pass in its loop, its length wobbling a little so the tail breathes.
// Dwell drives the springs harder, Tone darkens them, Mix sets the tank against the dry
// sound, Boost is a clean treble lift on the dry path.

class Twang : public DeviceProcessor {
public:
    explicit Twang(float sr) : m_sr(sr) {
        static const float ms[2][kSprings] = {{31.7f, 39.1f, 46.3f}, {33.9f, 41.7f, 48.9f}};
        for (int c = 0; c < 2; ++c) {
            for (int s = 0; s < kSprings; ++s) { m_spring[c][s].init(int(sr * ms[c][s] * 0.001f) + 16); m_len[c][s] = sr * ms[c][s] * 0.001f; }
            m_inHp[c].setLowpass(250, sr);
            m_dc[c].set(sr);
        }
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        const float dwell = p[0], tone = p[1], mix = p[2], boost = p[3];
        const float drive = 1 + dwell * 5, fb = 0.62f + dwell * 0.3f;
        const float boostGain = db(boost * 12);
        for (int c = 0; c < 2; ++c) {
            m_toneLp[c][0].setLowpass(1800 * std::pow(10.0f, tone * 0.7f), m_sr);
            for (int s = 1; s < kSprings; ++s) m_toneLp[c][s].a = m_toneLp[c][0].a;
        }
        m_shelf.highShelf(2500, boost * 5, m_sr);
        float* io[2] = {L, R};
        float peak = 0;
        for (int c = 0; c < 2; ++c) {
            float* x = io[c];
            Biquad& shelf = m_shelfCh[c];
            shelf.b0 = m_shelf.b0; shelf.b1 = m_shelf.b1; shelf.b2 = m_shelf.b2; shelf.a1 = m_shelf.a1; shelf.a2 = m_shelf.a2;
            for (int i = 0; i < n; ++i) {
                m_wobble[c] += (0.37f + 0.11f * c) / m_sr; if (m_wobble[c] >= 1) m_wobble[c] -= 1;
                const float dry = std::tanh(shelf.process(x[i]) * boostGain * 0.9f) / 0.9f;
                const float in = std::tanh(m_inHp[c].hp(dry) * drive) * 0.4f;
                float wet = 0;
                for (int s = 0; s < kSprings; ++s) {
                    const float len = m_len[c][s] + 0.6f * std::sin(kTwoPi * (m_wobble[c] + 0.31f * s));
                    float v = m_spring[c][s].read(len);
                    for (int k = 0; k < kStages; ++k) v = m_disp[c][s][k].process(v, -0.6f - 0.02f * s);   // dispersion: the chirp
                    v = m_toneLp[c][s].lp(v);
                    m_spring[c][s].push(in + std::tanh(v * fb));
                    wet += v;
                }
                wet = m_dc[c].process(wet) * 0.6f;
                peak = std::max(peak, std::fabs(wet));
                x[i] = dry + wet * mix * 1.3f;
            }
        }
        meter[0].store(peak, std::memory_order_relaxed);   // the panel shakes the springs
    }
    void panic() override {
        for (int c = 0; c < 2; ++c) for (int s = 0; s < kSprings; ++s) { m_spring[c][s].clear(); for (Allpass1& a : m_disp[c][s]) a.z = 0; }
    }
private:
    static constexpr int kStages = 12, kSprings = 3;
    float m_sr;
    DelayLine m_spring[2][kSprings];
    float m_len[2][kSprings] = {}, m_wobble[2] = {0, 0.5f};
    Allpass1 m_disp[2][kSprings][kStages];
    OnePole m_toneLp[2][kSprings], m_inHp[2];
    DcBlock m_dc[2];
    Biquad m_shelf, m_shelfCh[2];
};

// ============================================================================ Phase Pedal (four stages)
//
// Four all-pass stages swept by a rounded triangle between 200 Hz and 3 kHz, the output fed
// back a little for the resonance (Script: less, the older voicing); Depth is how far the
// sweep goes. meter[0] = the LFO's phase, meter[1] = where the sweep is (-1..1), meter[2] =
// the stages' all-pass coefficient (the panel draws the notches from it).

class PhasePedal : public DeviceProcessor {
public:
    explicit PhasePedal(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        const float speed = pv(T::PhasePedal, p, 0);
        const float fb = pb(p, 1) ? 0.12f : 0.45f;   // Script: less resonance
        const float depth = p[2];
        float coef = m_coef, shaped = 0;
        for (int i = 0; i < n; ++i) {
            m_ph += speed / m_sr; if (m_ph >= 1) m_ph -= 1;
            if ((i & 15) == 0) {
                const float tri = 2 * std::fabs(2 * m_ph - 1) - 1;
                shaped = tri * 0.8f + 0.2f * std::sin(kTwoPi * m_ph);
                coef = allpassCoef(700 * std::pow(10.0f, 0.6f * depth * shaped), m_sr);   // 175 Hz .. 2.8 kHz at full depth
                m_coef = coef;
            }
            for (int c = 0; c < 2; ++c) {
                float* x = c ? R : L;
                float y = x[i] + m_fb[c] * fb;
                for (Allpass1& a : m_ap[c]) y = a.process(y, coef);
                m_fb[c] = y;
                x[i] = 0.5f * (x[i] + y);
            }
        }
        meter[0].store(m_ph, std::memory_order_relaxed);
        meter[1].store(shaped, std::memory_order_relaxed);
        meter[2].store(coef, std::memory_order_relaxed);
    }
    void panic() override { for (auto& ch : m_ap) for (Allpass1& a : ch) a.z = 0; m_fb[0] = m_fb[1] = 0; }
    // The notches at an all-pass coefficient `coef` (dB at f): four stages mixed half and half with the input.
    static float response(float coef, float f, float sr) {
        const std::complex<float> z = std::polar(1.0f, -kTwoPi * f / sr);
        const std::complex<float> h1 = (coef + z) / (1.0f + coef * z);
        std::complex<float> h = 1.0f;
        for (int k = 0; k < 4; ++k) h *= h1;
        return gainToDb(std::max(1e-5f, std::abs(0.5f * (1.0f + h))));
    }
private:
    float m_sr, m_ph = 0, m_fb[2] = {0, 0}, m_coef = 0;
    Allpass1 m_ap[2][4];
};

// ============================================================================ Chorus Pedal (bucket brigade)
//
// One bucket-brigade line (dark above 6 kHz, a little squashed), its delay swept by a rounded
// triangle: 2 .. 10 ms with Depth. Stereo reads the line at the opposite point for the right
// side. Mix: 0 dry, 50 % the classic chorus, 100 % vibrato.

class ChorusPedal : public DeviceProcessor {
public:
    explicit ChorusPedal(float sr) : m_sr(sr) {
        m_line.init(int(sr * 0.03f) + 8);
        m_dark[0].setLowpass(6000, sr); m_dark[1].a = m_dark[0].a;
        m_emph.setLowpass(2500, sr);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        const float rate = pv(T::ChorusPedal, p, 0), depth = p[1], mix = p[3];
        const bool stereo = pb(p, 2);
        const float base = 0.004f * m_sr, swing = depth * 0.006f * m_sr;
        const float wet = mix, dry = 1 - mix;
        for (int i = 0; i < n; ++i) {
            m_ph += rate / m_sr; if (m_ph >= 1) m_ph -= 1;
            const float tri = 2 * std::fabs(2 * m_ph - 1) - 1;
            const float sh = 0.85f * tri + 0.15f * std::sin(kTwoPi * m_ph);   // a rounded triangle
            const float in = 0.5f * (L[i] + R[i]);
            m_line.push(std::tanh((in + 0.3f * m_emph.hp(in)) * 1.4f) / 1.4f);   // pre-emphasis, the bucket brigade's gentle squash
            const float a = m_dark[0].lp(m_line.read(base + swing * 0.5f * (1 + sh)));
            const float b = stereo ? m_dark[1].lp(m_line.read(base + swing * 0.5f * (1 - sh))) : a;
            L[i] = L[i] * dry + a * wet * 1.5f;
            R[i] = R[i] * dry + b * wet * 1.5f;
        }
        meter[0].store(m_ph, std::memory_order_relaxed);
    }
    void panic() override { m_line.clear(); }
private:
    float m_sr, m_ph = 0;
    DelayLine m_line;
    OnePole m_dark[2], m_emph;
};

// ============================================================================ Wah
//
// A resonant band-pass that sweeps 380 Hz .. 2.4 kHz with the pedal (180 Hz .. 1.3 kHz for a
// bass), its peak getting sharper and louder toward the toe like the inductor does; the lows
// pass underneath at -14 dB. Auto: the pedal follows your picking.

class Wah : public DeviceProcessor {
public:
    explicit Wah(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        const bool autoMode = pc(T::Wah, p, 1) == 1;
        const bool bass = pc(T::Wah, p, 4) == 1;
        const float lo = bass ? 180.0f : 380.0f, hi = bass ? 1300.0f : 2400.0f;
        const float att = onePoleCoef(0.004f, m_sr), rel = onePoleCoef(0.12f, m_sr), glide = onePoleCoef(0.005f, m_sr);
        for (int i = 0; i < n; ++i) {
            const float level = 0.5f * (std::fabs(L[i]) + std::fabs(R[i]));
            m_env = level + (m_env - level) * (level > m_env ? att : rel);
            const float target = autoMode ? std::clamp(p[0] * 0.4f + m_env * (0.5f + p[2] * 12), 0.0f, 1.0f) : p[0];
            m_pos = target + (m_pos - target) * glide;
            if ((i & 7) == 0) {
                const float fc = lo * std::pow(hi / lo, m_pos);
                const float q = (2 + p[3] * 10) * (0.7f + 0.6f * m_pos);   // sharper toward the toe
                m_f[0].set(fc, q, m_sr);
                m_f[1].set(fc, q, m_sr);
                m_gain = (1.2f + p[3] * 0.8f) * (0.85f + 0.3f * m_pos);
            }
            for (int c = 0; c < 2; ++c) {
                float* x = c ? R : L;
                float l, b, h;
                m_f[c].tick(x[i], l, b, h);
                x[i] = b * m_gain + l * 0.2f;
            }
        }
        meter[0].store(m_pos, std::memory_order_relaxed);
    }
    void panic() override { m_f[0].reset(); m_f[1].reset(); m_env = 0; }
    static float response(const float* p, float f, float sr) {
        const bool bass = choiceIndex(T::Wah, 4, p[4]) == 1;
        const float lo = bass ? 180.0f : 380.0f, hi = bass ? 1300.0f : 2400.0f;
        const float pos = p[0];
        const float fc = lo * std::pow(hi / lo, pos), q = (2 + p[3] * 10) * (0.7f + 0.6f * pos), gain = (1.2f + p[3] * 0.8f) * (0.85f + 0.3f * pos);
        (void)sr;
        return gainToDb(std::max(1e-5f, gain * Svf::magnitude(2, f, fc, q) + 0.2f * Svf::magnitude(0, f, fc, q)));
    }
private:
    float m_sr, m_env = 0, m_pos = 0.5f, m_gain = 1.5f;
    Svf m_f[2];
};

} // namespace

float ampResponseDb(const float* p, float freq, float sampleRate) {
    BiquadChain tone, cab;
    designTone(p, sampleRate, tone);
    const int c = cabOf(p);
    if (c > 0 && c < guitaramp::kLoadedIr) designCab(c, micOf(p), sampleRate, cab);
    return gainToDb(tone.magnitude(freq, sampleRate) * cab.magnitude(freq, sampleRate));
}

float pedalResponseDb(DeviceType type, const float* params, float freq, float sampleRate) {
    if (type == DeviceType::Wah) return Wah::response(params, freq, sampleRate);
    return DrivePedal::response(type, params, freq, sampleRate);
}

float pedalTransfer(DeviceType type, const float* params, float x) { return DrivePedal::transfer(type, params, x); }
float phaserResponseDb(float coef, float freq, float sampleRate) { return PhasePedal::response(coef, freq, sampleRate); }

// ============================================================================ Cabinet
//
// An impulse response on its own: the Convolver (one block of latency) at Mix against the dry
// sound, Low Cut / High Cut biquads after it, then Gain. No IR (or IR off): the dry sound.
namespace {
class Cabinet : public DeviceProcessor {
public:
    explicit Cabinet(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        storePeak(meter[0], L, R, n);
        const bool on = p[cabinet::Ir] >= 0.5f;
        const IrKernel* ir = on ? ctx.ir : nullptr;
        if (ir != m_irNow) { m_irNow = ir; m_conv[0].reset(); m_conv[1].reset(); }
        const float lowCut = pv(T::Cabinet, p, cabinet::LowCut), highCut = pv(T::Cabinet, p, cabinet::HighCut);
        if (lowCut != m_lowCut || highCut != m_highCut) {
            m_lowCut = lowCut; m_highCut = highCut;
            BiquadChain c;
            if (lowCut > 20.5f) c.highpass(lowCut, 0.707f, m_sr);
            if (highCut < 19900.0f) c.lowpass(highCut, 0.707f, m_sr);
            for (int ch = 0; ch < 2; ++ch) m_filter[ch].copyCoefficients(c);
        }
        const float mix = p[cabinet::Mix], gain = dbToGain(pv(T::Cabinet, p, cabinet::Gain));
        const float coef = onePoleCoef(0.01f, m_sr);
        float* io[2] = {L, R};
        for (int ch = 0; ch < 2; ++ch) {
            float* x = io[ch];
            if (ir) {
                m_conv[ch].process(*ir, ir->stereo ? ch : 0, x, m_wet, n);
                for (int i = 0; i < n; ++i) {
                    const float g = m_gain[ch].next(gain, coef);
                    x[i] = m_filter[ch].process(x[i] * (1 - mix) + m_wet[i] * mix) * g;
                }
            } else {
                for (int i = 0; i < n; ++i) x[i] = m_filter[ch].process(x[i]) * m_gain[ch].next(gain, coef);
            }
        }
        storePeak(meter[1], L, R, n);
    }
    void panic() override { for (int c = 0; c < 2; ++c) { m_conv[c].reset(); m_filter[c].reset(); } }
private:
    float m_sr;
    float m_lowCut = -1, m_highCut = -1;
    BiquadChain m_filter[2];
    Convolver m_conv[2];
    const IrKernel* m_irNow = nullptr;
    Smoothed m_gain[2];
    float m_wet[kMaxBlock] = {};
};
} // namespace

std::vector<float> cabinetImpulse(int cab, int mic, float sampleRate, int frames) {
    std::vector<float> out(size_t(std::max(1, frames)), 0.0f);
    BiquadChain c;
    designCab(std::clamp(cab, 1, guitaramp::kLoadedIr - 1), std::clamp(mic, 0, 2), sampleRate, c);
    DelayLine room;
    const bool far = mic == 2;
    if (far) room.init(int(sampleRate * 0.012f) + 8);
    for (int i = 0; i < frames; ++i) {
        float v = c.process(i == 0 ? 1.0f : 0.0f);
        if (far) { room.push(v); v = 0.8f * v + 0.35f * room.read(0.0019f * sampleRate) + 0.2f * room.read(0.0071f * sampleRate); }
        out[size_t(i)] = v;
    }
    // a short fade at the end, so a cut tail doesn't click
    const int fade = std::min(frames / 8, 256);
    for (int i = 0; i < fade; ++i) out[size_t(frames - 1 - i)] *= float(i) / fade;
    return out;
}

std::shared_ptr<DeviceProcessor> createGuitar(DeviceType type, float sr) {
    switch (type) {
    case T::Tuner:           return std::make_shared<Tuner>(sr);
    case T::GuitarAmp:       return std::make_shared<GuitarAmp>(sr);
    case T::Cabinet:         return std::make_shared<Cabinet>(sr);
    case T::Overdrive: case T::Distortion: case T::MetalDistortion: case T::Chainsaw:
    case T::MetalBoost: case T::Fuzz: case T::Crunch:
                             return std::make_shared<DrivePedal>(type, sr);
    case T::Twang:           return std::make_shared<Twang>(sr);
    case T::PhasePedal:      return std::make_shared<PhasePedal>(sr);
    case T::ChorusPedal:     return std::make_shared<ChorusPedal>(sr);
    case T::Wah:             return std::make_shared<Wah>(sr);
    default:                 return nullptr;
    }
}

} // namespace sessions::audio
