#pragma once
// Time-stretching for the warp modes that aren't slices (0.21). There are six; Beats (slices)
// and Re-Pitch (a record) live in Engine::renderAudio, the four here keep the pitch while the
// time stretches, each its own way:
//  - Tones: overlapping grains of the audio (Hann windows, half a grain apart), each one lined
//    up with the last one's continuation by cross-correlation (WSOLA), so a sustained note
//    stays smooth. Grain Size sets the grain.
//  - Texture: the same grains, but wandering: Flux moves each grain's start and length by a
//    random amount, for a loose, cloudy sound.
//  - Complex: a stereo-linked, peak phase-locked vocoder. An FFT frame (2048 points) every
//    512 output samples, read at
//    the warp map's position; each bin's phase is advanced at the frequency its own phase
//    difference says it has, so every partial keeps its pitch however the time stretches.
//  - Complex Pro: the vocoder with its phases started afresh when the spectrum
//    jumps (a transient stays sharp), and Formants: the spectral envelope is put back where
//    it was when the clip is transposed, so a voice stays a voice.
// Transposition is the same for all four: the stretched audio is read faster or slower (that is
// the pitch), and the stretch factor absorbs it, so the time still lines up with the beats.
// A Stretcher is one placement's state on the audio thread: its buffers are made once (the
// Engine owns a pool), it never allocates after that, and it restarts itself where it is asked
// to play when the clip time it's fed jumps (a loop point, a seek, another placement).

#include "core/Model.h"
#include "core/Sample.h"
#include <complex>
#include <cstdint>
#include <vector>

namespace sessions::audio {

enum class StretchMode { None = 0, Tones, Texture, Complex, ComplexPro };

// What a Stretcher reads: the sample, the warp map (clip beats to seconds) and the settings.
struct StretchSource {
    const SampleData* sample = nullptr;
    const WarpMarker* markers = nullptr;
    int    nMarkers = 0;
    double secPerBeat = 0.5;   // the song's tempo
    double ratio = 1;          // transpose, as a rate factor (2 = an octave up)
    double grainSec = 0.05;    // Tones / Texture
    double flux = 0.25;        // Texture
    double formants = 0;       // Complex Pro
    StretchMode mode = StretchMode::None;
    double secondsAt(double beat) const { return warpSecondsAt(markers, nMarkers, beat); }
};

class Stretcher {
public:
    static constexpr int kFft = 2048, kHop = kFft / 4, kBins = kFft / 2 + 1, kRing = 4 * kFft;
    static constexpr int kMaxGrains = 12;
    Stretcher();
    void reset();
    // Build the initial overlap on the preparation/UI thread, without consuming
    // an output sample. A newly launched audio-thread voice can copy this state
    // instead of running several FFTs at its first callback sample.
    void prime(const StretchSource& s, double local, float sampleRate);
    void copyPrepared(const Stretcher& prepared); // fixed-size copies into existing buffers; never allocates
    // The next output sample for clip beat `local` (the beats advance a sample at a time: a jump
    // restarts the stretcher there), at `sampleRate`, into l and r.
    void sample(const StretchSource& s, double local, float sampleRate, float& l, float& r);
    bool active() const { return m_active; }

private:
    void restart(const StretchSource& s, double local, float rate);
    void granular(const StretchSource& s, float rate, float& l, float& r);
    void spawnGrain(const StretchSource& s, float rate);
    void vocoder(const StretchSource& s, float rate, float& l, float& r);
    void frame(const StretchSource& s, float rate);
    float random();   // 0 .. 1

    bool   m_active = false;
    double m_local = 0;        // actual clip beat of the current output sample
    double m_t = 0;            // output seconds since the restart
    double m_expectBeat = 0;   // the beat the next call should bring (continuity check)
    uint32_t m_rng = 0x9e3779b9u;
    const SampleData* m_sample = nullptr;
    StretchMode m_mode = StretchMode::None;
    float m_rate = 0;
    bool m_direct = false;
    float m_previousL = 0, m_previousR = 0;
    float m_transitionL = 0, m_transitionR = 0;
    int m_transitionLeft = 0;

    // granular
    struct Grain { double src = 0; int age = 0, len = 0; }; // src is the next read position, age only controls its window
    Grain  m_grains[kMaxGrains];
    int    m_nGrains = 0;
    double m_nextGrainT = 0;   // output seconds (since the restart) the next grain starts at
    int    m_lastGrain = -1;   // index of the newest grain (what the next one lines up with)

    // phase vocoder
    std::vector<std::complex<float>> m_spec, m_cep;   // the frame's spectrum (both channels packed), the cepstrum work
    std::vector<float> m_win;                         // the Hann window
    std::vector<float> m_ring[2];                     // the overlap-added output, kRing long, indexed by output sample (modulo)
    std::vector<float> m_phasePrev[2], m_phaseSyn[2], m_magPrev[2], m_mag[2], m_phase[2], m_env[2];
    std::vector<float> m_weight;                      // formant correction per bin
    std::vector<int>   m_peak;                        // Complex Pro: the peak each bin follows
    long   m_nextFrame = 0;    // output sample index the next frame starts at
    double m_lastPos = 0;      // the last frame's analysis centre, in sample frames
    double m_readPos = kFft / 2; // continuous synthesis position; changing pitch never jumps it
    bool   m_first = true;     // no frame yet since the restart
    int    m_transientHold = 0;
};

} // namespace sessions::audio
