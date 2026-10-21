// The audio effects. Parameter indices follow core/DeviceSpecs.cpp. Effects that show
// something live on their panel write it to meter[0] / meter[1] once per block.

#include "DeviceKit.h"

namespace sessions::audio {

using namespace kit;

namespace {

// Highest of two meters across a block, for the panels.
inline void store(std::atomic<float>& m, float v) { m.store(v, std::memory_order_relaxed); }

// Stereo Width: mid is invariant; side processing cannot cancel the mono fold-down.
class StereoWidth : public DeviceProcessor {
public:
    explicit StereoWidth(float sr) : m_sr(sr) { m_delay.init(int(sr*.03f)+8); }
    void process(float* L,float* R,int n,const float* p,const Context&) override {
        namespace sw=stereowidth;constexpr T t=T::StereoWidth;
        const float cutoff=pv(t,p,sw::BassCutoff);
        if(cutoff!=m_cutoff){m_cutoff=cutoff;for(auto& f:m_bass)f.highpass(std::min(cutoff,m_sr*.4f),.70710678f,m_sr);}
        const float coef=onePoleCoef(.015f,m_sr);const int monitor=pc(t,p,sw::Monitor);
        double ll=0,rr=0,lr=0,midPower=0,sidePower=0;float peak=0;
        for(int i=0;i<n;++i){
            const float left=std::isfinite(L[i])?L[i]:0,right=std::isfinite(R[i])?R[i]:0;
            const float mid=(left+right)*.5f,side=(left-right)*.5f;
            const float width=m_width.next(pv(t,p,sw::Width),coef),bass=m_monoBass.next(pb(p,sw::MonoBass)?1.f:0.f,coef);
            const float spread=m_spread.next(p[sw::Spread],coef),time=m_time.next(1+pv(t,p,sw::Delay)*m_sr*.001f,coef);
            const float wet=m_mix.next(p[sw::Mix],coef),gain=m_output.next(dbToGain(pv(t,p,sw::Output)),coef);
            m_delay.push(mid);const float added=(mid-m_delay.read(time))*.5f*spread;
            const float rawSide=side+added;const float high=m_bass[1].process(m_bass[0].process(rawSide));
            const float processed=(rawSide+(high-rawSide)*bass)*width;
            float outSide=side+(processed-side)*wet;
            const float midMonitor=m_midMonitor.next(monitor==2?0.f:1.f,coef),sideMonitor=m_sideMonitor.next(monitor==1?0.f:1.f,coef);
            float l=(mid*midMonitor+outSide*sideMonitor)*gain,r=(mid*midMonitor-outSide*sideMonitor)*gain;
            // Finite guards protect a downstream chain after corrupt source data or extreme automation.
            if(!std::isfinite(l)||!std::isfinite(r)){l=r=0;panic();}
            L[i]=l;R[i]=r;ll+=double(l)*l;rr+=double(r)*r;lr+=double(l)*r;
            midPower+=double(l+r)*(l+r)*.25;sidePower+=double(l-r)*(l-r)*.25;peak=std::max(peak,std::max(std::abs(l),std::abs(r)));
        }
        const double decay=std::exp(-double(n)/(m_sr*.25));
        m_ll=ll+(m_ll-ll)*decay;m_rr=rr+(m_rr-rr)*decay;m_lr=lr+(m_lr-lr)*decay;
        meter[0].store(m_ll*m_rr>1e-18?float(std::clamp(m_lr/std::sqrt(m_ll*m_rr),-1.,1.)):0,std::memory_order_relaxed);
        meter[1].store(n?float(std::sqrt(sidePower/std::max(1e-18,midPower+sidePower))):0,std::memory_order_relaxed);
        meter[2].store(gainToDb(peak),std::memory_order_relaxed);
    }
    void panic() override {m_delay.clear();for(auto& f:m_bass)f.reset();m_ll=m_rr=m_lr=0;}
private:
    float m_sr,m_cutoff=-1;DelayLine m_delay;Biquad m_bass[2];double m_ll=0,m_rr=0,m_lr=0;
    Smoothed m_width,m_monoBass,m_spread,m_time,m_mix,m_output,m_midMonitor,m_sideMonitor;
};

// ===========================================================================
// The sidechain key of a Compressor, Gate or Auto Filter: the device's own input, or another
// track's audio (Context::keyL / keyR), through Key Gain, Key HP and Key LP (DeviceSpecs
// sidechain::). The detector reads it; Listen swaps it into the device's output (a 5 ms blend).
// ===========================================================================

struct KeyDetector {
    OnePole  hp[2], lp[2];
    Smoothed gain;
    float    listen = -1;   // the Listen blend, 0..1 (< 0: not run yet)
    float    keyL[kMaxBlock] = {}, keyR[kMaxBlock] = {};
    float    peak = 0;      // this block's highest key sample

    void prepare(T type, const float* p, const float* L, const float* R, int n, const Context& ctx, float sr) {
        const int base = sidechain::base(type);
        const float g = dbToGain(pv(type, p, base + sidechain::Gain));
        const bool hpOn = p[base + sidechain::HP] > 0.002f, lpOn = p[base + sidechain::LP] < 0.998f;
        if (hpOn) { hp[0].setLowpass(pv(type, p, base + sidechain::HP), sr); hp[1].a = hp[0].a; }
        if (lpOn) { lp[0].setLowpass(pv(type, p, base + sidechain::LP), sr); lp[1].a = lp[0].a; }
        const float* kl = ctx.keyL ? ctx.keyL : L;
        const float* kr = ctx.keyR ? ctx.keyR : R;
        const float coef = onePoleCoef(0.005f, sr);
        peak = 0;
        for (int i = 0; i < n; ++i) {
            const float gg = gain.next(g, coef);
            float l = kl[i] * gg, r = kr[i] * gg;
            if (!std::isfinite(l)) l = 0;
            if (!std::isfinite(r)) r = 0;
            if (hpOn) { l = hp[0].hp(l); r = hp[1].hp(r); }
            if (lpOn) { l = lp[0].lp(l); r = lp[1].lp(r); }
            keyL[i] = l;
            keyR[i] = r;
            peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
        }
    }
    float level(int i) const { return std::max(std::fabs(keyL[i]), std::fabs(keyR[i])); }
    // After the device has processed L / R: Listen replaces them with the key.
    void audition(T type, const float* p, float* L, float* R, int n, float sr) {
        const float target = pb(p, sidechain::param(type, sidechain::Listen)) ? 1.0f : 0.0f;
        if (listen < 0) listen = target;
        if (listen == 0 && target == 0) return;
        const float step = 1.0f / (0.005f * sr);
        for (int i = 0; i < n; ++i) {
            listen = target > listen ? std::min(target, listen + step) : std::max(target, listen - step);
            L[i] += (keyL[i] - L[i]) * listen;
            R[i] += (keyR[i] - R[i]) * listen;
        }
    }
    void reset() { for (OnePole& f : hp) f.z = 0; for (OnePole& f : lp) f.z = 0; gain.primed = false; peak = 0; }
};

// ===========================================================================
// EQ: low shelf, bell, high shelf, plus 12 dB/oct low and high cut.
// ===========================================================================

struct EqBands {
    Biquad lo, mid, hi, lowCut, highCut;
    bool useLowCut = false, useHighCut = false;
    void design(const float* p, float sr) {
        constexpr T t = T::EQ;
        lo.lowShelf(pv(t, p, 4), pv(t, p, 0), sr);
        mid.peak(pv(t, p, 5), pv(t, p, 6), pv(t, p, 1), sr);
        hi.highShelf(pv(t, p, 7), pv(t, p, 2), sr);
        useLowCut = p[8] > 0.002f;
        useHighCut = p[9] < 0.998f;
        if (useLowCut) lowCut.highpass(pv(t, p, 8), 0.707f, sr);
        if (useHighCut) highCut.lowpass(pv(t, p, 9), 0.707f, sr);
    }
    float process(float x) {
        x = hi.process(mid.process(lo.process(x)));
        if (useLowCut) x = lowCut.process(x);
        if (useHighCut) x = highCut.process(x);
        return x;
    }
    float magnitude(float f, float sr) const {
        float m = lo.magnitude(f, sr) * mid.magnitude(f, sr) * hi.magnitude(f, sr);
        if (useLowCut) m *= lowCut.magnitude(f, sr);
        if (useHighCut) m *= highCut.magnitude(f, sr);
        return m;
    }
};

constexpr int kEqParams = 10;

class Eq : public DeviceProcessor {
public:
    explicit Eq(float sr) : m_sr(sr) { std::fill(m_last, m_last + kEqParams, -1.0f); }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        if (!std::equal(p, p + kEqParams, m_last)) {
            std::copy(p, p + kEqParams, m_last);
            for (EqBands& b : m_bands) b.design(p, m_sr);
        }
        const float target = dbToGain(pv(T::EQ, p, 3)), a = onePoleCoef(0.01f, m_sr);
        for (int i = 0; i < n; ++i) {
            const float g = m_out.next(target, a);
            L[i] = m_bands[0].process(L[i]) * g;
            R[i] = m_bands[1].process(R[i]) * g;
        }
    }
    void panic() override { for (EqBands& b : m_bands) { b.lo.reset(); b.mid.reset(); b.hi.reset(); b.lowCut.reset(); b.highCut.reset(); } }
private:
    float m_sr, m_last[kEqParams];
    EqBands m_bands[2];
    Smoothed m_out;
};

// ===========================================================================
// Compressor: stereo-linked peak detector, soft knee. meter[0] = gain
// reduction (dB, positive), meter[1] = input peak (dBFS).
// ===========================================================================

class Compressor : public DeviceProcessor {
public:
    explicit Compressor(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        constexpr T t = T::Compressor;
        m_key.prepare(t, p, L, R, n, ctx, m_sr);
        const float thresh = pv(t, p, 0), ratio = pv(t, p, 1);
        const float att = onePoleCoef(pv(t, p, 2) / 1000.0f, m_sr);
        const float rel = onePoleCoef(pv(t, p, 3) / 1000.0f, m_sr);
        const float knee = pv(t, p, 4);
        const float makeup = pb(p, 5) ? -thresh * (1.0f - 1.0f / ratio) * 0.25f : 0.0f;   // partial: leaves transient headroom
        const float out = pv(t, p, 6), wet = p[7];
        float maxGr = 0;
        for (int i = 0; i < n; ++i) {
            const float level = m_key.level(i);
            const float gr = compReduction(gainToDb(level) - thresh, ratio, knee);
            m_env = gr + (m_env - gr) * (gr < m_env ? att : rel);
            maxGr = std::max(maxGr, -m_env);
            const float g = dbToGain(m_env + makeup + out), dry = dbToGain(out);
            L[i] = L[i] * (g * wet + dry * (1 - wet));
            R[i] = R[i] * (g * wet + dry * (1 - wet));
        }
        store(meter[0], maxGr);
        store(meter[1], gainToDb(m_key.peak));   // what the detector sees (the key)
        store(meter[2], gainToDb(m_key.peak));
        m_key.audition(t, p, L, R, n, m_sr);
    }
    void panic() override { m_env = 0; m_key.reset(); }
private:
    float m_sr, m_env = 0;   // gain reduction, dB (<= 0)
    KeyDetector m_key;
};

// ===========================================================================
// Saturator: drive into a waveshaper, then a tone low-pass. meter[0] = the
// driven peak, where the signal sits on the curve.
// ===========================================================================

class Saturator : public DeviceProcessor {
public:
    explicit Saturator(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Saturator;
        const float drive = dbToGain(pv(t, p, 0));
        const float comp = 1.0f / std::sqrt(std::max(1.0f, drive * 0.5f));
        m_tone[0].setLowpass(pv(t, p, 1), m_sr);
        m_tone[1].a = m_tone[0].a;
        const float wet = p[2], out = dbToGain(pv(t, p, 3));
        const int type = pc(t, p, 4);
        float peak = 0;
        for (int i = 0; i < n; ++i) {
            const float xl = L[i] * drive, xr = R[i] * drive;
            peak = std::max(peak, std::max(std::fabs(xl), std::fabs(xr)));
            const float wl = m_tone[0].lp(saturate(type, xl) * comp);
            const float wr = m_tone[1].lp(saturate(type, xr) * comp);
            L[i] = (L[i] * (1 - wet) + wl * wet) * out;
            R[i] = (R[i] * (1 - wet) + wr * wet) * out;
        }
        store(meter[0], peak);
    }
private:
    float m_sr;
    OnePole m_tone[2];
};

// ===========================================================================
// Auto Filter: a resonant filter moved by an LFO in time with the song and
// by the input's level (Env). meter[0] = the cutoff now (Hz), meter[1] = the
// LFO's phase.
// ===========================================================================

class AutoFilter : public DeviceProcessor {
public:
    explicit AutoFilter(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        constexpr T t = T::AutoFilter;
        m_key.prepare(t, p, L, R, n, ctx, m_sr);
        const float base = pv(t, p, 0), q = 0.6f + p[1] * p[1] * 12.0f, lfo = p[2], wet = p[3];
        const int type = pc(t, p, 4), shape = pc(t, p, 6);
        const double rate = std::max(0.0625, double(pv(t, p, 5)));
        const float envAmt = pv(t, p, 7);
        const float envAtt = onePoleCoef(0.005f, m_sr), envRel = onePoleCoef(0.15f, m_sr);
        float cut = base;
        double phase = 0;
        for (int i = 0; i < n; ++i) {
            const float level = m_key.level(i);
            m_env = level + (m_env - level) * (level > m_env ? envAtt : envRel);
            if ((i & 15) == 0) {
                phase = (ctx.beat + i * ctx.beatsPerSample) / rate;
                const float oct = lfo * 3.0f * lfoShape(shape, phase) + envAmt * 5.0f * std::min(1.0f, m_env * 1.5f);
                cut = std::clamp(base * std::exp2(oct), 20.0f, 20000.0f);
                m_f[0].set(cut, q, m_sr);
                m_f[1].set(cut, q, m_sr);
            }
            L[i] = L[i] * (1 - wet) + m_f[0].run(type, L[i]) * wet;
            R[i] = R[i] * (1 - wet) + m_f[1].run(type, R[i]) * wet;
        }
        store(meter[0], cut);
        store(meter[1], float(phase - std::floor(phase)));
        store(meter[2], gainToDb(m_key.peak));
        m_key.audition(t, p, L, R, n, m_sr);
    }
    void panic() override { m_f[0].reset(); m_f[1].reset(); m_env = 0; m_key.reset(); }
private:
    float m_sr, m_env = 0;
    Svf m_f[2];
    KeyDetector m_key;
};

// ===========================================================================
// Delay: synced echoes. Ping Pong bounces a mono sum between the sides;
// otherwise each side repeats itself. The feedback is filtered on every pass.
// ===========================================================================

class Delay : public DeviceProcessor {
public:
    explicit Delay(float sr) : m_sr(sr) {
        m_line[0].init(int(sr * 6) + 8);
        m_line[1].init(int(sr * 6) + 8);
    }
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        constexpr T t = T::Delay;
        const double spb = 60.0 / std::max(1.0, ctx.tempo) * m_sr;   // samples per beat
        const float maxT = float(m_line[0].buf.size() - 4);
        const float tl = std::clamp(float(pv(t, p, 0) * spb), 1.0f, maxT);
        const float tr = pb(p, 5) ? tl : std::clamp(float(pv(t, p, 6) * spb), 1.0f, maxT);
        if (m_time[0] <= 0) { m_time[0] = tl; m_time[1] = tr; }
        const float fb = pv(t, p, 1), wet = p[3];
        const bool ping = pb(p, 4);
        m_lp[0].setLowpass(pv(t, p, 2), m_sr); m_lp[1].a = m_lp[0].a;
        m_hp[0].setLowpass(pv(t, p, 7), m_sr); m_hp[1].a = m_hp[0].a;
        const float glide = onePoleCoef(0.05f, m_sr);
        for (int i = 0; i < n; ++i) {
            m_time[0] = tl + (m_time[0] - tl) * glide;
            m_time[1] = tr + (m_time[1] - tr) * glide;
            const float rl = m_line[0].read(m_time[0]), rr = m_line[1].read(m_time[1]);
            if (ping) {   // left feeds right, right feeds left
                m_line[0].push(0.5f * (L[i] + R[i]) + m_hp[0].hp(m_lp[0].lp(rr * fb)));
                m_line[1].push(m_hp[1].hp(m_lp[1].lp(rl * fb)));
            } else {
                m_line[0].push(L[i] + m_hp[0].hp(m_lp[0].lp(rl * fb)));
                m_line[1].push(R[i] + m_hp[1].hp(m_lp[1].lp(rr * fb)));
            }
            L[i] = L[i] * (1 - wet) + rl * wet;
            R[i] = R[i] * (1 - wet) + rr * wet;
        }
    }
    void panic() override { m_line[0].clear(); m_line[1].clear(); }
private:
    float m_sr, m_time[2] = {0, 0};
    DelayLine m_line[2];
    OnePole m_lp[2], m_hp[2];
};

// ===========================================================================
// Reverb: Freeverb (8 combs, 4 all-passes per side) with pre-delay, input
// cuts, width and freeze.
// ===========================================================================

class Reverb : public DeviceProcessor {
public:
    explicit Reverb(float sr) : m_sr(sr) {
        const float s = sr / 44100.0f;
        for (int k = 0; k < 8; ++k) {
            m_cl[k].init(int(kComb[k] * s * 1.5f) + 2);
            m_cr[k].init(int((kComb[k] + 23) * s * 1.5f) + 2);
        }
        for (int k = 0; k < 4; ++k) {
            m_al[k].init(int(kAllpass[k] * s));
            m_ar[k].init(int((kAllpass[k] + 23) * s));
        }
        m_pre.init(int(sr * 0.26f) + 8);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Reverb;
        const float size = 0.6f + 0.9f * p[0];
        if (std::fabs(size - m_size) > 0.005f) {
            m_size = size;
            const float s = m_sr / 44100.0f * size;
            for (int k = 0; k < 8; ++k) { m_cl[k].setLength(int(kComb[k] * s)); m_cr[k].setLength(int((kComb[k] + 23) * s)); }
        }
        const bool freeze = pb(p, 8);
        const float fb = freeze ? 1.0f : 0.7f + 0.28f * p[1], damp = freeze ? 0.0f : 0.05f + 0.4f * p[2], wet = p[3];
        const float inGain = freeze ? 0.0f : 0.015f;
        const float pre = pv(t, p, 4) * 0.001f * m_sr;
        const bool lowCut = p[5] > 0.002f, highCut = p[6] < 0.998f;
        m_lc.setLowpass(pv(t, p, 5), m_sr);
        m_hc.setLowpass(pv(t, p, 6), m_sr);
        const float width = p[7];
        for (int i = 0; i < n; ++i) {
            m_pre.push((L[i] + R[i]) * inGain);
            float in = pre >= 1.0f ? m_pre.read(pre) : (L[i] + R[i]) * inGain;
            if (lowCut) in = m_lc.hp(in);
            if (highCut) in = m_hc.lp(in);
            float ol = 0, orr = 0;
            for (int k = 0; k < 8; ++k) { ol += m_cl[k].process(in, fb, damp); orr += m_cr[k].process(in, fb, damp); }
            for (int k = 0; k < 4; ++k) { ol = m_al[k].process(ol); orr = m_ar[k].process(orr); }
            const float mid = 0.5f * (ol + orr), side = 0.5f * (ol - orr) * width;
            L[i] = L[i] * (1 - wet) + (mid + side) * wet;
            R[i] = R[i] * (1 - wet) + (mid - side) * wet;
        }
    }
    void panic() override {
        for (auto& c : m_cl) std::fill(c.buf.begin(), c.buf.end(), 0.0f);
        for (auto& c : m_cr) std::fill(c.buf.begin(), c.buf.end(), 0.0f);
        for (auto& a : m_al) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
        for (auto& a : m_ar) std::fill(a.buf.begin(), a.buf.end(), 0.0f);
        m_pre.clear();
    }
private:
    static constexpr int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int kAllpass[4] = {556, 441, 341, 225};
    float m_sr, m_size = -1;
    Comb m_cl[8], m_cr[8];
    Allpass m_al[4], m_ar[4];
    DelayLine m_pre;
    OnePole m_lc, m_hc;
};

// ===========================================================================
// Chorus: short delays swept by an LFO. Chorus: one copy per side, opposite
// phases; Ensemble: three copies 120° apart; Vibrato: only the moving copy.
// meter[0] = the LFO's phase.
// ===========================================================================

class Chorus : public DeviceProcessor {
public:
    explicit Chorus(float sr) : m_sr(sr) {
        m_line[0].init(int(sr * 0.08f) + 8);
        m_line[1].init(int(sr * 0.08f) + 8);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Chorus;
        const float rate = pv(t, p, 0), depth = p[1], fb = pv(t, p, 2);
        const int mode = pc(t, p, 4);
        const float wet = mode == 2 ? 1.0f : p[3];
        const float base = pv(t, p, 5) * 0.001f * m_sr, swing = depth * 0.9f * base;
        const float width = p[6];
        for (int i = 0; i < n; ++i) {
            m_ph += rate / m_sr; if (m_ph >= 1) m_ph -= 1;
            auto tap = [&](int c, float offset) {
                float ph = m_ph + offset; ph -= std::floor(ph);
                return m_line[c].read(base + swing * 0.5f * (1 + sinTable(ph)));
            };
            float wl, wr;
            if (mode == 1) {
                const float a = tap(0, 0), b = tap(0, 1.0f / 3), c = tap(1, 2.0f / 3), d = tap(1, 1.0f / 3);
                wl = (a + 0.5f * b) / 1.5f; wr = (c + 0.5f * d) / 1.5f;
            } else {
                wl = tap(0, 0); wr = tap(1, 0.5f * width);
            }
            const float mid = 0.5f * (wl + wr), side = 0.5f * (wl - wr) * (mode == 1 ? width : 1.0f);
            wl = mid + side; wr = mid - side;
            m_line[0].push(L[i] + wl * fb);
            m_line[1].push(R[i] + wr * fb);
            L[i] = L[i] * (1 - wet) + wl * wet;
            R[i] = R[i] * (1 - wet) + wr * wet;
        }
        store(meter[0], m_ph);
    }
    void panic() override { m_line[0].clear(); m_line[1].clear(); }
private:
    float m_sr, m_ph = 0;
    DelayLine m_line[2];
};

// ===========================================================================
// Phaser / Flanger. Phaser: a chain of all-passes swept around Center.
// Flanger: a short delay whose first notch sits at Center. meter[0] = the
// sweep now (Hz).
// ===========================================================================

class Phaser : public DeviceProcessor {
public:
    explicit Phaser(float sr) : m_sr(sr) {
        m_line[0].init(int(sr * 0.03f) + 8);
        m_line[1].init(int(sr * 0.03f) + 8);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Phaser;
        const float rate = pv(t, p, 0), amount = p[1], fb = pv(t, p, 2), wet = p[3];
        const bool flanger = pc(t, p, 4) == 1;
        const float center = pv(t, p, 5);
        const int stages = std::clamp(int(pv(t, p, 6)), 2, 12);
        const float spread = pv(t, p, 7) / 360.0f;
        float sweep[2] = {center, center}, coef[2] = {0, 0};
        float* io[2] = {L, R};
        for (int i = 0; i < n; ++i) {
            m_ph += rate / m_sr; if (m_ph >= 1) m_ph -= 1;
            if ((i & 15) == 0) {
                for (int c = 0; c < 2; ++c) {
                    float ph = m_ph + (c ? spread : 0.0f); ph -= std::floor(ph);
                    sweep[c] = std::clamp(center * std::exp2(amount * (flanger ? 1.5f : 2.5f) * sinTable(ph)), 30.0f, 18000.0f);
                    coef[c] = allpassCoef(sweep[c], m_sr);
                }
            }
            for (int c = 0; c < 2; ++c) {
                const float x = io[c][i];
                float y;
                if (flanger) {
                    y = m_line[c].read(std::max(1.0f, m_sr / sweep[c]));
                    m_line[c].push(x + y * fb);
                } else {
                    y = x + m_fb[c] * fb;
                    for (int k = 0; k < stages; ++k) y = m_ap[c][k].process(y, coef[c]);
                    m_fb[c] = y;
                }
                io[c][i] = x * (1 - wet) + y * wet;
            }
        }
        store(meter[0], sweep[0]);
    }
    void panic() override {
        m_line[0].clear(); m_line[1].clear();
        for (auto& ch : m_ap) for (Allpass1& a : ch) a.z = 0;
        m_fb[0] = m_fb[1] = 0;
    }
private:
    float m_sr, m_ph = 0, m_fb[2] = {0, 0};
    Allpass1 m_ap[2][12];
    DelayLine m_line[2];
};

// ===========================================================================
// Utility: gain, balance, width (mid / side), mono, phase flips, mute.
// ===========================================================================

class Utility : public DeviceProcessor {
public:
    explicit Utility(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Utility;
        const float gain = pb(p, 6) ? 0.0f : dbToGain(pv(t, p, 0));
        const float pan = pv(t, p, 1);
        const float width = pb(p, 3) ? 0.0f : pv(t, p, 2);
        const float tl = gain * std::min(1.0f, 1 - pan) * (pb(p, 4) ? -1.0f : 1.0f);
        const float tr = gain * std::min(1.0f, 1 + pan) * (pb(p, 5) ? -1.0f : 1.0f);
        const float a = onePoleCoef(0.01f, m_sr);
        for (int i = 0; i < n; ++i) {
            const float w = m_width.next(width, a);
            const float mid = 0.5f * (L[i] + R[i]), side = 0.5f * (L[i] - R[i]) * w;
            L[i] = (mid + side) * m_l.next(tl, a);
            R[i] = (mid - side) * m_r.next(tr, a);
        }
    }
private:
    float m_sr;
    Smoothed m_l, m_r, m_width;
};

// ===========================================================================
// Auto Pan: an LFO on each side's level, in time with the song; Phase sets how
// far apart the sides move (180° pans, 0° is a tremolo). meter[0] = the phase.
// ===========================================================================

class AutoPan : public DeviceProcessor {
public:
    explicit AutoPan(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        constexpr T t = T::AutoPan;
        static const int kShapes[] = {0, 1, 4, 5};   // Sine, Triangle, Square, Random in lfoShape's terms
        const float amount = p[0];
        const double rate = std::max(0.0625, double(pv(t, p, 1)));
        const int shape = kShapes[std::clamp(pc(t, p, 2), 0, 3)];
        const double phase = pv(t, p, 3) / 360.0, offset = pv(t, p, 4) / 360.0;
        const float a = onePoleCoef(0.003f, m_sr);   // softens the square's edges
        double ph = 0;
        for (int i = 0; i < n; ++i) {
            ph = (ctx.beat + i * ctx.beatsPerSample) / rate + offset;
            const float gl = 1 - amount * (0.5f + 0.5f * lfoShape(shape, ph));
            const float gr = 1 - amount * (0.5f + 0.5f * lfoShape(shape, ph + phase));
            L[i] *= m_l.next(gl, a);
            R[i] *= m_r.next(gr, a);
        }
        store(meter[0], float(ph - std::floor(ph)));
    }
private:
    float m_sr;
    Smoothed m_l, m_r;
};

// ===========================================================================
// Redux: sample-and-hold to a lower rate, then fewer bits.
// ===========================================================================

class Redux : public DeviceProcessor {
public:
    explicit Redux(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Redux;
        const int bits = std::clamp(int(pv(t, p, 0)), 1, 16);
        const float steps = float(1 << (bits - 1));
        const float step = std::min(1.0f, pv(t, p, 1) / m_sr);
        const float wet = p[2], out = dbToGain(pv(t, p, 3));
        auto crush = [&](float x) { return bits >= 16 ? x : std::round(std::clamp(x, -1.0f, 1.0f) * steps) / steps; };
        for (int i = 0; i < n; ++i) {
            m_hold += step;
            if (m_hold >= 1.0f) { m_hold -= std::floor(m_hold); m_l = crush(L[i]); m_r = crush(R[i]); }
            L[i] = (L[i] * (1 - wet) + m_l * wet) * out;
            R[i] = (R[i] * (1 - wet) + m_r * wet) * out;
        }
    }
private:
    float m_sr, m_hold = 1, m_l = 0, m_r = 0;
};

// ===========================================================================
// Gate: opens above the threshold, closes below threshold - Return after the
// hold time. meter[0] = input peak (dBFS), meter[1] = the gate's gain (0..1).
// ===========================================================================

class Gate : public DeviceProcessor {
public:
    explicit Gate(float sr) : m_sr(sr) {}
    void process(float* L, float* R, int n, const float* p, const Context& ctx) override {
        constexpr T t = T::Gate;
        m_key.prepare(t, p, L, R, n, ctx, m_sr);
        const float thresh = pv(t, p, 0);
        const float att = onePoleCoef(pv(t, p, 1) * 0.001f, m_sr);
        const int hold = int(pv(t, p, 2) * 0.001f * m_sr);
        const float rel = onePoleCoef(pv(t, p, 3) * 0.001f / 4.6f, m_sr);
        const float floorDb = pv(t, p, 4), floorGain = floorDb <= -79.9f ? 0.0f : dbToGain(floorDb);
        const float closeAt = thresh - pv(t, p, 5);
        const float detRel = onePoleCoef(0.01f, m_sr);
        for (int i = 0; i < n; ++i) {
            const float level = m_key.level(i);
            m_det = std::max(level, m_det * detRel);
            const float db = gainToDb(m_det);
            if (db > thresh) { m_open = true; m_holdLeft = hold; }
            else if (db < closeAt) { if (m_holdLeft > 0) --m_holdLeft; else m_open = false; }
            const float target = m_open ? 1.0f : floorGain;
            m_gain = target + (m_gain - target) * (m_open ? att : rel);
            L[i] *= m_gain;
            R[i] *= m_gain;
        }
        store(meter[0], gainToDb(m_key.peak));   // the level the gate listens to (its key)
        store(meter[1], m_gain);
        store(meter[2], gainToDb(m_key.peak));
        m_key.audition(t, p, L, R, n, m_sr);
    }
    void panic() override { m_det = 0; m_open = false; m_gain = 0; m_key.reset(); }
private:
    float m_sr, m_det = 0, m_gain = 0;
    bool  m_open = false;
    int   m_holdLeft = 0;
    KeyDetector m_key;
};

// ===========================================================================
// Limiter: 1.5 ms look-ahead brick wall. meter[0] = gain reduction (dB).
// ===========================================================================

class Limiter : public DeviceProcessor {
public:
    explicit Limiter(float sr) : m_sr(sr), m_ahead(std::max(1, int(sr * 0.0015f))) {
        m_line[0].init(m_ahead + 8);
        m_line[1].init(m_ahead + 8);
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr T t = T::Limiter;
        const float in = dbToGain(pv(t, p, 0)), ceil = dbToGain(pv(t, p, 1));
        const float rel = onePoleCoef(pv(t, p, 2) * 0.001f, m_sr);
        const float att = onePoleCoef(float(m_ahead) / 4.0f / m_sr, m_sr);
        float minGain = 1;
        for (int i = 0; i < n; ++i) {
            const float xl = L[i] * in, xr = R[i] * in;
            m_line[0].push(xl); m_line[1].push(xr);
            const float peak = std::max(std::fabs(xl), std::fabs(xr));
            const float target = peak > ceil ? ceil / peak : 1.0f;
            if (target <= m_hold) { m_hold = target; m_holdLeft = m_ahead; }
            else if (m_holdLeft > 0) --m_holdLeft;
            else m_hold = target + (m_hold - target) * rel;
            m_gain = m_hold < m_gain ? m_hold + (m_gain - m_hold) * att : m_hold;
            minGain = std::min(minGain, m_gain);
            L[i] = std::clamp(m_line[0].read(float(m_ahead)) * m_gain, -ceil, ceil);
            R[i] = std::clamp(m_line[1].read(float(m_ahead)) * m_gain, -ceil, ceil);
        }
        store(meter[0], -gainToDb(minGain));
    }
    void panic() override { m_line[0].clear(); m_line[1].clear(); m_hold = m_gain = 1; m_holdLeft = 0; }
private:
    float m_sr;
    int   m_ahead;
    DelayLine m_line[2];
    float m_hold = 1, m_gain = 1;
    int   m_holdLeft = 0;
};

} // namespace

float eqResponseDb(const float* params, float freq, float sampleRate) {
    EqBands b;
    b.design(params, sampleRate);
    return gainToDb(b.magnitude(freq, sampleRate));
}

std::shared_ptr<DeviceProcessor> createEffect(DeviceType type, float sr) {
    switch (type) {
    case T::DrumBuss:   return createDrumBuss(sr);
    case T::StereoWidth:return std::make_shared<StereoWidth>(sr);
    case T::EQ:         return std::make_shared<Eq>(sr);
    case T::Compressor: return std::make_shared<Compressor>(sr);
    case T::Saturator:  return std::make_shared<Saturator>(sr);
    case T::AutoFilter: return std::make_shared<AutoFilter>(sr);
    case T::Delay:      return std::make_shared<Delay>(sr);
    case T::Reverb:     return std::make_shared<Reverb>(sr);
    case T::Chorus:     sinTable(0); return std::make_shared<Chorus>(sr);
    case T::Phaser:     sinTable(0); return std::make_shared<Phaser>(sr);
    case T::Utility:    return std::make_shared<Utility>(sr);
    case T::AutoPan:    return std::make_shared<AutoPan>(sr);
    case T::Redux:      return std::make_shared<Redux>(sr);
    case T::Gate:       return std::make_shared<Gate>(sr);
    case T::Limiter:    return std::make_shared<Limiter>(sr);
    default:            return nullptr;
    }
}

} // namespace sessions::audio
