#pragma once
// Impulse responses (the Guitar Amp's Loaded IR cabinet): uniformly partitioned FFT
// convolution. An IrKernel is the impulse response cut into 128-sample partitions and
// transformed, made once on the UI thread (makeIrKernel); a Convolver holds one channel's
// running state on the audio thread and never allocates. Latency: one block (128 samples).

#include "core/Sample.h"
#include <complex>
#include <memory>
#include <vector>

namespace sessions::audio {

constexpr int kIrBlock = 128;                 // samples per partition
constexpr int kIrFft = 2 * kIrBlock;          // FFT size
constexpr int kIrBins = kIrBlock + 1;         // bins kept (real signal: the rest mirror them)
constexpr int kIrMaxPartitions = 64;          // 8192 samples: 170 ms at 48 kHz, plenty for a cabinet

struct IrKernel {
    int partitions = 0;
    bool stereo = false;
    std::vector<std::complex<float>> spectra[2];   // [partition * kIrBins + bin], left and right
    SamplePtr source;                               // the file it came from (keeps the pointer valid)
    double seconds = 0;                             // how long the IR is (after trimming)
};

// The impulse response of `sample` at `sampleRate`: resampled, trimmed to kIrMaxPartitions
// partitions (with a short fade), scaled so white noise keeps its level. Null for an empty sample.
std::shared_ptr<const IrKernel> makeIrKernel(const SamplePtr& sample, float sampleRate, int maxPartitions = kIrMaxPartitions);

class Convolver {
public:
    explicit Convolver(int maxPartitions = kIrMaxPartitions);
    void reset();
    // Convolves `in` with channel `channel` of `k` into `out` (may be the same buffer).
    void process(const IrKernel& k, int channel, const float* in, float* out, int n);
private:
    void block(const IrKernel& k, int channel);
    std::vector<std::complex<float>> m_fdl;   // the last kIrMaxPartitions input spectra
    int   m_head = 0;
    int   m_capacity = kIrMaxPartitions;
    float m_prev[kIrBlock] = {}, m_in[kIrBlock] = {}, m_out[kIrBlock] = {};
    int   m_fill = 0;
    std::complex<float> m_work[kIrFft];
};

} // namespace sessions::audio
