// The instruments: Analog, FM and Wavetable synths, Sampler, Organ, Bass, Electric piano and
// the drum kits. Parameter indices follow core/DeviceSpecs.cpp.

#include "DeviceKit.h"
#include "Wavetables.h"
#include "core/Sample.h"

namespace sessions::audio {

using namespace kit;

namespace {

// ===========================================================================
// Analog Synth: two oscillators and noise -> resonant filter with its own
// envelope -> amp envelope. Glide slides each new note from the last one.
// ===========================================================================

struct AnalogParams : SynthParams {
    float cutoff = 1000, q = 0.7f, envOct = 3, detune = 0, mix = 0.5f, noise = 0, keyTrack = 0.5f, glide = 0, oct2 = 1;
    int   w1 = 0, w2 = 0, ftype = 0;
    float fa = 0.001f, fd = 0.3f, fs = 0, fr = 0.2f, aa = 0.003f, ad = 0.6f, as = 0.6f, ar = 0.18f;
    void update(const float* p, float rate, const Context&) {
        constexpr T t = T::AnalogSynth;
        sr = rate;
        cutoff = pv(t, p, 0);
        q = 0.6f + p[1] * p[1] * 12.0f;
        envOct = pv(t, p, 2);
        detune = pv(t, p, 3);
        w1 = pc(t, p, 4); w2 = pc(t, p, 5);
        oct2 = std::exp2(pv(t, p, 6) + detune / 1200.0f);
        mix = p[7]; noise = p[8]; ftype = pc(t, p, 9); keyTrack = p[10];
        fa = pv(t, p, 11); fd = pv(t, p, 12); fs = p[13]; fr = pv(t, p, 14);
        aa = pv(t, p, 15); ad = pv(t, p, 16); as = p[17]; ar = pv(t, p, 18);
        glide = pv(t, p, 19);
        gain = dbToGain(pv(t, p, 20));
    }
};

struct AnalogVoice {
    Osc o1, o2; Svf filt; Adsr amp, fenv; Noise rng;
    float note = 60, vel = 0; int gate = 0, pitch = 60; uint32_t age = 0;

    bool active() const { return amp.active(); }
    void start(int p, float v, int dur, const AnalogParams& P) {
        if (!amp.active()) { o1.phase = 0; o2.phase = 0.31f; filt.reset(); }
        note = P.glide > 0.0015f && P.lastPitch >= 0 ? float(P.lastPitch) : float(p);
        pitch = p; vel = v; gate = dur;
        fenv.level = 0;
        amp.on(); fenv.on();
    }
    void release() { gate = 0; amp.off(); fenv.off(); }
    void kill() { amp.kill(); fenv.kill(); }
    void render(float* L, float* R, int n, const AnalogParams& P) {
        amp.set(P.aa, P.ad, P.as, P.ar, P.sr);
        fenv.set(P.fa, P.fd, P.fs, P.fr, P.sr);
        const float glide4 = std::pow(onePoleCoef(P.glide / 3.0f, P.sr), 4.0f);
        float dt1 = 0, dt2 = 0;
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            const float fe = fenv.next();
            if ((i & 3) == 0) {
                note = float(pitch) + (note - float(pitch)) * glide4;
                const float f = mtof(note);
                dt1 = f / P.sr; dt2 = f * P.oct2 / P.sr;
                const float track = std::exp2((note - 60.0f) / 12.0f * P.keyTrack);
                filt.set(P.cutoff * track * std::exp2(P.envOct * fe * vel), P.q, P.sr);
            }
            const float s = (1 - P.mix) * wave(o1, P.w1, dt1) + P.mix * wave(o2, P.w2, dt2) + P.noise * 0.7f * rng.next();
            const float y = filt.run(P.ftype, s) * amp.next() * vel * 0.25f;
            L[i] += y; R[i] += y;
        }
    }
};

// ===========================================================================
// FM Synth: four sine operators. A is always heard; the algorithm says which
// of B, C, D modulate whom and which are heard too. The top modulator feeds
// back on itself.
// ===========================================================================

struct FmParams : SynthParams {
    float ratio[4] = {1, 1, 2, 3}, level[4] = {1, 0.3f, 0, 0};
    float attack = 0.001f, decay = 1, sustain = 0.08f, release = 0.4f;
    float bDecay = 0.41f, bSustain = 0.15f, cDecay = 0.5f, dDecay = 0.5f, feedback = 0, velMod = 0.6f;
    int   alg = 0;
    void update(const float* p, float rate, const Context&) {
        constexpr T t = T::FMSynth;
        sr = rate;
        ratio[0] = pv(t, p, 16); ratio[1] = pv(t, p, 0); ratio[2] = pv(t, p, 9); ratio[3] = pv(t, p, 12);
        level[1] = p[1]; level[2] = p[10]; level[3] = p[13];
        attack = pv(t, p, 2); decay = pv(t, p, 3); alg = pc(t, p, 4);
        sustain = p[5]; release = pv(t, p, 6);
        bDecay = pv(t, p, 7); bSustain = p[8]; cDecay = pv(t, p, 11); dDecay = pv(t, p, 14);
        feedback = p[15]; gain = dbToGain(pv(t, p, 17)); velMod = p[18];
    }
};

// Which operators are heard (besides A) and which one feeds back, per algorithm.
constexpr bool kFmCarrier[7][4] = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 1, 0}, {1, 1, 1, 1}};
constexpr int  kFmFeedback[7] = {1, 2, 3, 2, 3, 3, 0};

struct FmVoice {
    Adsr env[4]; float ph[4] = {};
    float fbState = 0, fbLast = 0;
    float freq = 440, vel = 0; int gate = 0, pitch = 60; uint32_t age = 0;

    bool active() const { return env[0].active() || env[1].active() || env[2].active() || env[3].active(); }
    void start(int p, float v, int dur, const FmParams&) {
        if (!env[0].active()) { std::fill(ph, ph + 4, 0.0f); fbState = fbLast = 0; }
        pitch = p; freq = mtof(float(p)); vel = v; gate = dur;
        for (int k = 1; k < 4; ++k) env[k].level = 0;
        for (Adsr& e : env) e.on();
    }
    void release() { gate = 0; for (Adsr& e : env) e.off(); }
    void kill() { for (Adsr& e : env) e.kill(); }
    void render(float* L, float* R, int n, const FmParams& P) {
        const int alg = std::clamp(P.alg, 0, 6);
        const float decays[4] = {P.decay, P.bDecay, P.cDecay, P.dDecay};
        for (int k = 0; k < 4; ++k) {
            const bool heard = kFmCarrier[alg][k];
            env[k].set(heard ? P.attack : 0.001f, decays[k], k == 0 ? P.sustain : P.bSustain, P.release, P.sr);
        }
        const float d[4] = {freq * P.ratio[0] / P.sr, freq * P.ratio[1] / P.sr, freq * P.ratio[2] / P.sr, freq * P.ratio[3] / P.sr};
        const float m = 10.0f * ((1 - P.velMod) + P.velMod * vel);   // modulation index per unit of level
        const float fbAmt = P.feedback * 2.0f;
        const int fbOp = kFmFeedback[alg];
        const float* l = P.level;
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            float e[4];
            for (int k = 0; k < 4; ++k) e[k] = env[k].next();
            auto op = [&](int k, float mod) {
                if (k == fbOp) mod += fbAmt * fbState;
                const float v = std::sin(kTwoPi * ph[k] + mod) * e[k];
                if (k == fbOp) { fbState = 0.5f * (v + fbLast); fbLast = v; }
                return v;
            };
            float out = 0;
            switch (alg) {
            case 0: out = op(0, op(1, 0) * l[1] * m); break;
            case 1: out = op(0, op(1, op(2, 0) * l[2] * m) * l[1] * m); break;
            case 2: out = op(0, op(1, op(2, op(3, 0) * l[3] * m) * l[2] * m) * l[1] * m); break;
            case 3: out = op(0, (op(1, 0) * l[1] + op(2, 0) * l[2]) * m); break;
            case 4: out = op(0, (op(1, 0) * l[1] + op(2, 0) * l[2] + op(3, 0) * l[3]) * m); break;
            case 5: out = op(0, op(1, 0) * l[1] * m) + op(2, op(3, 0) * l[3] * m) * l[2]; break;
            default: out = op(0, 0) + op(1, 0) * l[1] + op(2, 0) * l[2] + op(3, 0) * l[3]; break;
            }
            for (int k = 0; k < 4; ++k) { ph[k] += d[k]; if (ph[k] >= 1) ph[k] -= std::floor(ph[k]); }
            const float y = out * vel * 0.22f;
            L[i] += y; R[i] += y;
        }
    }
};

// ===========================================================================
// Wavetable Synth: band-limited tables scanned by Position, up to five unison
// voices spread in stereo, a sub oscillator, a filter that opens with the
// amp envelope (Filter Env).
// ===========================================================================

struct WtParams : SynthParams {
    const Wavetable* table = nullptr;
    float pos = 0.3f, cutoff = 2000, spread = 10, q = 0.7f, envOct = 0, sub = 0;
    float attack = 0.1f, decay = 0.8f, sustain = 0.75f, release = 0.3f;
    int   ftype = 0, unison = 3;
    void update(const float* p, float rate, const Context&) {
        constexpr T t = T::WavetableSynth;
        sr = rate;
        pos = p[0]; cutoff = pv(t, p, 1); spread = pv(t, p, 2); attack = pv(t, p, 3);
        table = &wavetable(pc(t, p, 4));
        q = 0.6f + p[5] * p[5] * 12.0f; ftype = pc(t, p, 6);
        decay = pv(t, p, 7); sustain = p[8]; release = pv(t, p, 9);
        envOct = pv(t, p, 10); sub = p[11]; unison = int(pv(t, p, 12));
        gain = dbToGain(pv(t, p, 13));
    }
};

struct WtVoice {
    float ph[5] = {}, subPh = 0; Svf fl, fr; Adsr amp; Noise rng;
    float freq = 440, vel = 0; int gate = 0, pitch = 60; uint32_t age = 0;

    bool active() const { return amp.active(); }
    void start(int p, float v, int dur, const WtParams&) {
        if (!amp.active()) {
            for (float& x : ph) x = rng.uniform() * 0.999f;
            subPh = 0;
            fl.reset(); fr.reset();
        }
        pitch = p; freq = mtof(float(p)); vel = v; gate = dur;
        amp.on();
    }
    void release() { gate = 0; amp.off(); }
    void kill() { amp.kill(); }
    void render(float* L, float* R, int n, const WtParams& P) {
        if (!P.table) return;
        amp.set(P.attack, P.decay, P.sustain, P.release, P.sr);
        static const float off1[] = {0}, off3[] = {-1, 0, 1}, off5[] = {-1, -0.5f, 0, 0.5f, 1};
        const int U = P.unison >= 5 ? 5 : P.unison >= 3 ? 3 : 1;
        const float* offs = U == 5 ? off5 : U == 3 ? off3 : off1;
        const float norm = std::sqrt(3.0f / float(U));
        float dt[5], wl[5], wr[5];
        for (int k = 0; k < U; ++k) {
            dt[k] = freq * std::exp2(offs[k] * P.spread / 1200.0f) / P.sr;
            wl[k] = 0.5f * (1 - 0.5f * offs[k]) * norm;
            wr[k] = 0.5f * (1 + 0.5f * offs[k]) * norm;
        }
        const int level = wavetableLevel(freq * std::exp2(P.spread / 1200.0f), P.sr);
        const float subDt = freq * 0.5f / P.sr;
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            const float e = amp.next();
            if ((i & 7) == 0) {
                const float cut = P.cutoff * std::exp2(P.envOct * e);
                fl.set(cut, P.q, P.sr);
                fr.set(cut, P.q, P.sr);
            }
            float sl = 0, sr = 0;
            for (int k = 0; k < U; ++k) {
                const float s = wavetableRead(*P.table, level, P.pos, ph[k]);
                sl += s * wl[k]; sr += s * wr[k];
                ph[k] += dt[k]; if (ph[k] >= 1) ph[k] -= 1;
            }
            if (P.sub > 0) {
                const float s = sinTable(subPh) * P.sub * 0.7f;
                sl += s; sr += s;
            }
            subPh += subDt; if (subPh >= 1) subPh -= 1;
            const float g = e * vel * 0.16f;
            L[i] += fl.run(P.ftype, sl) * g;
            R[i] += fr.run(P.ftype, sr) * g;
        }
    }
};

class WavetableSynth : public PolySynth<WtVoice, WtParams, 12> {
public:
    explicit WavetableSynth(float sr) : PolySynth(sr) { wavetable(0); sinTable(0); }
};

// ===========================================================================
// Sampler: plays its sample from Root (pitched by the key), between Start and
// End, looping that stretch if Loop is on; ADSR, filter and velocity. With no
// sample it's a soft tone.
// ===========================================================================

struct SamplerParams : SynthParams {
    float tune = 0, attack = 0.001f, release = 0.4f, start = 0, end = 1, cutoff = 20000, q = 0.7f;
    float decay = 10, sustain = 1, root = 60, velAmt = 1;
    bool  loop = false, filter = false;
    const SampleData* sample = nullptr;
    void update(const float* p, float rate, const Context& ctx) {
        constexpr T t = T::Sampler;
        sr = rate;
        tune = pv(t, p, 0); attack = pv(t, p, 1); release = pv(t, p, 2); gain = faderGain(p[3]);
        start = p[4]; end = p[5]; loop = pb(p, 6);
        cutoff = pv(t, p, 7); q = 0.6f + p[8] * p[8] * 12.0f; filter = p[7] < 0.995f || p[8] > 0.01f;
        decay = pv(t, p, 9); sustain = p[10]; root = pv(t, p, 11); velAmt = p[12];
        sample = ctx.sample;
    }
};

struct SamplerVoice {
    Osc osc; Adsr amp; Svf fl, fr;
    float vel = 0, velGain = 1; int gate = 0, pitch = 60; uint32_t age = 0;
    double pos = 0;                      // sample mode: read position in frames
    const SampleData* playing = nullptr; // the sample this voice started on

    bool active() const { return amp.active(); }
    void start(int p, float v, int dur, const SamplerParams& P) {
        if (!amp.active()) { osc.phase = 0; fl.reset(); fr.reset(); }
        pitch = p; vel = v; gate = dur;
        velGain = 1 - P.velAmt + P.velAmt * v;
        playing = P.sample;
        pos = playing ? std::clamp(double(P.start), 0.0, 1.0) * double(std::max<qint64>(0, playing->frames() - 1)) : 0;
        amp.on();
    }
    void release() { gate = 0; amp.off(); }
    void kill() { amp.kill(); }
    void render(float* L, float* R, int n, const SamplerParams& P) {
        amp.set(P.attack, P.decay, P.sustain, P.release, P.sr);
        if (P.filter) { fl.set(P.cutoff, P.q, P.sr); fr.set(P.cutoff, P.q, P.sr); }
        if (playing && playing == P.sample) { renderSample(L, R, n, P); return; }
        if (playing) { kill(); return; }   // the sample was swapped while the note rang: stop it
        const float dt = mtof(float(pitch) + P.tune + 60.0f - P.root) / P.sr;
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            float y = (0.6f * osc.triAt() + 0.4f * osc.sineAt()) * amp.next() * velGain * 0.25f;
            osc.advance(dt);
            if (P.filter) y = fl.lp(y);
            L[i] += y; R[i] += y;
        }
    }
    void renderSample(float* L, float* R, int n, const SamplerParams& P) {
        const SampleData& s = *playing;
        const qint64 frames = s.frames();
        if (frames < 4) { kill(); return; }
        const double last = double(frames - 1);
        const double a = std::clamp(double(P.start), 0.0, 1.0) * last;
        const double b = std::max(std::min(last, a + 2.0), std::clamp(double(P.end), 0.0, 1.0) * last);
        const double step = s.rate / P.sr * std::exp2((pitch - P.root + P.tune) / 12.0);
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            if (pos >= b) {
                if (P.loop && b - a > 8) pos = a + std::fmod(pos - a, b - a);
                else { kill(); return; }   // the sample ended
            }
            const qint64 k = std::clamp<qint64>(qint64(pos), 0, frames - 2);
            const float f = float(pos - double(k));
            const float g = amp.next() * velGain;
            float l = s.left[size_t(k)] + (s.left[size_t(k + 1)] - s.left[size_t(k)]) * f;
            float r = s.right[size_t(k)] + (s.right[size_t(k + 1)] - s.right[size_t(k)]) * f;
            if (P.filter) { l = fl.lp(l); r = fr.lp(r); }
            L[i] += l * g;
            R[i] += r * g;
            pos += step;
        }
    }
};

// ===========================================================================
// Organ: nine drawbars of sine partials, percussion, key click; then the
// scanner vibrato / chorus, drive and the rotary speaker on the whole sound.
// ===========================================================================

constexpr float kFootage[9] = {0.5f, 1.5f, 1, 2, 3, 4, 5, 6, 8};

struct OrganParams : SynthParams {
    float bar[9] = {1, 1, 1};
    bool  perc = false; float percHarm = 3, percDecay = 0.2f, click = 0.3f;
    int   vibrato = 0, rotary = 0; float drive = 0.2f;
    void update(const float* p, float rate, const Context&) {
        constexpr T t = T::Organ;
        sr = rate;
        for (int k = 0; k < 9; ++k) {
            const int n = int(pv(t, p, k));
            bar[k] = n <= 0 ? 0.0f : dbToGain(float(n - 8) * 3.0f);
        }
        perc = pb(p, 9); percHarm = pc(t, p, 10) ? 3.0f : 2.0f; percDecay = pc(t, p, 11) ? 1.0f : 0.25f;
        click = p[12]; vibrato = pc(t, p, 13); rotary = pc(t, p, 14); drive = p[15];
        gain = dbToGain(pv(t, p, 16));
    }
};

struct OrganVoice {
    float ph[9] = {}, percPh = 0, percEnv = 0, clickEnv = 0;
    Adsr amp; Svf clickFilter; Noise rng;
    float freq = 440, vel = 0; int gate = 0, pitch = 60; uint32_t age = 0;

    bool active() const { return amp.active(); }
    void start(int p, float v, int dur, const OrganParams& P) {
        if (!amp.active()) for (float& x : ph) x = rng.uniform() * 0.999f;   // tonewheels never stop turning
        pitch = p; freq = mtof(float(p)); vel = v; gate = dur;
        amp.set(0.004f, 1.0f, 1.0f, 0.012f, P.sr);
        amp.on();
        percEnv = P.perc ? 1.0f : 0.0f; percPh = 0;
        clickEnv = P.click;
        clickFilter.set(std::min(P.sr * 0.4f, freq * 6.0f), 0.8f, P.sr);
    }
    void release() { gate = 0; amp.off(); }
    void kill() { amp.kill(); }
    void render(float* L, float* R, int n, const OrganParams& P) {
        float dt[9];
        for (int k = 0; k < 9; ++k) dt[k] = freq * kFootage[k] < P.sr * 0.45f ? freq * kFootage[k] / P.sr : 0.0f;
        const float percDt = freq * P.percHarm / P.sr;
        const float percCoef = onePoleCoef(P.percDecay / 4.6f, P.sr), clickCoef = onePoleCoef(0.0025f, P.sr);
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            float s = 0;
            for (int k = 0; k < 9; ++k) {
                if (P.bar[k] > 0 && dt[k] > 0) s += P.bar[k] * sinTable(ph[k]);
                ph[k] += dt[k]; if (ph[k] >= 1) ph[k] -= 1;
            }
            if (percEnv > 1e-4f) {
                s += percEnv * 0.8f * sinTable(percPh);
                percPh += percDt; if (percPh >= 1) percPh -= 1;
                percEnv *= percCoef;
            }
            if (clickEnv > 1e-4f) { s += clickFilter.bp(rng.next()) * clickEnv * 1.5f; clickEnv *= clickCoef; }
            const float y = s * amp.next() * 0.07f;
            L[i] += y; R[i] += y;
        }
    }
};

class Organ : public PolySynth<OrganVoice, OrganParams, 16> {
public:
    explicit Organ(float sr) : PolySynth(sr) {
        sinTable(0);
        m_scanner.init(int(sr * 0.004f) + 8);
        m_horn[0].init(int(sr * 0.006f) + 8);
        m_horn[1].init(int(sr * 0.006f) + 8);
        m_split.setLowpass(800, sr);
    }

protected:
    void post(float* L, float* R, int n, const Context&) override {
        const OrganParams& P = m_params;
        // scanner vibrato (V1-V3) / chorus (C1-C3): a delay swept at ~7 Hz, alone or mixed with the dry sound
        static const float depthMs[] = {0, 0.25f, 0.5f, 0.9f, 0.25f, 0.5f, 0.9f};
        const int vib = std::clamp(P.vibrato, 0, 6);
        const float depth = depthMs[vib] * 0.001f * m_sr;
        // drive
        const float k = 1 + P.drive * 5.0f, makeup = 1.0f / std::pow(k, 0.6f);
        // rotary: horn and drum speeds ramp toward Slow / Fast like the real motors
        const float hornTarget = P.rotary == 2 ? 6.8f : 0.8f, drumTarget = P.rotary == 2 ? 5.9f : 0.67f;
        const float hornRamp = 1 - onePoleCoef(0.6f, m_sr), drumRamp = 1 - onePoleCoef(2.5f, m_sr);
        for (int i = 0; i < n; ++i) {
            float x = L[i];
            if (vib) {
                m_scanner.push(x);
                m_scanPh += 6.9f / m_sr; if (m_scanPh >= 1) m_scanPh -= 1;
                const float wet = m_scanner.read(2.0f + depth * (1 + sinTable(m_scanPh)));
                x = vib >= 4 ? 0.5f * (x + wet) * 1.3f : wet;
            }
            x = std::tanh(x * k) * makeup;
            if (P.rotary == 0) { L[i] = R[i] = x; continue; }
            m_hornHz += (hornTarget - m_hornHz) * hornRamp;
            m_drumHz += (drumTarget - m_drumHz) * drumRamp;
            m_hornPh += m_hornHz / m_sr; if (m_hornPh >= 1) m_hornPh -= 1;
            m_drumPh += m_drumHz / m_sr; if (m_drumPh >= 1) m_drumPh -= 1;
            const float lo = m_split.lp(x), hi = x - lo;
            const float hs = sinTable(m_hornPh), hc = sinTable(m_hornPh + (m_hornPh < 0.75f ? 0.25f : -0.75f));
            const float ds = sinTable(m_drumPh), dc = sinTable(m_drumPh + (m_drumPh < 0.75f ? 0.25f : -0.75f));
            m_horn[0].push(hi); m_horn[1].push(hi);
            const float base = 0.0025f * m_sr, swing = 0.0012f * m_sr;
            const float hl = m_horn[0].read(base + swing * hs) * (0.75f + 0.35f * hc);
            const float hr = m_horn[1].read(base - swing * hs) * (0.75f - 0.35f * hc);
            L[i] = hl + lo * (0.85f + 0.15f * ds);
            R[i] = hr + lo * (0.85f + 0.15f * dc);
        }
        meter[1].store(m_hornPh, std::memory_order_relaxed);   // the panel spins the horn
    }
    void reset() override { m_scanner.clear(); m_horn[0].clear(); m_horn[1].clear(); }

private:
    DelayLine m_scanner, m_horn[2];
    OnePole   m_split;
    float m_scanPh = 0, m_hornPh = 0, m_drumPh = 0, m_hornHz = 0.8f, m_drumHz = 0.67f;
};

// ===========================================================================
// Bass: one voice. A note played while another is still held slides to it
// without restarting the envelopes (the acid slide); hard-played notes
// (velocity above 100) get the accent: louder, with more filter sweep.
// ===========================================================================

class Bass : public DeviceProcessor {
public:
    explicit Bass(float sr) : m_sr(sr) {}

    void render(const EventList& ev, float* L, float* R, int frames, const float* p, const Context&) override {
        constexpr T t = T::Bass;
        m_wave = pc(t, p, 0);
        m_cutoff = pv(t, p, 1);
        m_q = 0.7f + p[2] * p[2] * 14.0f;
        m_envMod = p[3];
        m_decay = pv(t, p, 4);
        m_accentAmt = p[5];
        m_glide = pv(t, p, 6);
        m_sub = p[7];
        m_drive = p[8];
        const float vol = dbToGain(pv(t, p, 9));
        m_amp.set(0.002f, 1.5f, 0.6f, 0.02f, m_sr);
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) handle(ev.ev[i++], pos);
            const int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            renderRange(L, R, pos, end, vol);
            pos = end;
        }
        m_clock += frames;
        meter[0].store(m_amp.active() ? m_fenv : 0.0f, std::memory_order_relaxed);
    }
    void panic() override { m_count = 0; m_amp.kill(); m_f1.reset(); m_f2.reset(); }

private:
    struct Held { int pitch; float vel; int64_t end; };

    void handle(const NoteEvent& e, int pos) {
        const int64_t now = m_clock + pos;
        if (e.allOff) { m_count = 0; m_amp.off(); return; }
        if (e.noteOff) {
            for (int k = 0; k < m_count; ++k) if (m_stack[k].pitch == e.pitch) { remove(k); break; }
            follow();
            return;
        }
        for (int k = 0; k < m_count; ++k) if (m_stack[k].end <= now) remove(k--);   // notes that end here don't slide
        const bool legato = m_count > 0 && m_amp.active();
        for (int k = 0; k < m_count; ++k) if (m_stack[k].pitch == e.pitch) { remove(k); break; }
        if (m_count == 16) remove(0);
        m_stack[m_count++] = {e.pitch, e.velocity, now + std::max(1, e.duration)};
        m_target = float(e.pitch);
        m_accent = e.velocity > 0.79f ? m_accentAmt : 0.0f;
        if (!legato) {
            m_note = m_target;
            m_amp.level = 0; m_amp.on();
            m_fenv = 1;
            m_vel = 0.6f + 0.4f * e.velocity;
        }
    }
    void remove(int k) { for (int j = k; j + 1 < m_count; ++j) m_stack[j] = m_stack[j + 1]; --m_count; }
    // After a note ends: slide back to the newest note still held, or let go.
    void follow() {
        if (m_count > 0) m_target = float(m_stack[m_count - 1].pitch);
        else m_amp.off();
    }

    void renderRange(float* L, float* R, int from, int to, float vol) {
        const float glideCoef = onePoleCoef(m_glide / 3.0f, m_sr);
        const float fenvCoef = onePoleCoef(m_decay * (m_accent > 0 ? 0.6f : 1.0f) / 4.6f, m_sr);
        const float k = 1 + m_drive * 6.0f, makeup = 1.0f / std::pow(k, 0.55f);
        const float accentGain = 1 + m_accent * 0.8f;
        for (int i = from; i < to; ++i) {
            const int64_t now = m_clock + i;
            bool expired = false;
            for (int j = 0; j < m_count; ++j) if (m_stack[j].end <= now) { remove(j--); expired = true; }
            if (expired) follow();
            if (!m_amp.active()) continue;
            m_note = m_target + (m_note - m_target) * glideCoef;
            if ((i & 3) == 0 || i == from) {
                m_dt = mtof(m_note) / m_sr;
                const float sweep = m_envMod * 4.5f * m_fenv * (1 + m_accent);
                m_f1.set(m_cutoff * std::exp2(sweep), 0.707f, m_sr);
                m_f2.set(m_cutoff * std::exp2(sweep), m_q, m_sr);
            }
            m_fenv *= fenvCoef;
            float s = m_wave ? m_osc.squareAt(m_dt) : m_osc.sawAt(m_dt);
            m_osc.advance(m_dt);
            s = m_f2.lp(m_f1.lp(s));
            s += sinTable(m_subPh) * m_sub * 0.8f;
            m_subPh += m_dt * 0.5f; if (m_subPh >= 1) m_subPh -= 1;
            const float y = std::tanh(s * k) * makeup * m_amp.next() * m_vel * accentGain * 0.35f * vol;
            L[i] += y; R[i] += y;
        }
    }

    float m_sr;
    Held  m_stack[16]; int m_count = 0;
    int64_t m_clock = 0;
    Osc   m_osc; Svf m_f1, m_f2; Adsr m_amp;
    float m_note = 48, m_target = 48, m_dt = 0, m_subPh = 0, m_fenv = 0, m_vel = 1, m_accent = 0;
    int   m_wave = 0;
    float m_cutoff = 400, m_q = 1, m_envMod = 0.5f, m_decay = 0.3f, m_accentAmt = 0.4f, m_glide = 0.06f, m_sub = 0.3f, m_drive = 0.3f;
};

// ===========================================================================
// Electric: a tine piano. A sine carrier with a velocity-sensitive FM bark
// (Hammer) and a bell partial (Tine) that fade fast; higher notes die
// sooner. Then the amp's stereo tremolo and a little drive.
// ===========================================================================

struct EpParams : SynthParams {
    float tine = 0.5f, hammer = 0.4f, decay = 4, release = 0.25f, rate = 4, depth = 0.3f, drive = 0.1f;
    void update(const float* p, float rate_, const Context&) {
        constexpr T t = T::Electric;
        sr = rate_;
        tine = p[0]; hammer = p[1]; decay = pv(t, p, 2); release = pv(t, p, 3);
        rate = pv(t, p, 4); depth = p[5]; drive = p[6];
        gain = dbToGain(pv(t, p, 7));
    }
};

struct EpVoice {
    float ph1 = 0, ph2 = 0, ph3 = 0, modEnv = 0, tineEnv = 0, index = 1;
    Adsr amp;
    float freq = 440, vel = 0; int gate = 0, pitch = 60; uint32_t age = 0;

    bool active() const { return amp.active(); }
    void start(int p, float v, int dur, const EpParams& P) {
        pitch = p; freq = mtof(float(p)); vel = v; gate = dur;
        ph1 = 0; ph2 = 0; ph3 = 0.25f;
        modEnv = 1; tineEnv = 1;
        index = (0.3f + 3.0f * P.hammer) * v * v;
        amp.level = 0;
        amp.on();
    }
    void release() { gate = 0; amp.off(); }
    void kill() { amp.kill(); }
    void render(float* L, float* R, int n, const EpParams& P) {
        const float keyScale = std::exp2(-(float(pitch) - 60.0f) / 24.0f);
        amp.set(0.0015f, P.decay * keyScale, 0.0f, P.release, P.sr);
        const float modCoef = onePoleCoef(0.25f * keyScale, P.sr), tineCoef = onePoleCoef(0.06f * keyScale, P.sr);
        const float d1 = freq / P.sr, d3 = freq * 14.0f < P.sr * 0.45f ? freq * 14.0f / P.sr : 0.0f;
        for (int i = 0; i < n; ++i) {
            if (gate > 0 && --gate == 0) release();
            const float mod = std::sin(kTwoPi * ph2) * index * modEnv;
            float s = std::sin(kTwoPi * ph1 + mod);
            if (d3 > 0) s += sinTable(ph3) * P.tine * 0.35f * tineEnv * vel;
            modEnv *= modCoef; tineEnv *= tineCoef;
            ph1 += d1; if (ph1 >= 1) ph1 -= 1;
            ph2 += d1; if (ph2 >= 1) ph2 -= 1;
            ph3 += d3; if (ph3 >= 1) ph3 -= 1;
            const float y = s * amp.next() * vel * 0.3f;
            L[i] += y; R[i] += y;
        }
    }
};

class Electric : public PolySynth<EpVoice, EpParams, 12> {
public:
    explicit Electric(float sr) : PolySynth(sr) { sinTable(0); }
protected:
    void post(float* L, float* R, int n, const Context&) override {
        const EpParams& P = m_params;
        const float k = 1 + P.drive * 6.0f, makeup = 1.0f / std::pow(k, 0.6f);
        for (int i = 0; i < n; ++i) {
            m_lfo += P.rate / m_sr; if (m_lfo >= 1) m_lfo -= 1;
            const float s = sinTable(m_lfo);
            const float gl = 1 - P.depth * (0.5f + 0.5f * s), gr = 1 - P.depth * (0.5f - 0.5f * s);
            L[i] = std::tanh(L[i] * k) * makeup * gl;
            R[i] = std::tanh(R[i] * k) * makeup * gr;
        }
        meter[1].store(m_lfo, std::memory_order_relaxed);
    }
private:
    float m_lfo = 0;
};

// ===========================================================================
// Drum kits: synthesized one-shots on the General MIDI drum map
// (36 kick, 38 snare, 39 clap, 37 rim, 42/44 closed hat, 46 open hat,
// 49/51/57 cymbals, everything else a tuned tom).
// ===========================================================================

enum class Drum { Kick, Snare, Clap, Rim, ClosedHat, OpenHat, Cymbal, Tom };

Drum drumFor(int pitch) {
    switch (pitch) {
    case 35: case 36:                              return Drum::Kick;
    case 38: case 40:                              return Drum::Snare;
    case 39:                                       return Drum::Clap;
    case 37:                                       return Drum::Rim;
    case 42: case 44:                              return Drum::ClosedHat;
    case 46:                                       return Drum::OpenHat;
    case 49: case 51: case 52: case 55: case 57: case 59: return Drum::Cymbal;
    default:                                       return Drum::Tom;
    }
}

struct DrumVoice {
    bool  on = false;
    Drum  kind = Drum::Kick;
    int   pitch = 0, samples = 0;
    float vel = 0, gain = 1, panL = 1, panR = 1;
    // tonal body with a falling pitch
    float phase = 0, fStart = 0, fEnd = 0, sweep = 0, sweepCoef = 0;
    float tone = 0, toneCoef = 0, toneLevel = 0;
    // noise / metal
    float noiseEnv = 0, noiseCoef = 0, noiseLevel = 0, noiseMix = 1;
    float metal[6] = {}, metalFreq[6] = {};
    Svf   filt; bool bandpass = false;
    // transient click
    float click = 0, clickCoef = 0, clickLevel = 0;
    // clap re-triggers
    int   clapBursts = 0, clapTimer = 0, clapGap = 0; float tailCoef = 0;
    Noise noise;
    bool  saturate = false;
};

class DrumKit : public DeviceProcessor {
public:
    DrumKit(float sr, int variant) : m_sr(sr), m_variant(variant) {}

    void render(const EventList& ev, float* L, float* R, int frames, const float* p, const Context&) override {
        m_kickDecay  = pv(T::DrumKit, p, 0);
        m_snareDecay = pv(T::DrumKit, p, 1);
        m_hatDecay   = pv(T::DrumKit, p, 2);
        m_tune       = std::exp2(pv(T::DrumKit, p, 3) / 12.0f);
        const float vol = dbToGain(pv(T::DrumKit, p, 4));
        std::fill(m_l, m_l + frames, 0.0f);
        std::fill(m_r, m_r + frames, 0.0f);
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) trigger(ev.ev[i++]);
            int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            for (DrumVoice& v : m_voices) if (v.on) renderVoice(v, m_l + pos, m_r + pos, end - pos);
            pos = end;
        }
        const float coef = onePoleCoef(0.01f, m_sr);
        for (int k = 0; k < frames; ++k) {
            const float g = m_gain.next(vol, coef);
            L[k] += m_l[k] * g;
            R[k] += m_r[k] * g;
        }
    }
    void panic() override { for (DrumVoice& v : m_voices) v.on = false; }

private:
    float coef(float decaySeconds) const { return onePoleCoef(decaySeconds / 4.6f, m_sr); }   // ~-40 dB after decaySeconds

    void trigger(const NoteEvent& e) {
        if (e.allOff || e.noteOff) return;   // one-shots ring out, like a drum rack
        // the panel lights the pad that was hit
        meter[0].store(float(e.pitch), std::memory_order_relaxed);
        meter[1].store(meter[1].load(std::memory_order_relaxed) + 1.0f, std::memory_order_relaxed);
        Drum kind = drumFor(e.pitch);
        if (kind == Drum::ClosedHat)   // closed hat chokes the open hat
            for (DrumVoice& v : m_voices) if (v.on && v.kind == Drum::OpenHat) v.noiseCoef = coef(0.02f);
        DrumVoice* pick = nullptr;
        for (DrumVoice& v : m_voices) if (v.on && v.pitch == e.pitch) { pick = &v; break; }
        if (!pick) for (DrumVoice& v : m_voices) if (!v.on) { pick = &v; break; }
        if (!pick) { pick = &m_voices[0]; for (DrumVoice& v : m_voices) if (v.samples > pick->samples) pick = &v; }

        DrumVoice& v = *pick;
        Noise keep = v.noise;
        v = DrumVoice();
        v.noise = keep;
        v.on = true; v.kind = kind; v.pitch = e.pitch; v.vel = e.velocity;
        const bool is808 = m_variant == 1, acoustic = m_variant == 2;
        const float t = m_tune;
        switch (kind) {
        case Drum::Kick: {
            float decay = m_kickDecay * (is808 ? 1.5f : acoustic ? 0.7f : 1.0f);
            v.fStart = (is808 ? 110.0f : 160.0f) * t; v.fEnd = (is808 ? 45.0f : 52.0f) * t;
            v.sweep = 1; v.sweepCoef = onePoleCoef(is808 ? 0.06f : 0.035f, m_sr);
            v.tone = 1; v.toneCoef = coef(decay); v.toneLevel = 1.0f;
            v.click = 1; v.clickCoef = onePoleCoef(0.0015f, m_sr); v.clickLevel = acoustic ? 0.5f : 0.25f;
            v.gain = 0.55f; v.saturate = is808;
            break;
        }
        case Drum::Snare:
            v.fStart = 260.0f * t; v.fEnd = 185.0f * t;
            v.sweep = 1; v.sweepCoef = onePoleCoef(0.01f, m_sr);
            v.tone = 1; v.toneCoef = coef(is808 ? 0.18f : 0.12f); v.toneLevel = is808 ? 0.7f : 0.45f;
            v.noiseEnv = 1; v.noiseCoef = coef(m_snareDecay); v.noiseLevel = acoustic ? 0.9f : 0.65f;
            v.filt.set(1800.0f * t, 0.6f, m_sr);
            v.gain = 0.3f;
            break;
        case Drum::Clap:
            v.noiseEnv = 1; v.noiseCoef = onePoleCoef(0.003f, m_sr); v.noiseLevel = 1.0f;
            v.filt.set(1100.0f * t, 1.5f, m_sr); v.bandpass = true;
            v.clapBursts = 2; v.clapGap = int(0.009f * m_sr); v.clapTimer = v.clapGap;
            v.tailCoef = coef(m_snareDecay * 0.8f);
            v.gain = 0.4f;
            break;
        case Drum::Rim:
            v.fStart = v.fEnd = 520.0f * t; v.tone = 1; v.toneCoef = coef(0.05f); v.toneLevel = 0.6f;
            v.noiseEnv = 1; v.noiseCoef = coef(0.03f); v.noiseLevel = 0.5f;
            v.filt.set(2000.0f * t, 2.0f, m_sr); v.bandpass = true;
            v.gain = 0.5f;
            break;
        case Drum::ClosedHat:
        case Drum::OpenHat:
        case Drum::Cymbal: {
            static const float ratios[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
            float base = kind == Drum::Cymbal ? 1.2f : 1.6f;
            for (int k = 0; k < 6; ++k) { v.metalFreq[k] = ratios[k] * base * t; v.metal[k] = v.noise.uniform(); }
            float decay = kind == Drum::ClosedHat ? m_hatDecay : kind == Drum::OpenHat ? m_hatDecay * 5.0f : 1.6f;
            v.noiseEnv = 1; v.noiseCoef = coef(decay);
            v.noiseLevel = 1.0f;
            v.noiseMix = acoustic ? 0.75f : is808 ? 0.3f : 0.5f;   // noise vs metal
            v.filt.set(kind == Drum::Cymbal ? 5000.0f : 7000.0f, 0.7f, m_sr);
            v.gain = kind == Drum::Cymbal ? 0.22f : 0.25f;
            v.panL = kind == Drum::Cymbal ? 1.0f : 0.8f; v.panR = kind == Drum::Cymbal ? 0.8f : 1.0f;
            break;
        }
        case Drum::Tom: {
            v.fEnd = 80.0f * std::exp2((e.pitch - 41) / 10.0f) * t; v.fStart = v.fEnd * 1.6f;
            v.sweep = 1; v.sweepCoef = onePoleCoef(0.04f, m_sr);
            v.tone = 1; v.toneCoef = coef(0.4f); v.toneLevel = 0.8f;
            v.noiseEnv = 1; v.noiseCoef = coef(0.03f); v.noiseLevel = 0.1f;
            v.filt.set(3000.0f, 0.7f, m_sr); v.bandpass = true;
            float pan = std::clamp((e.pitch - 45) / 12.0f, -0.4f, 0.4f);
            v.panL = std::min(1.0f, 1.0f - pan); v.panR = std::min(1.0f, 1.0f + pan);
            v.gain = 0.5f;
            break;
        }
        }
    }

    void renderVoice(DrumVoice& v, float* L, float* R, int n) {
        const bool metallic = v.kind == Drum::ClosedHat || v.kind == Drum::OpenHat || v.kind == Drum::Cymbal;
        for (int i = 0; i < n; ++i) {
            float s = 0;
            if (v.toneLevel > 0) {
                float f = v.fEnd + (v.fStart - v.fEnd) * v.sweep;
                v.sweep *= v.sweepCoef;
                v.phase += f / m_sr; if (v.phase >= 1) v.phase -= 1;
                s += std::sin(kTwoPi * v.phase) * v.tone * v.toneLevel;
                v.tone *= v.toneCoef;
            }
            if (v.noiseLevel > 0) {
                float src = v.noise.next();
                if (metallic) {
                    float m = 0;
                    for (int k = 0; k < 6; ++k) {
                        v.metal[k] += v.metalFreq[k] / m_sr; if (v.metal[k] >= 1) v.metal[k] -= 1;
                        m += v.metal[k] < 0.5f ? 1.0f : -1.0f;
                    }
                    src = src * v.noiseMix + m * (1.0f / 6.0f) * (1.0f - v.noiseMix);
                }
                float filtered = v.bandpass ? v.filt.bp(src) : v.filt.hp(src);
                s += filtered * v.noiseEnv * v.noiseLevel;
                v.noiseEnv *= v.noiseCoef;
                if (v.clapBursts > 0 && --v.clapTimer <= 0) {   // clap: re-trigger, then a longer tail
                    v.noiseEnv = 1;
                    if (--v.clapBursts == 0) v.noiseCoef = v.tailCoef; else v.clapTimer = v.clapGap;
                }
            }
            if (v.clickLevel > 0) { s += v.noise.next() * v.click * v.clickLevel; v.click *= v.clickCoef; }
            if (v.saturate) s = std::tanh(s * 1.4f);
            s *= v.vel * v.gain;
            L[i] += s * v.panL;
            R[i] += s * v.panR;
            ++v.samples;
        }
        bool tail = v.tone > 1e-4f && v.toneLevel > 0;
        tail |= v.noiseEnv > 1e-4f && v.noiseLevel > 0;
        tail |= v.clapBursts > 0;
        if (!tail && v.samples > 64) v.on = false;
    }

    DrumVoice m_voices[16];
    float m_sr;
    int   m_variant;   // 0 kit, 1 808, 2 acoustic
    float m_kickDecay = 0.4f, m_snareDecay = 0.2f, m_hatDecay = 0.06f, m_tune = 1;
    Smoothed m_gain;
    float m_l[kMaxBlock] = {}, m_r[kMaxBlock] = {};
};

} // namespace

// ===========================================================================
// Drum Rack: sixteen pads, each a one-shot sample on its note (pad 1 on C1). A pad's
// Transpose plays it faster or slower, Decay fades it out, and a pad in a choke group
// silences the other pads of that group (the closed hat cuts the open one).
// ===========================================================================

// The Drum Rack. When pads hold chains (the engine runs them), one call renders the pads without
// chains and starts every sample voice; the voices of pads whose chain has effects wait for the
// call that renders just that pad (Context::padPlay) into its chain, starting where they were hit.
class DrumRack : public DeviceProcessor {
public:
    explicit DrumRack(float sr) : m_sr(sr), m_mix(sr) {}

    void render(const EventList& ev, float* L, float* R, int frames, const float* p, const Context& ctx) override {
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) trigger(ev.ev[i++], p, ctx, pos);
            const int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            for (Voice& v : m_voices) if (v.on && (ctx.padPlay >> v.pad & 1u)) play(v, L + pos, R + pos, end - pos, p, ctx);
            pos = end;
        }
    }
    void panic() override { for (Voice& v : m_voices) v.on = false; m_mix.reset(); }
    ChainMixer* chainMixer() override { return &m_mix; }

private:
    struct Voice {
        bool on = false, choking = false;
        int  pad = 0;
        int  delay = 0;                  // frames into the block before it starts (a pad rendered in its own call)
        const SampleData* s = nullptr;   // the pad's sample when it started
        double pos = 0, step = 1;
        float vel = 1, env = 1, choke = 1;
        uint32_t age = 0;
    };
    static constexpr T kT = T::DrumRack;
    static int knob(int pad, int which) { return drumrack::param(pad, which); }

    void trigger(const NoteEvent& e, const float* p, const Context& ctx, int at) {
        if (e.allOff || e.noteOff) return;   // one-shots ring out
        const int pad = e.pitch - drumrack::kFirstNote;
        if (pad < 0 || pad >= drumrack::kPads) return;
        const bool sample = ctx.padSample >> pad & 1u;   // else the pad's instrument plays it
        const SampleData* s = sample && ctx.pads ? ctx.pads[pad].get() : nullptr;
        if (sample && (!s || s->frames() < 2)) return;
        if (const int group = pc(kT, p, knob(pad, drumrack::Choke)); group > 0)   // choke the others in the group
            for (Voice& v : m_voices)
                if (v.on && v.pad != pad && pc(kT, p, knob(v.pad, drumrack::Choke)) == group) v.choking = true;
        if (!sample) {   // the panel lights the pad all the same
            meter[0].store(float(e.pitch), std::memory_order_relaxed);
            meter[1].store(meter[1].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
            return;
        }
        Voice* pick = nullptr;
        for (Voice& v : m_voices) if (!v.on) { pick = &v; break; }
        if (!pick) { pick = &m_voices[0]; for (Voice& v : m_voices) if (v.age < pick->age) pick = &v; }
        const float velAmt = p[1];
        *pick = Voice{};
        pick->on = true;
        pick->pad = pad;
        pick->s = s;
        pick->step = s->rate / m_sr * std::exp2(pv(kT, p, knob(pad, drumrack::Transpose)) / 12.0);
        pick->vel = 1 - velAmt + velAmt * e.velocity;
        pick->age = ++m_age;
        pick->delay = ctx.padPlay >> pad & 1u ? 0 : at;
        meter[0].store(float(e.pitch), std::memory_order_relaxed);   // the panel lights the pad
        meter[1].store(meter[1].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    void play(Voice& v, float* L, float* R, int n, const float* p, const Context& ctx) {
        if (!ctx.pads || ctx.pads[v.pad].get() != v.s) { v.on = false; return; }   // the pad's sample was replaced
        if (v.delay > 0) {   // hit later in this block
            const int d = std::min(v.delay, n);
            v.delay -= d;
            L += d; R += d; n -= d;
        }
        const SampleData& s = *v.s;
        const qint64 frames = s.frames();
        const float gain = dbToGain(pv(kT, p, 0)) * dbToGain(pv(kT, p, knob(v.pad, drumrack::Vol))) * v.vel;
        const float pan = pv(kT, p, knob(v.pad, drumrack::Pan));
        const float gl = gain * std::min(1.0f, 1.0f - pan), gr = gain * std::min(1.0f, 1.0f + pan);
        // -60 dB after Decay; all the way up, the whole sample plays
        const float decay = p[knob(v.pad, drumrack::Decay)] >= 0.999f ? 1.0f
                          : std::pow(0.001f, 1.0f / std::max(1.0f, pv(kT, p, knob(v.pad, drumrack::Decay)) * m_sr));
        const float chokeCoef = onePoleCoef(0.004f, m_sr);
        for (int i = 0; i < n; ++i) {
            if (v.pos >= double(frames - 1)) { v.on = false; return; }
            const qint64 k = qint64(v.pos);
            const float f = float(v.pos - double(k));
            const float a = v.env * v.choke;
            L[i] += (s.left[size_t(k)] + (s.left[size_t(k + 1)] - s.left[size_t(k)]) * f) * gl * a;
            R[i] += (s.right[size_t(k)] + (s.right[size_t(k + 1)] - s.right[size_t(k)]) * f) * gr * a;
            v.pos += v.step;
            v.env *= decay;
            if (v.choking) v.choke *= chokeCoef;
            if (a < 1e-5f) { v.on = false; return; }
        }
    }

    float      m_sr;
    Voice      m_voices[32];
    uint32_t   m_age = 0;
    ChainMixer m_mix;   // the pads' chains into the rack's output
};

std::shared_ptr<DeviceProcessor> createInstrument(DeviceType type, float sr) {
    switch (type) {
    case T::DrumRack:       return std::make_shared<DrumRack>(sr);
    case T::DrumKit:        return std::make_shared<DrumKit>(sr, 0);
    case T::Drum808:        return std::make_shared<DrumKit>(sr, 1);
    case T::DrumAcoustic:   return std::make_shared<DrumKit>(sr, 2);
    case T::AnalogSynth:    return std::make_shared<PolySynth<AnalogVoice, AnalogParams, 8>>(sr);
    case T::FMSynth:        return std::make_shared<PolySynth<FmVoice, FmParams, 10>>(sr);
    case T::WavetableSynth: return std::make_shared<WavetableSynth>(sr);
    case T::Monolith:       return createMonolith(sr);
    case T::Osmium:         return createOsmium(sr);
    case T::DrumStudio:     return createDrumStudio(sr);
    case T::Sampler:        return std::make_shared<PolySynth<SamplerVoice, SamplerParams, 8>>(sr);
    case T::Organ:          return std::make_shared<Organ>(sr);
    case T::Bass:           return std::make_shared<Bass>(sr);
    case T::Electric:       return std::make_shared<Electric>(sr);
    default:                return nullptr;
    }
}

} // namespace sessions::audio
