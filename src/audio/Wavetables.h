#pragma once
// The Wavetable Synth's tables: four families of eight single-cycle frames each, built once
// from their harmonics. Every frame is stored at ten band limits (mip levels) so high notes
// don't alias: level L holds at most 512 >> L harmonics. The device panel draws level 0.

#include <vector>

namespace sessions::audio {

constexpr int kWtSize   = 2048;   // samples per cycle
constexpr int kWtFrames = 8;      // frames per table (the Position knob scans them)
constexpr int kWtLevels = 10;

struct Wavetable {
    const char* name;
    std::vector<float> data;   // [level][frame][kWtSize + 1], the last sample repeats the first
    const float* frame(int level, int frame) const { return data.data() + (size_t(level) * kWtFrames + size_t(frame)) * (kWtSize + 1); }
};

int              wavetableCount();
const Wavetable& wavetable(int index);   // built on first use: call once on the UI thread first
int              wavetableLevel(float freq, float sampleRate);   // the richest level that stays under Nyquist

// A sample of `table` at `position` (0..1 across the frames) and `phase` (0..1), linear in both.
inline float wavetableRead(const Wavetable& t, int level, float position, float phase) {
    const float fp = position * float(kWtFrames - 1);
    const int f0 = int(fp), f1 = f0 + 1 < kWtFrames ? f0 + 1 : f0;
    const float ff = fp - float(f0);
    const float x = phase * float(kWtSize);
    const int i = int(x);
    const float fr = x - float(i);
    const float* a = t.frame(level, f0);
    const float* b = t.frame(level, f1);
    const float sa = a[i] + (a[i + 1] - a[i]) * fr;
    const float sb = b[i] + (b[i + 1] - b[i]) * fr;
    return sa + (sb - sa) * ff;
}

} // namespace sessions::audio
