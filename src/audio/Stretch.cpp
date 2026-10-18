#include "Stretch.h"
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

inline float readCubic(const std::vector<float>& x, double pos) {
    const long i = long(std::floor(pos));
    const float t = float(pos - double(i));
    const float y0 = at(x, i - 1), y1 = at(x, i), y2 = at(x, i + 1), y3 = at(x, i + 2);
    return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
}

inline float wrapPhase(float p) {
    p = std::fmod(p + float(M_PI), float(2 * M_PI));
    if (p < 0) p += float(2 * M_PI);
    return p - float(M_PI);
}

constexpr float kTransientFlux = 2.0f;   // Complex Pro: the spectrum grew by this much of itself between frames: a hit
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
    reset();
}

void Stretcher::reset() {
    m_active = false;
    m_nGrains = 0;
    m_lastGrain = -1;
    m_first = true;
}

float Stretcher::random() {
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    return float(m_rng & 0xffffff) / float(0x1000000);
}

void Stretcher::restart(const StretchSource& s, double local, float rate) {
    m_active = true;
    m_beat0 = local;
    m_t = 0;
    m_expectBeat = local;
    m_nGrains = 0;
    m_lastGrain = -1;
    m_nextGrainT = 0;
    m_first = true;
    m_nextFrame = -2 * kHop;   // two frames of pre-roll, so the first output samples have their full overlap
    m_lastPos = s.secondsAt(local) * rate;
    for (int c = 0; c < 2; ++c) {
        std::fill(m_ring[c].begin(), m_ring[c].end(), 0.0f);
        std::fill(m_magPrev[c].begin(), m_magPrev[c].end(), 0.0f);
    }
}

void Stretcher::sample(const StretchSource& s, double local, float rate, float& l, float& r) {
    l = r = 0;
    if (!s.sample || s.sample->left.empty() || rate <= 0) return;
    const double beatPerSample = 1.0 / (rate * s.secPerBeat);
    if (!m_active || std::abs(local - m_expectBeat) > beatPerSample * 0.75) restart(s, local, rate);
    if (s.mode == StretchMode::Tones || s.mode == StretchMode::Texture) granular(s, rate, l, r);
    else vocoder(s, rate, l, r);
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
    const double beat = m_beat0 + m_nextGrainT / s.secPerBeat;
    double src = s.secondsAt(beat) * rate;   // where this grain should start, in frames
    if (texture) src += s.flux * grainSec * rate * (random() * 2 - 1);
    else if (m_lastGrain >= 0) {
        // Tones: line the grain up with the last one's continuation (WSOLA): the start that
        // correlates best with where the last grain would be now, within a quarter grain
        const Grain& last = m_grains[m_lastGrain];
        const double cont = last.src + last.age * s.ratio;
        const int W = std::min(len / 2, 512), S = std::max(8, len / 4);
        auto score = [&](double delta) {
            double num = 0, den = 1e-9;
            for (int i = 0; i < W; i += 2) {
                const long a = long(cont) + long(i * s.ratio), b = long(src + delta) + long(i * s.ratio);
                const float x = at(smp.left, a) + at(smp.right, a), y = at(smp.left, b) + at(smp.right, b);
                num += double(x) * y;
                den += double(y) * y;
            }
            return num / std::sqrt(den);
        };
        double best = 0, bestScore = score(0);
        for (int d = -S; d <= S; d += 8) { const double sc = score(d); if (sc > bestScore) { bestScore = sc; best = d; } }
        for (int d = int(best) - 7; d <= int(best) + 7; ++d) { const double sc = score(d); if (sc > bestScore) { bestScore = sc; best = d; } }
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
    while (m_nextGrainT <= m_t + 1e-12) spawnGrain(s, rate);
    float sumL = 0, sumR = 0, wsum = 0;
    for (int k = 0; k < m_nGrains; ++k) {
        Grain& g = m_grains[k];
        if (g.age >= g.len) continue;
        const float w = float(0.5 - 0.5 * std::cos(2.0 * M_PI * g.age / g.len));
        const double pos = g.src + g.age * s.ratio;
        sumL += w * readCubic(smp.left, pos);
        sumR += w * readCubic(smp.right, pos);
        wsum += w;
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
    const float norm = 1.0f / std::max(wsum, 0.5f);   // Texture's uneven grains: level evens out
    l = sumL * norm;
    r = sumR * norm;
}

// ---- Complex / Complex Pro ---------------------------------------------------------------

void Stretcher::frame(const StretchSource& s, float rate) {
    const SampleData& smp = *s.sample;
    const Fft& f = fft();
    const bool pro = s.mode == StretchMode::ComplexPro;
    // where this frame's centre is in the clip: output time, then the warp map
    const double tau = double(m_nextFrame) / (rate * s.ratio);
    const double beat = m_beat0 + tau / s.secPerBeat;
    const double pos = s.secondsAt(beat) * rate;
    const long a0 = long(std::lround(pos)) - kFft / 2;
    // both channels in one transform: left in the real part, right in the imaginary
    for (int i = 0; i < kFft; ++i) m_spec[size_t(i)] = {at(smp.left, a0 + i) * m_win[size_t(i)], at(smp.right, a0 + i) * m_win[size_t(i)]};
    f.forward(m_spec.data());
    for (int k = 0; k < kBins; ++k) {
        const std::complex<float> z = m_spec[size_t(k)], zc = std::conj(m_spec[size_t((kFft - k) % kFft)]);
        const std::complex<float> xl = 0.5f * (z + zc), xr = std::complex<float>(0, -0.5f) * (z - zc);
        m_mag[0][size_t(k)] = std::abs(xl); m_phase[0][size_t(k)] = std::arg(xl);
        m_mag[1][size_t(k)] = std::abs(xr); m_phase[1][size_t(k)] = std::arg(xr);
    }
    const double dpos = pos - m_lastPos;   // the analysis hop, in frames (any size: the warp map sets it)
    for (int c = 0; c < 2; ++c) {
        std::vector<float>& mag = m_mag[c];
        std::vector<float>& ph = m_phase[c];
        std::vector<float>& syn = m_phaseSyn[c];
        bool fresh = m_first;
        if (pro && !fresh) {   // a transient: the spectrum grew a lot since the last frame; start the phases afresh
            float grow = 0, was = 1e-6f;
            for (int k = 1; k < kBins; ++k) { grow += std::max(0.0f, mag[size_t(k)] - m_magPrev[c][size_t(k)]); was += m_magPrev[c][size_t(k)]; }
            if (grow > kTransientFlux * was) fresh = true;
        }
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
            if (pro) {   // lock every bin to the nearest peak's phase: a peak's region moves as one partial
                int lastPeak = -1;
                for (int k = 0; k < kBins; ++k) {
                    bool isPeak = mag[size_t(k)] > 1e-6f;
                    for (int d = -kPeakReach; isPeak && d <= kPeakReach; ++d)
                        if (d && k + d >= 0 && k + d < kBins && mag[size_t(k + d)] > mag[size_t(k)]) isPeak = false;
                    m_peak[size_t(k)] = isPeak ? k : -1;
                    if (!isPeak) continue;
                    for (int j = lastPeak + 1; j < k; ++j) m_peak[size_t(j)] = lastPeak < 0 || k - j <= j - lastPeak ? k : lastPeak;
                    lastPeak = k;
                }
                for (int j = lastPeak + 1; j < kBins; ++j) m_peak[size_t(j)] = lastPeak;
                for (int k = 0; k < kBins; ++k) {
                    const int p = m_peak[size_t(k)];
                    if (p >= 0 && p != k) syn[size_t(k)] = wrapPhase(syn[size_t(p)] + ph[size_t(k)] - ph[size_t(p)]);
                }
            }
        }
        for (int k = 0; k < kBins; ++k) { m_phasePrev[c][size_t(k)] = ph[size_t(k)]; m_magPrev[c][size_t(k)] = mag[size_t(k)]; }
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
    const double m = m_t * rate * s.ratio + kFft / 2;   // where in the stretched stream this output sample reads
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
}

} // namespace sessions::audio
