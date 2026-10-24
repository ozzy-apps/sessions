#include "DeviceKit.h"
#include "ProductionEffects.h"
#include "Resample.h"
#include "core/Sample.h"
#include <array>

namespace sessions::audio {
using namespace kit;
namespace {
struct ZoneVoice {
    Adsr env;
    Svf filter[2];
    const SampleData* sample = nullptr;
    int zone = 0, pitch = 60, remaining = 0;
    float velocity = 0;
    double position = 0;
    uint64_t age = 0;
    bool released = false;
};
class Multisampler final : public DeviceProcessor {
    float sr;
    std::array<ZoneVoice, multisampler::kVoices> voices;
    std::array<uint64_t, 128> sequence{};
    uint64_t age = 0;
    void configure(ZoneVoice& v, const float* p) {
        using namespace multisampler;
        v.env.set(pv(T::Multisampler, p, Attack), pv(T::Multisampler, p, Decay), p[Sustain],
                  pv(T::Multisampler, p, Release), sr);
        for (auto& f : v.filter)
            f.set(std::min(pv(T::Multisampler, p, Cutoff), sr * .45f), pv(T::Multisampler, p, Resonance), sr);
    }
    void event(const NoteEvent& e, const float* p, const Context& ctx) {
        if (e.allOff || e.noteOff) {
            for (auto& v : voices)
                if (v.env.active() && (e.allOff || v.pitch == e.pitch)) {
                    bool oneShot = v.zone < ctx.sampleZoneCount && ctx.sampleZones[v.zone].oneShot;
                    if (e.allOff || !oneShot) {
                        v.released = true;
                        v.env.off();
                    }
                }
            return;
        }
        int velocity = std::clamp(int(std::round(e.velocity * 127)), 1, 127);
        uint64_t seq = sequence[size_t(std::clamp(e.pitch, 0, 127))]++;
        for (int i = 0; i < ctx.sampleZoneCount && i < multisampler::kMaxZones; ++i) {
            const auto& z = ctx.sampleZones[i];
            if (!z.sample || z.sample->frames() < 2 || e.pitch < z.keyLow || e.pitch > z.keyHigh ||
                velocity < z.velocityLow || velocity > z.velocityHigh ||
                int(seq % std::max(1, z.sequenceLength)) + 1 != z.sequencePosition)
                continue;
            ZoneVoice* found = nullptr;
            for (auto& v : voices)
                if (!v.env.active()) {
                    found = &v;
                    break;
                }
            if (!found)
                found = &*std::min_element(voices.begin(), voices.end(),
                                           [](const ZoneVoice& a, const ZoneVoice& b) { return a.age < b.age; });
            auto& v = *found;
            v.env.kill();
            v.zone = i;
            v.sample = z.sample.get();
            v.pitch = e.pitch;
            v.position = 0;
            v.remaining = e.duration;
            v.velocity = e.velocity;
            v.age = ++age;
            v.released = false;
            for (auto& f : v.filter)
                f.reset();
            configure(v, p);
            v.env.on();
        }
    }

  public:
    explicit Multisampler(float rate) : sr(rate) {}
    void render(const EventList& notes, float* l, float* r, int n, const float* p, const Context& ctx) override {
        using namespace multisampler;
        for (auto& v : voices)
            if (v.env.active())
                configure(v, p);
        int eventAt = 0;
        float globalGain = dbToGain(pv(T::Multisampler, p, Volume));
        for (int f = 0; f < n; ++f) {
            while (eventAt < notes.count && notes.ev[eventAt].offset <= f)
                event(notes.ev[eventAt++], p, ctx);
            for (auto& v : voices) {
                if (!v.env.active())
                    continue;
                if (v.zone >= ctx.sampleZoneCount || !ctx.sampleZones ||
                    ctx.sampleZones[v.zone].sample.get() != v.sample) {
                    v.env.kill();
                    continue;
                }
                const auto& z = ctx.sampleZones[v.zone];
                const auto& sample = *v.sample;
                double step = sample.rate / sr *
                              std::exp2((v.pitch - z.root + z.tune + pv(T::Multisampler, p, Tune) + ctx.bend * 2) / 12);
                bool loop = (z.loop || pb(p, Loop)) && !v.released;
                double start = std::clamp(z.loopStart, 0., double(sample.frames() - 1)),
                       end = z.loopEnd > start ? std::min(z.loopEnd, double(sample.frames())) : double(sample.frames()),
                       fade = std::min(64., (end - start) * .25);
                if (v.position >= end) {
                    if (loop && end > start + 1)
                        v.position = start + fade + std::fmod(v.position - end, std::max(1., end - start - fade));
                    else {
                        v.env.kill();
                        continue;
                    }
                }
                float a = resample::read(sample.left, v.position, step),
                      b = resample::read(sample.right, v.position, step);
                if (loop && fade > 0 && v.position > end - fade) {
                    float t = float((v.position - (end - fade)) / fade);
                    double at = start + v.position - (end - fade);
                    a += (resample::read(sample.left, at, step) - a) * t;
                    b += (resample::read(sample.right, at, step) - b) * t;
                }
                float g = v.env.next() * v.velocity * globalGain * dbToGain(z.gain);
                l[f] += v.filter[0].lp(a) * g * float(std::min(1., 1 - z.pan));
                r[f] += v.filter[1].lp(b) * g * float(std::min(1., 1 + z.pan));
                v.position += step;
                if (v.remaining < audio::kHeld && v.remaining > 0 && --v.remaining == 0 && !z.oneShot) {
                    v.released = true;
                    v.env.off();
                }
            }
        }
        int count = 0;
        for (const auto& v : voices)
            count += v.env.active();
        scope[0].store(float(count), std::memory_order_relaxed);
    }
    void panic() override {
        for (auto& v : voices) {
            v.env.kill();
            v.sample = nullptr;
        }
        sequence.fill(0);
        age = 0;
    }
};
} // namespace
std::shared_ptr<DeviceProcessor> createMultisampler(float sr) {
    return std::make_shared<Multisampler>(sr);
}
} // namespace sessions::audio
