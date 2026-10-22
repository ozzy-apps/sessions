#pragma once
// DeviceProcessor: the DSP behind a device in a track's chain. One processor
// per Device (keyed on Device::id), created on the UI thread and then only
// touched by the audio thread. Instruments render note events; audio effects
// process the track buffer in place; MIDI effects rewrite the event list.
//
// Note events carry their length in samples, so the engine never has to
// track note-offs across clip changes; a clip switch or stop sends allOff.
// Notes played live (computer keyboard) use kHeld as their length and end
// with an explicit noteOff.

#include "core/DeviceSpecs.h"
#include <atomic>
#include <memory>
#include <vector>

namespace sessions { struct SampleData; using SamplePtr = std::shared_ptr<const SampleData>; }
namespace sessions::audio { struct IrKernel; }
namespace sessions::nam { class Runtime; }
namespace sessions::audio::drums { struct Content; }

namespace sessions::audio {

constexpr int kMaxBlock  = 128;   // the engine renders in chunks of at most this many frames
constexpr int kMaxParams = 192;   // knobs per device; Osmium uses 191, Monolith 170
constexpr int kMaxEvents = 256;
constexpr int kHeld      = 1 << 30;   // duration of a live note: until its noteOff

struct NoteEvent {
    int   offset   = 0;      // sample within the block
    int   pitch    = 60;
    float velocity = 0.8f;
    int   duration = 0;      // samples until release
    bool  allOff   = false;  // release everything that's sounding
    bool  noteOff  = false;  // release this pitch (live notes)
};

struct EventList {
    NoteEvent ev[kMaxEvents];
    int count = 0;
    void clear() { count = 0; }
    void add(const NoteEvent& e) {   // keeps events sorted by offset (stable)
        if (count >= kMaxEvents) return;
        int i = count++;
        while (i > 0 && ev[i - 1].offset > e.offset) { ev[i] = ev[i - 1]; --i; }
        ev[i] = e;
    }
};

struct Context {
    float  sampleRate = 48000;
    double beat = 0;            // transport beat at the block start (free-running while stopped)
    double beatsPerSample = 0;
    double tempo = 120;
    bool   playing = false;
    // Performance controls from a keyboard, for the track's instruments: pitch bend (-1..1), the
    // mod wheel (CC 1) and channel pressure (0..1). Monolith bends by its Bend Range and offers them as sources.
    float  bend = 0, wheel = 0, pressure = 0;
    const SampleData* sample = nullptr;   // the device's sample (Sampler), kept alive by the render state
    // Sampler (0.32): its manual slices (seconds, sorted; kept alive by the render state) and the sample's own tempo (0: none).
    const double* slices = nullptr;
    int           sliceCount = 0;
    double        sampleBpm = 0;
    const SamplePtr* pads = nullptr;      // Drum Rack: its 16 pads' samples (null entries are empty pads)
    // Drum Rack, when pads hold chains (bit k = pad k): hits on padSample pads start sample voices
    // (the others' instruments play them); voices of padPlay pads are rendered in this call (the
    // others wait for the call that renders their pad into its chain).
    unsigned padSample = ~0u, padPlay = ~0u;
    const IrKernel* ir = nullptr;         // Guitar Amp: its loaded impulse response, ready to convolve
    nam::Runtime*   nam = nullptr;        // NAM: its capture's network, prepared by the engine (null: no capture, plays dry)
    const drums::Content* drumContent = nullptr;
    bool waitForStreams = false;   // no driver runs (exports, tests): streamed recordings are waited for, not skipped
    // Sidechain (Compressor, Gate, Auto Filter): another track's audio for this block, the
    // detector's key. nullptr: the device listens to its own input.
    const float* keyL = nullptr;
    const float* keyR = nullptr;
    // Monolith: its modulation routes (kept alive by the render state).
    const ModRoute* mods = nullptr;
    int             modCount = 0;
    // Monolith/Osmium: curveStride slots per curve, curveCount[c] of them used.
    const CurvePoint* curves = nullptr;
    const int*        curveCount = nullptr;
    int             curveStride = monolith::kCurvePoints; // Engine uses Osmium capacity; legacy callers retain 32.
};

class ChainMixer;

class DeviceProcessor {
public:
    virtual ~DeviceProcessor() = default;
    virtual void midi(const EventList& in, EventList& out, const float* params, const Context&, int frames);
    virtual void render(const EventList&, float* /*L*/, float* /*R*/, int /*frames*/, const float* /*params*/, const Context&) {}
    virtual void process(float* /*L*/, float* /*R*/, int /*frames*/, const float* /*params*/, const Context&) {}
    virtual void panic() {}     // silence voices and tails immediately
    virtual ChainMixer* chainMixer() { return nullptr; }   // racks and the Drum Rack: how their chains are mixed

    // What the device panel shows live (gain reduction, a moving cutoff, …): written on the
    // audio thread, read on the UI thread. Meaning per device; see its processor. meter[2] is
    // the sidechain key's level in dB on the devices that have one.
    std::atomic<float> meter[3] = {{0.0f}, {0.0f}, {-120.0f}};
    // More for the panels that animate what the device does inside (the Monolith's envelope
    // levels and stages, its LFOs and steppers): written on the audio thread, read on the UI thread.
    std::atomic<float> scope[16] = {};

    // The engine's on / off crossfade (audio thread only): 1 fully on, 0 off. Switching a
    // device on or off fades over ~12 ms instead of jumping, so it never pops.
    float onMix = -1;   // < 0: not run yet (starts where it should be, no fade)
};

std::shared_ptr<DeviceProcessor> createProcessor(DeviceType type, float sampleRate);

// Mixes chains into a rack's output (a rack's chains, a Drum Rack's pads): gains that glide
// (~10 ms) so nothing clicks, and each chain's peak for the panel.
class ChainMixer {
public:
    static constexpr int kMax = 16;
    explicit ChainMixer(float sampleRate) : m_sr(sampleRate) {}
    // Adds `in` into `out` at gains gl / gr (gliding there from the last block's).
    void  mix(int chain, const float* inL, const float* inR, float* outL, float* outR, int frames, float gl, float gr);
    // Scales `L / R` in place by g, gliding (an Audio Effect Rack chain's input, by its chain-select zone).
    void  scale(int chain, float* L, float* R, int frames, float g);
    float takePeak(int chain);   // UI thread: the peak since the last call
    void  reset() { for (bool& p : m_primed) p = false; for (bool& p : m_inPrimed) p = false; }
private:
    float m_sr;
    float m_gl[kMax] = {}, m_gr[kMax] = {}, m_in[kMax] = {};
    bool  m_primed[kMax] = {}, m_inPrimed[kMax] = {};
    std::atomic<float> m_peak[kMax] = {};
};

// A rack (Instrument / Audio Effect / MIDI Effect Rack). The engine runs its chains' devices
// itself; the rack mixes each chain's output in at its Volume / Pan / On / Solo.
class RackProcessor : public DeviceProcessor {
public:
    explicit RackProcessor(float sampleRate) : m_mix(sampleRate) {}
    // Adds `in` (one chain's output) into `out`. `params` are the rack's knobs.
    void  mixChain(int chain, const float* inL, const float* inR, float* outL, float* outR, int frames, const float* params, bool anySolo);
    float takeChainPeak(int chain) { return m_mix.takePeak(chain); }   // UI thread: the peak since the last call
    void  panic() override { m_mix.reset(); }
    ChainMixer* chainMixer() override { return &m_mix; }
private:
    ChainMixer m_mix;
};

// The same math the processors use, for the device panels' displays. `params` are knob
// positions (0..1) in the device's parameter order.
float eqResponseDb(const float* params, float freq, float sampleRate);   // EQ: gain at freq, without Output
float velocityMap(const float* params, float velocity);                  // Velocity: without Random
float ampResponseDb(const float* params, float freq, float sampleRate);   // Guitar Amp: tone stack and built-in cabinet
// The impulse response of one of the amp's built-in cabinets (1 .. 5) through a mic (0 on axis, 1 off axis, 2 far),
// `frames` long at `sampleRate`: the Impulse Responses library's built-in IRs (audio/Builtin.h).
std::vector<float> cabinetImpulse(int cab, int mic, float sampleRate, int frames);
float pedalResponseDb(DeviceType type, const float* params, float freq, float sampleRate);   // drive pedals' filters, the Wah
float pedalTransfer(DeviceType type, const float* params, float x);                         // a drive pedal's clipper alone: output for an input x (-1..1)
float phaserResponseDb(float coef, float freq, float sampleRate);                            // the Phase Pedal's notches at an all-pass coefficient (its meter[2])
int   scaleMap(const float* params, int pitch);                           // Scale: where a note goes
float monolithWarpPhase(int mode, float phase, float intensity);           // Monolith: an oscillator's Bend / Formant / Sync warp
float osmiumWarpPhase(int mode, float phase, float intensity);             // extended phase shapes; Cross FM applies its modulator in the processor
float osmiumFold(float sample, float amount);                             // instantaneous wavefold curve for the visual editor
float monolithFilterQ(int type, float reso);                               // Monolith: a filter's Q from its Reso knob

} // namespace sessions::audio
