// Recorded acoustic kits with velocity layers and round robins. A fixed voice pool
// plays aligned close/overhead/room channels, natural tails and short hat chokes.
// Legacy banks and immutable expansion content are prepared outside the audio callback.
#include "DeviceKit.h"
#include "DrumSamples.h"
#include "DrumMicModel.h"

namespace sessions::audio {
using namespace kit;
namespace ds = drumstudio;
namespace {
constexpr int kVoices = 64;
constexpr float kPan[ds::kDrums] = {0.f, -.15f, -.48f, -.3f, .08f, .42f, .52f, -.45f};
constexpr float kLevel[ds::kDrums] = {.56f, .46f, .28f, .48f, .48f, .5f, .28f, .3f};
struct Voice {
    const drums::Layer* sample = nullptr;
    const drums::Layer* overhead = nullptr;
    const drums::Layer* room = nullptr;
    int drum = 0, pitch = 0;
    uint64_t age = 0;
    double position = 0, step = 1, end = 0;
    float gain = 1, panL = 1, panR = 1, ohL = 1, ohR = 1;
    float envelope = 1, decay = 1, choke = 1, high = 1;
    OnePole tone, toneR;
};
class DrumStudio : public DeviceProcessor {
public:
    explicit DrumStudio(float sr) : m_sr(sr), m_library(drums::library()) {
        for (int c = 0; c < 2; ++c) {
            m_ohDelay[c].init(int(sr * .012f) + 8); m_pre[c].init(int(sr * .06f) + 8);
            m_recordedOhDelay[c].init(int(sr*.012f)+8);m_recordedRoomDelay[c].init(int(sr*.025f)+8);
            for (int j = 0; j < 4; ++j) m_comb[c][j].init(int(sr * .09f) + 8);
            m_ap[c][0].init(int(sr * .0051f * (c ? 1.07f : 1.f)));
            m_ap[c][1].init(int(sr * .0017f * (c ? 1.11f : 1.f)));
            m_ohShelf[c].setLowpass(5000, sr); m_bleedLp[c].setLowpass(1800, sr);
            for (int d = 0; d < ds::kDrums; ++d) m_closeDelay[d][c].init(int(sr * .004f) + 8);
        }
    }
    void render(const EventList& ev, float* L, float* R, int frames, const float* p, const Context& ctx) override {
        bool changed=false;for(int i=0;i<6;++i){const auto* bank=ctx.drumContent?ctx.drumContent->banks[i].get():nullptr;changed|=bank!=m_seenBanks[i];m_seenBanks[i]=bank;}
        if(changed)panic(); // changing content invalidates old hit pointers before any dereference
        m_content=ctx.drumContent;
        m_p = p; m_kit = std::clamp(pc(T::DrumStudio, p, ds::Kit), 0, ds::kKits - 1);
        std::fill_n(m_cl, frames, 0.f); std::fill_n(m_cr, frames, 0.f);
        std::fill_n(m_ohl, frames, 0.f); std::fill_n(m_ohr, frames, 0.f); std::fill_n(m_room, frames, 0.f);
        std::fill_n(m_peaks, ds::kDrums, 0.f);
        for(auto& channel:m_recordedRoom)std::fill_n(channel,frames,0.f);
        for(auto& channel:m_recordedOh)std::fill_n(channel,frames,0.f);
        for (auto& drum : m_close) for (auto& channel : drum) std::fill_n(channel, frames, 0.f);
        int pos = 0, i = 0;
        while (pos < frames) {
            while (i < ev.count && ev.ev[i].offset <= pos) trigger(ev.ev[i++]);
            const int end = i < ev.count ? std::min(frames, ev.ev[i].offset) : frames;
            for (auto& voice : m_voices) if (voice.sample) renderVoice(voice, pos, end - pos);
            pos = end;
        }
        colorCloseMics(frames);
        mixMics(L, R, frames, p);
        for (int d = 0; d < ds::kDrums; ++d) {
            m_lights[d] *= .93f; scope[d].store(m_lights[d], std::memory_order_relaxed);
            scope[8 + d].store(std::max(m_peaks[d], scope[8 + d].load(std::memory_order_relaxed) * .985f), std::memory_order_relaxed);
        }
    }
    void panic() override {
        for (auto& v : m_voices) v.sample = nullptr;
        for (int c = 0; c < 2; ++c) {
            m_ohDelay[c].clear(); m_pre[c].clear();
            m_recordedOhDelay[c].clear();m_recordedRoomDelay[c].clear();
            m_busColor[0][c].z = m_busColor[1][c].z = 0;
            for (int d = 0; d < ds::kDrums; ++d) { m_closeDelay[d][c].clear(); m_closeColor[d][c].z = 0; }
            for (auto& comb : m_comb[c]) { std::fill(comb.buf.begin(), comb.buf.end(), 0.f); comb.idx = 0; comb.store = 0; }
            for (auto& ap : m_ap[c]) std::fill(ap.buf.begin(), ap.buf.end(), 0.f);
        }
        for (auto& light : m_lights) light = 0;
    }
private:
    float knob(int drum, int which) const { return pv(T::DrumStudio, m_p, ds::param(drum, which)); }
    void trigger(const NoteEvent& e) {
        if (e.allOff) { for (auto& v : m_voices) if (v.sample) v.choke = onePoleCoef(.012f, m_sr); return; }
        if (e.noteOff || e.velocity <= 0) return;
        const int drum = ds::drumFor(e.pitch); if (drum < 0) return;
        if (m_p[ds::piece(drum, ds::Present)] < .5f) return;
        const float vel = std::pow(std::clamp(e.velocity, .01f, 1.f), .5f + m_p[ds::Velocity] * 1.5f);
        const int midiVelocity = std::clamp(int(std::lround(e.velocity * 127.f)), 1, 127);
        const int layer = midiVelocity <= 26 ? 0 : midiVelocity <= 52 ? 1 : midiVelocity <= 77 ? 2 : midiVelocity <= 102 ? 3 : 4;
        const int source = pc(T::DrumStudio, m_p, ds::piece(drum, ds::Source));
        const drums::RecordedHit* recording=nullptr;const int content=drums::contentIndex(m_p,drum);
        if(content>=0&&m_content&&m_content->banks[content])recording=drums::recordedHit(*m_content->banks[content],e.pitch==35?36:e.pitch,midiVelocity,m_roundRobin[e.pitch&127]++);
        const bool alternate=m_p[ds::recordedMic(drum)]>=.5f;
        const auto& sample=recording?(alternate&&!recording->alternate.mono.empty()?recording->alternate:recording->close):m_library[source ? source - 1 : drums::bankFor(m_kit)].notes[drums::sampleNote(e.pitch)][layer];
        if (sample.mono.empty()) return;
        if (drum == ds::HiHat) for (auto& v : m_voices)
            if (v.sample && v.drum == ds::HiHat) v.choke = onePoleCoef(e.pitch == 42 || e.pitch == 44 ? .008f : .025f, m_sr);
        Voice* voice = nullptr;
        for (auto& v : m_voices) if (!v.sample) { voice = &v; break; }
        if (!voice) { voice = &m_voices[0]; for (auto& v : m_voices) if (v.age < voice->age) voice = &v; }
        auto& v = *voice; v = Voice{}; v.sample = &sample; v.drum = drum; v.pitch = e.pitch; v.age = ++m_age;
        if(recording){v.overhead=&recording->overhead;v.room=&recording->room;}
        v.end=sample.mono.size();for(const auto* channel:{v.overhead,v.room})if(channel&&!channel->mono.empty())v.end=std::max(v.end,channel->mono.size()/channel->rate*sample.rate);
        const float tune = knob(drum, ds::Tune) + (recording?0:drums::sampleTune(e.pitch)) + (m_kit == 1 ? -1.f : m_kit == 2 ? .6f : 0.f);
        v.step = sample.rate / m_sr * std::exp2(tune / 12.f);
        const float decay = knob(drum, ds::Decay) * (m_kit == 2 ? .65f : m_kit == 3 ? .8f : 1.f);
        // Default retains the recording. Shorter decay adds damping; longer emphasizes the recorded tail
        // independently of pitch. Values >=1 retain the full recorded tail.
        v.decay = decay < 1.f ? onePoleCoef((drum >= ds::Ride ? 1.5f : .35f) * decay / (1.f - decay), m_sr) : std::exp2((1.f - 1.f / decay) / (m_sr * .6f));
        const float stagePan = float(m_p[ds::piece(drum, ds::StageX)] - ds::stageX[drum]) * 2.f;
        const float pan = std::clamp((kPan[drum] + stagePan) * m_p[ds::Width] + knob(drum, ds::Pan), -1.f, 1.f);
        v.panL = std::sqrt((1.f - pan) * .5f); v.panR = std::sqrt((1.f + pan) * .5f);
        v.ohL = v.panL; v.ohR = v.panR;
        v.gain = vel * dbToGain(knob(drum, ds::Level)) * kLevel[drum] * .75f; // headroom for layered tom fills and simultaneous cymbals
        if(recording)v.gain*=recording->gain;
        const float tone = m_p[ds::param(drum, ds::Tone)] + (m_kit == 1 ? -.08f : m_kit == 3 ? .12f : 0.f);
        v.tone.setLowpass(drum == ds::Kick ? 1700.f : 4200.f, m_sr);
        v.toneR.a=v.tone.a;
        v.high = std::exp2((tone - .5f) * 3.f);
        if (drum == ds::Kick) v.high *= .5f + m_p[ds::Beater];
        if (drum == ds::Snare && e.pitch != 37) v.high *= .4f + m_p[ds::Wires];
        m_lights[drum] = std::max(m_lights[drum], vel);
        meter[0].store(float(drum), std::memory_order_relaxed);
        meter[1].store(meter[1].load(std::memory_order_relaxed) + 1.f, std::memory_order_relaxed);
    }
    void renderVoice(Voice& v, int at, int count) {
        if (m_p[ds::piece(v.drum, ds::Present)] < .5f) { v.sample = nullptr; return; }
        const auto& data = v.sample->mono;
        bool solo = false; for (int d = 0; d < ds::kDrums; ++d) solo |= m_p[ds::piece(d, ds::Present)] >= .5f && m_p[ds::solo(d)] >= .5f;
        const bool audible = m_p[ds::piece(v.drum, ds::Present)] >= .5f && m_p[ds::mute(v.drum)] < .5f && (!solo || m_p[ds::solo(v.drum)] >= .5f);
        for (int i = at; i < at + count; ++i) {
            const int index = int(v.position);
            if (v.position + 1 >= v.end || v.envelope < .00001f) { v.sample = nullptr; break; }
            const float frac = float(v.position - index);
            float s = index+1<int(data.size())?data[index] + (data[index + 1] - data[index]) * frac:0;
            float right=v.sample->right.empty()?s:index+1<int(v.sample->right.size())?v.sample->right[index]+(v.sample->right[index+1]-v.sample->right[index])*frac:0;
            const float low = v.tone.lp(s); s = low + (s - low) * v.high;
            const float lowR=v.toneR.lp(right);right=lowR+(right-lowR)*v.high;
            const float amplitude=v.gain*v.envelope*(audible?1.f:0.f);
            s *= amplitude;right*=amplitude;
            // Fade the last 2ms if the original recording ends above zero.
            const float endFade=std::clamp(float(double(data.size()) - index - 1) / float(std::max(1., v.sample->rate * .002)),0.f,1.f);s*=endFade;right*=endFade;
            m_peaks[v.drum] = std::max(m_peaks[v.drum], std::abs(s));
            m_cl[i] += s * v.panL; m_cr[i] += right * v.panR;
            m_close[v.drum][0][i] += s * v.panL; m_close[v.drum][1][i] += right * v.panR;
            auto channelAt=[&](const drums::Layer* source,bool rightChannel){if(!source||source->mono.empty())return 0.f;const auto& samples=rightChannel&&!source->right.empty()?source->right:source->mono;
                const double position=v.position/v.sample->rate*source->rate;const size_t at=size_t(position);if(at+1>=samples.size())return 0.f;const float f=float(position-at);return (samples[at]+(samples[at+1]-samples[at])*f)*amplitude;};
            if(v.overhead&&!v.overhead->mono.empty()){m_recordedOh[0][i]+=channelAt(v.overhead,false);m_recordedOh[1][i]+=channelAt(v.overhead,true);}
            else{m_ohl[i] += s * v.ohL; m_ohr[i] += right * v.ohR;}
            if(v.room&&!v.room->mono.empty()){m_recordedRoom[0][i]+=channelAt(v.room,false);m_recordedRoom[1][i]+=channelAt(v.room,true);}
            else m_room[i] += (s+right)*.5f;
            v.position += v.step; v.envelope = std::min(2.f, v.envelope * v.decay * v.choke);
        }
    }
    void colorCloseMics(int n) {
        const float coef = onePoleCoef(.01f, m_sr);
        bool solo = false;
        for (int d = 0; d < ds::kDrums; ++d) solo |= m_p[ds::piece(d, ds::Present)] >= .5f && m_p[ds::solo(d)] >= .5f;
        for (int d = 0; d < ds::kDrums; ++d) {
            const float cm = pv(T::DrumStudio, m_p, ds::piece(d, ds::MicDistance));
            const float angle = pv(T::DrumStudio, m_p, ds::piece(d, ds::MicAngle));
            const auto color = drums::micColor(pc(T::DrumStudio, m_p, ds::piece(d, ds::MicModel)), angle);
            const float gain = drums::micGain(cm, angle);
            const float delay = 1 + std::max(0.f, cm - 10.f) / 34300.f * m_sr;
            const bool audible = m_p[ds::piece(d, ds::Present)] >= .5f && m_p[ds::mute(d)] < .5f && (!solo || m_p[ds::solo(d)] >= .5f);
            for (int c = 0; c < 2; ++c) {
                m_closeColor[d][c].setLowpass(color.cutoff, m_sr);
                if (!audible) { m_closeDelay[d][c].clear(); m_closeColor[d][c].z = 0; }
            }
            for (int i = 0; i < n; ++i) {
                const float g = m_micGain[d].next(gain, coef), high = m_micHigh[d].next(color.high, coef), time = m_micDelay[d].next(delay, coef);
                for (int c = 0; c < 2; ++c) {
                    auto& lp = m_closeColor[d][c]; auto& line = m_closeDelay[d][c];
                    float* out = c ? m_cr : m_cl;
                    const float source = m_close[d][c][i]; line.push(source);
                    const float input = line.read(time), low = lp.lp(input);
                    // The Recording default takes an exact bypass while keeping filters warm.
                    const float filtered = high == 1 ? input : low + (input - low) * high;
                    out[i] += (audible ? filtered * g : 0.f) - source;
                }
            }
        }
    }
    // The three mic channels into the track: close + overheads (bright, wide, a hair late) + room
    // (pre-delay, four combs and two all-passes a side, their lengths by Room Size) + bleed.
    void mixMics(float* L, float* R, int n, const float* p) {
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
        const float shelf = dbToGain(2.f);
        const float height = pv(T::DrumStudio, p, ds::OhHeight), distance = pv(T::DrumStudio, p, ds::RoomDistance);
        const float preDelay = m_sr * (0.006f + 0.016f * size + std::max(0.f, distance - 3) / 343.f);
        const float extraOh = m_sr * std::max(0.f, height - 1.5f) / 343.f;
        const float ohDelayL = m_sr * 0.0012f * width + extraOh, ohDelayR = m_sr * 0.0021f * width + extraOh;
        const auto ohColor = drums::micColor(pc(T::DrumStudio, p, ds::OhModel));
        const auto roomColor = drums::micColor(pc(T::DrumStudio, p, ds::RoomModel));
        for (int c = 0; c < 2; ++c) { m_busColor[0][c].setLowpass(ohColor.cutoff, m_sr); m_busColor[1][c].setLowpass(roomColor.cutoff, m_sr); }
        auto colorBus = [](float x, OnePole& lp, const drums::MicColor& color) { const float low = lp.lp(x); return color.high == 1 ? x : low + (x - low) * color.high; };
        for (int i = 0; i < n; ++i) {
            // overheads: a bright copy, each side a little late (the far side more)
            m_ohDelay[0].push(m_ohl[i]); m_ohDelay[1].push(m_ohr[i]);
            float ol = m_ohDelay[0].read(1 + ohDelayL), orr = m_ohDelay[1].read(1 + ohDelayR);
            ol = ol + (ol - m_ohShelf[0].lp(ol)) * (shelf - 1);
            orr = orr + (orr - m_ohShelf[1].lp(orr)) * (shelf - 1);
            m_recordedOhDelay[0].push(m_recordedOh[0][i]);m_recordedOhDelay[1].push(m_recordedOh[1][i]);
            const float recordedL=m_recordedOhDelay[0].read(1+extraOh),recordedR=m_recordedOhDelay[1].read(1+extraOh);
            const float recordedMid=(recordedL+recordedR)*.5f,recordedSide=(recordedL-recordedR)*.5f*(width/.8f);
            ol+=recordedMid+recordedSide;orr+=recordedMid-recordedSide;
            ol = colorBus(ol, m_busColor[0][0], ohColor) * std::sqrt(1.5f / height);
            orr = colorBus(orr, m_busColor[0][1], ohColor) * std::sqrt(1.5f / height);
            // bleed: a dull copy of the overheads into the close mics
            const float bl = m_bleedLp[0].lp(ol) * bleed * 0.5f, br = m_bleedLp[1].lp(orr) * bleed * 0.5f;
            // the room: a mono feed, pre-delayed, into a stereo pair of combs
            m_pre[0].push(m_room[i]); m_pre[1].push(m_room[i]);
            float rl = 0, rr = 0;
            const float feed = 0.35f;
            for (int j = 0; j < 4; ++j) {
                rl += m_comb[0][j].process(m_pre[0].read(preDelay) * feed, feedback, damp);
                rr += m_comb[1][j].process(m_pre[1].read(preDelay * 1.07f) * feed, feedback, damp);
            }
            rl = m_ap[0][1].process(m_ap[0][0].process(rl * 0.25f));
            rr = m_ap[1][1].process(m_ap[1][0].process(rr * 0.25f));
            m_recordedRoomDelay[0].push(m_recordedRoom[0][i]);m_recordedRoomDelay[1].push(m_recordedRoom[1][i]);
            const float roomDelay=1+std::max(0.f,distance-3.f)/343.f*m_sr;
            rl = colorBus(rl+m_recordedRoomDelay[0].read(roomDelay), m_busColor[1][0], roomColor) * std::sqrt(3.f / distance);
            rr = colorBus(rr+m_recordedRoomDelay[1].read(roomDelay), m_busColor[1][1], roomColor) * std::sqrt(3.f / distance);
            const float g = m_gain.next(vol, coef), go = m_ohGain.next(oh, coef), gr = m_roomGain.next(roomLvl, coef);
            L[i] += ((m_cl[i] + bl) + ol * go + rl * gr) * g;
            R[i] += ((m_cr[i] + br) + orr * go + rr * gr) * g;
        }
        float peak = 0;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::fabs(m_room[i]));
        meter[2].store(gainToDb(peak * roomLvl), std::memory_order_relaxed);
    }

    float m_sr;
    const std::array<drums::Bank, 3>& m_library;
    const float* m_p = nullptr;
    int m_kit = 0;
    uint64_t m_age = 0;
    Voice m_voices[kVoices];
    const drums::Content* m_content=nullptr;const drums::ContentBank* m_seenBanks[6]{};unsigned m_roundRobin[128]{};
    float m_recordedRoom[2][kMaxBlock]{};
    float m_recordedOh[2][kMaxBlock]{};DelayLine m_recordedOhDelay[2],m_recordedRoomDelay[2];
    float m_lights[ds::kDrums]{}, m_peaks[ds::kDrums]{};
    float m_cl[kMaxBlock]{}, m_cr[kMaxBlock]{}, m_ohl[kMaxBlock]{}, m_ohr[kMaxBlock]{}, m_room[kMaxBlock]{};
    float m_close[ds::kDrums][2][kMaxBlock]{};
    DelayLine m_closeDelay[ds::kDrums][2]; OnePole m_closeColor[ds::kDrums][2], m_busColor[2][2];
    Smoothed m_micGain[ds::kDrums], m_micHigh[ds::kDrums], m_micDelay[ds::kDrums];
    DelayLine m_ohDelay[2], m_pre[2];
    OnePole m_ohShelf[2], m_bleedLp[2];
    Comb m_comb[2][4]; Allpass m_ap[2][2];
    float m_size = -1;
    Smoothed m_gain, m_ohGain, m_roomGain;
};
}
std::shared_ptr<DeviceProcessor> createDrumStudio(float sr) { return std::make_shared<DrumStudio>(sr); }
}
