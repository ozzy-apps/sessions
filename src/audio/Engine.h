#pragma once
// Engine: the audio engine.
//
// Threads: the UI thread owns the Project. Whenever it changes, publish()
// snapshots it into an immutable RenderState and hands it to the audio thread
// through an atomic pointer; the audio thread hands finished states back
// through a lock-free queue so they are freed on the UI thread. Transport
// actions travel as Commands through a second lock-free queue. The audio
// thread owns the playhead and the session launch state and publishes them
// back through atomics, which Transport mirrors into the model for the views.
//
// Tracks are identified by Track::id and clips by Clip::id, so inserting,
// moving or deleting tracks and scenes never confuses what is playing.
//
// Audio input: a capture thread pushes stereo blocks into a lock-free ring; process()
// takes one block per call, so input and output share the output thread's clock. Audio
// tracks hear it through their Monitor setting. While a recording runs, process() also
// copies each block it took into a second ring, which Transport drains into WAV files.
//
// Groups: tracks in a group play into the group's bus; group tracks run after every
// other track, through their own audio effects and mixer, into Main.
//
// Sidechains: a Compressor, Gate or Auto Filter can take its key from another track. publish()
// resolves each key to a track index and orders the regular tracks so that every source renders
// before the tracks keyed from it (RenderState::order); while a track is a source, its Pre FX,
// Post FX and Post Mix audio is copied into fixed buffers (m_keyL/R) that the keyed devices
// read through Context::keyL/R in the same block. Nothing is allocated for it on the audio thread.
//
// process() never locks or allocates. Device DSP state lives in
// DeviceProcessors keyed on Device::id, so reverb tails and voices survive
// knob moves, undo and re-publishing.

#include "Devices.h"
#include "Dsp.h"
#include "Spsc.h"
#include "Driver.h"
#include "Stretch.h"
#include "core/Sample.h"
#include <QObject>
#include <QHash>
#include <QString>
#include <atomic>
#include <array>
#include <memory>
#include <functional>
#include <vector>

namespace sessions::nam { class Runtime; }

namespace sessions {

class Project;

namespace audio {

class Driver;
struct RenderState;
struct TrackData;
struct ClipData;

constexpr int kMaxTracks  = 64;
constexpr int kMaxReturns = 8;
constexpr int kMaxGroups  = 16;
constexpr int kMaxInputFrames = 16384;   // the most frames one process() call takes from the input

struct Command {
    enum Type : quint8 {
        Play, Pause, Stop, Locate, Launch, StopTrack, Unrepeat, BackToArrangement,
        Loop, Metronome, Preview, NoteOn, NoteOff, Capture,
        SessionTakeover,   // every track plays its Session clip (or nothing): the Arrangement stays silent until BackToArrangement
        SeekSlot,          // the playing (or just launched) Session clip `slot` of the track jumps to clip beat a (a click in the sample editor)
        Perform,           // a performance control for a track's instruments: a = kind (0 pitch bend -1..1, 1 mod wheel 0..1, 2 pressure 0..1), b = value
    };   // Play: a = beats of count-in first (0 = none)
    Type    type = Play;
    int     trackId = -1, slot = -1;   // tracks by Track::id
    double  a = 0, b = 0;   // Launch/StopTrack: a = quantization (0 = now). Locate: a = beat.
                            // Loop: a = start, b = length. Preview / NoteOn / NoteOff: a = pitch, b = velocity.
    bool    on = false;     // Stop: rewind to 1.1.1. Loop / Metronome: enabled. Launch: repeat while held.
    quint64 seq = 0;
};

// Export Audio: the Arrangement from `startBeat` for `lengthBeats`, plus a tail for reverbs and delays.
struct RenderOptions {
    double startBeat = 0, lengthBeats = 16, tailSeconds = 0;
    samples::WavFormat format = samples::WavFormat::Int24;
    bool   normalize = false;   // peak at -0.1 dBFS
};

class Engine : public QObject {
    Q_OBJECT
public:
    explicit Engine(Project* project, QObject* parent = nullptr);
    ~Engine() override;

    // Opens the sound server. Falls back to a silent real-time clock when there's
    // no server, under QT_QPA_PLATFORM=offscreen (unless SESSIONS_AUDIO=pulse),
    // or with SESSIONS_AUDIO=none.
    void    start();
    void    shutdown();
    // Output device and buffer (Preferences): takes effect now when the engine is running.
    void    setOutput(const DriverOptions& options);
    DriverOptions output() const { return m_output; }
    bool    hasAudio() const { return m_hasAudio; }
    bool    audioDisabled() const { return m_disabled; }   // silent on purpose (offscreen, SESSIONS_AUDIO=none)
    QString outputName() const { return m_outputName; }
    float   sampleRate() const { return m_sr; }
    float   cpuLoad() const { return m_cpu.load(std::memory_order_relaxed); }
    double  latencySeconds() const;

    // ---- UI thread
    quint64 send(Command c);    // returns the command's sequence number
    void    publish();          // snapshot the project for the audio thread now
    void    flushPublish() { if (m_republishQueued) publish(); }   // before a command that refers to fresh edits
    void    collectGarbage();   // free states the audio thread has finished with
    // True when the audio thread runs on the latest snapshot, so per-index
    // feedback (trackStatus, peaks) lines up with the project's track list.
    bool    inSync() const { return !m_republishQueued && m_fbGeneration.load(std::memory_order_acquire) == m_generation; }

    // ---- feedback from the audio thread (read on the UI thread)
    double  beat() const      { return m_fbBeat.load(std::memory_order_acquire); }
    bool    playing() const   { return m_fbPlaying.load(std::memory_order_acquire); }   // also while counting in
    double  countInLeft() const { return m_fbCountIn.load(std::memory_order_acquire); }  // beats of count-in to go, 0 = none
    quint64 doneSeq() const   { return m_doneSeq.load(std::memory_order_acquire); }
    struct TrackStatus { int playingSlot, queuedSlot; double slotStart, queuedAt; bool overridden; };
    TrackStatus trackStatus(int trackIndex) const;
    float   takeTrackPeak(int trackIndex);     // peak since the last call
    float   takeReturnPeak(int ret);
    float   takeMainPeak();

    // ---- the render callback: the driver thread, or the caller when no driver runs
    // `in` (duplex drivers): this period's input, interleaved stereo; else it comes from the input ring.
    void process(float* interleavedStereo, int frames, const float* in = nullptr);
    bool duplexAudio() const { return m_duplex; }   // playing through PipeWire / JACK
    QString jackError() const { return m_jackError; }   // why PipeWire / JACK couldn't be used, if it was asked for
    QString audioProblem() const { return m_driver ? m_driver->problem() : QString(); }   // e.g. an input that was dropped

    // Offline: play the arrangement from 1.1.1 for `seconds` into a 16-bit WAV.
    // Only while no driver is running.
    bool renderToWav(const QString& path, double seconds);

    // Offline export (Export Audio): see RenderOptions. Use a fresh Engine on the project
    // (never the one playing). `progress` gets 0..1 and returns false to cancel.
    bool renderOffline(const QString& path, const RenderOptions& options,
                       const std::function<bool(double)>& progress = {}, QString* error = nullptr);

    // A device's live meter (see DeviceProcessor::meter), for its panel.
    float deviceMeter(int deviceId, int which) const;
    float deviceScope(int deviceId, int which) const;   // DeviceProcessor::scope (0 .. 15)
    // A rack chain's output peak since the last call (the rack panel's chain meters).
    float takeChainPeak(int rackId, int chain);

    // Browser preview: plays a sample once on the main output at the cue level (nullptr stops it).
    void previewSample(const SamplePtr& sample);

    // ---- audio input (UI thread)
    // The capture stream opens while it's wanted (an armed or monitoring audio track) and
    // closes otherwise. With audio disabled (tests, offscreen) the input is "open" and only
    // what pushInput() is given comes in.
    void    setInputDevice(const QString& device);   // a source name; "" = the system default
    QString inputDevice() const { return m_inputDevice; }
    void    setInputWanted(bool wanted);
    bool    inputOpen() const { return m_inputLive.load(std::memory_order_acquire); }
    QString inputStatus() const { return m_inputStatus; }
    double  inputLatencySeconds() const;   // the capture path plus what waits in the ring
    void    pushInput(const float* interleavedStereo, int frames);   // the capture thread (or a test)
    float   takeInputPeak(int trackIndex);   // an audio track's input level since the last call

    // ---- recording (UI thread). Between startCapture() and stopCapture() every block of
    // input the engine takes is queued for takeRecorded(), starting at captureStartBeat()
    // (the engine beat of the first frame, once captureStarted()). Frames run on in real
    // time at the set's tempo from there, through loops and locates, until stopCapture().
    void    startCapture();
    void    stopCapture();
    bool    captureStarted() const { return m_capStarted.load(std::memory_order_acquire); }
    double  captureStartBeat() const { return m_capStartBeat.load(std::memory_order_acquire); }
    int     takeRecorded(float* interleavedStereo, int maxFrames);   // frames copied
    qint64  recordOverflows() const { return m_capOverflow.load(std::memory_order_relaxed); }

private:
    struct PlayState {
        int    trackId = -1;
        int    playingSlot = -1, playingClip = 0;   // slot index + clip id (the id survives scene moves)
        int    queuedSlot = -2, queuedClip = 0;     // -2 none, -1 stop, >=0 launch
        double slotStart = 0, queuedAt = 0;
        bool   overridden = false;
        bool   pendingAllOff = false;
        bool   followDone = false;                  // this launch's follow action has fired
        int    repeatSlot = -1;                     // Repeat launch mode: retrigger while held
        double seekBeat = 0; bool hasSeek = false;  // SeekSlot before the queued launch lands: the clip starts at this clip beat
        double repeatQuant = 1;
        float  gainL = 0, gainR = 0, send[kMaxReturns] = {};
    };
    struct Live { int track; int pitch; float velocity; bool off; int duration; };
    // Audio tracks: the stretches of audio clips under the playhead in this block.
    // start: the clip beat the first pass begins at (Clip::start for a launched clip, a placement's offset)
    struct AudioSeg { const ClipData* clip; double origin, start, lo, hi, from, to; };

    void scheduleRepublish();
    void adopt(RenderState* s);
    int  indexOfTrack(int id) const;
    void apply(const Command& c);
    void renderBlock(float* out, int n, const float* in);
    void pullInput(int frames);
    void scheduleTrack(const TrackData& td, PlayState& ps, double b0, double b1, double bps, int n, EventList& ev);
    void emitSource(const TrackData& td, PlayState& ps, double from, double to, double b0, double bps, int n, EventList& ev);
    int  followTarget(const TrackData& td, int current, int action);
    void allNotesOff();
    void addSegment(const ClipData& c, double origin, double start, double lo, double hi, double from, double to);
    void renderAudio(double b0, double bps, double tempo, int n, float* L, float* R);
    void renderComp(const ClipData& c, double local, double fade, double secPerBeat, float& L, float& R) const;   // a comped clip's sample at a clip beat
    // Tones / Texture / Complex / Complex Pro: a Stretcher per playing placement (by clip id and
    // placement origin), kept between blocks while it plays; one not used in a block is free again.
    static constexpr int kStretchVoices = 12;
    struct StretchSlot { int clipId = 0; double origin = 0; int lastUse = -1; };
    Stretcher*  stretchVoice(int clipId, double origin);
    Stretcher   m_stretch[kStretchVoices];
    StretchSlot m_stretchKey[kStretchVoices];
    int         m_blockNo = 0;
    void clipOverrides(const TrackData& td, const PlayState& ps, double b0);   // fills m_ov* from the playing clip's envelopes
    double quantized(double q) const;

    Project* m_project;
    float    m_sr = 48000;
    std::unique_ptr<Driver> m_driver;
    bool     m_hasAudio = false;
    bool     m_disabled = false;
    DriverOptions m_output;
    bool    m_duplex = false;     // the driver is duplex (JACK): input arrives with each period
    QString m_jackError;
    QString  m_outputName = "Not started";
    bool     m_republishQueued = false;
    quint64  m_generation = 0;
    SamplePtr m_preview;            // UI side of previewSample()
    quint64   m_previewSerial = 0;
    bool      m_exporting = false;  // renderOffline: sidechain Listen switches are forced off

    // UI-side caches for publish()
    QHash<int, std::shared_ptr<const ClipData>> m_clipCache;   // by clip id
    bool m_clipsDirty = true;
    struct Proc { DeviceType type; std::shared_ptr<DeviceProcessor> proc; };
    QHash<int, Proc> m_procs;   // by device id
    QHash<const SampleData*, std::shared_ptr<const struct IrKernel>> m_irCache;   // Guitar Amp IRs, by sample (each keeps its sample alive)
    QHash<int, std::shared_ptr<nam::Runtime>> m_namCache;   // NAM devices' prepared networks, by device id (reused while the capture and rate stay)
    quint64 m_nextSeq = 0;

    // hand-over between threads
    std::atomic<RenderState*>  m_pending{nullptr};
    SpscQueue<RenderState*, 64> m_retired;
    SpscQueue<Command, 1024>    m_commands;

    // ---- audio thread only
    RenderState* m_state = nullptr;
    bool   m_playing = false, m_loop = false, m_metro = false;
    // Count-in (Play with Command::a beats): only the click plays, the playhead waits, then playback starts.
    double m_countIn = 0, m_countTotal = 0;
    double m_beat = 0, m_freeBeat = 0, m_loopStart = 0, m_loopLen = 16;
    quint64 m_lastSeq = 0;
    std::array<PlayState, kMaxTracks> m_play;
    std::array<PlayState, kMaxTracks> m_scratch;   // used while re-mapping tracks; no allocation
    std::array<PlayState, kMaxReturns> m_retPlay;
    Live   m_live[64]; int m_liveCount = 0;
    // Performance controls (pitch bend, mod wheel, channel pressure) per track, by track id: the
    // last values a controller sent, handed to the track's instruments through Context.
    struct Perf { int trackId = 0; float bend = 0, wheel = 0, pressure = 0; };
    Perf   m_perf[kMaxTracks];
    const Perf* perfFor(int trackId) const { for (const Perf& p : m_perf) if (p.trackId == trackId) return &p; return nullptr; }
    float  m_mainGain = 0, m_clickEnv = 0, m_clickPhase = 0, m_clickFreq = 1000;
    float  m_cpuAvg = 0;
    dsp::Noise m_rng;
    EventList m_evA, m_evB;
    AudioSeg m_segs[16]; int m_segCount = 0;
    quint64 m_previewPlaying = 0;   // serial of the preview being played
    double  m_previewPos = -1;      // frames into it; < 0 = silent
    float  m_trackL[kMaxBlock], m_trackR[kMaxBlock];
    float  m_chainL[kMaxBlock], m_chainR[kMaxBlock];     // racks: one chain's audio
    float  m_rackInL[kMaxBlock], m_rackInR[kMaxBlock];   // an Audio Effect Rack's input
    float  m_rackSumL[kMaxBlock], m_rackSumR[kMaxBlock]; // and its chains mixed
    float  m_fadeL[kMaxBlock], m_fadeR[kMaxBlock];       // a device fading on / off: its dry or its own sound
    float  m_rackFadeL[kMaxBlock], m_rackFadeR[kMaxBlock];   // the same for a rack
    EventList m_chainEvA, m_chainEvB;                     // a chain's notes (its own MIDI effects change them)
    EventList m_rackEv;                                   // a MIDI Effect Rack: its chains' notes merged
    float  m_retL[kMaxReturns][kMaxBlock], m_retR[kMaxReturns][kMaxBlock];
    float  m_mainL[kMaxBlock], m_mainR[kMaxBlock];
    float  m_busL[kMaxGroups][kMaxBlock], m_busR[kMaxGroups][kMaxBlock];   // group buses
    // Sidechain keys: a source track's audio at each tap (0 Pre FX, 1 Post FX, 2 Post Mix) this block.
    // On the heap (sized once in the constructor): it keeps the Engine itself small.
    std::vector<float> m_keyStore;
    float* keyL(int tap, int track) { return m_keyStore.data() + size_t((tap * kMaxTracks + track) * 2) * kMaxBlock; }
    float* keyR(int tap, int track) { return keyL(tap, track) + kMaxBlock; }
    // Clip envelopes of the clip playing on the track being rendered: -1 = not set.
    float  m_ovVolume = -1, m_ovPan = -1, m_ovSend[kMaxReturns] = {};
    struct ParamOverride { int deviceId, param; float value; };
    ParamOverride m_ovParams[64]; int m_ovCount = 0;

    // ---- audio input
    std::unique_ptr<InputDriver> m_inputDriver;   // UI thread
    QString m_inputDevice, m_inputStatus = "Off";
    bool    m_inputWanted = false;
    std::atomic<bool> m_inputLive{false};          // the input is open (audio thread reads it)
    SpscFloatRing m_inRing{1 << 16};               // capture thread -> audio thread (~0.7 s)
    std::vector<float> m_inBlock;                  // this process() call's input, interleaved
    bool    m_inPrimed = false;                    // audio thread: a cushion has built up in the ring
    std::atomic<float> m_inFill{0};                // seconds waiting in the ring (latency)
    std::array<std::atomic<float>, kMaxTracks> m_inputPeak{};
    // recording
    SpscFloatRing m_recRing{1 << 21};              // audio thread -> UI (~22 s of stereo at 48 kHz)
    bool                m_capturing = false;      // audio thread
    bool                m_capPending = false;     // audio thread: capture just switched on
    std::atomic<bool>   m_capStarted{false};
    std::atomic<double> m_capStartBeat{0};
    std::atomic<qint64> m_capOverflow{0};

    // ---- published to the UI
    std::atomic<double>  m_fbBeat{0};
    std::atomic<bool>    m_fbPlaying{false};
    std::atomic<double>  m_fbCountIn{0};
    std::atomic<quint64> m_doneSeq{0};
    std::atomic<quint64> m_fbGeneration{0};
    std::atomic<float>   m_cpu{0};
    struct AtomicStatus {
        std::atomic<int> playingSlot{-1}, queuedSlot{-2};
        std::atomic<double> slotStart{0}, queuedAt{0};
        std::atomic<bool> overridden{false};
    };
    std::array<AtomicStatus, kMaxTracks> m_status;
    std::array<std::atomic<float>, kMaxTracks> m_trackPeak{};
    std::array<std::atomic<float>, kMaxReturns> m_returnPeak{};
    std::atomic<float> m_mainPeak{0};
};

} // namespace audio
} // namespace sessions
