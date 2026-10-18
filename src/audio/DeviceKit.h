#pragma once
// Private to the device processors (Instruments.cpp, Effects.cpp, Devices.cpp): parameter
// helpers and the polyphonic voice scaffold.

#include "Devices.h"
#include "Dsp.h"
#include <algorithm>

namespace sessions::audio {

std::shared_ptr<DeviceProcessor> createInstrument(DeviceType type, float sampleRate);   // nullptr if not one
std::shared_ptr<DeviceProcessor> createEffect(DeviceType type, float sampleRate);
std::shared_ptr<DeviceProcessor> createGuitar(DeviceType type, float sampleRate);   // Guitar.cpp
std::shared_ptr<DeviceProcessor> createMonolith(float sampleRate);                   // Monolith.cpp
std::shared_ptr<DeviceProcessor> createNam(float sampleRate);                        // Nam.cpp
std::shared_ptr<DeviceProcessor> createDrumStudio(float sampleRate);                 // DrumStudio.cpp

namespace kit {

using namespace dsp;
using T = DeviceType;

inline float pv(T type, const float* p, int i) { return float(paramValue(type, i, p[i])); }   // in real units
inline int   pc(T type, const float* p, int i) { return choiceIndex(type, i, p[i]); }          // a choice's index
inline bool  pb(const float* p, int i) { return p[i] >= 0.5f; }                                   // a toggle

// One wave of the Analog / Bass oscillators, advancing the phase: 0 saw, 1 square, 2 triangle, 3 sine.
inline float wave(Osc& o, int shape, float dt) {
    float v;
    switch (shape) {
    case 1:  v = o.squareAt(dt); break;
    case 2:  v = o.triAt(); break;
    case 3:  v = o.sineAt(); break;
    default: v = o.sawAt(dt); break;
    }
    o.advance(dt);
    return v;
}

// A gain that glides to its target over ~10 ms, so knob moves don't click.
struct Smoothed {
    float value = 0; bool primed = false;
    float next(float target, float coef) {
        if (!primed) { value = target; primed = true; }
        value = target + (value - target) * coef;
        return value;
    }
};

// What every instrument's parameters share: the rate, the output gain (Volume) and the
// last note played (where a glide starts from).
struct SynthParams {
    float sr = 48000, gain = 1;
    int   lastPitch = -1;
};

// Polyphonic instrument: splits the block at event offsets so every note starts on its
// exact sample. Voice provides active / start / release / kill / render and an `age` used
// to steal the oldest voice. Voices render into a scratch buffer; post() can process it
// (tremolo, rotary, …) before it's added to the track at the Volume.
template <class Voice, class Params, int N>
class PolySynth : public DeviceProcessor {
public:
    explicit PolySynth(float sr) : m_sr(sr) { m_params.sr = sr; }

    void render(const EventList& ev, float* L, float* R, int frames, const float* params, const Context& ctx) override {
        m_params.update(params, m_sr, ctx);
        std::fill(m_l, m_l + frames, 0.0f);
        std::fill(m_r, m_r + frames, 0.0f);
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) handle(ev.ev[i++]);
            const int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            for (Voice& v : m_voices) if (v.active()) v.render(m_l + pos, m_r + pos, end - pos, m_params);
            pos = end;
        }
        post(m_l, m_r, frames, ctx);
        const float coef = onePoleCoef(0.01f, m_sr);
        for (int k = 0; k < frames; ++k) {
            const float g = m_gain.next(m_params.gain, coef);
            L[k] += m_l[k] * g;
            R[k] += m_r[k] * g;
        }
        int sounding = 0;
        for (const Voice& v : m_voices) sounding += v.active() ? 1 : 0;
        meter[0].store(float(sounding), std::memory_order_relaxed);   // voices sounding
    }
    void panic() override { for (Voice& v : m_voices) v.kill(); reset(); }

protected:
    virtual void post(float* /*L*/, float* /*R*/, int /*frames*/, const Context&) {}
    virtual void reset() {}

    Voice  m_voices[N];
    Params m_params;
    float  m_sr;

private:
    void handle(const NoteEvent& e) {
        if (e.allOff) { for (Voice& v : m_voices) if (v.active()) v.release(); return; }
        if (e.noteOff) { for (Voice& v : m_voices) if (v.active() && v.pitch == e.pitch && v.gate > 0) v.release(); return; }
        Voice* pick = nullptr;
        for (Voice& v : m_voices) if (!v.active()) { pick = &v; break; }
        if (!pick) { pick = &m_voices[0]; for (Voice& v : m_voices) if (v.age < pick->age) pick = &v; }
        pick->age = ++m_age;
        pick->start(e.pitch, e.velocity, std::max(1, e.duration), m_params);
        m_params.lastPitch = e.pitch;
    }

    uint32_t m_age = 0;
    Smoothed m_gain;
    float    m_l[kMaxBlock] = {}, m_r[kMaxBlock] = {};
};

} // namespace kit
} // namespace sessions::audio
