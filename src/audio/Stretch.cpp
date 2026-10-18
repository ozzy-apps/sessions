#include "Stretch.h"
#include "Resample.h"
#include <algorithm>
#include <cmath>

namespace sessions::audio {

namespace {

// Radix-2 complex FFT of size Stretcher::kFft, tables made once (the first Stretcher is made on
// the UI thread, in the Engine's constructor, which touches them).
struct Fft {
    static constexpr int N = Stretcher::kFft;
    std::vector<std::complex<float>> tw;
    std::vector<int> rev;
    Fft() : tw(N / 2), rev(N) {
        for (int i = 0; i < N / 2; ++i) tw[size_t(i)] = std::polar(1.0f, float(-2.0 * M_PI * i / N));
        int bits = 0;
        while ((1 << bits) < N) ++bits;
        for (int i = 0; i < N; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[size_t(i)] = r;
        }
    }
    void forward(std::complex<float>* a) const {
        for (int i = 0; i < N; ++i) if (i < rev[size_t(i)]) std::swap(a[i], a[rev[size_t(i)]]);
        for (int len = 2; len <= N; len <<= 1) {
            const int step = N / len;
            for (int i = 0; i < N; i += len)
                for (int k = 0; k < len / 2; ++k) {
                    const std::complex<float> t = a[i + k + len / 2] * tw[size_t(k * step)];
                    a[i + k + len / 2] = a[i + k] - t;
                    a[i + k] += t;
                }
        }
    }
    void inverse(std::complex<float>* a) const {   // unscaled: divide by N
        for (int i = 0; i < N; ++i) a[i] = std::conj(a[i]);
        forward(a);
        for (int i = 0; i < N; ++i) a[i] = std::conj(a[i]);
    }
};

const Fft& fft() {
    static const Fft f;
    return f;
}

inline float at(const std::vector<float>& x, long i) { return i >= 0 && i < long(x.size()) ? x[size_t(i)] : 0.0f; }

// All DSP positions use the output clock. Files keep their native rate, so every
// read (including an FFT window's neighbouring samples) must convert that clock.
inline float readSource(const std::vector<float>& x, double pos, double sourceStep, const resample::Plan& plan) {
    return resample::read(x, pos * sourceStep, plan);
}

inline float wrapPhase(float p) {
    p = std::fmod(p + float(M_PI), float(2 * M_PI));
    if (p < 0) p += float(2 * M_PI);
    return p - float(M_PI);
}

constexpr float kTransientFlux = 0.35f;  // positive flux as a fraction of the current spectrum
constexpr int   kPeakReach = 2;          // a bin is a peak when it tops its neighbours this far either side

} // namespace

Stretcher::Stretcher()
    : m_spec(kFft), m_cep(kFft), m_win(kFft), m_weight(kBins), m_peak(kBins, -1) {
    for (int i = 0; i < kFft; ++i) m_win[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * i / kFft));
    for (int c = 0; c < 2; ++c) {
        m_ring[c].assign(kRing, 0.0f);
        m_phasePrev[c].assign(kBins, 0.0f);
        m_phaseSyn[c].assign(kBins, 0.0f);
        m_magPrev[c].assign(kBins, 0.0f);
        m_mag[c].assign(kBins, 0.0f);
        m_phase[c].assign(kBins, 0.0f);
        m_env[c].assign(kBins, 0.0f);
    }
    fft();
    resample::prepare();
    reset();
}

void Stretcher::reset() {
    m_active = false;
    m_nGrains = 0;
    m_lastGrain = -1;
    m_first = true;
    m_transitionLeft = 0;
}

void Stretcher::copyPrepared(const Stretcher& p) {
    if (&p == this) return;
    m_active = p.m_active; m_local = p.m_local; m_t = p.m_t; m_expectBeat = p.m_expectBeat;
    m_rng = p.m_rng; m_sample = p.m_sample; m_mode = p.m_mode; m_rate = p.m_rate;
    m_direct = p.m_direct; m_previousL = p.m_previousL; m_previousR = p.m_previousR;
    m_transitionL = p.m_transitionL; m_transitionR = p.m_transitionR; m_transitionLeft = p.m_transitionLeft;
    std::copy(std::begin(p.m_grains), std::end(p.m_grains), std::begin(m_grains));
    m_nGrains = p.m_nGrains; m_nextGrainT = p.m_nextGrainT; m_lastGrain = p.m_lastGrain;
    auto copy = [](const auto& from, auto& to) { std::copy(from.begin(), from.end(), to.begin()); };
    for (int c = 0; c < 2; ++c) {
        copy(p.m_ring[c], m_ring[c]); copy(p.m_phasePrev[c], m_phasePrev[c]);
        copy(p.m_phaseSyn[c], m_phaseSyn[c]); copy(p.m_magPrev[c], m_magPrev[c]);
    }
    // FFT/cepstral scratch, the current frame's temporary magnitudes/phases,
    // and peak/formant scratch are overwritten before use. Hann windows are
    // identical in every constructor. Only carry history and the output ring.
    m_nextFrame = p.m_nextFrame; m_lastPos = p.m_lastPos; m_readPos = p.m_readPos;
    m_first = p.m_first; m_transientHold = p.m_transientHold;
}

void Stretcher::prime(const StretchSource& s, double local, float rate) {
    if (!s.sample || s.sample->left.empty() || rate <= 0 || s.sample->rate <= 0
        || !std::isfinite(local) || !std::isfinite(s.secPerBeat) || s.secPerBeat <= 0
        || !std::isfinite(s.ratio) || s.ratio <= 0) { reset(); return; }
    restart(s, local, rate);
    const double beatPerSample = 1.0 / (rate * s.secPerBeat);
    const double seconds = s.secondsAt(local);
    const double slope = (s.secondsAt(local + beatPerSample) - seconds) * rate;
    m_direct = s.mode == StretchMode::None || (std::abs(s.ratio - 1.0) < 1e-9
        && std::abs(slope - 1.0) < 1e-7 && (s.mode != StretchMode::Texture || s.flux <= 1e-9));
    m_previousL = m_previousR = 0;
    if (m_direct) return;
    if (s.mode == StretchMode::Tones || s.mode == StretchMode::Texture) {
        while (m_nextGrainT <= m_t + 1e-12) spawnGrain(s, rate);
    } else {
        while (m_nextFrame <= long(std::floor(m_readPos)) + 2) frame(s, rate);
    }
}

float Stretcher::random() {
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return float(m_rng & 0xffffff) / float(0x1000000);
}

void Stretcher::restart(const StretchSource& s, double local, float rate) {
    m_active = true;
    m_sample = s.sample;
    m_mode = s.mode;
    m_rate = rate;
    m_direct = false;
    m_transitionLeft = 0;
    m_local = local;
    m_t = 0;
    m_expectBeat = local;
    m_nGrains = 0;
    m_lastGrain = -1;
    m_nextGrainT = 0;
    m_first = true;
    m_transientHold = 0;
    m_readPos = kFft / 2;
    m_nextFrame = -kHop;      // earliest frame with nonzero weight at the first output sample
    m_lastPos = s.secondsAt(local) * rate;
    for (int c = 0; c < 2; ++c) {
        std::fill(m_ring[c].begin(), m_ring[c].end(), 0.0f);
        std::fill(m_magPrev[c].begin(), m_magPrev[c].end(), 0.0f);
    }
    if (s.mode == StretchMode::Tones || s.mode == StretchMode::Texture) {
        // Prime the overlap with the second half of a grain. The first sample
        // now has full weight instead of a grain-size-long fade from silence.
        const int len = std::max(64, int(std::clamp(s.grainSec, 0.012, 0.256) * rate));
        const int half = len / 2;
        m_grains[0] = {s.secondsAt(local) * rate, half, len};
        m_nGrains = 1;
        m_lastGrain = 0;
    }
}

void Stretcher::sample(const StretchSource& s, double local, float rate, float& l, float& r) {
    l = r = 0;
    if (!s.sample || s.sample->left.empty() || rate <= 0 || s.sample->rate <= 0
        || !std::isfinite(local) || !std::isfinite(s.secPerBeat) || s.secPerBeat <= 0
        || !std::isfinite(s.ratio) || s.ratio <= 0) return;
    const double beatPerSample = 1.0 / (rate * s.secPerBeat);
    // Compare the local map slope with the output clock. Neutral warping should
    // be transparent, including the attack and stereo image, rather than going
    // through a grain/FFT process that needlessly changes unwarped audio.
    const double seconds = s.secondsAt(local);
    const double slope = (s.secondsAt(local + beatPerSample) - seconds) * rate;
    const bool direct = s.mode == StretchMode::None || (std::abs(s.ratio - 1.0) < 1e-9
        && std::abs(slope - 1.0) < 1e-7 && (s.mode != StretchMode::Texture || s.flux <= 1e-9));
    const bool switchBranch = m_active && m_sample == s.sample && m_mode == s.mode && m_rate == rate
        && m_direct != direct && std::abs(local - m_expectBeat) <= beatPerSample * 1.5;
    if (!m_active || m_sample != s.sample || m_mode != s.mode || m_rate != rate
        || m_direct != direct || std::abs(local - m_expectBeat) > beatPerSample * 1.5) restart(s, local, rate);
    m_direct = direct;
    m_local = local;
    if (direct) {
        const auto plan = resample::plan(s.sample->rate / rate);
        l = resample::read(s.sample->left, seconds * s.sample->rate, plan);
        r = resample::read(s.sample->right, seconds * s.sample->rate, plan);
    } else if (s.mode == StretchMode::Tones || s.mode == StretchMode::Texture) granular(s, rate, l, r);
    else vocoder(s, rate, l, r);
    // A piecewise map can enter/leave a transparent segment mid-note. Both
    // renderers are valid there but have different phases; remove their branch
    // discontinuity over 32 samples without fading a newly launched attack.
    if (switchBranch) {
        m_transitionL = m_previousL - l; m_transitionR = m_previousR - r;
        m_transitionLeft = 32;
    }
    if (m_transitionLeft > 0) {
        const float w = float(m_transitionLeft) / 32;
        l += m_transitionL * w; r += m_transitionR * w;
        --m_transitionLeft;
    }
    m_previousL = l; m_previousR = r;
    m_t += 1.0 / rate;
    m_expectBeat = local + beatPerSample;
}

// ---- Tones / Texture ---------------------------------------------------------------------

void Stretcher::spawnGrain(const StretchSource& s, float rate) {
    const SampleData& smp = *s.sample;
    const bool texture = s.mode == StretchMode::Texture;
    double grainSec = std::clamp(s.grainSec, 0.012, 0.256);
    if (texture) grainSec *= 1.0 + s.flux * (random() - 0.5);   // ± half the flux
    const int len = std::max(64, int(grainSec * rate));
    const double beat = m_local + (m_nextGrainT - m_t) / s.secPerBeat;
    double src = s.secondsAt(beat) * rate;   // where this grain should start, in frames
    if (texture) src += s.flux * grainSec * rate * (random() * 2 - 1);
    else if (m_lastGrain >= 0) {
        // Tones: line the grain up with the last one's continuation (WSOLA): the start that
        // correlates best with where the last grain would be now, within a quarter grain
        const Grain& last = m_grains[m_lastGrain];
        const double cont = last.src;
        const int W = std::min(len / 2, 512), S = std::max(8, len / 4);
        auto score = [&](double delta) {
            double num = 0, xx = 1e-12, yy = 1e-12;
            const double sourceStep = smp.rate / rate;
            for (int i = 0; i < W; i += 2) {
                const long a = long((cont + i * s.ratio) * sourceStep), b = long((src + delta + i * s.ratio) * sourceStep);
                // Correlate the channels separately. Summing stereo first loses
                // the signal altogether for wide / opposite-polarity material.
                const float xl = at(smp.left, a), xr = at(smp.right, a);
                const float yl = at(smp.left, b), yr = at(smp.right, b);
                num += double(xl) * yl + double(xr) * yr;
                xx += double(xl) * xl + double(xr) * xr;
                yy += double(yl) * yl + double(yr) * yr;
            }
            // A small distance penalty resolves periodic/silent ties towards
            // the warp map instead of wandering arbitrarily between grains.
            return num / std::sqrt(xx * yy) - 0.002 * std::abs(delta) / S;
        };
        double best = 0, bestScore = score(0);
        for (int d = -S; d <= S; d += 8) { const double sc = score(d); if (sc > bestScore) { bestScore = sc; best = d; } }
        const int coarse = int(best);
        for (int d = std::max(-S, coarse - 7); d <= std::min(S, coarse + 7); ++d) { const double sc = score(d); if (sc > bestScore) { bestScore = sc; best = d; } }
        src += best;
    }
    int slot = m_nGrains < kMaxGrains ? m_nGrains++ : 0;
    if (slot == 0 && m_nGrains == kMaxGrains) {   // full: replace the oldest
        for (int k = 1; k < kMaxGrains; ++k) if (m_grains[k].age > m_grains[slot].age) slot = k;
    }
    m_grains[slot] = {src, 0, len};
    m_lastGrain = slot;
    m_nextGrainT += 0.5 * len / rate;   // half a grain on: Hann windows then sum to one
}

void Stretcher::granular(const StretchSource& s, float rate, float& l, float& r) {
    const SampleData& smp = *s.sample;
    const double sourceStep = smp.rate / rate;
    const auto plan = resample::plan(sourceStep * s.ratio);
    while (m_nextGrainT <= m_t + 1e-12) spawnGrain(s, rate);
    float sumL = 0, sumR = 0, wsum = 0;
    for (int k = 0; k < m_nGrains; ++k) {
        Grain& g = m_grains[k];
        if (g.age >= g.len) continue;
        const float w = float(0.5 - 0.5 * std::cos(2.0 * M_PI * g.age / g.len));
        const double pos = g.src;
        sumL += w * readSource(smp.left, pos, sourceStep, plan);
        sumR += w * readSource(smp.right, pos, sourceStep, plan);
        wsum += w;
        g.src += s.ratio;
        ++g.age;
    }
    // drop the grains that ended (keeping the newest one's slot known)
    for (int k = 0; k < m_nGrains;) {
        if (m_grains[k].age >= m_grains[k].len && k != m_lastGrain) {
            m_grains[k] = m_grains[m_nGrains - 1];
            if (m_lastGrain == m_nGrains - 1) m_lastGrain = k;
            --m_nGrains;
        } else ++k;
    }
    const float norm = 1.0f / std::max(wsum, 1e-6f);   // normalize varying Texture windows without pumping
    l = sumL * norm;
    r = sumR * norm;
}

// ---- Complex / Complex Pro ---------------------------------------------------------------

void Stretcher::frame(const StretchSource& s, float rate) {
    const SampleData& smp = *s.sample;
    const Fft& f = fft();
    const bool pro = s.mode == StretchMode::ComplexPro;
    // where this frame's centre is in the clip: output time, then the warp map
    const double tau = (m_nextFrame + kFft / 2 - m_readPos) / (rate * s.ratio);
    const double beat = m_local + tau / s.secPerBeat;
    const double sourceStep = smp.rate / rate;
    const auto plan = resample::plan(sourceStep);
    // Integer analysis hops are sufficient at matching file/device rates. Use
    // the actual rounded hop for phase estimation, and avoid thousands of cubic
    // interpolations at every FFT on the common live-playback path.
    const double mappedPos = s.secondsAt(beat) * rate;
    const double pos = sourceStep == 1 ? std::round(mappedPos) : mappedPos;
    const double a0 = pos - kFft / 2;
    // both channels in one transform: left in the real part, right in the imaginary
    if (sourceStep == 1) {
        const long start = long(a0);
        for (int i = 0; i < kFft; ++i) m_spec[size_t(i)] = {at(smp.left, start + i) * m_win[size_t(i)], at(smp.right, start + i) * m_win[size_t(i)]};
    } else {
        for (int i = 0; i < kFft; ++i) m_spec[size_t(i)] = {readSource(smp.left, a0 + i, sourceStep, plan) * m_win[size_t(i)],
                                                         readSource(smp.right, a0 + i, sourceStep, plan) * m_win[size_t(i)]};
    }
    f.forward(m_spec.data());
    for (int k = 0; k < kBins; ++k) {
        const std::complex<float> z = m_spec[size_t(k)], zc = std::conj(m_spec[size_t((kFft - k) % kFft)]);
        const std::complex<float> xl = 0.5f * (z + zc), xr = std::complex<float>(0, -0.5f) * (z - zc);
        m_mag[0][size_t(k)] = std::abs(xl); m_phase[0][size_t(k)] = std::arg(xl);
        m_mag[1][size_t(k)] = std::abs(xr); m_phase[1][size_t(k)] = std::arg(xr);
    }
    const double dpos = pos - m_lastPos;   // the analysis hop, in frames (any size: the warp map sets it)
    // Detect onsets using both channels, once for the stereo pair. The previous
    // 200%-of-previous threshold missed most drum attacks in a running mix.
    bool fresh = m_first;
    if (pro && !fresh && m_transientHold == 0) {
        double grow = 0, now = 1e-9, was = 1e-9;
        for (int k = 1; k < kBins; ++k) {
            const float a = m_mag[0][size_t(k)] + m_mag[1][size_t(k)];
            const float b = m_magPrev[0][size_t(k)] + m_magPrev[1][size_t(k)];
            grow += std::max(0.0f, a - b); now += a; was += b;
        }
        fresh = grow > kTransientFlux * now && now > was * 1.2;
        if (fresh) m_transientHold = 2;
    } else if (m_transientHold > 0) --m_transientHold;
    for (int c = 0; c < 2; ++c) {
        std::vector<float>& mag = m_mag[c];
        std::vector<float>& ph = m_phase[c];
        std::vector<float>& syn = m_phaseSyn[c];
        if (fresh) {
            for (int k = 0; k < kBins; ++k) syn[size_t(k)] = ph[size_t(k)];
        } else {
            for (int k = 0; k < kBins; ++k) {
                const float omega = float(2.0 * M_PI * k / kFft);   // radians per sample
                float inst = omega;
                if (std::abs(dpos) >= 0.5) {
                    const float dev = wrapPhase(ph[size_t(k)] - m_phasePrev[c][size_t(k)] - omega * float(dpos));
                    inst = omega + dev / float(dpos);
                }
                syn[size_t(k)] = wrapPhase(syn[size_t(k)] + inst * kHop);
            }
        }
        for (int k = 0; k < kBins; ++k) { m_phasePrev[c][size_t(k)] = ph[size_t(k)]; m_magPrev[c][size_t(k)] = mag[size_t(k)]; }
    }
    // One set of spectral regions for the pair, identity phase locking within
    // each region, and the original inter-channel phase difference at every
    // bin. Independent phase accumulators can otherwise destroy the stereo
    // image even when both inputs contain exactly the same signal.
    auto stereoMag = [&](int k) { return m_mag[0][size_t(k)] + m_mag[1][size_t(k)]; };
    int lastPeak = -1;
    for (int k = 0; k < kBins; ++k) {
        const float mag = stereoMag(k);
        bool isPeak = mag > 1e-6f;
        for (int d = -kPeakReach; isPeak && d <= kPeakReach; ++d)
            if (d && k + d >= 0 && k + d < kBins && stereoMag(k + d) > mag) isPeak = false;
        m_peak[size_t(k)] = isPeak ? k : -1;
        if (!isPeak) continue;
        for (int j = lastPeak + 1; j < k; ++j) m_peak[size_t(j)] = lastPeak < 0 || k - j <= j - lastPeak ? k : lastPeak;
        lastPeak = k;
    }
    for (int j = lastPeak + 1; j < kBins; ++j) m_peak[size_t(j)] = lastPeak;
    for (int k = 0; k < kBins; ++k) {
        const int p = m_peak[size_t(k)];
        if (p < 0) continue;
        const int reference = m_mag[0][size_t(p)] >= m_mag[1][size_t(p)] ? 0 : 1;
        for (int c = 0; c < 2; ++c) {
            if (k == p && c == reference) continue;
            m_phaseSyn[c][size_t(k)] = wrapPhase(m_phaseSyn[reference][size_t(p)]
                + m_phase[c][size_t(k)] - m_phase[reference][size_t(p)]);
        }
    }
    // Complex Pro's Formants: the spectral envelope (the cepstrum, liftered) put back where the transposition moved it from
    const bool formants = pro && s.formants > 0.001 && std::abs(s.ratio - 1) > 1e-6;
    if (formants) {
        for (int k = 0; k < kFft; ++k) {
            const int b = k < kBins ? k : kFft - k;
            m_cep[size_t(k)] = {std::log(m_mag[0][size_t(b)] + 1e-7f), std::log(m_mag[1][size_t(b)] + 1e-7f)};
        }
        f.forward(m_cep.data());
        const int lifter = std::clamp(int(rate / 800), 24, 120);   // keeps the envelope, drops the harmonics' ripple
        for (int k = 0; k < kFft; ++k) { const int q = k < kFft / 2 ? k : kFft - k; if (q > lifter) m_cep[size_t(k)] = 0; }
        f.inverse(m_cep.data());
        for (int k = 0; k < kBins; ++k) { m_env[0][size_t(k)] = m_cep[size_t(k)].real() / kFft; m_env[1][size_t(k)] = m_cep[size_t(k)].imag() / kFft; }
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < kBins; ++k) {
                const double kk = std::min(double(kBins - 1), k * s.ratio);   // what plays at bin k after the resampling comes from here
                const int i0 = int(kk); const float t = float(kk - i0);
                const float env = m_env[c][size_t(i0)] * (1 - t) + m_env[c][size_t(std::min(kBins - 1, i0 + 1))] * t;
                m_weight[size_t(k)] = std::exp(float(s.formants) * (env - m_env[c][size_t(k)]));
                m_mag[c][size_t(k)] *= std::clamp(m_weight[size_t(k)], 0.05f, 20.0f);
            }
    }
    // synthesis: both channels packed again, one inverse transform
    for (int k = 0; k < kBins; ++k) {
        // Pitch-up resampling must discard frequencies above the new Nyquist,
        // with a short transition band instead of folding them into the bass.
        if (s.ratio > 1) {
            const double limit = (kBins - 1.0) / s.ratio;
            const float aa = float(std::clamp((limit - k) / 8.0, 0.0, 1.0));
            m_mag[0][size_t(k)] *= aa; m_mag[1][size_t(k)] *= aa;
        }
        const std::complex<float> yl = std::polar(m_mag[0][size_t(k)], m_phaseSyn[0][size_t(k)]);
        const std::complex<float> yr = std::polar(m_mag[1][size_t(k)], m_phaseSyn[1][size_t(k)]);
        m_spec[size_t(k)] = yl + std::complex<float>(0, 1) * yr;
        if (k > 0 && k < kFft / 2) m_spec[size_t(kFft - k)] = std::conj(yl) + std::complex<float>(0, 1) * std::conj(yr);
    }
    f.inverse(m_spec.data());
    // overlap-add into the ring: the last hop of this frame's span is fresh (stale data from long ago goes first)
    const long base = m_nextFrame + 64L * kFft;   // kept positive for the modulo
    for (int i = kFft - kHop; i < kFft; ++i) { const size_t j = size_t((base + i) & (kRing - 1)); m_ring[0][j] = 0; m_ring[1][j] = 0; }
    const float norm = 1.0f / (1.5f * kFft);   // the inverse transform's size, and Hann² summed over the 4 overlaps
    for (int i = 0; i < kFft; ++i) {
        const size_t j = size_t((base + i) & (kRing - 1));
        const float w = m_win[size_t(i)] * norm;
        m_ring[0][j] += m_spec[size_t(i)].real() * w;
        m_ring[1][j] += m_spec[size_t(i)].imag() * w;
    }
    m_lastPos = pos;
    m_first = false;
    m_nextFrame += kHop;
}

void Stretcher::vocoder(const StretchSource& s, float rate, float& l, float& r) {
    const double m = m_readPos;
    const long mi = long(std::floor(m));
    while (m_nextFrame <= mi + 2) frame(s, rate);   // every frame that touches the samples the interpolation reads
    const float t = float(m - double(mi));
    const long base = mi + 64L * kFft;
    auto rd = [&](int c, long i) { return m_ring[c][size_t((base + i) & (kRing - 1))]; };
    for (int c = 0; c < 2; ++c) {
        const float y0 = rd(c, -1), y1 = rd(c, 0), y2 = rd(c, 1), y3 = rd(c, 2);
        const float v = y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
        (c ? r : l) = v;
    }
    m_readPos += s.ratio;
}

} // namespace sessions::audio
