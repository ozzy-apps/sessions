#include "Convolver.h"
#include <algorithm>
#include <cmath>

namespace sessions::audio {

namespace {

// Radix-2 complex FFT of size kIrFft, tables made once (the first call is on the UI thread:
// makeIrKernel or a Convolver's constructor).
struct Fft {
    std::vector<std::complex<float>> tw;
    std::vector<int> rev;
    Fft() : tw(kIrFft / 2), rev(kIrFft) {
        for (int i = 0; i < kIrFft / 2; ++i) tw[size_t(i)] = std::polar(1.0f, float(-2.0 * M_PI * i / kIrFft));
        int bits = 0;
        while ((1 << bits) < kIrFft) ++bits;
        for (int i = 0; i < kIrFft; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[size_t(i)] = r;
        }
    }
    // inverse: conjugated twiddles, unscaled
    void run(std::complex<float>* a, bool inverse) const {
        for (int i = 0; i < kIrFft; ++i) if (i < rev[size_t(i)]) std::swap(a[i], a[rev[size_t(i)]]);
        for (int len = 2; len <= kIrFft; len <<= 1) {
            const int step = kIrFft / len;
            for (int i = 0; i < kIrFft; i += len)
                for (int k = 0; k < len / 2; ++k) {
                    std::complex<float> w = tw[size_t(k * step)];
                    if (inverse) w = std::conj(w);
                    const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                }
        }
    }
};

const Fft& fft() {
    static const Fft f;
    return f;
}

} // namespace

std::shared_ptr<const IrKernel> makeIrKernel(const SamplePtr& sample, float sampleRate) {
    if (!sample || sample->frames() < 2) return nullptr;
    auto k = std::make_shared<IrKernel>();
    k->source = sample;
    k->stereo = sample->channels > 1;
    const double ratio = sample->rate / sampleRate;
    const int maxLen = kIrMaxPartitions * kIrBlock;
    const int len = std::min(maxLen, int(std::ceil(double(sample->frames()) / ratio)));
    k->partitions = (len + kIrBlock - 1) / kIrBlock;
    k->seconds = double(len) / sampleRate;
    const Fft& f = fft();
    for (int ch = 0; ch < 2; ++ch) {
        const std::vector<float>& src = ch == 0 || !k->stereo ? sample->left : sample->right;
        std::vector<float> h(size_t(k->partitions) * kIrBlock, 0.0f);
        for (int i = 0; i < len; ++i) {   // resample (linear) to the engine's rate
            const double pos = i * ratio;
            const size_t a = size_t(pos);
            if (a + 1 >= src.size()) { if (a < src.size()) h[size_t(i)] = src[a]; continue; }
            h[size_t(i)] = float(src[a] + (src[a + 1] - src[a]) * (pos - double(a)));
        }
        if (double(sample->frames()) / ratio > maxLen) {   // trimmed: fade out the last 10 ms
            const int fade = std::min(len, int(0.01 * sampleRate));
            for (int i = 0; i < fade; ++i) h[size_t(len - 1 - i)] *= float(i) / fade;
        }
        double energy = 0;
        for (float x : h) energy += double(x) * x;
        const float scale = energy > 1e-12 ? float(1.0 / std::sqrt(energy)) : 0.0f;
        k->spectra[ch].assign(size_t(k->partitions) * kIrBins, {});
        std::complex<float> work[kIrFft];
        for (int p = 0; p < k->partitions; ++p) {
            for (int i = 0; i < kIrFft; ++i) work[i] = i < kIrBlock ? h[size_t(p * kIrBlock + i)] * scale : 0.0f;
            f.run(work, false);
            std::copy(work, work + kIrBins, k->spectra[ch].begin() + p * kIrBins);
        }
    }
    return k;
}

Convolver::Convolver() : m_fdl(size_t(kIrMaxPartitions) * kIrBins) { fft(); }

void Convolver::reset() {
    std::fill(m_fdl.begin(), m_fdl.end(), std::complex<float>());
    std::fill(m_prev, m_prev + kIrBlock, 0.0f);
    std::fill(m_in, m_in + kIrBlock, 0.0f);
    std::fill(m_out, m_out + kIrBlock, 0.0f);
    m_fill = 0;
    m_head = 0;
}

void Convolver::process(const IrKernel& k, int channel, const float* in, float* out, int n) {
    for (int i = 0; i < n; ++i) {
        m_in[m_fill] = in[i];
        out[i] = m_out[m_fill];   // the previous block's result: one block of latency
        if (++m_fill == kIrBlock) { block(k, channel); m_fill = 0; }
    }
}

// Overlap-save: transform [previous block, this block], push it on the delay line, multiply
// each partition's spectrum by the input spectrum that far back, transform back, keep the
// second half.
void Convolver::block(const IrKernel& k, int channel) {
    const Fft& f = fft();
    for (int i = 0; i < kIrBlock; ++i) { m_work[i] = m_prev[i]; m_work[i + kIrBlock] = m_in[i]; }
    std::copy(m_in, m_in + kIrBlock, m_prev);
    f.run(m_work, false);
    m_head = (m_head + kIrMaxPartitions - 1) % kIrMaxPartitions;
    std::copy(m_work, m_work + kIrBins, m_fdl.begin() + m_head * kIrBins);
    std::complex<float> acc[kIrBins];
    std::fill(acc, acc + kIrBins, std::complex<float>());
    const std::vector<std::complex<float>>& H = k.spectra[channel];
    const int parts = std::min(k.partitions, kIrMaxPartitions);
    for (int p = 0; p < parts; ++p) {
        const std::complex<float>* x = &m_fdl[size_t((m_head + p) % kIrMaxPartitions) * kIrBins];
        const std::complex<float>* h = &H[size_t(p) * kIrBins];
        for (int b = 0; b < kIrBins; ++b) acc[b] += x[b] * h[b];
    }
    for (int b = 0; b < kIrBins; ++b) m_work[b] = acc[b];
    for (int b = kIrBins; b < kIrFft; ++b) m_work[b] = std::conj(acc[kIrFft - b]);   // a real signal's mirror half
    f.run(m_work, true);
    const float norm = 1.0f / kIrFft;
    for (int i = 0; i < kIrBlock; ++i) m_out[i] = m_work[i + kIrBlock].real() * norm;
}

} // namespace sessions::audio
