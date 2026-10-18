// Monolith: the big synth, after NI Massive. The knobs are listed in core/DeviceSpecs.h
// (monolith::). Signal flow, per voice: three wavetable oscillators (each warped by Bend,
// Formant or Sync, phase-modulated and ring-modulated by the modulation oscillator), noise and
// the previous sample fed back -> filter 1 and filter 2 (each oscillator's Filter knob splits it
// between them; Routing feeds filter 1's output into filter 2; F Mix balances the two outputs)
// -> the insert drive -> Env 1 and velocity. Modulation routes (Context::mods) add a share of a
// knob's range to any knob, per voice, every kSub samples: the envelopes, velocity, key and a
// per-note random value are the voice's own, the macros are shared, and the LFOs and steppers
// are shared unless their Retrig is on (then each note restarts its own, and an LFO's Fade ramps
// it in from the note's start). The keyboard's pitch bend (by Bend Range), mod wheel and
// pressure come through Context. Restart puts every oscillator's phase at 0 on a new note.
// 0.20: a sub oscillator (an octave or two under oscillator 1) into filter 1, F Drive (a tanh
// into both filters), the modulation oscillator's wave, one-shot LFOs (per note, held at the
// end: extra envelopes), and the FX after the voices, modulated by the shared sources only
// (macros, the shared LFOs and steppers, the wheel, pressure, bend): an EQ (low and high
// shelves), a Dimension chorus (two modulated delays, the sides a half cycle apart), a synced
// ping-pong delay with a tone in its feedback, and a reverb (four combs and two all-passes a
// side, sized by Reverb Size).
// 0.21: LFO 3 and 4, Macros 5 .. 8, and each envelope's Curve (the level bent logarithmic or
// exponential) and Loop (the attack and decay repeat while the note holds).
// Also 0.21: two drawn curves (Context::curves, points along a synced length; Loop or One-shot,
// Retrig per note or with the song, Smooth rounds the corners) as sources: the phrase tool.
// meter[0] = voices sounding, meter[1] / [2] = LFO 1 / 2 (-1..1); scope[0..3] the newest
// voice's envelope levels, [4..7] their stages (0 idle .. 4 release), [8..9] LFO 1 / 2, [10..11] the steppers,
// [12..13] LFO 3 / 4, [14..15] the curves' phase (0 .. 1 along the curve).

#include "DeviceKit.h"
#include "Wavetables.h"
#include "core/DeviceSpecs.h"
#include "core/ModulationCurve.h"

namespace sessions::audio {

using namespace kit;

float monolithWarpPhase(int mode, float ph, float a) {
    switch (mode) {
    case 1:  return std::min(0.99995f, ph * (1 + 7 * a));                      // Formant: the cycle squeezed to the left, the rest holds
    case 2:  { const float x = ph * (1 + 7 * a); return x - std::floor(x); }   // Sync: the cycle restarts faster than the note
    default: {                                                                 // Bend: the first half squeezed, the second stretched
        const float k = 0.5f - 0.45f * a;
        return ph < k ? ph / k * 0.5f : 0.5f + (ph - k) / (1 - k) * 0.5f;
    }
    }
}

float monolithFilterQ(int type, float reso) {
    if (type == 6) return 0.8f + reso * reso * 10.0f;   // Scream
    if (type == 5) return reso * 0.95f;                  // Comb: its feedback
    return 0.6f + reso * reso * 14.0f;
}

namespace {

using namespace monolith;
constexpr T kType = T::Monolith;
constexpr int kVoices = 8, kMaxUnison = 4, kCombLen = 4096, kCombMask = kCombLen - 1, kSub = 8;

// One of a voice's two filters, stereo. The Comb has its own delay line; Scream drives the
// input into a resonant 24 dB low-pass.
struct VoiceFilter {
    Svf   a[2], b[2];
    float comb[2][kCombLen] = {};
    int   w = 0, type = 0;
    float cutoff = 1000, q = 0.7f, drive = 1, makeup = 1;
    void reset() {
        for (Svf& s : a) s.reset();
        for (Svf& s : b) s.reset();
        for (auto& c : comb) std::fill(c, c + kCombLen, 0.0f);
    }
    void set(int t, float cut, float reso, float sr) {
        type = t;
        cutoff = std::clamp(cut, 20.0f, sr * 0.45f);
        q = monolithFilterQ(type, reso);
        drive = type == 6 ? 1 + 8 * reso : 1;
        makeup = 1.0f / (1 + 0.15f * (drive - 1));
        if (type != 5) for (int c = 0; c < 2; ++c) { a[c].set(cutoff, q, sr); b[c].set(cutoff, 0.707f, sr); }
    }
    float run(int ch, float x, float sr) {   // channel 0 then 1 each sample
        switch (type) {
        case 0:  return a[ch].lp(x);
        case 1:  return b[ch].lp(a[ch].lp(x));
        case 2:  return a[ch].hp(x);
        case 3:  return a[ch].bp(x) * 2.0f;
        case 4:  return a[ch].notch(x);
        case 5: {
            const int d = std::clamp(int(sr / cutoff), 2, kCombLen - 2);
            const float y = x + q * comb[ch][(w - d) & kCombMask];
            comb[ch][w] = y;
            if (ch == 1) w = (w + 1) & kCombMask;
            return y * 0.7f;
        }
        default: return b[ch].lp(a[ch].lp(std::tanh(x * drive))) * makeup;
        }
    }
};

struct Voice {
    bool     on = false;
    int      pitch = 60, gate = 0;
    uint32_t age = 0;
    float    vel = 1, rnd = 0, note = 60, target = 60, ampVel = 1;
    float    ph[kOscs][kMaxUnison] = {}, modPh = 0, noiseZ = 0, fbL = 0, fbR = 0;
    // its own LFOs and steppers (Retrig), from the note's start
    double   startBeat = 0, lfoPh[kLfosAll] = {};
    int64_t  startClock = 0;
    float    lfoVal[kLfosAll] = {}, stepOut[kSteppers] = {};
    float    curveVal[kCurves] = {}, curveZ[kCurves] = {};   // the drawn curves (its own when Retrig), smoothed
    double   curvePh[kCurves] = {};
    float    curve[kEnvs] = {};      // each envelope's Curve, -1 .. 1
    bool     loop[kEnvs] = {};       // and whether it loops while the note holds
    Adsr     env[kEnvs];
    VoiceFilter f[kFilters];
    Noise    rng;
    // worked out every kSub samples from the modulated knobs
    float mp[osmium::kParams] = {};
    double envPhase[kEnvs] = {};
    float envStart[kEnvs] = {}, envDuration[kEnvs][3] = {}, envBend[kEnvs][3] = {};
    int envStage[kEnvs] = {};
    float dt[kOscs][kMaxUnison] = {}, wl[kOscs][kMaxUnison] = {}, wr[kOscs][kMaxUnison] = {};
    int   table[kOscs] = {}, level[kOscs] = {}, mode[kOscs] = {};
    const Wavetable* tbl[kOscs] = {};
    float pos[kOscs] = {}, inten[kOscs] = {}, lvl[kOscs] = {}, fmixO[kOscs] = {};
    float modDt = 0, modPhaseAmt = 0, ring = 0, noiseLvl = 0, noiseCoef = 0, fb = 0, routing = 0, fmix = 0, drive = 1, makeup = 1;
    int   insert = 0, modWave = 0, subWave = 0;
    float subPh = 0, subDt = 0, subLvl = 0, fdrive = 1, fdriveNorm = 1;
    bool  active() const { return on && env[0].active(); }
};

class Monolith : public DeviceProcessor {
public:
    explicit Monolith(float sr, bool osmium = false) : m_sr(sr), m_volCoef(onePoleCoef(0.01f, sr)),
        m_spec(specOf(osmium ? T::Osmium : kType)), m_osmium(osmium), m_params(osmium ? osmium::kParams : kParams) {
        wavetable(0);   // the tables are built here, never on the audio thread
        sinTable(0);
        for (int k = 0; k < kVoices; ++k) {
            for (VoiceFilter& f : m_v[k].f) f.reset();
            if (k) m_v[k].rng.s ^= uint32_t(k) * 0x85ebca6bu;   // each voice its own run of random phases (else two voices start alike)
        }
        for (int c = 0; c < 2; ++c) {
            m_chorus[c].init(int(sr * 0.03f) + 8);
            m_delay[c].init(int(sr * 2.2f) + 8);
            for (int k = 0; k < 4; ++k) m_comb[c][k].init(int(sr * 0.08f) + 8);
            m_ap[c][0].init(int(sr * 0.0051f * (c ? 1.07f : 1.0f)));
            m_ap[c][1].init(int(sr * 0.0017f * (c ? 1.11f : 1.0f)));
        }
    }

    void render(const EventList& ev, float* L, float* R, int frames, const float* p, const Context& ctx) override {
        std::copy(p, p + m_params, m_p);
        m_mods = ctx.mods;
        m_modCount = ctx.mods ? std::min(ctx.modCount, kMaxRoutes) : 0;
        m_ctx = ctx;
        m_voiceMode = rc(p, VoiceMode);
        m_unison = std::clamp(int(std::lround(rv(p, Unison))), 1, kMaxUnison);
        m_glideCoef = onePoleCoef(rv(p, Glide) / 3.0f, m_sr);
        const float vol = dbToGain(rv(p, Volume));
        std::fill(m_fxL, m_fxL + frames, 0.0f);
        std::fill(m_fxR, m_fxR + frames, 0.0f);
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) handle(ev.ev[i++], pos);
            const int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            renderRange(pos, end);
            pos = end;
        }
        globalMods();
        processFx(frames);
        for (int k = 0; k < frames; ++k) {
            const float vg = m_vol.next(vol, m_volCoef);
            L[k] += m_fxL[k] * vg;
            R[k] += m_fxR[k] * vg;
        }
        m_clock += frames;
        int sounding = 0;
        const Voice* newest = nullptr;
        for (const Voice& v : m_v) if (v.active()) { ++sounding; if (!newest || v.age > newest->age) newest = &v; }
        meter[0].store(float(sounding), std::memory_order_relaxed);
        meter[1].store(m_lfo[0], std::memory_order_relaxed);
        meter[2].store(m_lfo[1], std::memory_order_relaxed);
        for (int e = 0; e < kEnvs; ++e) {
            scope[e].store(newest ? newest->env[e].level : 0.0f, std::memory_order_relaxed);
            scope[4 + e].store(newest ? float(int(newest->env[e].stage)) : 0.0f, std::memory_order_relaxed);
        }
        scope[8].store(newest ? newest->lfoVal[0] : m_lfo[0], std::memory_order_relaxed);
        scope[9].store(newest ? newest->lfoVal[1] : m_lfo[1], std::memory_order_relaxed);
        scope[10].store(newest ? newest->stepOut[0] : m_stepOut[0], std::memory_order_relaxed);
        scope[11].store(newest ? newest->stepOut[1] : m_stepOut[1], std::memory_order_relaxed);
        scope[12].store(newest ? newest->lfoVal[2] : m_lfo[2], std::memory_order_relaxed);
        scope[13].store(newest ? newest->lfoVal[3] : m_lfo[3], std::memory_order_relaxed);
        for (int c = 0; c < kCurves; ++c) scope[14 + c].store(float(newest && m_p[cparam(c, CRetrig)] >= 0.5f ? newest->curvePh[c] : m_curvePh[c]), std::memory_order_relaxed);
    }

    void panic() override {
        for (Voice& v : m_v) { v.on = false; for (Adsr& e : v.env) e.kill(); for (VoiceFilter& f : v.f) f.reset(); v.fbL = v.fbR = 0; }
        m_count = 0;
        for (int c = 0; c < 2; ++c) {
            m_chorus[c].clear(); m_delay[c].clear();
            for (int k = 0; k < 4; ++k) m_comb[c][k].init(int(m_comb[c][k].buf.size()));
            m_ap[c][0].init(int(m_ap[c][0].buf.size())); m_ap[c][1].init(int(m_ap[c][1].buf.size()));
            m_eqLow[c].reset(); m_eqHigh[c].reset();
            m_delayZ[c] = 0;
        }
    }

private:
    struct Held { int pitch; float vel; int64_t end; };

    void release(Voice& v) { v.gate = 0; for (Adsr& e : v.env) e.off(); }
    void remove(int k) { for (int j = k; j + 1 < m_count; ++j) m_stack[j] = m_stack[j + 1]; --m_count; }
    void follow() {   // mono: back to the newest note still held, or let go
        if (m_count > 0) { m_v[0].target = float(m_stack[m_count - 1].pitch); m_v[0].pitch = m_stack[m_count - 1].pitch; }
        else release(m_v[0]);
    }

    void start(Voice& v, int pitch, float vel, int dur, bool retrigger, bool glide, int pos) {
        const bool fresh = !v.active();
        v.age = ++m_age;
        v.pitch = pitch;
        v.gate = dur;
        v.target = float(pitch);
        if (!glide || fresh) v.note = float(pitch);
        if (fresh) {
            const bool restart = m_p[OscRestart] >= 0.5f;
            for (auto& o : v.ph) for (float& x : o) x = restart ? 0.0f : v.rng.uniform() * 0.999f;
            v.modPh = 0; v.fbL = v.fbR = 0; v.noiseZ = 0;
            for (VoiceFilter& f : v.f) f.reset();
        }
        if (retrigger || fresh) {
            v.vel = vel;
            v.rnd = v.rng.uniform() * 2 - 1;
            for (Adsr& e : v.env) e.on();
            std::fill(std::begin(v.envStage), std::end(v.envStage), int(Adsr::Idle));
            v.startBeat = m_ctx.beat + pos * m_ctx.beatsPerSample;   // its own LFOs and steppers count from here
            v.startClock = m_clock + pos;
            for (double& ph : v.lfoPh) ph = 0;
            for (int s = 0; s < kSteppers; ++s) v.stepOut[s] = (m_p[step(s, 0)] - 0.5f) * 2;
        }
        v.on = true;
    }

    void handle(const NoteEvent& e, int pos) {
        const int64_t now = m_clock + pos;
        if (e.allOff) { for (Voice& v : m_v) release(v); m_count = 0; return; }
        if (e.noteOff) {
            if (m_voiceMode == 0) { for (Voice& v : m_v) if (v.on && v.pitch == e.pitch && v.gate > 0) release(v); }
            else { for (int k = 0; k < m_count; ++k) if (m_stack[k].pitch == e.pitch) { remove(k); break; } follow(); }
            return;
        }
        if (m_voiceMode == 0) {   // poly: a free voice, else the oldest
            Voice* pick = nullptr;
            for (Voice& v : m_v) if (!v.active()) { pick = &v; break; }
            if (!pick) { pick = &m_v[0]; for (Voice& v : m_v) if (v.age < pick->age) pick = &v; }
            start(*pick, e.pitch, e.velocity, std::max(1, e.duration), true, false, pos);
            return;
        }
        // mono / legato: one voice and a stack of held notes (their ends are watched in renderRange)
        for (int k = 0; k < m_count; ++k) if (m_stack[k].end <= now) remove(k--);
        const bool held = m_count > 0 && m_v[0].active();
        for (int k = 0; k < m_count; ++k) if (m_stack[k].pitch == e.pitch) { remove(k); break; }
        if (m_count == 16) remove(0);
        m_stack[m_count++] = {e.pitch, e.velocity, now + std::max(1, e.duration)};
        start(m_v[0], e.pitch, e.velocity, kHeld, !held || m_voiceMode == 1, held, pos);
    }

    float source(int s, const Voice& v) const {
        switch (s) {
        case SrcEnv1: case SrcEnv2: case SrcEnv3: case SrcEnv4: {
            const int e = s - SrcEnv1;
            return m_osmium ? curved(v.env[e].level, v.curve[e]) : v.env[e].level;
        }
        case SrcLfo1: case SrcLfo2: case SrcLfo3: case SrcLfo4: return v.lfoVal[lfoOfSource(s)];
        case SrcStep1: case SrcStep2: return v.stepOut[s - SrcStep1];
        case SrcMacro1: case SrcMacro2: case SrcMacro3: case SrcMacro4: case SrcMacro5: case SrcMacro6: case SrcMacro7: case SrcMacro8: return m_p[macro(macroOfSource(s))];
        case SrcCurve1: case SrcCurve2: return v.curveVal[s - SrcCurve1];
        case SrcVelocity: return v.vel;
        case SrcKey: return std::clamp((v.note - 60.0f) / 36.0f, -1.0f, 1.0f);
        case SrcRandom: return v.rnd;
        case SrcWheel: return m_ctx.wheel;
        case SrcPressure: return m_ctx.pressure;
        case SrcBend: return m_ctx.bend;
        default: return 0;
        }
    }

    // A stepper's value at `beat` counted from `origin`, glided from `out` (every kSub samples).
    float stepValue(int s, double beat, double origin, float out) const {
        const float beats = std::max(0.0625f, rv(m_p, sparam(s, SRate)));
        const int count = std::clamp(int(std::lround(rv(m_p, sparam(s, SCount)))), 1, kSteps);
        const long idx = long(std::floor((beat - origin) / beats));
        const int cur = int(((idx % count) + count) % count);
        const float target = (m_p[step(s, cur)] - 0.5f) * 2;
        const float glide = m_p[sparam(s, SGlide)];
        if (glide < 0.01f) return target;
        const float stepSec = beats * 60.0f / float(std::max(1.0, m_ctx.tempo));
        return target + (out - target) * std::exp(-float(kSub) / (glide * stepSec * 0.5f * m_sr));
    }

    // A drawn curve's value at `phase` (0 .. 1 along it): linear between its points, flat before the
    // first and after the last; 0 without points.
    float curveAt(int c, double phase) const {
        const int n = m_ctx.curveCount ? m_ctx.curveCount[c] : 0;
        if (n <= 0 || !m_ctx.curves) return 0;
        const CurvePoint* pts = m_ctx.curves + c * m_ctx.curveStride;
        return float(modcurve::value(pts, n, phase));
    }
    double curvePhase(int c, double beat, double origin) const {
        double beats = rv(m_p, cparam(c, CRate));
        if (beats <= 0) beats = 4;   // "Free" makes no sense for a phrase: a bar
        double ph = (beat - origin) / beats;
        if (rc(m_p, cparam(c, CMode)) == 1) return std::clamp(ph, 0.0, 0.99999);   // one-shot: holds its end
        ph -= std::floor(ph);
        return ph < 0 ? 0 : ph;
    }
    float curveSmooth(int c, float target, float& z) const {   // Smooth: a one-pole over the kSub steps
        const float s = m_p[cparam(c, CSmooth)];
        if (s < 0.005f) { z = target; return z; }
        const float tau = 0.25f * s * s;
        z += (target - z) * (1 - std::exp(-float(kSub) / (tau * m_sr)));
        return z;
    }

    // A voice's LFOs and steppers for this kSub window: the shared ones, or its own when Retrig is
    // on (counting from the note's start); an LFO's Fade ramps it in over its first seconds.
    void voiceSources(Voice& v) {
        const float age = float(m_nowClock - v.startClock) / m_sr;
        for (int c = 0; c < kCurves; ++c) {
            if (m_p[cparam(c, CRetrig)] >= 0.5f) {
                v.curvePh[c] = curvePhase(c, m_nowBeat, v.startBeat);
                v.curveVal[c] = curveSmooth(c, curveAt(c, v.curvePh[c]), v.curveZ[c]);
            } else { v.curvePh[c] = m_curvePh[c]; v.curveVal[c] = m_curveVal[c]; }
        }
        for (int l = 0; l < kLfosAll; ++l) {
            float raw = m_lfo[l];
            const bool oneShot = rc(m_p, lmode(l)) == 1;   // runs once from the note and holds its end: an extra envelope
            if (m_p[lretrig(l)] >= 0.5f || oneShot) {
                const float beats = rv(m_p, lparam(l, LRate));
                double phase;
                if (beats > 0) phase = (m_nowBeat - v.startBeat) / beats;
                else {
                    v.lfoPh[l] += double(rv(m_p, lparam(l, LHz))) / m_sr * kSub;
                    if (v.lfoPh[l] >= 1e6) v.lfoPh[l] -= 1e6;
                    phase = v.lfoPh[l];
                }
                if (oneShot) phase = std::min(phase, 0.99999);
                raw = lfoShape(rc(m_p, lparam(l, LShape)), std::max(0.0, phase));
            }
            const float fade = rv(m_p, lfade(l));
            v.lfoVal[l] = fade > 0.002f ? raw * std::min(1.0f, age / fade) : raw;
        }
        for (int s = 0; s < kSteppers; ++s)
            v.stepOut[s] = m_p[sretrig(s)] >= 0.5f ? stepValue(s, m_nowBeat, v.startBeat, v.stepOut[s]) : m_stepOut[s];
    }

    // The LFOs and steppers at sample `i` of the block (synced to the beat, or free-running).
    void updateGlobals(int i) {
        const double beat = m_ctx.beat + i * m_ctx.beatsPerSample;
        for (int l = 0; l < kLfosAll; ++l) {
            const float beats = rv(m_p, lparam(l, LRate));
            double phase;
            if (beats > 0) phase = beat / beats;
            else {
                m_lfoPh[l] += double(rv(m_p, lparam(l, LHz))) / m_sr * kSub;
                if (m_lfoPh[l] >= 1e6) m_lfoPh[l] -= 1e6;
                phase = m_lfoPh[l];
            }
            m_lfo[l] = lfoShape(rc(m_p, lparam(l, LShape)), phase);
        }
        for (int s = 0; s < kSteppers; ++s) m_stepOut[s] = stepValue(s, beat, 0.0, m_stepOut[s]);
        for (int c = 0; c < kCurves; ++c) {
            m_curvePh[c] = curvePhase(c, beat, 0.0);
            m_curveVal[c] = curveSmooth(c, curveAt(c, m_curvePh[c]), m_curveZ[c]);
        }
    }

    // A voice's knobs with its modulation applied, and everything derived from them.
    void modulate(Voice& v) {
        std::copy(m_p, m_p + m_params, v.mp);
        voiceSources(v);
        for (int r = 0; r < m_modCount; ++r) {
            const ModRoute& m = m_mods[r];
            if (m.param < 0 || m.param >= m_params) continue;
            v.mp[m.param] = std::clamp(v.mp[m.param] + float(m.amount) * source(m.source, v), 0.0f, 1.0f);
        }
        const float* p = v.mp;
        const float detune = rv(p, Detune), spread = p[Spread];
        const float bend = m_ctx.bend * rv(p, BendRange);   // the keyboard's pitch bend, by the Bend Range
        static const float offs1[] = {0}, offs2[] = {-1, 1}, offs3[] = {-1, 0, 1}, offs4[] = {-1, -0.33f, 0.33f, 1};
        const float* offs = m_unison == 4 ? offs4 : m_unison == 3 ? offs3 : m_unison == 2 ? offs2 : offs1;
        const float norm = 1.0f / std::sqrt(float(m_unison));
        for (int o = 0; o < kOscs; ++o) {
            const float semis = rv(p, oparam(o, Pitch)) + rv(p, oparam(o, Fine)) / 100.0f + bend;
            const float freq = mtof(v.note + semis);
            v.table[o] = rc(p, oparam(o, Table));
            v.tbl[o] = &wavetable(v.table[o]);
            v.mode[o] = rc(p, oparam(o, Mode));
            v.inten[o] = p[oparam(o, Intensity)];
            v.pos[o] = p[oparam(o, Position)];
            v.lvl[o] = p[oparam(o, Level)];
            v.fmixO[o] = p[oparam(o, FilterMix)];
            // warping reads the table faster: pick the band limit for that speed
            const float speed = v.mode[o] == 0 ? 1 + 3 * v.inten[o] : 1 + 7 * v.inten[o];
            v.level[o] = wavetableLevel(freq * speed * std::exp2(detune / 1200.0f), m_sr);
            for (int u = 0; u < m_unison; ++u) {
                v.dt[o][u] = freq * std::exp2(offs[u] * detune / 1200.0f) / m_sr;
                v.wl[o][u] = 0.5f * (1 - spread * offs[u]) * norm;
                v.wr[o][u] = 0.5f * (1 + spread * offs[u]) * norm;
            }
        }
        v.modDt = mtof(v.note + rv(p, ModPitch) + bend) / m_sr;
        v.modWave = rc(p, ModOscWave);
        v.subLvl = p[SubLevel];
        v.subWave = rc(p, SubWave);
        v.subDt = mtof(v.note + bend - 12.0f * float(rc(p, SubOct) + 1)) / m_sr;
        v.fdrive = 1 + 5 * p[FDrive];
        v.fdriveNorm = 1.0f / std::sqrt(v.fdrive);
        v.modPhaseAmt = p[ModPhase];
        v.ring = p[ModRing];
        v.noiseLvl = p[NoiseLevel];
        v.noiseCoef = std::exp(-kTwoPi * 100.0f * std::exp2(p[NoiseColor] * 7.6f) / m_sr);
        v.fb = p[Feedback] * 0.9f;
        const float keyOct = (v.note - 60.0f) / 12.0f * p[KeyTrack];
        for (int f = 0; f < kFilters; ++f)
            v.f[f].set(rc(p, fparam(f, FType)), rv(p, fparam(f, FCutoff)) * std::exp2(keyOct), p[fparam(f, FReso)], m_sr);
        v.routing = p[FRouting];
        v.fmix = p[FMix];
        v.insert = rc(p, InsertType);
        v.drive = 1 + 11 * p[InsertAmount];
        v.makeup = 1.0f / std::pow(v.drive, 0.6f);
        for (int e = 0; e < kEnvs; ++e) {
            v.env[e].set(rv(p, eparam(e, A)), rv(p, eparam(e, D)), p[eparam(e, S)], rv(p, eparam(e, R)), m_sr);
            v.curve[e] = (p[ecurve(e)] - 0.5f) * 2;
            v.loop[e] = p[eloop(e)] >= 0.5f;
            if (m_osmium) for (int s = 0; s < 3; ++s) {
                v.envDuration[e][s] = rv(p, eparam(e, s == 0 ? A : s == 1 ? D : R));
                v.envBend[e][s] = (p[osmium::bend(e, s)] - 0.5f) * 2;
            }
        }
        v.ampVel = 1 - p[VelAmp] + p[VelAmp] * v.vel;
    }

    // An envelope's level bent by its Curve: +1 exponential (slow start, quick end), -1 logarithmic.
    static float curved(float x, float c) {
        if (std::fabs(c) < 0.01f) return x;
        return c > 0 ? std::pow(x, 1 + 3 * c) : 1 - std::pow(1 - x, 1 - 3 * c);
    }

    float osmiumEnvelope(Voice& v, int k) {
        Adsr& a = v.env[k];
        if (v.envStage[k] != int(a.stage)) {
            v.envStage[k] = int(a.stage);
            v.envPhase[k] = 0;
            v.envStart[k] = a.level;
        }
        if (a.stage == Adsr::Idle) return 0;
        if (a.stage == Adsr::Sustain) {
            a.level = a.sustain;
            if (v.loop[k] && v.gate != 0) a.stage = Adsr::Attack;
            return a.level;
        }
        const int s = a.stage == Adsr::Attack ? 0 : a.stage == Adsr::Decay ? 1 : 2;
        v.envPhase[k] = std::min(1.0, v.envPhase[k] + 1.0 / std::max(1.0f, v.envDuration[k][s] * m_sr));
        const float target = s == 0 ? 1 : s == 1 ? a.sustain : 0;
        a.level = v.envStart[k] + (target - v.envStart[k]) * float(modcurve::bend(v.envPhase[k], v.envBend[k][s]));
        if (v.envPhase[k] >= 1) {
            a.level = target;
            a.stage = s == 0 ? Adsr::Decay : s == 1 ? Adsr::Sustain : Adsr::Idle;
            if (s == 1 && target == 0 && !v.loop[k]) a.stage = Adsr::Idle;
        }
        return curved(a.level, v.curve[k]);
    }

    static float shapeAt(int wave, float ph) {   // the modulation oscillator's and the sub's waves
        switch (wave) {
        case 1:  return 4 * std::fabs(ph - 0.5f) - 1;
        case 2:  return 2 * ph - 1;
        case 3:  return ph < 0.5f ? 1.0f : -1.0f;
        default: return sinTable(ph);
        }
    }

    void renderRange(int from, int to) {
        for (int i = from; i < to; ++i) {
            if (i % kSub == 0 || i == from) { m_nowBeat = m_ctx.beat + i * m_ctx.beatsPerSample; m_nowClock = m_clock + i; }
            if (i % kSub == 0) updateGlobals(i);
            if (m_voiceMode != 0 && m_count > 0) {   // mono: notes whose time is up leave the stack
                const int64_t now = m_clock + i;
                bool expired = false;
                for (int k = 0; k < m_count; ++k) if (m_stack[k].end <= now) { remove(k--); expired = true; }
                if (expired) follow();
            }
            float outL = 0, outR = 0;
            for (Voice& v : m_v) {
                if (!v.active()) { v.on = false; continue; }
                if (v.gate > 0 && v.gate != kHeld && --v.gate == 0) release(v);
                if (m_voiceMode != 0) v.note += (v.target - v.note) * (1 - m_glideCoef); else v.note = v.target;
                if (i % kSub == 0 || i == from) modulate(v);
                float e[kEnvs];
                for (int k = 0; k < kEnvs; ++k) {
                    Adsr& a = v.env[k];
                    if (m_osmium) e[k] = osmiumEnvelope(v, k);
                    else {
                        if (v.loop[k] && v.gate != 0 && (a.stage == Adsr::Sustain || (a.stage == Adsr::Decay && a.level - a.sustain < 2e-4f))) a.stage = Adsr::Attack;
                        e[k] = curved(a.next(), v.curve[k]);
                    }
                }
                const float m = shapeAt(v.modWave, v.modPh);
                v.modPh += v.modDt; if (v.modPh >= 1) v.modPh -= 1;
                float in1L = 0, in1R = 0, in2L = 0, in2R = 0;
                for (int o = 0; o < kOscs; ++o) {
                    float sL = 0, sR = 0;
                    const Wavetable& t = *v.tbl[o];
                    for (int u = 0; u < m_unison; ++u) {
                        float ph = v.ph[o][u] + v.modPhaseAmt * m * 0.5f;
                        ph -= std::floor(ph);
                        const float s = v.lvl[o] > 0.0005f ? wavetableRead(t, v.level[o], v.pos[o], monolithWarpPhase(v.mode[o], ph, v.inten[o])) : 0.0f;
                        sL += s * v.wl[o][u];
                        sR += s * v.wr[o][u];
                        v.ph[o][u] += v.dt[o][u]; if (v.ph[o][u] >= 1) v.ph[o][u] -= 1;
                    }
                    const float g = v.lvl[o] * (1 - v.ring + v.ring * m);   // ring modulation
                    sL *= g; sR *= g;
                    in1L += sL * (1 - v.fmixO[o]); in1R += sR * (1 - v.fmixO[o]);
                    in2L += sL * v.fmixO[o];       in2R += sR * v.fmixO[o];
                }
                if (v.noiseLvl > 0) {
                    const float wn = v.rng.next();
                    v.noiseZ = wn + (v.noiseZ - wn) * v.noiseCoef;
                    const float nz = v.noiseZ * v.noiseLvl * 0.4f;
                    in1L += nz; in1R += nz; in2L += nz; in2R += nz;
                }
                if (v.subLvl > 0.0005f) {   // the sub, straight into filter 1
                    const float sub = shapeAt(v.subWave == 1 ? 3 : v.subWave == 2 ? 1 : 0, v.subPh) * v.subLvl * 0.7f;
                    v.subPh += v.subDt; if (v.subPh >= 1) v.subPh -= 1;
                    in1L += sub; in1R += sub;
                }
                in1L += v.fbL * v.fb; in1R += v.fbR * v.fb;
                if (v.fdrive > 1.01f) {   // drive into both filters
                    in1L = std::tanh(in1L * v.fdrive) * v.fdriveNorm; in1R = std::tanh(in1R * v.fdrive) * v.fdriveNorm;
                    in2L = std::tanh(in2L * v.fdrive) * v.fdriveNorm; in2R = std::tanh(in2R * v.fdrive) * v.fdriveNorm;
                }
                const float f1L = v.f[0].run(0, in1L, m_sr), f1R = v.f[0].run(1, in1R, m_sr);
                const float f2L = v.f[1].run(0, in2L + f1L * v.routing, m_sr), f2R = v.f[1].run(1, in2R + f1R * v.routing, m_sr);
                float yL = f1L * (1 - v.fmix) + f2L * v.fmix, yR = f1R * (1 - v.fmix) + f2R * v.fmix;
                if (v.insert > 0) { yL = saturate(v.insert - 1, yL * v.drive) * v.makeup; yR = saturate(v.insert - 1, yR * v.drive) * v.makeup; }
                v.fbL = std::clamp(yL, -1.0f, 1.0f);
                v.fbR = std::clamp(yR, -1.0f, 1.0f);
                const float g = e[0] * v.ampVel * 0.3f;
                outL += yL * g;
                outR += yR * g;
            }
            m_fxL[i] += outL;
            m_fxR[i] += outR;
        }
    }

    // The FX knobs with the shared sources' routes applied (macros, the shared LFOs and
    // steppers, the wheel, pressure and bend; a voice's own sources count nothing here).
    void globalMods() {
        std::copy(m_p + kFxBase, m_p + kParams, m_gp + kFxBase);
        for (int r = 0; r < m_modCount; ++r) {
            const ModRoute& m = m_mods[r];
            if (m.param < kFxBase || m.param >= kParams) continue;
            float src = 0;
            switch (m.source) {
            case SrcLfo1: case SrcLfo2: case SrcLfo3: case SrcLfo4: src = m_lfo[lfoOfSource(m.source)]; break;
            case SrcStep1: case SrcStep2: src = m_stepOut[m.source - SrcStep1]; break;
            case SrcMacro1: case SrcMacro2: case SrcMacro3: case SrcMacro4: case SrcMacro5: case SrcMacro6: case SrcMacro7: case SrcMacro8: src = m_p[macro(macroOfSource(m.source))]; break;
            case SrcCurve1: case SrcCurve2: src = m_curveVal[m.source - SrcCurve1]; break;
            case SrcWheel: src = m_ctx.wheel; break;
            case SrcPressure: src = m_ctx.pressure; break;
            case SrcBend: src = m_ctx.bend; break;
            default: break;
            }
            m_gp[m.param] = std::clamp(m_gp[m.param] + float(m.amount) * src, 0.0f, 1.0f);
        }
    }

    // EQ -> chorus -> delay -> reverb over the block, each skipped when it's off.
    void processFx(int n) {
        const float* p = m_gp;
        const float low = rv(p, fx(EqLow)), high = rv(p, fx(EqHigh));
        if (std::fabs(low - m_eqLowDb) > 1e-4f || std::fabs(high - m_eqHighDb) > 1e-4f) {
            m_eqLowDb = low; m_eqHighDb = high;
            for (int c = 0; c < 2; ++c) { m_eqLow[c].lowShelf(120, low, m_sr); m_eqHigh[c].highShelf(6000, high, m_sr); }
        }
        const bool eq = std::fabs(low) > 0.05f || std::fabs(high) > 0.05f;
        const float chMix = p[fx(ChorusMix)], chDepth = p[fx(ChorusDepth)], chRate = rv(p, fx(ChorusRate));
        const float dlMix = p[fx(DelayMix)], dlFb = p[fx(DelayFeedback)] * 0.95f, dlBeats = rv(p, fx(DelayTime));
        const float rvMix = p[fx(ReverbMix)], rvSize = p[fx(ReverbSize)];
        const float dlSamples = std::clamp(dlBeats * 60.0f / float(std::max(20.0, m_ctx.tempo)) * m_sr, 1.0f, m_sr * 2.0f);
        m_delayTone.setLowpass(rv(p, fx(DelayTone)), m_sr);
        m_delayTone2.setLowpass(rv(p, fx(DelayTone)), m_sr);
        if (std::fabs(rvSize - m_rvSize) > 1e-4f) {
            m_rvSize = rvSize;
            static const float base[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
            for (int c = 0; c < 2; ++c) for (int j = 0; j < 4; ++j) m_comb[c][j].setLength(int(m_sr * base[j] * (0.6f + 1.2f * rvSize) * (c ? 1.013f : 1.0f)));
        }
        const float rvFb = 0.7f + 0.22f * rvSize, rvDamp = 0.2f + 0.3f * (1 - rvSize);
        float* io[2] = {m_fxL, m_fxR};
        for (int i = 0; i < n; ++i) {
            float x[2] = {io[0][i], io[1][i]};
            if (eq) for (int c = 0; c < 2; ++c) x[c] = m_eqHigh[c].process(m_eqLow[c].process(x[c]));
            if (chMix > 0.001f) {
                m_chorusPh += chRate / m_sr; if (m_chorusPh >= 1) m_chorusPh -= 1;
                for (int c = 0; c < 2; ++c) {
                    const float ph = m_chorusPh + (c ? 0.5f : 0.0f);
                    const float d = m_sr * (0.007f + 0.006f * chDepth * (0.5f + 0.5f * sinTable(ph - std::floor(ph))));
                    m_chorus[c].push(x[c]);
                    x[c] = x[c] * (1 - chMix * 0.5f) + m_chorus[c].read(d) * chMix * 0.7f;
                }
            }
            if (dlMix > 0.001f) {
                const float dl = m_delay[0].read(dlSamples), dr = m_delay[1].read(dlSamples);
                m_delayZ[0] = m_delayTone.lp(dr * dlFb);   // ping-pong: each side feeds the other
                m_delayZ[1] = m_delayTone2.lp(dl * dlFb);
                m_delay[0].push(x[0] + m_delayZ[0]);
                m_delay[1].push(x[1] * 0.3f + m_delayZ[1]);
                x[0] += dl * dlMix; x[1] += dr * dlMix;
            } else { m_delay[0].push(0); m_delay[1].push(0); }
            if (rvMix > 0.001f) {
                const float in = (x[0] + x[1]) * 0.25f;
                for (int c = 0; c < 2; ++c) {
                    float r = 0;
                    for (int j = 0; j < 4; ++j) r += m_comb[c][j].process(in, rvFb, rvDamp);
                    r = m_ap[c][1].process(m_ap[c][0].process(r * 0.25f));
                    x[c] = x[c] * (1 - rvMix * 0.5f) + r * rvMix;
                }
            }
            io[0][i] = x[0]; io[1][i] = x[1];
        }
    }

    // the knobs in real units, from the spec kept at hand (specOf() searches every device: never per sample)
    float rv(const float* p, int i) const { return float(paramValue(m_spec->params[size_t(i)], p[i])); }
    int   rc(const float* p, int i) const { return choiceIndex(m_spec->params[size_t(i)], p[i]); }

    float   m_sr, m_volCoef;
    const DeviceSpec* m_spec;
    bool m_osmium;
    int m_params;
    Smoothed m_vol;
    Voice   m_v[kVoices];
    Held    m_stack[16]; int m_count = 0;
    int64_t m_clock = 0;
    uint32_t m_age = 0;
    float   m_p[osmium::kParams] = {};
    const ModRoute* m_mods = nullptr; int m_modCount = 0;
    Context m_ctx;
    int     m_voiceMode = 0, m_unison = 1;
    float   m_glideCoef = 0;
    double  m_lfoPh[kLfosAll] = {}, m_curvePh[kCurves] = {};
    float   m_lfo[kLfosAll] = {}, m_stepOut[kSteppers] = {}, m_curveVal[kCurves] = {}, m_curveZ[kCurves] = {};
    double  m_nowBeat = 0;       // the beat and sample at the current kSub window (for the voices' own LFOs and steppers)
    int64_t m_nowClock = 0;
    // the FX after the voices
    float   m_gp[kParams] = {};  // the knobs with the shared sources' routes applied (FX only)
    float   m_fxL[kMaxBlock] = {}, m_fxR[kMaxBlock] = {};
    Biquad  m_eqLow[2], m_eqHigh[2];
    float   m_eqLowDb = 1e9f, m_eqHighDb = 1e9f;
    DelayLine m_chorus[2], m_delay[2];
    float   m_chorusPh = 0, m_delayZ[2] = {};
    OnePole m_delayTone, m_delayTone2;
    Comb    m_comb[2][4];
    Allpass m_ap[2][2];
    float   m_rvSize = -1;
};

} // namespace

std::shared_ptr<DeviceProcessor> createMonolith(float sr) { return std::make_shared<Monolith>(sr); }
std::shared_ptr<DeviceProcessor> createOsmium(float sr) { return std::make_shared<Monolith>(sr, true); }

} // namespace sessions::audio
