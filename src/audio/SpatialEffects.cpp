#include "Convolver.h"
#include "DeviceKit.h"
#include "ProductionEffects.h"
#include <array>

namespace sessions::audio {
using namespace kit;
namespace {
class HybridReverb final : public DeviceProcessor {
    float sr;
    Convolver convolution[2];
    DelayLine delays[8], predelay[2], aligned[2];
    OnePole damp[8];
    Biquad highpass[2], lowpass[2];
    Smoothed mix, out;
    const IrKernel* previous = nullptr;
    void algorithm(float l, float r, float decay, float size, float damping, bool freeze, float& outL, float& outR) {
        const float ms[] = {29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 61.1f, 71.9f, 79.7f};
        float v[8], sum = 0;
        for (int k = 0; k < 8; ++k) {
            const float tap = delays[k].read(std::round(ms[k] * .001f * size * sr));
            v[k] = freeze ? tap : damp[k].lp(tap);
            sum += v[k];
        }
        for (int k = 0; k < 8; ++k) {
            float gain = freeze ? 1.f : std::pow(.001f, ms[k] * .001f * size / decay);
            float input = freeze ? 0 : (k % 2 ? r : l) * .25f;
            delays[k].push((sum * .25f - v[k]) * gain + input);
        }
        outL = (v[0] + v[2] - v[4] + v[6]) * .5f;
        outR = (v[1] + v[3] - v[5] + v[7]) * .5f;
    }

  public:
    explicit HybridReverb(float r)
        : sr(r), convolution{Convolver(int(std::ceil(8 * r / kIrBlock))), Convolver(int(std::ceil(8 * r / kIrBlock)))} {
        for (auto& d : delays)
            d.init(int(sr * .2f) + 16);
        for (auto& p : predelay)
            p.init(int(sr * .26f) + 16);
        for (auto& d : aligned)
            d.init(kIrBlock + 8);
    }
    void process(float* l, float* r, int n, const float* p, const Context& ctx) override {
        using namespace hybrid;
        const T t = T::HybridReverb;
        if (ctx.ir != previous) {
            for (auto& c : convolution)
                c.reset();
            previous = ctx.ir;
        }
        float cL[kMaxBlock]{}, cR[kMaxBlock]{}, aL[kMaxBlock]{}, aR[kMaxBlock]{}, dryL[kMaxBlock], dryR[kMaxBlock];
        const int route = pc(t, p, Routing);
        bool useIr = route != 0 && ctx.ir;
        float pd = pv(t, p, PreDelay) * .001f * sr, decay = pv(t, p, Decay), size = pv(t, p, Size), blend = p[Blend],
              width = pv(t, p, Width);
        bool freeze = pb(p, Freeze);
        for (auto& f : damp)
            f.setLowpass(freeze ? sr * .45f : 800 + std::pow(1 - p[Damping], 2) * 18000, sr);
        for (int c = 0; c < 2; ++c) {
            highpass[c].highpass(std::min(pv(t, p, LowCut), sr * .4f), .707f, sr);
            lowpass[c].lowpass(std::min(pv(t, p, HighCut), sr * .45f), .707f, sr);
        }
        for (int i = 0; i < n; ++i) {
            dryL[i] = l[i];
            dryR[i] = r[i];
            predelay[0].push(l[i]);
            predelay[1].push(r[i]);
            aL[i] = cL[i] = highpass[0].process(predelay[0].read(pd));
            aR[i] = cR[i] = highpass[1].process(predelay[1].read(pd));
        }
        if (useIr) {
            convolution[0].process(*ctx.ir, 0, cL, cL, n);
            convolution[1].process(*ctx.ir, 1, cR, cR, n);
        }
        if (route != 1)
            for (int i = 0; i < n; ++i) {
                float xL = route == 3 ? cL[i] : aL[i], xR = route == 3 ? cR[i] : aR[i];
                algorithm(xL, xR, decay, size, p[Damping], freeze, aL[i], aR[i]);
            }
        const float target = dbToGain(pv(t, p, Output));
        for (int i = 0; i < n; ++i) {
            float wl = 0, wr = 0;
            if (route == 0) {
                wl = aL[i];
                wr = aR[i];
            } else if (route == 1) {
                wl = useIr ? cL[i] : 0;
                wr = useIr ? cR[i] : 0;
            } else if (route == 2) {
                wl = (useIr ? cL[i] * (1 - blend) : 0) + aL[i] * blend;
                wr = (useIr ? cR[i] * (1 - blend) : 0) + aR[i] * blend;
            } else {
                wl = aL[i];
                wr = aR[i];
            }
            wl = lowpass[0].process(wl);
            wr = lowpass[1].process(wr);
            float mid = (wl + wr) * .5f, side = (wl - wr) * .5f * width;
            float m = mix.next(p[Mix], onePoleCoef(.01f, sr)), g = out.next(target, onePoleCoef(.01f, sr));
            aligned[0].push(dryL[i]);
            aligned[1].push(dryR[i]);
            float dl = useIr ? aligned[0].read(kIrBlock) : dryL[i], dr = useIr ? aligned[1].read(kIrBlock) : dryR[i];
            l[i] = (dl * (1 - m) + (mid + side) * m) * g;
            r[i] = (dr * (1 - m) + (mid - side) * m) * g;
        }
        scope[0].store(ctx.ir ? float(ctx.ir->seconds) : 0, std::memory_order_relaxed);
    }
    void panic() override {
        for (auto& d : delays)
            d.clear();
        for (auto& d : predelay)
            d.clear();
        for (auto& d : aligned)
            d.clear();
        for (auto& c : convolution)
            c.reset();
        for (auto& d : damp)
            d.z = 0;
        for (auto& f : highpass)
            f.reset();
        for (auto& f : lowpass)
            f.reset();
        mix.primed = out.primed = false;
    }
};
} // namespace
std::shared_ptr<DeviceProcessor> createSpatialEffect(T t, float sr) {
    if (t == T::HybridReverb)
        return std::make_shared<HybridReverb>(sr);
    return {};
}
} // namespace sessions::audio
