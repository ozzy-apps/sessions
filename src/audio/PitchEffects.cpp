#include "DeviceKit.h"
#include "Fourier.h"
#include "ProductionEffects.h"
#include <array>

namespace sessions::audio {
using namespace kit;
namespace {
constexpr int N = 2048, Hop = 256, Bins = N / 2 + 1;
struct Vocoder {
    Fourier<N> fft;
    std::array<float, N> input{}, overlap{}, window{};
    std::array<float, Hop> output{};
    std::array<float, Bins> lastPhase{}, phase{}, mag{}, frequency{}, synthMag{}, synthFreq{}, envelope{};
    std::array<float, Bins + 1> prefix{};
    std::array<std::complex<float>, N> work{};
    int at = N - Hop;
    Vocoder() {
        for (int i = 0; i < N; ++i)
            window[i] = .5f - .5f * std::cos(kTwoPi * i / N);
    }
    void reset() {
        input.fill(0);
        overlap.fill(0);
        output.fill(0);
        lastPhase.fill(0);
        phase.fill(0);
        at = N - Hop;
    }
    float sample(float x, float ratio, float formant, bool preserve) {
        input[at] = x;
        float y = output[at - (N - Hop)];
        if (++at < N)
            return y;
        at = N - Hop;
        for (int i = 0; i < N; ++i)
            work[i] = {input[i] * window[i], 0};
        fft.run(work.data());
        for (int k = 0; k < Bins; ++k) {
            mag[k] = std::abs(work[k]);
            float ph = std::arg(work[k]), delta = ph - lastPhase[k] - kTwoPi * Hop * k / N;
            lastPhase[k] = ph;
            delta -= kTwoPi * std::round(delta / kTwoPi);
            frequency[k] = k + delta * N / (kTwoPi * Hop);
            prefix[k + 1] = prefix[k] + std::log(std::max(1e-8f, mag[k]));
        }
        for (int k = 0; k < Bins; ++k) {
            int lo = std::max(0, k - 12), hi = std::min(Bins, k + 13);
            envelope[k] = (prefix[hi] - prefix[lo]) / float(hi - lo);
        }
        synthMag.fill(0);
        synthFreq.fill(0);
        for (int k = 0; k < Bins; ++k) {
            float dst = k * ratio;
            int j = int(dst);
            if (j >= Bins)
                break;
            float scale = 1;
            if (preserve || std::abs(formant - 1) > 1e-4f) {
                int envAt = std::clamp(int(dst / (preserve ? formant : ratio * formant)), 0, Bins - 1);
                scale = std::exp(std::clamp(envelope[envAt] - envelope[k], -3.f, 3.f));
            }
            float fraction = dst - j;
            for (int b = 0; b < 2; ++b)
                if (j + b < Bins) {
                    float weight = b ? fraction : 1 - fraction, amp = mag[k] * scale * weight;
                    synthMag[j + b] += amp;
                    synthFreq[j + b] += amp * frequency[k] * ratio;
                }
        }
        work.fill({});
        for (int k = 0; k < Bins; ++k) {
            float f = synthMag[k] > 1e-9f ? synthFreq[k] / synthMag[k] : float(k);
            phase[k] += kTwoPi * Hop * f / N;
            phase[k] = std::remainder(phase[k], kTwoPi);
            work[k] = std::polar(synthMag[k], phase[k]);
        }
        work[0] = {work[0].real(), 0};
        work[N / 2] = {work[N / 2].real(), 0};
        for (int k = 1; k < N / 2; ++k)
            work[N - k] = std::conj(work[k]);
        fft.run(work.data(), true);
        for (int i = 0; i < N; ++i)
            overlap[i] += work[i].real() * window[i] / 3.f;
        std::copy_n(overlap.begin(), Hop, output.begin());
        std::move(overlap.begin() + Hop, overlap.end(), overlap.begin());
        std::fill(overlap.end() - Hop, overlap.end(), 0);
        std::move(input.begin() + Hop, input.end(), input.begin());
        return y;
    }
};
class Detector {
    std::array<float, 1024> ring{};
    std::array<float, 512> difference{};
    int head = 0, decimate = 0, count = 0;
    float sum = 0, sr;

  public:
    float hz = 0, confidence = 0;
    explicit Detector(float rate) : sr(rate / 3) {}
    void reset() {
        ring.fill(0);
        head = decimate = count = 0;
        sum = hz = confidence = 0;
    }
    void push(float x) {
        sum += x;
        if (++decimate < 3)
            return;
        ring[head] = sum / 3;
        sum = 0;
        decimate = 0;
        head = (head + 1) % 1024;
        if (++count < 128)
            return;
        count = 0;
        int maxLag = std::min(510, int(sr / 55)), minLag = std::max(2, int(sr / 1200));
        float energy = 0;
        for (int i = 0; i < 512; ++i) {
            float a = ring[(head + 1024 - 512 + i) % 1024];
            energy += a * a;
        }
        if (energy < 1e-5f) {
            hz = confidence = 0;
            return;
        }
        float running = 0;
        int chosen = 0;
        for (int lag = 1; lag <= maxLag; ++lag) {
            double v = 0;
            for (int i = 0; i < 512; ++i) {
                float a = ring[(head + 1024 - 512 + i) % 1024], b = ring[(head + 1024 - 512 + i - lag + 1024) % 1024];
                v += (a - b) * (a - b);
            }
            running += float(v);
            difference[lag] = running > 1e-12f ? float(v) * lag / running : 1;
            if (lag > minLag && difference[lag - 1] < .15f && difference[lag] > difference[lag - 1]) {
                chosen = lag - 1;
                break;
            }
        }
        if (!chosen) {
            hz = confidence = 0;
            return;
        }
        float a = difference[chosen - 1], b = difference[chosen], c = difference[chosen + 1], den = a - 2 * b + c;
        float offset = std::abs(den) > 1e-9f ? .5f * (a - c) / den : 0;
        hz = sr / (chosen + std::clamp(offset, -.5f, .5f));
        confidence = 1 - b;
    }
};
class PitchEffect final : public DeviceProcessor {
    T type;
    float sr;
    Vocoder vocoder[2];
    Detector detector;
    DelayLine dry[2];
    std::array<float, 129> history[2]{}, hilbert{};
    int hilbertAt = 0;
    float phase = 0, correction = 0;
    Smoothed out;

  public:
    PitchEffect(T t, float rate) : type(t), sr(rate), detector(rate) {
        for (auto& d : dry)
            d.init(N + 8);
        for (int i = 0; i < 129; ++i) {
            int k = i - 64;
            hilbert[i] = (k && k % 2) ? 2.f / (kPi * k) * (.54f - .46f * std::cos(kTwoPi * i / 128)) : 0;
        }
    }
    void process(float* l, float* r, int n, const float* p, const Context&) override {
        using namespace pitchfx;
        const int mode = pc(type, p, Mode);
        const float shift = pv(type, p, Pitch) + pv(type, p, Fine) / 100,
                    formant = std::exp2(pv(type, p, Formant) / 12), mix = p[Mix], gain = dbToGain(pv(type, p, Output)),
                    a = onePoleCoef(pv(type, p, Retune) * .001f, sr);
        bool preserve = pb(p, Preserve);
        for (int i = 0; i < n; ++i) {
            float original[2] = {l[i], r[i]}, wet[2];
            if (mode == 0 && p[Strength] > 0)
                detector.push((l[i] + r[i]) * .5f);
            float want = 0;
            if (mode == 0 && p[Strength] > 0 && detector.hz > 0) {
                float note = 69 + 12 * std::log2(detector.hz / 440), nearest = std::round(note);
                int root = pc(type, p, Root), scale = pc(type, p, Scale);
                float distance = 100;
                for (int step = -12; step <= 12; ++step) {
                    int key = int(std::round(note)) + step, degree = ((key - root) % 12 + 12) % 12;
                    bool in = scale == 0 ||
                              (scale == 1 && (degree == 0 || degree == 2 || degree == 4 || degree == 5 || degree == 7 ||
                                              degree == 9 || degree == 11)) ||
                              (scale == 2 && (degree == 0 || degree == 2 || degree == 3 || degree == 5 || degree == 7 ||
                                              degree == 8 || degree == 10)) ||
                              (scale == 3 && (degree == 0 || degree == 2 || degree == 4 || degree == 7 || degree == 9));
                    if (in && std::abs(note - key) < distance) {
                        distance = std::abs(note - key);
                        nearest = key;
                    }
                }
                want = (nearest - note) * p[Strength];
            }
            correction = want + (correction - want) * a;
            float ratio = std::exp2((shift + correction) / 12);
            if (mode == 0) {
                for (int c = 0; c < 2; ++c) {
                    wet[c] = vocoder[c].sample(original[c], ratio, formant, preserve);
                    dry[c].push(original[c]);
                    original[c] = dry[c].read(float(N));
                }
            } else {
                float hz = pv(type, p, Frequency);
                phase += hz / sr;
                phase -= std::floor(phase);
                for (int c = 0; c < 2; ++c) {
                    history[c][hilbertAt] = original[c];
                    float real = history[c][(hilbertAt + 129 - 64) % 129];
                    if (mode == 2)
                        wet[c] = real * std::cos(kTwoPi * phase);
                    else {
                        float imaginary = 0;
                        for (int k = 0; k < 129; ++k)
                            imaginary += hilbert[k] * history[c][(hilbertAt + 129 - k) % 129];
                        wet[c] = real * std::cos(kTwoPi * phase) - imaginary * std::sin(kTwoPi * phase);
                    }
                    original[c] = real;
                }
                hilbertAt = (hilbertAt + 1) % 129;
            }
            float g = out.next(gain, onePoleCoef(.01f, sr));
            l[i] = (original[0] + (wet[0] - original[0]) * mix) * g;
            r[i] = (original[1] + (wet[1] - original[1]) * mix) * g;
        }
        scope[0].store(detector.hz, std::memory_order_relaxed);
        scope[1].store(correction, std::memory_order_relaxed);
        scope[2].store(detector.confidence, std::memory_order_relaxed);
    }
    void panic() override {
        for (auto& v : vocoder)
            v.reset();
        detector.reset();
        for (auto& d : dry)
            d.clear();
        for (auto& h : history)
            h.fill(0);
        phase = correction = 0;
        out.primed = false;
    }
};
} // namespace
std::shared_ptr<DeviceProcessor> createPitchEffect(T t, float sr) {
    if (t == T::Shifter || t == T::PitchCorrector)
        return std::make_shared<PitchEffect>(t, sr);
    return {};
}
} // namespace sessions::audio
