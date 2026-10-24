#include "ProductionEffects.h"
#include "DeviceKit.h"
#include "core/ModulationCurve.h"
#include <array>
#include <complex>

namespace sessions::audio {
using namespace kit;
namespace {
// Fixed storage and precomputed tables: the callback never prepares resources or allocates.
class Analysis {
    static constexpr int N = 2048, Hop = 1024;
    std::array<float, N> ring[2]{};
    std::array<float, N> window{};
    std::array<int, N> rev{};
    std::array<std::complex<float>, N / 2> tw{};
    std::array<std::complex<float>, N> work{};
    std::array<float, N / 2 + 1> energy{};
    std::array<std::atomic<float>, 256> bins;
    int head = 0, count = 0;
    float sr;

  public:
    explicit Analysis(float rate) : sr(rate) {
        for (int i = 0; i < N; ++i) {
            window[i] = .5f - .5f * std::cos(kTwoPi * i / N);
            int x = i, v = 0;
            for (int b = 0; b < 11; ++b) {
                v = (v << 1) | (x & 1);
                x >>= 1;
            }
            rev[i] = v;
        }
        for (int i = 0; i < N / 2; ++i)
            tw[i] = std::polar(1.f, -kTwoPi * i / N);
        for (auto& b : bins)
            b.store(-120);
    }
    float read(int b) const {
        return b >= 0 && b < 256 ? bins[size_t(b)].load(std::memory_order_relaxed) : -120;
    }
    void reset() {
        for (auto& r : ring)
            r.fill(0);
        head = count = 0;
        for (auto& b : bins)
            b.store(-120, std::memory_order_relaxed);
    }
    void push(float l, float r, float smooth, bool hold = false) {
        ring[0][head] = l;
        ring[1][head] = r;
        head = (head + 1) % N;
        if (++count < Hop)
            return;
        count = 0;
        if (hold)
            return;
        energy.fill(0);
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < N; ++i)
                work[rev[i]] = {ring[c][(head + i) % N] * window[i], 0};
            for (int len = 2; len <= N; len *= 2)
                for (int base = 0; base < N; base += len)
                    for (int j = 0; j < len / 2; ++j) {
                        auto a = work[base + j], b = work[base + j + len / 2] * tw[j * N / len];
                        work[base + j] = a + b;
                        work[base + j + len / 2] = a - b;
                    }
            for (int i = 0; i <= N / 2; ++i)
                energy[i] += std::norm(work[i]) * .5f;
        }
        const float a = std::exp(-float(Hop) / std::max(1.f, smooth * sr));
        for (int b = 0; b < 256; ++b) {
            float f = 20.f * std::pow(1000.f, float(b) / 255), at = f * N / sr;
            int i = std::clamp(int(std::round(at)), 1, N / 2);
            float db =
                f < sr * .5f
                    ? std::max(-120.f, 10.f * std::log10(std::max(1e-20f, energy[i])) + 20.f * std::log10(4.f / N))
                    : -120.f;
            float old = bins[b].load(std::memory_order_relaxed);
            bins[b].store(db > old ? db : db + (old - db) * a, std::memory_order_relaxed);
        }
    }
};

struct FilterBand {
    Biquad f[2][4];
    int stages = 1, channel = 0;
    bool enabled = true;
    void design(const float* p, int b, float sr) {
        using namespace parametric;
        const T t = T::ParametricEQ;
        enabled = pb(p, param(b, Enabled));
        channel = pc(t, p, param(b, Channel));
        const int type = pc(t, p, param(b, Type));
        const float hz = std::min(pv(t, p, param(b, Frequency)), sr * .45f), q = pv(t, p, param(b, Q)),
                    g = pv(t, p, param(b, Gain));
        stages = (type == 3 || type == 4) ? (1 << pc(t, p, param(b, Slope))) : 1;
        for (int c = 0; c < 2; ++c)
            for (int j = 0; j < stages; ++j) {
                auto& f0 = f[c][j];
                switch (type) {
                case 0:
                    f0.peak(hz, q, g, sr);
                    break;
                case 1:
                    f0.lowShelf(hz, g, sr);
                    break;
                case 2:
                    f0.highShelf(hz, g, sr);
                    break;
                case 3:
                    f0.highpass(hz, .70710678f, sr);
                    break;
                case 4:
                    f0.lowpass(hz, .70710678f, sr);
                    break;
                default: {
                    const float w = kTwoPi * hz / sr, al = std::sin(w) / (2 * q), co = std::cos(w);
                    if (type == 5)
                        f0.normalize(1 + al, 1, -2 * co, 1, -2 * co, 1 - al);
                    else
                        f0.normalize(1 + al, al, 0, -al, -2 * co, 1 - al);
                } break;
                }
            }
    }
    float run(float x, int c) {
        for (int j = 0; j < stages; ++j)
            x = f[c][j].process(x);
        return x;
    }
    void process(float& l, float& r) {
        if (!enabled)
            return;
        if (channel == 3 || channel == 4) {
            float m = (l + r) * .5f, s = (l - r) * .5f;
            if (channel == 3)
                m = run(m, 0);
            else
                s = run(s, 1);
            l = m + s;
            r = m - s;
        } else {
            if (channel != 2)
                l = run(l, 0);
            if (channel != 1)
                r = run(r, 1);
        }
    }
    void reset() {
        for (auto& c : f)
            for (auto& b : c)
                b.reset();
    }
    float magnitude(float hz, float sr, int c) const {
        if (!enabled || (channel == 1 && c == 1) || (channel == 2 && c == 0) || (channel == 3 && c == 1) ||
            (channel == 4 && c == 0))
            return 1;
        float m = 1;
        for (int j = 0; j < stages; ++j)
            m *= f[0][j].magnitude(hz, sr);
        return m;
    }
};
class ParametricEq final : public DeviceProcessor {
    float sr;
    FilterBand bands[8];
    Analysis input, output;
    Smoothed gain;
    std::array<float, parametric::kParams> last;
    Biquad audition[2];
    int lastAudition = -1;

  public:
    explicit ParametricEq(float r) : sr(r), input(r), output(r) {
        last.fill(-1);
    }
    float spectrum(int b, bool out) const override {
        return out ? output.read(b) : input.read(b);
    }
    void process(float* l, float* r, int n, const float* p, const Context&) override {
        using namespace parametric;
        if (!std::equal(last.begin(), last.end(), p)) {
            for (int b = 0; b < 8; ++b)
                bands[b].design(p, b, sr);
            std::copy_n(p, kParams, last.begin());
        }
        const int solo = pc(T::ParametricEQ, p, Audition) - 1;
        if (solo != lastAudition) {
            for (auto& f : audition)
                f.reset();
            lastAudition = solo;
        }
        if (solo >= 0) {
            float hz = std::min(pv(T::ParametricEQ, p, param(solo, Frequency)), sr * .45f),
                  q = pv(T::ParametricEQ, p, param(solo, Q));
            for (auto& f : audition) {
                float w = kTwoPi * hz / sr, al = std::sin(w) / (2 * q), co = std::cos(w);
                f.normalize(1 + al, al, 0, -al, -2 * co, 1 - al);
            }
        }
        const float target = dbToGain(pv(T::ParametricEQ, p, Output)), a = onePoleCoef(.01f, sr);
        for (int i = 0; i < n; ++i) {
            if (pb(p, Analyze))
                input.push(l[i], r[i], .2f);
            if (solo >= 0) {
                l[i] = audition[0].process(l[i]);
                r[i] = audition[1].process(r[i]);
            } else
                for (auto& b : bands)
                    b.process(l[i], r[i]);
            float g = gain.next(target, a);
            l[i] *= g;
            r[i] *= g;
            if (pb(p, Analyze))
                output.push(l[i], r[i], .2f);
        }
    }
    void panic() override {
        for (auto& b : bands)
            b.reset();
        for (auto& f : audition)
            f.reset();
        input.reset();
        output.reset();
        gain.primed = false;
    }
};
class Spectrum final : public DeviceProcessor {
    Analysis analysis;

  public:
    explicit Spectrum(float sr) : analysis(sr) {}
    float spectrum(int b, bool) const override {
        return analysis.read(b);
    }
    void process(float* l, float* r, int n, const float* p, const Context&) override {
        for (int i = 0; i < n; ++i)
            analysis.push(l[i], r[i], pv(T::Spectrum, p, 0), pb(p, 1));
    }
    void panic() override {
        analysis.reset();
    }
};

struct Splitter {
    Biquad low[2], high[2];
    void design(float hz, float sr) {
        for (auto& f : low)
            f.lowpass(hz, .70710678f, sr);
        for (auto& f : high)
            f.highpass(hz, .70710678f, sr);
    }
    void split(float x, float& l, float& h) {
        l = low[1].process(low[0].process(x));
        h = high[1].process(high[0].process(x));
    }
    void reset() {
        for (auto& f : low)
            f.reset();
        for (auto& f : high)
            f.reset();
    }
};
class Multiband final : public DeviceProcessor {
    float sr;
    Splitter low[2], high[2], phase[2];
    float env[3] = {}, gains[3] = {1, 1, 1};
    Smoothed out;

  public:
    explicit Multiband(float r) : sr(r) {}
    void process(float* l, float* r, int n, const float* p, const Context&) override {
        using namespace multiband;
        const T t = T::MultibandDynamics;
        float lo = std::min(pv(t, p, LowCross), sr * .2f), hi = std::clamp(pv(t, p, HighCross), lo * 1.1f, sr * .45f);
        for (int c = 0; c < 2; ++c) {
            low[c].design(lo, sr);
            high[c].design(hi, sr);
            phase[c].design(hi, sr);
        }
        float attack[3], release[3], above[3], ar[3], below[3], br[3], makeup[3];
        bool solo[3], on[3], any = false;
        for (int b = 0; b < 3; ++b) {
            attack[b] = onePoleCoef(pv(t, p, param(b, Attack)) * .001f, sr);
            release[b] = onePoleCoef(pv(t, p, param(b, Release)) * .001f, sr);
            above[b] = pv(t, p, param(b, AboveThreshold));
            ar[b] = pv(t, p, param(b, AboveRatio));
            below[b] = std::min(above[b], pv(t, p, param(b, BelowThreshold)));
            br[b] = pv(t, p, param(b, BelowRatio));
            makeup[b] = pv(t, p, param(b, Gain));
            solo[b] = pb(p, param(b, Solo));
            on[b] = pb(p, param(b, Enabled));
            any |= solo[b];
        }
        const float amount = p[Amount], mix = p[Mix], target = dbToGain(pv(t, p, Output));
        float reduction[3] = {};
        for (int i = 0; i < n; ++i) {
            float v[2][3];
            for (int c = 0; c < 2; ++c) {
                float rem, a, b;
                low[c].split(c ? r[i] : l[i], v[c][0], rem);
                high[c].split(rem, v[c][1], v[c][2]);
                phase[c].split(v[c][0], a, b);
                v[c][0] = a + b;
            }
            float dry[2] = {v[0][0] + v[0][1] + v[0][2], v[1][0] + v[1][1] + v[1][2]}, wet[2] = {};
            for (int b = 0; b < 3; ++b) {
                float level = std::max(std::fabs(v[0][b]), std::fabs(v[1][b]));
                float a = level > env[b] ? attack[b] : release[b];
                env[b] = level + (env[b] - level) * a;
                float db = std::max(-120.f, gainToDb(env[b])), change = 0;
                if (on[b]) {
                    if (db > above[b])
                        change = (above[b] + (db - above[b]) / ar[b]) - db;
                    else if (db < below[b])
                        change = (below[b] + (db - below[b]) * br[b]) - db;
                    change = std::clamp(change, -60.f, 36.f) * amount + makeup[b];
                }
                float want = dbToGain(change);
                gains[b] = want + (gains[b] - want) * (want < gains[b] ? attack[b] : release[b]);
                reduction[b] = std::max(reduction[b], -gainToDb(gains[b]));
                scope[b + 3].store(db, std::memory_order_relaxed);
                if (!any || solo[b])
                    for (int c = 0; c < 2; ++c)
                        wet[c] += v[c][b] * gains[b];
            }
            float g = out.next(target, onePoleCoef(.01f, sr));
            l[i] = (dry[0] + (wet[0] - dry[0]) * mix) * g;
            r[i] = (dry[1] + (wet[1] - dry[1]) * mix) * g;
        }
        for (int b = 0; b < 3; ++b) {
            meter[b].store(reduction[b], std::memory_order_relaxed);
            scope[b].store(gainToDb(gains[b]), std::memory_order_relaxed);
        }
    }
    void panic() override {
        for (auto* s : {low, high, phase})
            for (int c = 0; c < 2; ++c)
                s[c].reset();
        std::fill_n(env, 3, 0);
        std::fill_n(gains, 3, 1);
        out.primed = false;
    }
};

class Modulator final : public DeviceProcessor {
    T type;
    float sr, value = 0, envelope = 0, phase = 0, random = .5f;
    Noise noise;
    Biquad hp[2], lp[2];
    float listen = 0;

  public:
    Modulator(T t, float r) : type(t), sr(r) {}
    float modulationValue() const override {
        return value;
    }
    void process(float* l, float* r, int n, const float* p, const Context& ctx) override {
        using namespace modulator;
        const bool follower = type == T::EnvelopeFollower;
        const float rise = onePoleCoef(pv(type, p, modulator::param(type, Rise)) * .001f, sr),
                    fall = onePoleCoef(pv(type, p, modulator::param(type, Fall)) * .001f, sr),
                    g = dbToGain(pv(type, p, modulator::param(type, Gain)));
        const double beats = follower ? 0 : pv(type, p, Rate);
        float hz = follower ? 0 : beats > 0 ? float(ctx.tempo / 60 / beats) : pv(type, p, Hz);
        if (beats > 0 && ctx.playing)
            phase = float(ctx.beat / beats - std::floor(ctx.beat / beats));
        const int keyBase = modulator::kFollowerParams;
        const bool highPass = follower && p[keyBase + sidechain::HP] > .002f,
                   lowPass = follower && p[keyBase + sidechain::LP] < .998f;
        const float keyGain = follower ? dbToGain(pv(type, p, keyBase + sidechain::Gain)) : 1;
        if (highPass)
            for (auto& f : hp)
                f.highpass(std::min(pv(type, p, keyBase + sidechain::HP), sr * .45f), .707f, sr);
        if (lowPass)
            for (auto& f : lp)
                f.lowpass(std::min(pv(type, p, keyBase + sidechain::LP), sr * .45f), .707f, sr);
        float peak = 0;
        for (int i = 0; i < n; ++i) {
            float x = 0;
            if (type == T::EnvelopeFollower) {
                float key[2] = {ctx.keyL ? ctx.keyL[i] : l[i], ctx.keyR ? ctx.keyR[i] : r[i]};
                for (int c = 0; c < 2; ++c) {
                    key[c] *= keyGain;
                    if (highPass)
                        key[c] = hp[c].process(key[c]);
                    if (lowPass)
                        key[c] = lp[c].process(key[c]);
                }
                float level = std::max(std::fabs(key[0]), std::fabs(key[1])) * g;
                peak = std::max(peak, level);
                float wanted = pb(p, keyBase + sidechain::Listen) ? 1 : 0;
                listen += std::clamp(wanted - listen, -1.f / (.005f * sr), 1.f / (.005f * sr));
                l[i] += (key[0] - l[i]) * listen;
                r[i] += (key[1] - r[i]) * listen;
                float a = level > envelope ? rise : fall;
                envelope = level + (envelope - level) * a;
                x = std::clamp(envelope, 0.f, 1.f);
            } else {
                float at = phase + pv(type, p, Phase) / 360;
                at -= std::floor(at);
                if (type == T::Shaper) {
                    switch (pc(type, p, Shape)) {
                    case 1:
                        at = 1 - at;
                        break;
                    case 2:
                        at = 1 - std::abs(2 * at - 1);
                        break;
                    case 3:
                        at = std::floor(at * 8) / 8;
                        break;
                    case 4:
                        at = random;
                        break;
                    case 5:
                        at = .5f - .5f * std::cos(kTwoPi * at);
                        break;
                    default:
                        break;
                    }
                    x = ctx.curves && ctx.curveCount && ctx.curveCount[0] > 0
                            ? float(modcurve::value(ctx.curves, ctx.curveCount[0], at))
                            : at;
                } else
                    switch (pc(type, p, Shape)) {
                    case 0:
                        x = .5f - .5f * std::cos(kTwoPi * at);
                        break;
                    case 1:
                        x = 1 - std::fabs(2 * at - 1);
                        break;
                    case 2:
                        x = at;
                        break;
                    case 3:
                        x = 1 - at;
                        break;
                    case 4:
                        x = at < .5f ? 1 : 0;
                        break;
                    default:
                        x = random;
                        break;
                    }
                phase += hz / sr;
                if (phase >= 1) {
                    phase -= std::floor(phase);
                    random = noise.uniform();
                }
                float target = std::clamp(x * g, 0.f, 1.f), a = target > envelope ? rise : fall;
                envelope = target + (envelope - target) * a;
                x = envelope;
            }
            value = std::clamp(p[modulator::param(type, Offset)] + x * p[modulator::param(type, Depth)], 0.f, 1.f);
        }
        scope[0].store(value, std::memory_order_relaxed);
        if (follower)
            meter[2].store(gainToDb(peak), std::memory_order_relaxed);
    }
    void panic() override {
        phase = envelope = value = 0;
        listen = 0;
        for (auto& f : hp)
            f.reset();
        for (auto& f : lp)
            f.reset();
    }
};
} // namespace
float parametricResponseDb(const float* p, float hz, float sr, int channel) {
    float m = 1;
    for (int b = 0; b < 8; ++b) {
        FilterBand band;
        band.design(p, b, sr);
        m *= band.magnitude(hz, sr, channel);
    }
    return gainToDb(m);
}
void parametricResponseCurve(const float* p, float* response, int count, float sr, int channel) {
    FilterBand bands[8];
    for (int b = 0; b < 8; ++b)
        bands[b].design(p, b, sr);
    for (int i = 0; i < count; ++i) {
        float hz = 20 * std::pow(1000.f, float(i) / std::max(1, count - 1)), m = 1;
        for (const auto& b : bands)
            m *= b.magnitude(hz, sr, channel);
        response[i] = gainToDb(m);
    }
}
std::shared_ptr<DeviceProcessor> createProductionEffect(DeviceType t, float sr) {
    switch (t) {
    case T::ParametricEQ:
        return std::make_shared<ParametricEq>(sr);
    case T::Spectrum:
        return std::make_shared<Spectrum>(sr);
    case T::MultibandDynamics:
        return std::make_shared<Multiband>(sr);
    case T::Lfo:
    case T::EnvelopeFollower:
    case T::Shaper:
        return std::make_shared<Modulator>(t, sr);
    case T::Looper:
        return createLooper(sr);
    default:
        if (auto p = createPitchEffect(t, sr))
            return p;
        return createSpatialEffect(t, sr);
    }
}
} // namespace sessions::audio
