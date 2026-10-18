// Drum Studio: an acoustic kit, synthesized drum by drum (no samples), with six kit voicings,
// per-drum Level / Pan / Tune / Decay / Tone, and three "mic" channels mixed like a drum
// recording: the close mics (each drum straight, panned where it sits), the overheads (every
// drum again, brighter, placed across the stereo field by where it stands on the kit, a hair
// of delay between the sides) and the room (the kit through a small reverb pair, Room Size
// setting its length). Bleed leaks a dull copy of the overheads into the close mics, which
// glues the kit the way real leakage does.
//
// Every drum is a few decaying sine modes (the shell / membrane: pitch sweeping down on a
// kick or a tom), a burst of filtered noise (snare wires, the beater's click, a cymbal's wash)
// and, on the metal, a bank of inharmonic square partials (hats, ride, crash), through a tone
// filter and a soft saturation. Velocity reaches the level, the brightness and the sweep.
// Parameter indices follow core/DeviceSpecs.h (drumstudio::).

#include "DeviceKit.h"

namespace sessions::audio {

using namespace kit;
namespace ds = drumstudio;

namespace {

constexpr int kVoices = 24;

// How one kit voices each drum. Frequencies in Hz, times in seconds, levels 0 .. 1.
struct KitVoicing {
    const char* name;
    // kick: fundamental, how far the pitch starts above it (ratio), the sweep time, the body's decay,
    // the beater click, the shell's low-pass, saturation
    float kickHz, kickSweep, kickSweepT, kickDecay, kickClick, kickLp, kickSat;
    // snare: fundamental, the second mode's ratio and level, the body's decay, the wires' decay,
    // band-pass centre and level
    float snareHz, snareRatio, snareMode2, snareBody, snareWires, snareWireHz, snareWireLvl;
    // hats: the partials' base, closed / pedal / open decays, the high-pass, noise vs metal
    float hatHz, hatClosed, hatPedal, hatOpen, hatHp, hatNoise;
    // toms: the three fundamentals (high, mid, floor), the decay, the sweep ratio
    float tomHz[3], tomDecay, tomSweep;
    // ride: the partials' base, the decay, the wash (noise) level; crash: the decay, the band-pass centre
    float rideHz, rideDecay, rideWash, crashDecay, crashHz;
    // the kit as a whole: the overheads' brightness (high-shelf dB), a room feel
    float ohBright, roomFeel;
};

const KitVoicing kKits[ds::kKits] = {
    //  name           kick: Hz  sweep  sweepT decay  click  lp    sat   snare: Hz ratio m2   body  wires  wireHz lvl    hats: Hz  cl    pd    op    hp    noise  toms: Hz            decay sweep  ride: Hz dec   wash  crash: dec  Hz    oh    room
    {"Studio Rock",       56,   3.2f,  0.045f, 0.42f, 0.55f, 2600, 0.35f,  196,   1.62f, 0.45f, 0.16f, 0.22f, 4200,  0.9f,     1.0f,  0.09f, 0.05f, 0.7f,  6500,  0.5f,  {170, 125, 88},   0.55f, 1.45f,   1.0f,   2.6f, 0.5f,   1.9f,  5200, 3.0f, 0.45f},
    {"Vintage",           50,   2.6f,  0.05f,  0.5f,  0.3f,  1700, 0.5f,   178,   1.55f, 0.35f, 0.2f,  0.19f, 3200,  0.7f,     0.92f, 0.08f, 0.045f,0.55f, 5000,  0.6f,  {150, 110, 78},   0.7f,  1.35f,   0.95f,  2.2f, 0.45f,  1.6f,  4200, 1.0f, 0.55f},
    {"Tight Pop",         62,   3.6f,  0.035f, 0.26f, 0.7f,  3200, 0.3f,   220,   1.7f,  0.5f,  0.1f,  0.14f, 5000,  0.95f,    1.08f, 0.06f, 0.04f, 0.45f, 7500,  0.45f, {190, 140, 100},  0.32f, 1.5f,    1.05f,  2.0f, 0.4f,   1.3f,  6000, 4.0f, 0.3f},
    {"Metal",             58,   4.0f,  0.03f,  0.3f,  0.9f,  4200, 0.45f,  232,   1.75f, 0.55f, 0.14f, 0.2f,  5400,  1.0f,     1.12f, 0.07f, 0.04f, 0.6f,  8000,  0.4f,  {180, 132, 92},   0.45f, 1.5f,    1.08f,  2.4f, 0.55f,  2.3f,  6400, 5.0f, 0.35f},
    {"Jazz",              66,   2.2f,  0.06f,  0.6f,  0.2f,  1500, 0.2f,   208,   1.6f,  0.4f,  0.24f, 0.3f,  3600,  0.6f,     0.9f,  0.11f, 0.06f, 0.9f,  5500,  0.65f, {175, 128, 90},   0.85f, 1.3f,    0.92f,  3.4f, 0.65f,  2.4f,  4800, 2.0f, 0.5f},
    {"Big Room",          52,   3.0f,  0.055f, 0.7f,  0.5f,  2200, 0.4f,   188,   1.58f, 0.45f, 0.26f, 0.28f, 3800,  1.0f,     0.98f, 0.1f,  0.055f,0.8f,  6000,  0.55f, {160, 118, 82},   0.9f,  1.4f,    1.0f,   3.0f, 0.55f,  2.6f,  5000, 2.5f, 0.8f},
};

// Where each drum stands on the kit, for the overheads: -1 (left) .. 1 (right), from the drummer's seat.
const float kOhPan[ds::kDrums] = {0.0f, -0.15f, -0.55f, -0.3f, 0.1f, 0.5f, 0.6f, -0.45f};

struct Voice {
    bool  on = false;
    int   drum = 0, pitch = 0, samples = 0;
    uint32_t age = 0;
    float vel = 1, gain = 1, panL = 1, panR = 1, ohL = 1, ohR = 1;
    // sine modes with a pitch sweep shared by all (a multiplier decaying to 1)
    float ph[3] = {}, freq[3] = {}, env[3] = {}, coef[3] = {}, lvl[3] = {};
    float sweep = 0, sweepCoef = 0;
    // noise through a filter (0 low-pass, 1 band-pass, 2 high-pass), its own envelope
    float nEnv = 0, nCoef = 0, nLvl = 0;
    int   nMode = 1;
    Svf   nFilt;
    // the metal: square partials through a filter, with their envelope
    float mPh[8] = {}, mFreq[8] = {}, mEnv = 0, mCoef = 0, mLvl = 0;
    int   mCount = 0;
    Svf   mFilt;
    bool  mHp = false;
    // the click, the shell's low-pass, saturation
    float click = 0, clickCoef = 0, clickLvl = 0;
    Svf   shell;
    bool  useShell = false;
    float sat = 0;
    // clap: re-triggers, then a tail
    int   bursts = 0, burstTimer = 0, burstGap = 0;
    float tailCoef = 0;
    Noise rng;
};

class DrumStudio : public DeviceProcessor {
public:
    explicit DrumStudio(float sr) : m_sr(sr) {
        for (int c = 0; c < 2; ++c) {
            m_ohDelay[c].init(int(sr * 0.004f) + 8);
            m_pre[c].init(int(sr * 0.03f) + 8);
            for (int k = 0; k < 4; ++k) m_comb[c][k].init(int(sr * 0.09f) + 8);
            m_ap[c][0].init(int(sr * 0.0051f * (c ? 1.07f : 1.0f)));
            m_ap[c][1].init(int(sr * 0.0017f * (c ? 1.11f : 1.0f)));
            m_ohShelf[c].setLowpass(5000, sr);
            m_bleedLp[c].setLowpass(1800, sr);
        }
        uint32_t seed = 0x1234567u;
        for (Voice& v : m_voices) { v.rng.s = seed; seed = seed * 1664525u + 1013904223u; }
    }

    void render(const EventList& ev, float* L, float* R, int frames, const float* p, const Context&) override {
        m_p = p;
        m_kit = std::clamp(pc(T::DrumStudio, p, ds::Kit), 0, ds::kKits - 1);
        std::fill(m_cl, m_cl + frames, 0.0f); std::fill(m_cr, m_cr + frames, 0.0f);
        std::fill(m_ohl, m_ohl + frames, 0.0f); std::fill(m_ohr, m_ohr + frames, 0.0f);
        std::fill(m_room, m_room + frames, 0.0f);
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) trigger(ev.ev[i++]);
            const int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            for (Voice& v : m_voices) if (v.on) renderVoice(v, pos, end - pos);
            pos = end;
        }
        mixMics(L, R, frames, p);
    }
    void panic() override {
        for (Voice& v : m_voices) v.on = false;
        for (int c = 0; c < 2; ++c) { m_ohDelay[c].clear(); m_pre[c].clear(); for (int k = 0; k < 4; ++k) m_comb[c][k].init(int(m_comb[c][k].buf.size())); }
    }

private:
    float decayCoef(float seconds) const { return onePoleCoef(std::max(0.002f, seconds) / 4.6f, m_sr); }   // ~-40 dB after `seconds`
    float knob(int drum, int which) const { return pv(T::DrumStudio, m_p, ds::param(drum, which)); }

    Voice& pickVoice(int pitch) {
        for (Voice& v : m_voices) if (v.on && v.pitch == pitch && v.drum != ds::Crash && v.drum != ds::Ride) return v;   // retrigger the same drum
        for (Voice& v : m_voices) if (!v.on) return v;
        Voice* oldest = &m_voices[0];
        for (Voice& v : m_voices) if (v.age < oldest->age) oldest = &v;
        return *oldest;
    }

    void trigger(const NoteEvent& e) {
        if (e.allOff || e.noteOff) return;   // one-shots ring out
        const int drum = ds::drumFor(e.pitch);
        if (drum < 0) return;
        meter[0].store(float(drum), std::memory_order_relaxed);
        meter[1].store(meter[1].load(std::memory_order_relaxed) + 1.0f, std::memory_order_relaxed);
        const KitVoicing& k = kKits[m_kit];
        const float velCurve = 0.5f + m_p[ds::Velocity] * 1.5f;
        const float vel = std::pow(std::clamp(e.velocity, 0.02f, 1.0f), velCurve);
        if (e.pitch == 42 || e.pitch == 44)   // a closed or pedal hat chokes the open one
            for (Voice& v : m_voices) if (v.on && v.pitch == 46) { v.mCoef = decayCoef(0.015f); v.nCoef = v.mCoef; }
        Voice& v = pickVoice(e.pitch);
        Noise keep = v.rng;
        v = Voice();
        v.rng = keep;
        v.on = true; v.drum = drum; v.pitch = e.pitch; v.vel = vel; v.age = ++m_age;
        const float tune = std::exp2(knob(drum, ds::Tune) / 12.0f);
        const float decay = knob(drum, ds::Decay);
        const float tone = m_p[ds::param(drum, ds::Tone)];   // 0 .. 1
        const float pan = knob(drum, ds::Pan);
        v.gain = dbToGain(knob(drum, ds::Level));
        v.panL = std::min(1.0f, 1.0f - pan); v.panR = std::min(1.0f, 1.0f + pan);
        const float oh = std::clamp(kOhPan[drum] * m_p[ds::Width], -1.0f, 1.0f);
        v.ohL = std::min(1.0f, 1.0f - oh); v.ohR = std::min(1.0f, 1.0f + oh);
        auto mode = [&](int i, float hz, float seconds, float level) { v.freq[i] = hz; v.env[i] = 1; v.coef[i] = decayCoef(seconds); v.lvl[i] = level; };
        auto noise = [&](float seconds, float level, float hz, float q, int filterMode) { v.nEnv = 1; v.nCoef = decayCoef(seconds); v.nLvl = level; v.nFilt.set(hz, q, m_sr); v.nMode = filterMode; };
        auto metal = [&](float base, int count, float seconds, float level, float hp) {
            static const float ratios[8] = {1.0f, 1.483f, 1.932f, 2.546f, 2.63f, 3.897f, 4.72f, 5.57f};
            v.mCount = count;
            for (int i = 0; i < count; ++i) { v.mFreq[i] = base * ratios[i] * tune; v.mPh[i] = v.rng.uniform(); }
            v.mEnv = 1; v.mCoef = decayCoef(seconds); v.mLvl = level;
            v.mFilt.set(hp, 0.7f, m_sr); v.mHp = true;
        };
        switch (drum) {
        case ds::Kick: {
            const float hz = k.kickHz * tune;
            mode(0, hz, k.kickDecay * decay, 1.0f);
            mode(1, hz * 2.02f, k.kickDecay * decay * 0.25f, 0.18f);
            v.sweep = (k.kickSweep - 1) * (0.7f + 0.3f * vel); v.sweepCoef = onePoleCoef(k.kickSweepT, m_sr);
            v.click = 1; v.clickCoef = onePoleCoef(0.0012f, m_sr); v.clickLvl = k.kickClick * (0.3f + 1.4f * m_p[ds::Beater]) * (0.4f + 0.6f * vel) * 0.5f;
            noise(0.02f, 0.08f * vel, 900 + 2500 * tone, 0.8f, 2);   // the beater's slap
            v.useShell = true; v.shell.set(k.kickLp * std::exp2((tone - 0.5f) * 3.0f), 0.8f, m_sr);
            v.sat = k.kickSat;
            v.gain *= 0.72f;
            break;
        }
        case ds::Snare: {
            const float hz = k.snareHz * tune;
            const bool rim = e.pitch == 40, stick = e.pitch == 37, clap = e.pitch == 39;
            if (clap) {
                noise(0.004f, 1.0f, 1300 * std::exp2((tone - 0.5f) * 2), 1.6f, 1);
                v.bursts = 2; v.burstGap = int(0.0095f * m_sr); v.burstTimer = v.burstGap; v.tailCoef = decayCoef(0.22f * decay);
                v.gain *= 0.45f;
                break;
            }
            if (stick) {
                mode(0, 1900 * tune, 0.03f * decay, 0.5f); mode(1, 720 * tune, 0.05f * decay, 0.35f);
                noise(0.012f, 0.6f, 2500 + 3000 * tone, 2.0f, 1);
                v.click = 1; v.clickCoef = onePoleCoef(0.0008f, m_sr); v.clickLvl = 0.3f;
                v.gain *= 0.55f;
                v.sat = 0.2f;
                break;
            }
            mode(0, hz, k.snareBody * decay * (rim ? 0.7f : 1.0f), rim ? 0.6f : 0.85f);
            mode(1, hz * k.snareRatio, k.snareBody * decay * 0.6f, k.snareMode2 * (rim ? 2.0f : 1.0f));
            mode(2, hz * 2.9f, k.snareBody * decay * 0.3f, rim ? 0.4f : 0.15f);
            v.sweep = 0.35f; v.sweepCoef = onePoleCoef(0.008f, m_sr);
            const float wires = k.snareWireLvl * (0.2f + 1.6f * m_p[ds::Wires]) * (0.5f + 0.5f * vel);
            noise(k.snareWires * decay * (rim ? 0.8f : 1.0f), wires, k.snareWireHz * std::exp2((tone - 0.5f) * 2.0f) * (0.8f + 0.4f * vel), 0.55f, 1);
            v.click = 1; v.clickCoef = onePoleCoef(0.0015f, m_sr); v.clickLvl = 0.25f * vel;
            v.sat = 0.3f + (rim ? 0.3f : 0.0f);
            v.gain *= 0.5f;
            break;
        }
        case ds::HiHat: {
            const bool open = e.pitch == 46, pedal = e.pitch == 44;
            const float seconds = (open ? k.hatOpen : pedal ? k.hatPedal : k.hatClosed) * decay * (0.7f + 0.3f * vel);
            const float hp = k.hatHp * std::exp2((tone - 0.5f) * 2.0f) * (pedal ? 0.7f : 1.0f);
            metal(3200 * k.hatHz, 6, seconds, (open ? 0.9f : 1.0f) * (1 - k.hatNoise), hp);
            noise(seconds * 1.1f, k.hatNoise * (open ? 1.0f : 0.9f), hp * 1.3f, 0.6f, 2);
            v.panL *= 1.0f; v.sat = 0.1f;
            v.gain *= (pedal ? 0.35f : 0.42f) * (open ? 0.9f : 1.0f);
            break;
        }
        case ds::Tom1: case ds::Tom2: case ds::Tom3: {
            const int t = drum - ds::Tom1;
            const float hz = k.tomHz[t] * tune;
            const float seconds = k.tomDecay * decay * (t == 2 ? 1.25f : t == 0 ? 0.8f : 1.0f);
            mode(0, hz, seconds, 1.0f);
            mode(1, hz * 1.52f, seconds * 0.5f, 0.3f);
            mode(2, hz * 2.4f, seconds * 0.25f, 0.12f);
            v.sweep = (k.tomSweep - 1) * (0.6f + 0.4f * vel); v.sweepCoef = onePoleCoef(0.04f, m_sr);
            noise(0.025f, 0.2f * vel, 1500 + 2500 * tone, 0.8f, 1);
            v.useShell = true; v.shell.set((1800 + 4000 * tone) * (t == 2 ? 0.8f : 1.0f), 0.7f, m_sr);
            v.sat = 0.25f;
            v.gain *= 0.75f;
            break;
        }
        case ds::Ride: {
            const bool bell = e.pitch == 53, edge = e.pitch == 59;
            const float seconds = k.rideDecay * decay * (bell ? 0.6f : edge ? 1.1f : 1.0f);
            metal((bell ? 1900 : 1150) * k.rideHz, 8, seconds, bell ? 1.0f : 0.8f, (bell ? 1400 : 1000) * std::exp2((tone - 0.5f) * 1.5f));
            noise(seconds * (edge ? 0.9f : 0.5f), k.rideWash * (bell ? 0.25f : edge ? 1.3f : 1.0f), 5500 * std::exp2((tone - 0.5f) * 1.5f), 0.5f, 1);
            v.click = 1; v.clickCoef = onePoleCoef(0.001f, m_sr); v.clickLvl = 0.3f * vel;
            v.gain *= 0.3f; v.sat = 0.15f;
            break;
        }
        case ds::Crash: {
            const bool splash = e.pitch == 55, china = e.pitch == 52;
            const float seconds = k.crashDecay * decay * (splash ? 0.35f : china ? 0.7f : 1.0f);
            const float hz = k.crashHz * std::exp2((tone - 0.5f) * 1.5f) * (splash ? 1.4f : china ? 0.6f : 1.0f);
            noise(seconds, 1.0f, hz, 0.45f, 1);
            metal((china ? 700 : splash ? 2400 : 1600), 8, seconds * 0.8f, china ? 0.7f : 0.35f, hz * 0.4f);
            v.click = 1; v.clickCoef = onePoleCoef(0.002f, m_sr); v.clickLvl = 0.5f * vel;
            v.sat = china ? 0.5f : 0.2f;
            v.gain *= 0.3f;
            break;
        }
        default: break;
        }
    }

    void renderVoice(Voice& v, int at, int n) {
        float* cl = m_cl + at; float* cr = m_cr + at; float* ohl = m_ohl + at; float* ohr = m_ohr + at; float* room = m_room + at;
        const float invSr = 1.0f / m_sr;
        const float satGain = 1 + v.sat * 2, satNorm = 1.0f / std::tanh(satGain * 0.8f) * 0.8f;
        for (int i = 0; i < n; ++i) {
            float s = 0;
            const float sweep = 1 + v.sweep;
            v.sweep *= v.sweepCoef;
            for (int m = 0; m < 3; ++m) {
                if (v.lvl[m] <= 0) continue;
                v.ph[m] += v.freq[m] * sweep * invSr; if (v.ph[m] >= 1) v.ph[m] -= 1;
                s += sinTable(v.ph[m]) * v.env[m] * v.lvl[m];
                v.env[m] *= v.coef[m];
            }
            if (v.nLvl > 0) {
                const float src = v.rng.next();
                const float f = v.nMode == 0 ? v.nFilt.lp(src) : v.nMode == 1 ? v.nFilt.bp(src) : v.nFilt.hp(src);
                s += f * v.nEnv * v.nLvl;
                v.nEnv *= v.nCoef;
                if (v.bursts > 0 && --v.burstTimer <= 0) { v.nEnv = 1; if (--v.bursts == 0) v.nCoef = v.tailCoef; else v.burstTimer = v.burstGap; }
            }
            if (v.mCount > 0) {
                float m = 0;
                for (int k = 0; k < v.mCount; ++k) {
                    v.mPh[k] += v.mFreq[k] * invSr; if (v.mPh[k] >= 1) v.mPh[k] -= 1;
                    m += v.mPh[k] < 0.5f ? 1.0f : -1.0f;
                }
                s += v.mFilt.hp(m * (1.0f / v.mCount)) * v.mEnv * v.mLvl;
                v.mEnv *= v.mCoef;
            }
            if (v.clickLvl > 0) { s += v.rng.next() * v.click * v.clickLvl; v.click *= v.clickCoef; }
            if (v.useShell) s = v.shell.lp(s);
            if (v.sat > 0) s = std::tanh(s * satGain) * satNorm;
            s *= v.vel * v.gain;
            cl[i] += s * v.panL; cr[i] += s * v.panR;
            ohl[i] += s * v.ohL; ohr[i] += s * v.ohR;
            room[i] += s;
            ++v.samples;
        }
        bool tail = false;
        for (int m = 0; m < 3; ++m) tail |= v.lvl[m] > 0 && v.env[m] > 1e-4f;
        tail |= v.nLvl > 0 && v.nEnv > 1e-4f;
        tail |= v.mCount > 0 && v.mEnv > 1e-4f;
        tail |= v.bursts > 0;
        if (!tail && v.samples > 64) v.on = false;
    }

    // The three mic channels into the track: close + overheads (bright, wide, a hair late) + room
    // (pre-delay, four combs and two all-passes a side, their lengths by Room Size) + bleed.
    void mixMics(float* L, float* R, int n, const float* p) {
        const KitVoicing& k = kKits[m_kit];
        const float vol = dbToGain(pv(T::DrumStudio, p, ds::Volume));
        const float ohDb = pv(T::DrumStudio, p, ds::Overheads), roomDb = pv(T::DrumStudio, p, ds::Room);
        const float oh = ohDb <= -59.9f ? 0.0f : dbToGain(ohDb), roomLvl = roomDb <= -59.9f ? 0.0f : dbToGain(roomDb);
        const float size = p[ds::RoomSize], bleed = p[ds::Bleed], width = p[ds::Width];
        const float coef = onePoleCoef(0.01f, m_sr);
        if (std::fabs(size - m_size) > 1e-4f) {
            m_size = size;
            static const float base[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
            for (int c = 0; c < 2; ++c)
                for (int j = 0; j < 4; ++j) m_comb[c][j].setLength(int(m_sr * base[j] * (0.5f + 1.5f * size) * (c ? 1.013f : 1.0f)));
        }
        const float feedback = 0.6f + 0.3f * size, damp = 0.25f + 0.3f * (1 - size);
        const float shelf = dbToGain(k.ohBright);
        const float preDelay = m_sr * (0.006f + 0.016f * size);
        const float ohDelayL = m_sr * 0.0012f * width, ohDelayR = m_sr * 0.0021f * width;
        for (int i = 0; i < n; ++i) {
            // overheads: a bright copy, each side a little late (the far side more)
            m_ohDelay[0].push(m_ohl[i]); m_ohDelay[1].push(m_ohr[i]);
            float ol = m_ohDelay[0].read(1 + ohDelayL), orr = m_ohDelay[1].read(1 + ohDelayR);
            ol = ol + (ol - m_ohShelf[0].lp(ol)) * (shelf - 1);
            orr = orr + (orr - m_ohShelf[1].lp(orr)) * (shelf - 1);
            // bleed: a dull copy of the overheads into the close mics
            const float bl = m_bleedLp[0].lp(ol) * bleed * 0.5f, br = m_bleedLp[1].lp(orr) * bleed * 0.5f;
            // the room: a mono feed, pre-delayed, into a stereo pair of combs
            m_pre[0].push(m_room[i]); m_pre[1].push(m_room[i]);
            float rl = 0, rr = 0;
            const float feed = 0.35f * (1 + k.roomFeel);
            for (int j = 0; j < 4; ++j) {
                rl += m_comb[0][j].process(m_pre[0].read(preDelay) * feed, feedback, damp);
                rr += m_comb[1][j].process(m_pre[1].read(preDelay * 1.07f) * feed, feedback, damp);
            }
            rl = m_ap[0][1].process(m_ap[0][0].process(rl * 0.25f));
            rr = m_ap[1][1].process(m_ap[1][0].process(rr * 0.25f));
            const float g = m_gain.next(vol, coef), go = m_ohGain.next(oh, coef), gr = m_roomGain.next(roomLvl, coef);
            L[i] += ((m_cl[i] + bl) + ol * go + rl * gr) * g;
            R[i] += ((m_cr[i] + br) + orr * go + rr * gr) * g;
        }
        float peak = 0;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(m_room[i]));
        meter[2].store(gainToDb(peak * roomLvl), std::memory_order_relaxed);
    }

    float m_sr;
    const float* m_p = nullptr;
    int m_kit = 0;
    uint32_t m_age = 0;
    Voice m_voices[kVoices];
    float m_cl[kMaxBlock] = {}, m_cr[kMaxBlock] = {}, m_ohl[kMaxBlock] = {}, m_ohr[kMaxBlock] = {}, m_room[kMaxBlock] = {};
    DelayLine m_ohDelay[2], m_pre[2];
    OnePole m_ohShelf[2], m_bleedLp[2];
    Comb m_comb[2][4];
    Allpass m_ap[2][2];
    float m_size = -1;
    Smoothed m_gain, m_ohGain, m_roomGain;
};

} // namespace

std::shared_ptr<DeviceProcessor> createDrumStudio(float sr) { return std::make_shared<DrumStudio>(sr); }

} // namespace sessions::audio
