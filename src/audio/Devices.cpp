// The MIDI effects and the processor factory. Instruments live in Instruments.cpp, audio
// effects in Effects.cpp. Parameter indices follow core/DeviceSpecs.cpp.

#include "DeviceKit.h"
#include "core/Scales.h"

namespace sessions::audio {

using namespace kit;

void DeviceProcessor::midi(const EventList& in, EventList& out, const float*, const Context&, int) { out = in; }

float velocityMap(const float* p, float v) {
    const float hi = p[0], lo = p[1], drive = pv(T::Velocity, p, 2);
    return lo + (hi - lo) * std::pow(std::clamp(v, 0.0f, 1.0f), std::exp2(-2.0f * drive));
}

int scaleMap(const float* p, int pitch) {
    const int root = pc(T::Scale, p, 0), scale = pc(T::Scale, p, 1);
    const int q = pitch + int(pv(T::Scale, p, 2));
    for (int d = 0; d < 12; ++d) {   // the nearest note of the scale; a tie goes down
        if (inScale(q - d, root, scale)) return std::clamp(q - d, 0, 127);
        if (inScale(q + d, root, scale)) return std::clamp(q + d, 0, 127);
    }
    return std::clamp(q, 0, 127);
}

namespace {

// Chord: up to five extra notes per note, shifted by semitones.
class Chord : public DeviceProcessor {
public:
    void midi(const EventList& in, EventList& out, const float* p, const Context&, int) override {
        int shifts[5];
        int k = 0;
        for (int i : {0, 1, 2, 4, 5}) shifts[k++] = int(pv(T::Chord, p, i));
        for (int i = 0; i < in.count; ++i) {
            const NoteEvent& e = in.ev[i];
            out.add(e);
            if (e.allOff) continue;
            for (int s : shifts) {
                if (!s) continue;
                NoteEvent n = e;
                n.pitch = std::clamp(e.pitch + s, 0, 127);
                n.velocity = std::max(0.05f, e.velocity * p[3]);
                out.add(n);
            }
        }
    }
};

// Scale: every note to the nearest note of the scale (note-offs follow the same way).
// meter[0] = the last note in, meter[1] = where it went.
class Scale : public DeviceProcessor {
public:
    void midi(const EventList& in, EventList& out, const float* p, const Context&, int) override {
        for (int i = 0; i < in.count; ++i) {
            NoteEvent n = in.ev[i];
            if (!n.allOff) {
                const int to = scaleMap(p, n.pitch);
                if (!n.noteOff) { meter[0].store(float(n.pitch), std::memory_order_relaxed); meter[1].store(float(to), std::memory_order_relaxed); }
                n.pitch = to;
            }
            out.add(n);
        }
    }
};

// Velocity: squeeze the range, bend the curve, add randomness.
// meter[0] = the last velocity in, meter[1] = out.
class Velocity : public DeviceProcessor {
public:
    void midi(const EventList& in, EventList& out, const float* p, const Context&, int) override {
        for (int i = 0; i < in.count; ++i) {
            NoteEvent n = in.ev[i];
            if (!n.allOff && !n.noteOff) {
                float v = velocityMap(p, n.velocity) + p[3] * 0.5f * m_rng.next();
                v = std::clamp(v, 0.01f, 1.0f);
                meter[0].store(n.velocity, std::memory_order_relaxed);
                meter[1].store(v, std::memory_order_relaxed);
                n.velocity = v;
            }
            out.add(n);
        }
    }
private:
    Noise m_rng;
};

class Arpeggiator : public DeviceProcessor {
public:
    void midi(const EventList& in, EventList& out, const float* p, const Context& ctx, int frames) override {
        const double rate = pv(T::Arpeggiator, p, 0);
        const double gate = pv(T::Arpeggiator, p, 1);
        const int octaves = int(pv(T::Arpeggiator, p, 2));
        const int style = int(pv(T::Arpeggiator, p, 3));
        const double bps = ctx.beatsPerSample;
        int ei = 0;
        if (bps > 0) {
            for (double step = std::ceil(ctx.beat / rate - 1e-9) * rate;; step += rate) {
                int off = int(std::lround((step - ctx.beat) / bps));
                if (off >= frames) break;
                off = std::max(0, off);
                while (ei < in.count && in.ev[ei].offset <= off) hold(in.ev[ei++], out);
                playStep(off, std::max(16, int(gate * rate / bps)), octaves, style, out);
            }
        }
        while (ei < in.count) hold(in.ev[ei++], out);
        m_clock += frames;
    }
    void panic() override { m_count = 0; m_step = 0; }

private:
    struct Held { int pitch; float vel; int64_t end; int order; };

    void hold(const NoteEvent& e, EventList& out) {
        if (e.allOff) { m_count = 0; m_step = 0; out.add(e); return; }
        if (e.noteOff) {   // a live key let go: drop it from the pattern
            for (int i = 0; i < m_count; ++i)
                if (m_held[i].pitch == e.pitch) { m_held[i] = m_held[--m_count]; break; }
            return;
        }
        int64_t end = m_clock + e.offset + e.duration;
        for (int i = 0; i < m_count; ++i)
            if (m_held[i].pitch == e.pitch) { m_held[i].end = end; m_held[i].vel = e.velocity; return; }
        if (m_count < 32) m_held[m_count++] = {e.pitch, e.velocity, end, m_order++};
    }

    void playStep(int off, int duration, int octaves, int style, EventList& out) {
        const int64_t now = m_clock + off;
        int k = 0;
        for (int i = 0; i < m_count; ++i) if (m_held[i].end > now) m_held[k++] = m_held[i];
        m_count = k;
        if (!m_count) { m_step = 0; meter[1].store(0.0f, std::memory_order_relaxed); return; }

        Held notes[32];
        std::copy(m_held, m_held + m_count, notes);
        if (style == 4) std::sort(notes, notes + m_count, [](const Held& a, const Held& b) { return a.order < b.order; });
        else std::sort(notes, notes + m_count, [](const Held& a, const Held& b) { return a.pitch < b.pitch; });

        int seq[128]; float vel[128]; int len = 0;
        for (int o = 0; o < octaves; ++o)
            for (int i = 0; i < m_count && len < 128; ++i) { seq[len] = notes[i].pitch + 12 * o; vel[len] = notes[i].vel; ++len; }

        int idx = 0;
        switch (style) {
        case 1: idx = len - 1 - m_step % len; break;                                  // down
        case 2: { int period = len > 1 ? 2 * len - 2 : 1; int s = m_step % period;    // up/down
                  idx = s < len ? s : period - s; break; }
        case 3: idx = int(m_rng.uniform() * len) % len; break;                        // random
        default: idx = m_step % len; break;                                           // up, played
        }
        meter[0].store(float(m_step), std::memory_order_relaxed);   // the panel lights the step
        meter[1].store(float(len), std::memory_order_relaxed);
        ++m_step;
        if (seq[idx] > 127) return;
        out.add({off, seq[idx], vel[idx], duration, false});
    }

    Held    m_held[32];
    int     m_count = 0, m_step = 0, m_order = 0;
    int64_t m_clock = 0;
    Noise   m_rng;
};

} // namespace

void ChainMixer::mix(int c, const float* inL, const float* inR, float* outL, float* outR, int n, float tl, float tr) {
    if (c < 0 || c >= kMax) return;
    if (!m_primed[c]) { m_gl[c] = tl; m_gr[c] = tr; m_primed[c] = true; }
    const float a = 1.0f - onePoleCoef(0.01f, m_sr);
    float peak = 0;
    for (int i = 0; i < n; ++i) {
        m_gl[c] += (tl - m_gl[c]) * a;
        m_gr[c] += (tr - m_gr[c]) * a;
        const float l = inL[i] * m_gl[c], r = inR[i] * m_gr[c];
        outL[i] += l;
        outR[i] += r;
        peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
    }
    if (peak > m_peak[c].load(std::memory_order_relaxed)) m_peak[c].store(peak, std::memory_order_relaxed);
}

void ChainMixer::scale(int c, float* L, float* R, int n, float g) {
    if (c < 0 || c >= kMax) return;
    if (!m_inPrimed[c]) { m_in[c] = g; m_inPrimed[c] = true; }
    if (m_in[c] == g && g == 1.0f) return;
    const float a = 1.0f - onePoleCoef(0.01f, m_sr);
    for (int i = 0; i < n; ++i) {
        m_in[c] += (g - m_in[c]) * a;
        if (std::fabs(m_in[c] - g) < 1e-6f) m_in[c] = g;
        L[i] *= m_in[c];
        R[i] *= m_in[c];
    }
}

float ChainMixer::takePeak(int c) {
    if (c < 0 || c >= kMax) return 0;
    return m_peak[c].exchange(0.0f, std::memory_order_relaxed);
}

void RackProcessor::mixChain(int c, const float* inL, const float* inR, float* outL, float* outR, int n, const float* p, bool anySolo) {
    if (c < 0 || c >= racks::kChains) return;
    const bool on = p[racks::param(c, racks::On)] >= 0.5f, solo = p[racks::param(c, racks::Solo)] >= 0.5f;
    const float g = on && (!anySolo || solo) ? faderGain(p[racks::param(c, racks::Volume)]) : 0.0f;
    const float pan = p[racks::param(c, racks::Pan)] * 2 - 1;
    m_mix.mix(c, inL, inR, outL, outR, n, g * std::min(1.0f, 1 - pan), g * std::min(1.0f, 1 + pan));
}

std::shared_ptr<DeviceProcessor> createProcessor(DeviceType type, float sr) {
    if (isRackType(type)) return std::make_shared<RackProcessor>(sr);   // the three racks (the Drum Rack mixes its pads itself)
    if (auto p = createInstrument(type, sr)) return p;
    if (auto p = createEffect(type, sr)) return p;
    if (type == T::Nam) return createNam(sr);
    if (auto p = createGuitar(type, sr)) return p;
    switch (type) {
    case T::Arpeggiator: return std::make_shared<Arpeggiator>();
    case T::Chord:       return std::make_shared<Chord>();
    case T::Scale:       return std::make_shared<Scale>();
    case T::Velocity:    return std::make_shared<Velocity>();
    default:             break;
    }
    return std::make_shared<DeviceProcessor>();   // passes MIDI through, makes no sound
}

} // namespace sessions::audio
