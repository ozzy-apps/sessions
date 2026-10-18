// Sessions' own drum-bus processor. The nonlinear stages run at 4x sample rate;
// the parallel reference uses the same FIR so Dry/Wet does not comb-filter.
// Everything is fixed-size; process() and panic() allocate nothing.
#include "DeviceKit.h"
#include <array>

namespace sessions::audio {
using namespace kit;
namespace db = drumbuss;
namespace {

const std::array<float, 64>& taps() {
    static const auto h = [] {
        std::array<float, 64> a{}; double sum = 0;
        for (int i = 0; i < 64; ++i) {
            const double x = i - 31.5;
            const double sinc = std::sin(2 * double(kPi) * .11 * x) / (double(kPi) * x);
            const double window = .42 - .5 * std::cos(2 * double(kPi) * i / 63) + .08 * std::cos(4 * double(kPi) * i / 63);
            a[i] = float(sinc * window); sum += a[i];
        }
        for (auto& v : a) v = float(v / sum); return a;
    }();
    return h;
}
struct Oversampler {
    float input[16]{}, output[64]{}; int at = 0;
    template<class F> float run(float x, F shape) {
        const auto& h = taps();
        for (int i = 15; i > 0; --i) input[i] = input[i - 1]; input[0] = x;
        for (int phase = 0; phase < 4; ++phase) {
            float y = 0; for (int j = 0; j < 16; ++j) y += h[phase + j * 4] * input[j];
            output[at] = shape(y * 4); at = (at + 1) & 63;
        }
        float y = 0; for (int i = 0; i < 64; ++i) y += h[i] * output[(at - 1 - i) & 63]; return y;
    }
    void reset() { std::fill_n(input, 16, 0.f); std::fill_n(output, 64, 0.f); at = 0; }
};

// Fourth-order Linkwitz-Riley crossover. Its summed response is flat; both
// parallel paths share its phase, and Decay can shorten bass without cancelling
// a phase-shifted low-pass copy against the original signal.
struct BassSplit {
    Biquad low[2], high[2];
    void prepare(float rate) { for (auto& f : low) f.lowpass(100, .70710678f, rate); for (auto& f : high) f.highpass(100, .70710678f, rate); }
    void run(float x, float& lo, float& hi) { lo = low[1].process(low[0].process(x)); hi = high[1].process(high[0].process(x)); }
    void reset() { for (auto& f : low) f.reset(); for (auto& f : high) f.reset(); }
};

class DrumBuss : public DeviceProcessor {
public:
    explicit DrumBuss(float rate) : m_rate(rate) {
        (void)taps(); // prepare the shared FIR on the UI thread
        for (int c = 0; c < 2; ++c) {
            m_crunchLow[c].setLowpass(350, rate * 4); m_hardLow[c].setLowpass(160, rate * 4);
            m_high[c][0].highpass(100, .7071f, rate); m_high[c][1].highpass(100, .7071f, rate);
            m_split[c].prepare(rate); m_referenceSplit[c].prepare(rate);
        }
    }
    void process(float* L, float* R, int n, const float* p, const Context&) override {
        constexpr auto type = T::DrumBuss;
        // Physical targets are calculated once per block. Every gain/amount and mode
        // transition glides; resonator coefficients are interpolated at audio rate.
        const float trim = dbToGain(pv(type, p, db::Trim)), out = dbToGain(pv(type, p, db::Output));
        const float driveGain = dbToGain(p[db::Drive] * 24), driveComp = std::pow(driveGain, -.25f);
        const float crunchGain = 1 + p[db::Crunch] * 9;
        const float decay = p[db::Decay];
        const float seconds = .04f * std::pow(50.f, decay); // T60 40 ms .. 2 s
        const float freq = float(db::tunedFrequency(pv(type, p, db::Frequency), pb(p, db::ForceNote)));
        const float radius = std::exp(-6.907755f / (m_rate * seconds));
        const float angle = kTwoPi * freq / m_rate;
        const float rc = radius * std::cos(angle), rs = radius * std::sin(angle);
        const float smooth = onePoleCoef(.008f, m_rate), release = onePoleCoef(.085f, m_rate), attack = onePoleCoef(.002f, m_rate);
        const float fastAttack = onePoleCoef(.0005f, m_rate), fastRelease = onePoleCoef(.008f, m_rate), slowCoef = onePoleCoef(.035f, m_rate);
        const float lowHold = onePoleCoef(.7f, m_rate), gateRelease = onePoleCoef(seconds / 6.9f, m_rate);
        const float damp = std::min(pv(type, p, db::Damp), m_rate * .45f);
        Biquad dampTarget; dampTarget.lowpass(damp, .7071f, m_rate);
        const int mode = pc(type, p, db::Type);
        float bassPeak = 0, outPeak = 0, inputPeak = 0, reduction = 0;
        for (int i = 0; i < n; ++i) {
            const float dry[2] = {std::isfinite(L[i]) ? L[i] : 0.f, std::isfinite(R[i]) ? R[i] : 0.f};
            float reference[2]; for (int c = 0; c < 2; ++c) {
                float lo, hi; m_referenceSplit[c].run(m_dry[c].run(dry[c], [](float x) { return x; }), lo, hi); reference[c] = lo + hi;
            }
            const float inputGain = m_trim.next(trim, smooth), outputGain = m_output.next(out, smooth);
            float x[2] = {dry[0] * inputGain, dry[1] * inputGain};
            const float peak = std::max(std::abs(x[0]), std::abs(x[1])); inputPeak = std::max(inputPeak, peak);
            const float gr = compReduction(gainToDb(peak) + 18.f, 3.f, 6.f);
            m_compEnvelope = gr + (m_compEnvelope - gr) * (gr < m_compEnvelope ? attack : release);
            const float comp = m_comp.next(pb(p, db::Comp) ? 1.f : 0.f, smooth);
            const float cg = 1 + (dbToGain(m_compEnvelope + 3.f) - 1) * comp;
            reduction = std::max(reduction, -m_compEnvelope * comp); x[0] *= cg; x[1] *= cg;
            const float dg = m_driveGain.next(driveGain, smooth), dc = m_driveComp.next(driveComp, smooth), amount = m_drive.next(p[db::Drive], smooth);
            const float crunch = m_crunch.next(p[db::Crunch], smooth), cc = m_crunchGain.next(crunchGain, smooth);
            float weights[3]; for (int j = 0; j < 3; ++j) weights[j] = m_modes[j].next(mode == j ? 1.f : 0.f, smooth);
            const float drivenPeak = std::max(std::abs(x[0]), std::abs(x[1])) * dg;
            const float limit = drivenPeak > .85f ? .85f / drivenPeak : 1.f;
            m_limit = limit < m_limit ? limit : limit + (m_limit - limit) * onePoleCoef(.025f, m_rate);
            const float damping = m_dampMix.next(p[db::Damp] >= .999f ? 0.f : 1.f, smooth);
            const float coefficients[5] = {dampTarget.b0, dampTarget.b1, dampTarget.b2, dampTarget.a1, dampTarget.a2};
            float smoothed[5]; for (int k = 0; k < 5; ++k) smoothed[k] = m_dampCoefficients[k].next(coefficients[k], smooth);
            for (auto& f : m_damp) { f.b0 = smoothed[0]; f.b1 = smoothed[1]; f.b2 = smoothed[2]; f.a1 = smoothed[3]; f.a2 = smoothed[4]; }
            float wet[2];
            for (int c = 0; c < 2; ++c) {
                const float shaped = m_wet[c].run(x[c], [&](float v) {
                    const float low = m_hardLow[c].lp(v);
                    const float soft = std::tanh(v * dg) * dc;
                    const float medium = std::clamp(v * dg * m_limit, -.85f, .85f) * dc;
                    const float hard = std::clamp((v + low * .65f) * dg, -.85f, .85f) * dc;
                    const float driven = v + (soft * weights[0] + medium * weights[1] + hard * weights[2] - v) * amount;
                    const float lower = m_crunchLow[c].lp(driven), high = driven - lower;
                    const float crunched = std::sin(std::clamp(high * cc, -kPi * .5f, kPi * .5f)) / std::sqrt(cc);
                    return driven + (crunched - high) * crunch;
                });
                const float filtered = m_damp[c].process(shaped); wet[c] = shaped + (filtered - shaped) * damping;
            }
            float high[2]; for (int c = 0; c < 2; ++c) high[c] = m_high[c][1].process(m_high[c][0].process(wet[c]));
            const float detector = std::max(std::abs(high[0]), std::abs(high[1]));
            m_fast = detector + (m_fast - detector) * (detector > m_fast ? fastAttack : fastRelease);
            m_slow = m_fast + (m_slow - m_fast) * slowCoef;
            const float onset = std::clamp((m_fast - m_slow) / (m_slow + .004f), 0.f, 1.f);
            const float sustain = std::clamp(m_slow / (m_fast + .004f), 0.f, 1.f);
            const float trans = m_trans.next(pv(type, p, db::Transients), smooth);
            const float target = trans >= 0 ? 1 + trans * (.85f * onset + .55f * sustain) : 1 + (-trans) * (-.88f + 1.65f * onset);
            const float transientGain = m_transGain.next(target, onePoleCoef(.0005f, m_rate));
            for (int c = 0; c < 2; ++c) wet[c] += high[c] * (transientGain - 1);
            float low[2], upper[2]; for (int c = 0; c < 2; ++c) m_split[c].run(wet[c], low[c], upper[c]);
            const float lowLevel = std::max(std::abs(low[0]), std::abs(low[1]));
            m_lowFast = lowLevel + (m_lowFast - lowLevel) * (lowLevel > m_lowFast ? attack : fastRelease);
            m_lowPeak = std::max(m_lowFast, m_lowPeak * lowHold);
            const float decayAmount = m_decay.next(decay, smooth);
            const float lowRatio = std::clamp(m_lowFast / (m_lowPeak * .3f + .00001f), 0.f, 1.f);
            const float gateTarget = std::pow(lowRatio, 2 * (1 - decayAmount));
            m_gate = gateTarget + (m_gate - gateTarget) * (gateTarget > m_gate ? attack : gateRelease);
            for (int c = 0; c < 2; ++c) wet[c] = upper[c] + low[c] * m_gate;
            // A damped complex resonator excited by low-band changes. The derivative
            // rejects DC; radius sets T60, rotation sets pitch. Mono bass stays centered.
            const double a = m_rc.next(rc, smooth), b = m_rs.next(rs, smooth);
            const float input = (low[0] + low[1]) * .5f, impulse = input - m_previousLow; m_previousLow = input;
            const double real = a * m_real - b * m_imag + impulse;
            m_imag = b * m_real + a * m_imag; m_real = real;
            // Bound sustained resonant input without an unstable feedback clipper.
            const double magnitude = std::hypot(m_real, m_imag);
            if (magnitude > 8) { m_real *= 8 / magnitude; m_imag *= 8 / magnitude; }
            const float boom = m_boom.next(p[db::Boom], smooth);
            const float bass = std::tanh(float(m_imag) * 2.f) * .7f * boom;
            bassPeak = std::max(bassPeak, std::abs(bass));
            const float solo = m_solo.next(pb(p, db::BassSolo) ? 1.f : 0.f, smooth), mix = m_mix.next(p[db::Mix], smooth);
            for (int c = 0; c < 2; ++c) {
                float value = wet[c] + bass; value += (bass - value) * solo;
                value = (reference[c] + (value - reference[c]) * mix) * outputGain;
                if (!std::isfinite(value)) value = 0;
                (c ? R[i] : L[i]) = value; outPeak = std::max(outPeak, std::abs(value));
            }
        }
        meter[0].store(bassPeak, std::memory_order_relaxed); meter[1].store(outPeak, std::memory_order_relaxed); meter[2].store(inputPeak, std::memory_order_relaxed);
        scope[0].store(reduction, std::memory_order_relaxed); scope[1].store(freq, std::memory_order_relaxed);
    }
    void panic() override {
        for (auto& o : m_dry) o.reset(); for (auto& o : m_wet) o.reset();
        for (auto& f : m_crunchLow) f.z = 0; for (auto& f : m_hardLow) f.z = 0;
        for (auto& f : m_damp) f.reset(); for (auto& pair : m_high) for (auto& f : pair) f.reset();
        for (auto& f : m_split) f.reset(); for (auto& f : m_referenceSplit) f.reset();
        m_compEnvelope = m_fast = m_slow = m_lowFast = m_lowPeak = m_previousLow = 0;
        m_real = m_imag = 0; m_limit = m_gate = 1;
        m_transGain.primed = false;
        for (auto& m : meter) m.store(0, std::memory_order_relaxed); for (auto& s : scope) s.store(0, std::memory_order_relaxed);
    }
private:
    float m_rate, m_limit = 1, m_compEnvelope = 0, m_fast = 0, m_slow = 0, m_lowFast = 0, m_lowPeak = 0, m_gate = 1, m_previousLow = 0;
    double m_real = 0, m_imag = 0;
    Oversampler m_dry[2], m_wet[2];
    OnePole m_crunchLow[2], m_hardLow[2]; Biquad m_damp[2], m_high[2][2];
    BassSplit m_split[2], m_referenceSplit[2]; Smoothed m_dampCoefficients[5];
    Smoothed m_trim, m_output, m_comp, m_driveGain, m_driveComp, m_drive, m_crunch, m_crunchGain, m_modes[3], m_dampMix, m_trans, m_transGain, m_decay, m_boom, m_rc, m_rs, m_solo, m_mix;
};
}
std::shared_ptr<DeviceProcessor> createDrumBuss(float sr) { return std::make_shared<DrumBuss>(sr); }
}
