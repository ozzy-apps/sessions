#include "Engine.h"
#include "Convolver.h"
#include "Nam.h"
#include "Resample.h"
#include "Driver.h"
#include "core/Model.h"
#include <QFile>
#include <QSet>
#include <QTimer>
#include <QtDebug>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#if defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace sessions::audio {

using namespace dsp;

// ===========================================================================
// RenderState: an immutable snapshot of everything the audio thread needs.
// ===========================================================================

struct ClipData {
    struct N { double start, length; int pitch; float velocity, chance; };
    int    id = 0;
    double length = 4;
    bool   loop = true;
    double start = 0, loopStart = 0;   // 0.23: the start marker and the brace (Model.h: clipPositionAt / forClipPasses)
    double end() const { return loopStart + length; }
    double firstPass() const { return end() > start ? end() - start : length; }   // beats from a launch to the end marker: "one loop" for follow actions
    bool   legato = false;
    bool   followEnabled = false;
    int    followA = 0, followB = 0;   // FollowAction values
    float  followChanceA = 1;
    double followTime = 0;             // 0 = one loop
    std::vector<N> notes;              // sorted by start
    // audio clips
    bool   audio = false;
    SamplePtr sample;                  // as it plays: reversed for a reversed clip; null = silent
    std::vector<WarpMarker> markers;
    bool   repitch = false;
    int    stretch = 0;                // StretchMode: Tones / Texture / Complex / Complex Pro (0: Beats or Re-Pitch)
    double grainSec = 0.05, flux = 0.25, formants = 0;   // their settings
    bool   warp = true;                // off: the file's own speed from the first marker
    double grain = 0;                  // Beats mode slices: 0 = transients, else a grid in beats
    int    beatsTail = 0;               // 0 gate, 1 forward sustain, 2 reflected sustain
    std::shared_ptr<const Stretcher> prepared; // initial spectral frames, prepared off the callback
    std::shared_ptr<const Stretcher> preparedLoop;
    double preparedStart = 0;
    double preparedTempo = 0;
    float preparedRate = 0;
    float  gain = 1;                   // linear
    double ratio = 1;                  // transpose, as a playback-rate factor
    double secondsAt(double beat) const { return warpSecondsAt(markers.data(), int(markers.size()), beat); }
    double beatAt(double seconds) const {   // the warp map backwards
        const int n = int(markers.size());
        if (n < 2) return (seconds - (n ? markers.front().seconds : 0.0)) * 2.0 + (n ? markers.front().beat : 0.0);
        int i = 0;
        while (i < n - 2 && markers[size_t(i) + 1].seconds <= seconds) ++i;
        const WarpMarker& a = markers[size_t(i)];
        const WarpMarker& b = markers[size_t(i) + 1];
        const double span = b.seconds - a.seconds;
        return span > 1e-12 ? a.beat + (seconds - a.seconds) * (b.beat - a.beat) / span : a.beat;
    }
    // clip envelopes, resolved: what each one drives
    struct Env { int kind; int index; int deviceId; std::vector<AutoPoint> pts; };   // kind: 0 volume, 1 pan, 2 send, 3 device knob
    std::vector<Env> envs;
    // Takes and the comp (loop-recorded clips): the clip is played from its takes, segment by
    // segment. segEnd[i] ends segment i (the last one at `length`), segTake[i] is the take it
    // plays; neighbouring segments crossfade where the take changes.
    std::vector<SamplePtr> takeSamples;   // null entries: missing files
    std::vector<double>    takeStart;     // clip beat where each take's file begins
    std::vector<double>    segEnd;
    std::vector<int>       segTake;
    bool comped() const { return !segTake.empty(); }
};
using ClipRef = std::shared_ptr<const ClipData>;

namespace {
StretchSource stretchSource(const ClipData& c, double secPerBeat) {
    StretchSource source;
    source.sample = c.sample.get();
    source.markers = c.markers.data();
    source.nMarkers = int(c.markers.size());
    source.secPerBeat = secPerBeat;
    source.ratio = c.ratio;
    source.grainSec = c.grainSec;
    source.flux = c.flux;
    source.formants = c.formants;
    source.mode = StretchMode(c.stretch);
    return source;
}

float curveAt(const std::vector<AutoPoint>& pts, double beat) {
    if (pts.empty()) return 0;
    if (beat <= pts.front().beat) return float(pts.front().value);
    if (beat >= pts.back().beat) return float(pts.back().value);
    auto it = std::upper_bound(pts.begin(), pts.end(), beat, [](double b, const AutoPoint& p) { return b < p.beat; });
    const AutoPoint& b = *it;
    const AutoPoint& a = *(it - 1);
    const double span = b.beat - a.beat;
    return float(span > 1e-9 ? a.value + (b.value - a.value) * (beat - a.beat) / span : b.value);
}
} // namespace

struct Placement { double start, length, offset; ClipRef clip; double fadeIn = 0, fadeOut = 0, fadeInCurve = 0.5, fadeOutCurve = 0.5; };

// An automation envelope, resolved to what it drives.
struct Auto {
    std::vector<AutoPoint> pts;   // sorted by beat
    float valueAt(double beat) const {
        if (pts.empty()) return 0;
        if (beat <= pts.front().beat) return float(pts.front().value);
        if (beat >= pts.back().beat) return float(pts.back().value);
        auto it = std::upper_bound(pts.begin(), pts.end(), beat, [](double b, const AutoPoint& p) { return b < p.beat; });
        const AutoPoint& b = *it;
        const AutoPoint& a = *(it - 1);
        const double span = b.beat - a.beat;
        return float(span > 1e-9 ? a.value + (b.value - a.value) * (beat - a.beat) / span : b.value);
    }
};

struct DeviceSlot {
    std::shared_ptr<DeviceProcessor> proc;
    std::shared_ptr<const IrKernel> ir;   // Guitar Amp: its loaded impulse response
    std::shared_ptr<nam::Runtime> nam;    // NAM: its capture's network and state (shared with the cache; one state is live at a time)
    int        deviceId = 0;
    DeviceType type = DeviceType::Unknown;
    int        keyTrack = -1;   // sidechain: index of the track that keys it (-1: its own input, or a missing source)
    std::vector<ModRoute> mods; // Monolith: its modulation routes
    CurvePoint curves[monolith::kCurves * osmium::kCurvePoints] = {};   // Monolith/Osmium: preallocated drawn curves
    int        curveCount[monolith::kCurves] = {};
    SamplePtr  sample;   // Sampler
    double     slices[sampler::kMaxSlices] = {};   // Sampler: its manual slices (seconds)
    int        sliceCount = 0;
    double     sampleBpm = 0;                      // Sampler: the sample's own tempo
    SamplePtr  pads[drumrack::kPads];   // Drum Rack
    DeviceKind kind = DeviceKind::AudioEffect;
    bool  enabled = true;
    int   rack = 0, chain = 0;     // in a rack's chain (or on a Drum Rack's pad): the rack's device id and the chain (pad)
    int   rackType = 0;            // a rack itself: 1 Instrument Rack, 2 Audio Effect Rack, 3 MIDI Effect Rack, 4 a Drum Rack holding chains
    int   chains = 0;              // a rack: how many chains it has
    ChainZones zones[racks::kChains];   // a rack: its chains' zones
    unsigned padHas = 0, padInst = 0;   // a Drum Rack: the pads holding devices (bit k = pad k), and those holding an instrument
    float params[kMaxParams] = {};
    int   paramAuto[kMaxParams];   // index into TrackData::autos, -1 = not automated
    DeviceSlot() { std::fill(paramAuto, paramAuto + kMaxParams, -1); }
};

struct TrackData {
    int id = 0;
    bool audio = false;   // an audio track: clips are audio, the chain holds audio effects
    bool group = false;   // a group track: plays its bus (its tracks), no clips
    int  bus = -1;        // group tracks: their bus; other tracks: their group's bus (-1 = Main)
    ArrangementSpan groupSpan;
    double groupFadeIn = 0, groupFadeOut = 0, groupFadeInCurve = 0.5, groupFadeOutCurve = 0.5;
    std::vector<int> groupMembers; // resolved once on the UI thread, used for Session/input bypass
    bool soloed = false;  // heard while something is soloed (its own solo, its group's, or one of its tracks')
    bool keySource = false;   // some device takes its sidechain key from this track: its taps are copied each block
    bool armed = false;
    int  monitor = int(Monitor::Auto);
    int  inputChannel = 0;
    std::vector<ClipRef>    slotClips;
    std::vector<Placement>  arrangement;   // sorted by start
    std::vector<DeviceSlot> devices;
    std::vector<Auto>       autos;
    // Macros: a knob (device slot `src`, parameter `macro`) setting another slot's parameter
    struct Link { int src, macro, dst, param; float lo, hi; };
    std::vector<Link>       links;
    int   volAuto = -1, panAuto = -1, sendAuto[kMaxReturns] = {-1, -1, -1, -1, -1, -1, -1, -1};
    float volume = 0.75f, pan = 0, sends[kMaxReturns] = {};
    bool  active = true, solo = false, preFader = false;
};

struct RenderState {
    quint64 generation = 0;
    std::vector<TrackData> tracks, returns;
    TrackData master;         // the Master's chain: every track and return goes through it, before the master fader (0.21)
    std::vector<int> order;   // the regular (non-group) tracks in rendering order: sidechain sources before what they key
    float  mainVolume = 0.75f, cueVolume = 0.6f;
    double tempo = 120;
    bool   anySolo = false;
    SamplePtr preview;          // the browser's preview, restarted whenever previewSerial changes
    quint64   previewSerial = 0;
};

namespace {

void flushDenormals() {
#if defined(__x86_64__) || defined(__SSE__)
    _mm_setcsr(_mm_getcsr() | 0x8040);   // flush-to-zero + denormals-are-zero
#endif
}

void raisePeak(std::atomic<float>& a, float p) {
    if (p > a.load(std::memory_order_relaxed)) a.store(p, std::memory_order_relaxed);
}

// Keeps the output inside +-1 without hard clipping; transparent below -1 dBFS.
inline float softClip(float x) {
    const float a = std::fabs(x);
    if (a <= 0.9f) return x;
    const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
    return x < 0 ? -y : y;
}

// The notes a rack's chain gets: note-ons inside its key and velocity zones while the Chain
// selector is inside its chain zone (softer in the fades), moved by `transpose` and scaled by
// `velocity`; releases always pass, so nothing hangs. `notes`: false drops every note-on (a chain
// that's off or not soloed).
void chainNotes(const EventList& in, EventList& out, const ChainZones& z, int select, bool notes, int transpose = 0, float velocity = 1) {
    out.clear();
    const double selected = notes ? z.select.weight(select) : 0.0;
    for (int i = 0; i < in.count; ++i) {
        NoteEvent e = in.ev[i];
        if (!e.allOff) e.pitch = std::clamp(e.pitch + transpose, 0, 127);
        if (!e.allOff && !e.noteOff) {
            const int vel = std::clamp(int(std::lround(in.ev[i].velocity * 127)), 1, 127);
            const double w = selected * z.key.weight(in.ev[i].pitch) * z.vel.weight(vel);
            if (w <= 0) continue;
            e.velocity = std::clamp(float(e.velocity * w * velocity), 1.0f / 127, 1.0f);
        }
        out.ev[out.count++] = e;
    }
}

inline int offsetOf(double beat, double b0, double bps, int n) {
    // An event sounds on the first sample at or after its beat, just like audio segments.
    return std::clamp(int(std::ceil((beat - b0) / bps - 1e-6)), 0, n - 1);
}

int slotOfClip(const TrackData& td, int clipId) {
    if (!clipId) return -1;
    for (int s = 0; s < int(td.slotClips.size()); ++s)
        if (td.slotClips[size_t(s)] && td.slotClips[size_t(s)]->id == clipId) return s;
    return -1;
}

int clipIdAt(const TrackData& td, int slot) {
    return slot >= 0 && slot < int(td.slotClips.size()) && td.slotClips[size_t(slot)] ? td.slotClips[size_t(slot)]->id : 0;
}

// Note-ons of `c` (launched at `origin`, its first pass from clip beat `start`, then round the
// brace) that start inside [from, to), limited to [lo, hi) for arrangement placements.
void emitClip(const ClipData& c, double origin, double start, double lo, double hi, double from, double to,
              double b0, double bps, int n, EventList& ev, Noise& rng) {
    if (c.length <= 0 || c.notes.empty()) return;
    const double r0 = std::max(from, lo) - origin, r1 = std::min(to, hi) - origin;
    if (r1 <= r0) return;
    forClipPasses(start, c.loopStart, c.length, c.loop, r0, r1, [&](double at, double pFrom, double pTo) {
        for (const ClipData::N& note : c.notes) {
            if (note.start < pFrom || note.start >= pTo) continue;
            const double t = origin + at + (note.start - pFrom);
            if (t < from || t >= to || t < lo || t >= hi) continue;
            if (note.chance < 1 && rng.uniform() >= note.chance) continue;
            double len = std::min(note.length, pTo - note.start);
            len = std::min(len, hi - t);
            ev.add({offsetOf(t, b0, bps, n), note.pitch, note.velocity, std::max(16, int(len / bps)), false, false});
        }
    });
}

constexpr double kNoLimit = 1e300;

} // namespace

// ===========================================================================
// UI thread
// ===========================================================================

Engine::Engine(Project* project, QObject* parent) : QObject(parent), m_project(project) {
    resample::prepare();
    deviceSpecs();   // build the device table here, never first on the audio thread
    m_inBlock.assign(size_t(kMaxInputFrames) * 2, 0.0f);
    m_keyStore.assign(size_t(3 * kMaxTracks * 2) * kMaxBlock, 0.0f);   // sidechain taps, never resized again
    auto clipsChanged = [this] { m_clipsDirty = true; scheduleRepublish(); };
    connect(project, &Project::structureChanged, this, clipsChanged);
    connect(project, &Project::clipChanged, this, clipsChanged);
    connect(project, &Project::mixerChanged, this, &Engine::scheduleRepublish);
    connect(project, &Project::deviceChanged, this, &Engine::scheduleRepublish);
    connect(project, &Project::automationChanged, this, &Engine::scheduleRepublish);
    connect(project, &Project::tempoChanged, this, clipsChanged); // refresh prepared launch frames at the new tempo
    publish();
}

Engine::~Engine() {
    if (m_inputDriver) m_inputDriver->stop();
    m_inputDriver.reset();
    shutdown();
    delete m_state;
    delete m_pending.exchange(nullptr);
    collectGarbage();
}

void Engine::start() {
    if (m_driver) return;
    const QByteArray mode = qgetenv("SESSIONS_AUDIO");   // "none" = silent, "pulse" = try the server even offscreen
    const bool silent = mode == "none" || (mode != "pulse" && qgetenv("QT_QPA_PLATFORM") == "offscreen");
    QString error;
    m_duplex = false;
    if (!silent && m_output.system == "jack") {   // PipeWire's JACK interface: low latency, the interface's own channels
        QString why;
        if (auto d = makeJackDriver(); d && d->open(int(m_sr), m_output, &why)) {
            m_driver = std::move(d);
            m_hasAudio = true;
            m_duplex = true;
            m_outputName = m_driver->name();
        } else {
            m_jackError = d ? why : QString("no JACK library (install pipewire-jack)");
            qWarning("Sessions: PipeWire / JACK unavailable (%s), using PulseAudio", qPrintable(m_jackError));
        }
    }
    if (!silent && !m_driver) {
        if (auto d = makePulseDriver(); d && d->open(int(m_sr), m_output, &error)) {
            m_driver = std::move(d);
            m_hasAudio = true;
            m_outputName = m_driver->name();
            if (m_output.system == "jack") m_outputName += " (PipeWire / JACK unavailable: " + m_jackError + ")";
        } else if (!d) {
            error = "built without PulseAudio";
        }
    }
    if (!m_driver) {
        m_driver = makeNullDriver();
        m_driver->open(int(m_sr), m_output, nullptr);
        m_hasAudio = false;
        m_disabled = silent;
        m_outputName = silent ? QString("Silent (audio disabled)") : "No audio output: " + error;
        if (!silent) qWarning("Sessions: %s", qPrintable(m_outputName));
    }
    if (m_duplex) m_driver->startDuplex([this](const float* in, float* out, int frames) { process(out, frames, in); });
    else m_driver->start([this](float* buf, int frames) { process(buf, frames); });
    if (m_inputWanted) { m_inputWanted = false; setInputWanted(true); }   // the input follows the driver
}

void Engine::setOutput(const DriverOptions& options) {
    m_output = options;
    if (!m_driver) return;
    if (m_inputDriver) { m_inputDriver->stop(); m_inputDriver.reset(); }
    shutdown();   // the audio thread stops; the render state and DSP survive
    start();
}

void Engine::shutdown() {
    if (m_driver) m_driver->stop();
    m_driver.reset();
}

double Engine::latencySeconds() const { return m_driver ? m_driver->latency() : 0.0; }

quint64 Engine::send(Command c) {
    c.seq = ++m_nextSeq;
    if (!m_commands.push(c)) qWarning("Sessions: audio command queue full, dropped a command");
    return c.seq;
}

void Engine::scheduleRepublish() {
    if (m_republishQueued) return;
    m_republishQueued = true;
    QTimer::singleShot(0, this, &Engine::publish);   // coalesce bursts (drags) into one snapshot
}

void Engine::publish() {
    m_republishQueued = false;
    if (m_clipsDirty) { m_clipCache.clear(); m_clipsDirty = false; }

    auto* s = new RenderState;
    s->generation = ++m_generation;
    s->tempo = m_project->tempo;
    s->mainVolume = float(m_project->mainVolume);
    s->cueVolume = float(m_project->cueVolume);
    s->anySolo = m_project->anySolo();
    s->preview = m_preview;
    s->previewSerial = m_previewSerial;

    auto clipRef = [this](const ClipPtr& c) -> ClipRef {
        if (!c) return nullptr;
        if (auto it = m_clipCache.constFind(c->id); it != m_clipCache.constEnd()) return *it;
        auto d = std::make_shared<ClipData>();
        d->id = c->id;
        d->length = c->length;
        d->loop = c->loop;
        d->start = std::max(0.0, c->start);
        d->loopStart = std::max(0.0, c->loopStart);
        d->legato = c->legato;
        d->followEnabled = c->followEnabled;
        d->followA = int(c->followA);
        d->followB = int(c->followB);
        d->followChanceA = float(c->followChanceA);
        d->followTime = c->followTime;
        d->notes.reserve(size_t(c->notes.size()));
        for (const Note& n : c->notes) d->notes.push_back({n.start, n.length, n.pitch, float(n.velocity), float(n.chance)});
        std::sort(d->notes.begin(), d->notes.end(), [](const ClipData::N& a, const ClipData::N& b) { return a.start < b.start; });
        if (c->isAudio()) {
            d->audio = true;
            d->sample = c->reverse ? samples::reversed(c->sample) : c->sample;
            d->markers.assign(c->warpMarkers.begin(), c->warpMarkers.end());
            d->repitch = c->warpMode == WarpMode::Repitch;
            d->stretch = c->warpMode == WarpMode::Tones ? int(StretchMode::Tones) : c->warpMode == WarpMode::Texture ? int(StretchMode::Texture)
                       : c->warpMode == WarpMode::Complex ? int(StretchMode::Complex) : c->warpMode == WarpMode::ComplexPro ? int(StretchMode::ComplexPro) : 0;
            d->grainSec = std::clamp(c->grainMs, 12.0, 256.0) / 1000.0;
            d->flux = std::clamp(c->flux, 0.0, 1.0);
            d->formants = std::clamp(c->formants, 0.0, 1.0);
            d->warp = c->warp;
            d->grain = c->grain;
            d->beatsTail = std::clamp(c->beatsTail, 0, 2);
            d->gain = float(std::pow(10.0, c->gain / 20.0));
            d->ratio = std::exp2((c->transpose + c->detune / 100.0) / 12.0);
            if (!c->takes.isEmpty()) {   // played from its takes: the comp's regions, the newest take in between
                for (const Take& t : c->takes) { d->takeSamples.push_back(t.sample); d->takeStart.push_back(t.startBeat); }
                const int newest = int(c->takes.size()) - 1;
                double at = 0;
                auto seg = [&](double end, int take) {
                    end = std::min(end, c->length);
                    if (end <= at + 1e-12) return;
                    if (!d->segTake.empty() && d->segTake.back() == take) d->segEnd.back() = end;
                    else { d->segEnd.push_back(end); d->segTake.push_back(take); }
                    at = end;
                };
                for (const CompRegion& r : c->comp) {
                    if (r.take < 0 || r.take >= c->takes.size()) continue;
                    if (r.start > at) seg(r.start, newest);
                    seg(r.end, r.take);
                }
                seg(c->length, newest);
            }
            if (d->sample && d->warp && d->stretch >= int(StretchMode::Complex) && !d->comped()) {
                auto prepared = std::make_shared<Stretcher>();
                const auto source = stretchSource(*d, 60.0 / m_project->tempo);
                d->preparedStart = clipPositionAt(d->start, d->loopStart, d->length, d->loop, 0);
                prepared->prime(source, d->preparedStart, m_sr);
                d->prepared = std::move(prepared);
                if (d->loop && d->loopStart != d->preparedStart) {
                    auto loop = std::make_shared<Stretcher>();
                    loop->prime(source, d->loopStart, m_sr);
                    d->preparedLoop = std::move(loop);
                }
                d->preparedTempo = m_project->tempo;
                d->preparedRate = m_sr;
            }
        }
        for (const Envelope& e : c->envelopes) {
            if (e.points.isEmpty()) continue;
            const QStringList parts = e.target.split(':');
            ClipData::Env env{-1, 0, 0, std::vector<AutoPoint>(e.points.begin(), e.points.end())};
            if (e.target == target::volume()) env.kind = 0;
            else if (e.target == target::pan()) env.kind = 1;
            else if (parts.size() == 3 && parts[1] == "send") { env.kind = 2; env.index = parts[2].toInt(); }
            else if (parts.size() == 3 && parts[0] == "device") { env.kind = 3; env.deviceId = parts[1].toInt(); env.index = parts[2].toInt(); }
            if (env.kind >= 0 && env.index >= 0 && env.index < kMaxParams) d->envs.push_back(std::move(env));
        }
        m_clipCache.insert(c->id, d);
        return d;
    };

    QSet<int> seen;
    auto fill = [&](const Track& t, TrackData& td) {
        td.id = t.id;
        td.audio = t.type == TrackType::Audio;
        td.volume = float(t.volume);
        td.pan = float(t.pan);
        for (int k = 0; k < t.sends.size() && k < kMaxReturns; ++k) td.sends[k] = float(t.sends[k]);
        td.active = t.active;
        td.solo = t.solo;
        td.preFader = t.preFader;
        td.group = t.isGroup();
        td.armed = t.armed;
        td.monitor = int(t.monitor);
        td.inputChannel = t.inputChannel;
        QHash<int, int> deviceIndex;   // device id -> slot in td.devices
        for (const Device& d : t.devices) {
            DeviceSlot slot;
            slot.deviceId = d.id;
            slot.kind = d.kind;
            slot.rack = d.rack;
            slot.chain = d.chain;
            slot.chains = std::min(int(d.chains.size()), racks::kChains);
            slot.enabled = d.enabled;
            slot.sample = d.sample;
            slot.sampleBpm = d.sampleBpm;
            slot.sliceCount = std::min(int(d.slices.size()), sampler::kMaxSlices);
            for (int k = 0; k < slot.sliceCount; ++k) slot.slices[k] = d.slices[k];
            for (int k = 0; k < d.pads.size() && k < drumrack::kPads; ++k) slot.pads[k] = d.pads[k].sample;
            for (const ModRoute& r : d.mods) if (int(slot.mods.size()) < monolith::kMaxRoutes) slot.mods.push_back(r);
            for (int c = 0; c < monolith::kCurves && c < d.curves.size(); ++c) {
                const QVector<CurvePoint>& pts = d.curves[c];
                slot.curveCount[c] = std::min(int(pts.size()), deviceType(d.name) == DeviceType::Osmium ? osmium::kCurvePoints : monolith::kCurvePoints);
                for (int k = 0; k < slot.curveCount[c]; ++k) slot.curves[c * osmium::kCurvePoints + k] = pts[k];
            }
            for (int i = 0; i < d.params.size() && i < kMaxParams; ++i) slot.params[i] = float(d.params[i].value);
            const DeviceType type = deviceType(d.name);
            slot.type = type;
            slot.rackType = type == DeviceType::InstrumentRack ? 1 : type == DeviceType::AudioEffectRack ? 2 : type == DeviceType::MidiEffectRack ? 3
                          : type == DeviceType::DrumRack && d.rack == 0 ? 4 : 0;
            for (int c = 0; c < slot.chains && c < d.zones.size(); ++c) slot.zones[c] = d.zones[c];
            if (m_exporting && sidechain::supports(type)) slot.params[sidechain::param(type, sidechain::Listen)] = 0;   // never export the key
            if (holdsIr(type) && d.sample) {   // prepare the IR here, never on the audio thread
                auto k = m_irCache.value(d.sample.get());
                if (!k) { k = makeIrKernel(d.sample, m_sr); m_irCache.insert(d.sample.get(), k); }
                slot.ir = k;
            }
            if (type == DeviceType::Nam) {   // the capture's network, built and prewarmed here; the audio thread only runs it
                nam::ModelPtr chosen = d.model;   // Quality Light: the lightest size a container holds
                if (chosen && !chosen->variants.empty() && choiceIndex(type, namdev::Quality, slot.params[namdev::Quality]) == 1) chosen = chosen->variants.front().model;
                auto rt = m_namCache.value(d.id);
                if (!chosen) { rt.reset(); m_namCache.remove(d.id); }
                else if (!rt || rt->model() != chosen || rt->engineRate() != m_sr) { rt = std::make_shared<nam::Runtime>(chosen, m_sr); m_namCache.insert(d.id, rt); }
                slot.nam = rt;
            }
            auto it = m_procs.find(d.id);
            if (it == m_procs.end() || it->type != type) it = m_procs.insert(d.id, {type, createProcessor(type, m_sr)});
            slot.proc = it->proc;
            seen.insert(d.id);
            deviceIndex.insert(d.id, int(td.devices.size()));
            td.devices.push_back(std::move(slot));
        }
        // Drum Racks: which pads hold devices, and which an instrument
        for (const Device& d : t.devices)
            if (d.rack && deviceIndex.contains(d.rack) && d.chain >= 0 && d.chain < drumrack::kPads) {
                DeviceSlot& holder = td.devices[size_t(deviceIndex.value(d.rack))];
                if (holder.rackType != 4) continue;
                holder.padHas |= 1u << d.chain;
                if (d.kind == DeviceKind::Instrument) holder.padInst |= 1u << d.chain;
            }
        // Macros knobs and what they turn (only parameters of devices on this track)
        for (const Device& d : t.devices)
            for (const MacroMap& m : d.maps)
                if (deviceIndex.contains(m.deviceId) && m.param >= 0 && m.param < kMaxParams && m.macro >= 0 && m.macro < macros::kKnobs)
                    td.links.push_back({deviceIndex.value(d.id), m.macro, deviceIndex.value(m.deviceId), m.param, float(m.lo), float(m.hi)});
        // Automation you've overridden by hand stays off until Re-Enable Automation.
        for (const Envelope& e : t.automation) {
            if (e.points.isEmpty() || t.overriddenTargets.contains(e.target)) continue;
            const QStringList parts = e.target.split(':');
            int* slot = nullptr;
            if (e.target == target::volume()) slot = &td.volAuto;
            else if (e.target == target::pan()) slot = &td.panAuto;
            else if (parts.size() == 3 && parts[1] == "send" && parts[2].toInt() < kMaxReturns) slot = &td.sendAuto[parts[2].toInt()];
            else if (parts.size() == 3 && parts[0] == "device" && deviceIndex.contains(parts[1].toInt()) && parts[2].toInt() < kMaxParams)
                slot = &td.devices[size_t(deviceIndex.value(parts[1].toInt()))].paramAuto[parts[2].toInt()];
            if (!slot) continue;
            *slot = int(td.autos.size());
            td.autos.push_back({std::vector<AutoPoint>(e.points.begin(), e.points.end())});
        }
    };

    // Groups: each group track gets a bus; its tracks play into it. Soloing a group lets its
    // tracks through, and soloing a track lets its group through.
    QHash<int, int> busOf;
    QSet<int> soloedGroups, groupsWithSolo;
    for (const Track& t : m_project->tracks) {
        if (t.isGroup() && busOf.size() < kMaxGroups) busOf.insert(t.id, int(busOf.size()));
        if (t.isGroup() && t.solo) soloedGroups.insert(t.id);
        if (t.groupId && t.solo) groupsWithSolo.insert(t.groupId);
    }
    for (const Track& t : m_project->tracks) {
        TrackData td;
        td.bus = t.isGroup() ? busOf.value(t.id, -1) : busOf.value(t.groupId, -1);
        td.soloed = t.solo || (t.isGroup() ? groupsWithSolo.contains(t.id) : soloedGroups.contains(t.groupId));
        for (const ClipPtr& c : t.clipSlots) td.slotClips.push_back(clipRef(c));
        for (const ArrangementClip& a : t.arrangement) td.arrangement.push_back({a.start, a.length, a.offset, clipRef(a.clip), a.fadeIn, a.fadeOut, a.fadeInCurve, a.fadeOutCurve});
        std::sort(td.arrangement.begin(), td.arrangement.end(), [](const Placement& a, const Placement& b) { return a.start < b.start; });
        fill(t, td);
        if (td.group) {
            const int groupIndex = int(s->tracks.size());
            td.groupSpan = m_project->groupArrangementSpan(groupIndex);
            td.groupFadeIn = t.groupFadeIn;
            td.groupFadeOut = t.groupFadeOut;
            td.groupFadeInCurve = t.groupFadeInCurve;
            td.groupFadeOutCurve = t.groupFadeOutCurve;
            for (int member : m_project->membersOf(groupIndex)) td.groupMembers.push_back(member);
        }
        s->tracks.push_back(std::move(td));
    }
    for (const Track& r : m_project->returns) {
        TrackData td;
        fill(r, td);
        s->returns.push_back(std::move(td));
    }
    fill(m_project->master, s->master);
    // Sidechains: each keyed device finds its source track (a regular, non-group track, not its
    // own); the regular tracks are then ordered so sources render first. A loop (which the model
    // refuses) is cut: the later track reads the previous block's key.
    {
        const int nt = std::min(int(s->tracks.size()), kMaxTracks);
        QHash<int, int> indexOfId, sourceOf;   // track id -> index; device id -> its key's track id
        for (int i = 0; i < nt; ++i) indexOfId.insert(s->tracks[size_t(i)].id, i);
        for (const Track& t : m_project->tracks) for (const Device& x : t.devices) if (x.sidechainTrack) sourceOf.insert(x.id, x.sidechainTrack);
        for (const Track& t : m_project->returns) for (const Device& x : t.devices) if (x.sidechainTrack) sourceOf.insert(x.id, x.sidechainTrack);
        std::vector<std::vector<int>> deps;
        deps.resize(size_t(nt));
        auto resolve = [&](TrackData& td, int self) {
            for (DeviceSlot& d : td.devices) {
                if (!sidechain::supports(d.type) || !sourceOf.contains(d.deviceId)) continue;
                const int si = indexOfId.value(sourceOf.value(d.deviceId), -1);
                if (si < 0 || si == self || s->tracks[size_t(si)].group) continue;   // missing, itself, a group: a silent key
                d.keyTrack = si;
                s->tracks[size_t(si)].keySource = true;
                if (self >= 0) deps[size_t(self)].push_back(si);
            }
        };
        for (int i = 0; i < nt; ++i) resolve(s->tracks[size_t(i)], i);
        for (TrackData& r : s->returns) resolve(r, -1);
        resolve(s->master, -1);
        std::vector<int> state(size_t(nt), 0);
        std::function<void(int)> visit = [&](int i) {
            if (state[size_t(i)]) return;
            state[size_t(i)] = 1;
            for (int j : deps[size_t(i)]) if (state[size_t(j)] != 1) visit(j);
            state[size_t(i)] = 2;
            if (!s->tracks[size_t(i)].group) s->order.push_back(i);
        };
        for (int i = 0; i < nt; ++i) visit(i);
    }
    // Forget processors of deleted devices. The audio thread keeps its own
    // reference until it retires the state that still uses them.
    for (auto it = m_procs.begin(); it != m_procs.end();)
        it = seen.contains(it.key()) ? std::next(it) : m_procs.erase(it);
    for (auto it = m_namCache.begin(); it != m_namCache.end();)
        it = seen.contains(it.key()) ? std::next(it) : m_namCache.erase(it);

    delete m_pending.exchange(s, std::memory_order_acq_rel);   // an unclaimed older snapshot is ours to free
    collectGarbage();
}

void Engine::collectGarbage() {
    RenderState* s = nullptr;
    while (m_retired.pop(s)) delete s;
}

Engine::TrackStatus Engine::trackStatus(int i) const {
    if (i < 0 || i >= kMaxTracks) return {-1, -2, 0, 0, false};
    const AtomicStatus& s = m_status[size_t(i)];
    return {s.playingSlot.load(std::memory_order_relaxed), s.queuedSlot.load(std::memory_order_relaxed),
            s.slotStart.load(std::memory_order_relaxed), s.queuedAt.load(std::memory_order_relaxed),
            s.overridden.load(std::memory_order_relaxed), s.clipBeat.load(std::memory_order_relaxed),
            s.clipPass.load(std::memory_order_relaxed)};
}

float Engine::takeTrackPeak(int i)  { return i >= 0 && i < kMaxTracks ? m_trackPeak[size_t(i)].exchange(0) : 0.0f; }
float Engine::takeReturnPeak(int i) { return i >= 0 && i < kMaxReturns ? m_returnPeak[size_t(i)].exchange(0) : 0.0f; }
float Engine::takeMainPeak()        { return m_mainPeak.exchange(0); }

bool Engine::renderToWav(const QString& path, double seconds) {
    if (m_driver) return false;
    publish();
    send({Command::Stop, -1, -1, 0, 0, true});
    send({Command::Play});
    const int total = std::max(0, int(seconds * m_sr));
    std::vector<int16_t> pcm(size_t(total) * 2);
    float buf[512 * 2];
    for (int done = 0; done < total;) {
        const int n = std::min(512, total - done);
        process(buf, n);
        for (int i = 0; i < n * 2; ++i)
            pcm[size_t(done) * 2 + size_t(i)] = int16_t(std::lround(std::clamp(buf[i], -1.0f, 1.0f) * 32767.0f));
        done += n;
    }
    send({Command::Stop, -1, -1, 0, 0, true});
    collectGarbage();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    auto le = [&f](quint32 v, int bytes) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; f.write(b, bytes); };
    const quint32 dataBytes = quint32(pcm.size() * 2), rate = quint32(m_sr);
    f.write("RIFF"); le(36 + dataBytes, 4); f.write("WAVE");
    f.write("fmt "); le(16, 4); le(1, 2); le(2, 2); le(rate, 4); le(rate * 4, 4); le(4, 2); le(16, 2);
    f.write("data"); le(dataBytes, 4);
    for (int16_t v : pcm) le(quint16(v), 2);
    return f.error() == QFileDevice::NoError;
}

bool Engine::renderOffline(const QString& path, const RenderOptions& o, const std::function<bool(double)>& progress, QString* error) {
    if (m_driver) { if (error) *error = "The engine is playing live."; return false; }
    m_exporting = true;   // Listen switches stay out of the file
    publish();
    send({Command::Stop, -1, -1, 0, 0, true});
    send({Command::Locate, -1, -1, std::max(0.0, o.startBeat)});
    send({Command::Play});
    const double seconds = std::max(0.0, o.lengthBeats) * 60.0 / std::max(1.0, m_project->tempo) + std::max(0.0, o.tailSeconds);
    const qint64 total = qint64(std::ceil(seconds * m_sr));
    constexpr int kChunk = 4096;
    std::vector<float> buf(size_t(kChunk) * 2);
    std::vector<float> all;   // normalizing needs the whole render before writing
    samples::WavWriter writer;
    if (!o.normalize && !writer.open(path, int(m_sr), o.format, error)) return false;
    if (o.normalize) all.reserve(size_t(total) * 2);
    float peak = 0;
    for (qint64 done = 0; done < total;) {
        const int n = int(std::min<qint64>(kChunk, total - done));
        process(buf.data(), n);
        collectGarbage();
        if (o.normalize) {
            all.insert(all.end(), buf.begin(), buf.begin() + n * 2);
            for (int i = 0; i < n * 2; ++i) peak = std::max(peak, std::fabs(buf[size_t(i)]));
        } else {
            writer.write(buf.data(), n);
        }
        done += n;
        if (progress && !progress(double(done) / double(total))) {
            if (!o.normalize) writer.close();
            QFile::remove(path);
            if (error) *error = "Cancelled.";
            return false;
        }
    }
    send({Command::Stop, -1, -1, 0, 0, true});
    if (o.normalize) {
        const float k = peak > 1e-6f ? 0.98855f / peak : 1.0f;   // -0.1 dBFS
        for (float& v : all) v *= k;
        return samples::writeWav(path, all.data(), qint64(all.size() / 2), int(m_sr), o.format, error);
    }
    return writer.close(error);
}

float Engine::takeChainPeak(int rackId, int chain) {
    const auto it = m_procs.constFind(rackId);
    ChainMixer* mixer = it == m_procs.constEnd() || !it->proc ? nullptr : it->proc->chainMixer();
    return mixer ? mixer->takePeak(chain) : 0.0f;
}

float Engine::deviceMeter(int deviceId, int which) const {
    const auto it = m_procs.constFind(deviceId);
    if (it == m_procs.constEnd() || !it->proc || which < 0 || which > 2) return which == 2 ? -120.0f : 0.0f;
    return it->proc->meter[which].load(std::memory_order_relaxed);
}

float Engine::deviceScope(int deviceId, int which) const {
    const auto it = m_procs.constFind(deviceId);
    if (it == m_procs.constEnd() || !it->proc || which < 0 || which > 15) return 0.0f;
    return it->proc->scope[which].load(std::memory_order_relaxed);
}

void Engine::previewSample(const SamplePtr& sample) {
    m_preview = sample;
    ++m_previewSerial;
    publish();
}

// ---- audio input --------------------------------------------------------------

void Engine::setInputDevice(const QString& device) {
    if (device == m_inputDevice) return;
    m_inputDevice = device;
    if (m_inputWanted) { setInputWanted(false); setInputWanted(true); }   // reopen on the new source
}

void Engine::setInputWanted(bool wanted) {
    if (wanted == m_inputWanted) return;
    m_inputWanted = wanted;
    if (!wanted) {
        m_inputLive.store(false, std::memory_order_release);
        if (m_inputDriver) m_inputDriver->stop();
        m_inputDriver.reset();
        m_inputStatus = "Off";
        return;
    }
    const QByteArray mode = qgetenv("SESSIONS_AUDIO");
    const bool silent = mode == "none" || (mode != "pulse" && qgetenv("QT_QPA_PLATFORM") == "offscreen");
    if (silent) {   // tests: only pushInput() feeds it
        m_inputStatus = "Test input";
        m_inputLive.store(true, std::memory_order_release);
        return;
    }
    if (m_duplex) {   // the JACK driver brings the input with every period: nothing to open
        m_inputStatus = "Your interface's inputs (PipeWire / JACK)";
        m_inputLive.store(true, std::memory_order_release);
        return;
    }
    QString error;
    auto d = makePulseInput();
    if (!d) { m_inputStatus = "No audio input (built without PulseAudio)"; return; }
    if (!d->open(int(m_sr), m_inputDevice, &error)) { m_inputStatus = "No audio input: " + error; qWarning("Sessions: %s", qPrintable(m_inputStatus)); return; }
    m_inputStatus = d->name();
    m_inputDriver = std::move(d);
    m_inputLive.store(true, std::memory_order_release);
    m_inputDriver->start([this](const float* buf, int frames) { pushInput(buf, frames); });
}

void Engine::pushInput(const float* buf, int frames) {
    if (!m_inputLive.load(std::memory_order_acquire)) return;
    m_inRing.write(buf, frames * 2);   // a full ring (nobody reading) just drops the newest block
}

double Engine::inputLatencySeconds() const {
    if (m_duplex && m_driver) return m_driver->inputLatency();
    return (m_inputDriver ? m_inputDriver->latency() : 0.0) + double(m_inFill.load(std::memory_order_relaxed));
}

float Engine::takeInputPeak(int i) { return i >= 0 && i < kMaxTracks ? m_inputPeak[size_t(i)].exchange(0) : 0.0f; }

void Engine::startCapture() {
    m_capStarted.store(false, std::memory_order_release);
    float scratch[1024];
    while (m_recRing.read(scratch, 1024) > 0) {}   // nothing left over from an earlier take
    send({Command::Capture, -1, -1, 0, 0, true});
}

void Engine::stopCapture() { send({Command::Capture, -1, -1, 0, 0, false}); }

int Engine::takeRecorded(float* dst, int maxFrames) { return m_recRing.read(dst, maxFrames * 2) / 2; }

// Audio thread: one process() call's worth of input. The ring first builds a small cushion
// (the capture and output threads run in step but not in phase); when it has grown past
// ~85 ms (clock drift, a stall) the oldest audio is dropped to keep the latency down.
void Engine::pullInput(int frames) {
    const int want = std::min(frames, kMaxInputFrames) * 2;
    float* dst = m_inBlock.data();
    if (!m_inputLive.load(std::memory_order_acquire)) {
        std::fill(dst, dst + want, 0.0f);
        m_inPrimed = false;
        m_inRing.read(nullptr, m_inRing.available());
        return;
    }
    int avail = m_inRing.available();
    const int cushion = 2 * 512;
    if (!m_inPrimed) {
        if (avail < want + cushion && m_hasAudio) { std::fill(dst, dst + want, 0.0f); return; }
        m_inPrimed = true;
    }
    const int maxBacklog = want + 2 * 4096;
    if (avail > maxBacklog) { m_inRing.read(nullptr, avail - (want + cushion)); avail = m_inRing.available(); }
    const int got = m_inRing.read(dst, want);
    if (got < want) { std::fill(dst + got, dst + want, 0.0f); if (m_hasAudio) m_inPrimed = false; }
    m_inFill.store(float(m_inRing.available() / 2) / m_sr, std::memory_order_relaxed);
}

// ===========================================================================
// Audio thread
// ===========================================================================

void Engine::process(float* out, int frames, const float* in) {
    const auto t0 = std::chrono::steady_clock::now();
    flushDenormals();

    // Pick up the newest snapshot; hand the old one back for the UI to free.
    if (!m_retired.full()) {
        if (RenderState* s = m_pending.exchange(nullptr, std::memory_order_acq_rel)) {
            RenderState* previous = m_state;
            adopt(s);
            if (previous) m_retired.push(previous); // adopt transfers live positions from the old immutable state
        }
    }
    Command c;
    while (m_commands.pop(c)) apply(c);
    if (m_capPending) {   // the first captured frame is this call's first, at this beat
        m_capPending = false;
        // during a count-in the playhead waits: the capture's first frame is that far before it
        m_capStartBeat.store(m_beat - m_countIn, std::memory_order_relaxed);
        m_capStarted.store(true, std::memory_order_release);
    }
    if (in) {   // a duplex driver: this period's input, straight in (no ring, no cushion)
        const int want = std::min(frames, kMaxInputFrames) * 2;
        if (m_inputLive.load(std::memory_order_acquire)) std::copy(in, in + want, m_inBlock.begin());
        else std::fill(m_inBlock.begin(), m_inBlock.begin() + want, 0.0f);
        m_inFill.store(0.0f, std::memory_order_relaxed);
    } else {
        pullInput(frames);
    }

    for (int done = 0; done < frames;) {
        int n = std::min(kMaxBlock, frames - done);
        if (m_playing && m_loop && m_loopLen > 0 && m_state) {   // end the block exactly at the loop end
            const double end = m_loopStart + m_loopLen, bps = m_state->tempo / 60.0 / m_sr;
            if (m_beat < end) n = std::min(n, std::max(1, int(std::ceil((end - m_beat) / bps - 1e-6))));
        }
        if (m_countIn > 0 && m_state) {   // and exactly where the count-in ends
            const double bps = m_state->tempo / 60.0 / m_sr;
            n = std::min(n, std::max(1, int(std::ceil(m_countIn / bps - 1e-6))));
        }
        renderBlock(out + 2 * done, n, m_inBlock.data() + 2 * std::min(done, kMaxInputFrames - n));
        done += n;
    }
    if (m_capturing) {
        const int want = std::min(frames, kMaxInputFrames) * 2;
        const int wrote = m_recRing.write(m_inBlock.data(), want);
        if (wrote < want) m_capOverflow.fetch_add(1, std::memory_order_relaxed);
    }

    const int nt = m_state ? std::min(int(m_state->tracks.size()), kMaxTracks) : 0;
    for (int i = 0; i < nt; ++i) {
        const PlayState& p = m_play[size_t(i)];
        AtomicStatus& s = m_status[size_t(i)];
        s.playingSlot.store(p.playingSlot, std::memory_order_relaxed);
        s.queuedSlot.store(p.queuedSlot, std::memory_order_relaxed);
        s.slotStart.store(p.slotStart, std::memory_order_relaxed);
        s.queuedAt.store(p.queuedAt, std::memory_order_relaxed);
        s.overridden.store(p.overridden, std::memory_order_relaxed);
        double clipBeat = -1;
        int clipPass = 0;
        const TrackData& td = m_state->tracks[size_t(i)];
        if (p.playingSlot >= 0 && p.playingSlot < int(td.slotClips.size())) {
            if (const ClipData* c = td.slotClips[size_t(p.playingSlot)].get()) {
                if (!c->warp && c->sample && !c->comped()) {
                    double seconds = c->secondsAt(c->start) + m_playSeconds - p.audioStartSeconds;
                    const double from = c->secondsAt(c->loopStart), to = std::min(c->secondsAt(c->end()), c->sample->seconds());
                    clipPass = 1;
                    if (c->loop && to > from && seconds >= to) {
                        clipPass = 2 + int(std::floor((seconds - to) / (to - from)));
                        seconds = from + std::fmod(seconds - from, to - from);
                    }
                    clipBeat = c->beatAt(seconds);
                } else clipBeat = clipPositionAt(c->start, c->loopStart, c->length, c->loop, m_beat - p.slotStart);
            }
        }
        s.clipBeat.store(clipBeat, std::memory_order_relaxed);
        s.clipPass.store(clipPass, std::memory_order_relaxed);
    }
    m_fbBeat.store(m_beat, std::memory_order_relaxed);
    m_fbPlaying.store(m_playing || m_countIn > 0, std::memory_order_relaxed);
    m_fbCountIn.store(m_countIn, std::memory_order_relaxed);
    m_fbGeneration.store(m_state ? m_state->generation : 0, std::memory_order_release);
    m_doneSeq.store(m_lastSeq, std::memory_order_release);

    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    m_cpuAvg = 0.95f * m_cpuAvg + 0.05f * float(used / (frames / double(m_sr)));
    m_cpu.store(m_cpuAvg, std::memory_order_relaxed);
}

// Re-map per-track playback state onto the new track order (by track id) and
// re-find playing / queued clips by clip id, since scenes may have moved.
void Engine::adopt(RenderState* s) {
    m_scratch = m_play;
    const int nt = std::min(int(s->tracks.size()), kMaxTracks);
    for (int i = 0; i < kMaxTracks; ++i) {
        PlayState fresh;
        if (i < nt) {
            const TrackData& td = s->tracks[size_t(i)];
            for (const PlayState& old : m_scratch) if (old.trackId == td.id) { fresh = old; break; }
            fresh.trackId = td.id;
            if (fresh.playingSlot >= 0) {
                const ClipData* previous = nullptr;
                if (m_state) for (const TrackData& oldTrack : m_state->tracks) if (oldTrack.id == td.id) {
                    const int slot = slotOfClip(oldTrack, fresh.playingClip);
                    if (slot >= 0) previous = oldTrack.slotClips[size_t(slot)].get();
                    break;
                }
                fresh.playingSlot = slotOfClip(td, fresh.playingClip);
                if (fresh.playingSlot < 0) fresh.pendingAllOff = true;   // its clip was deleted
                else {
                    const ClipData& current = *td.slotClips[size_t(fresh.playingSlot)];
                    if (previous && previous->warp != current.warp && current.sample && !current.comped() && !previous->comped()) {
                        if (!current.warp) {
                            const double position = clipPositionAt(previous->start, previous->loopStart, previous->length,
                                previous->loop, m_beat - fresh.slotStart);
                            const double sourceSeconds = previous->secondsAt(position);
                            fresh.audioStartSeconds = m_playSeconds - (sourceSeconds - current.secondsAt(current.start));
                        } else {
                            double seconds = previous->secondsAt(previous->start) + m_playSeconds - fresh.audioStartSeconds;
                            const double from = previous->secondsAt(previous->loopStart);
                            const double to = std::min(previous->secondsAt(previous->end()), previous->sample ? previous->sample->seconds() : seconds);
                            if (previous->loop && to > from && seconds >= to) seconds = from + std::fmod(seconds - from, to - from);
                            const double position = current.beatAt(seconds);
                            const double elapsed = current.loop && position < current.start
                                ? current.end() - current.start + position - current.loopStart : position - current.start;
                            fresh.slotStart = m_beat - elapsed;
                        }
                    }
                }
            }
            if (fresh.queuedSlot >= 0) {
                fresh.queuedSlot = slotOfClip(td, fresh.queuedClip);
                if (fresh.queuedSlot < 0) fresh.queuedSlot = -2;
            }
            if (fresh.repeatSlot >= 0) fresh.repeatSlot = fresh.playingSlot;
        }
        m_play[size_t(i)] = fresh;
    }
    m_state = s;
}

int Engine::indexOfTrack(int id) const {
    const int nt = m_state ? std::min(int(m_state->tracks.size()), kMaxTracks) : 0;
    for (int i = 0; i < nt; ++i) if (m_state->tracks[size_t(i)].id == id) return i;
    return -1;
}

double Engine::quantized(double q) const {
    return (m_playing && q > 0) ? std::ceil(m_beat / q - 1e-9) * q : m_beat;
}

void Engine::allNotesOff() {
    for (PlayState& p : m_play) p.pendingAllOff = true;
}

void Engine::apply(const Command& c) {
    const int ti = indexOfTrack(c.trackId);
    PlayState* ps = ti >= 0 ? &m_play[size_t(ti)] : nullptr;
    const TrackData* td = ti >= 0 ? &m_state->tracks[size_t(ti)] : nullptr;
    switch (c.type) {
    case Command::Play:
        if (c.a > 0 && !m_playing) m_countIn = m_countTotal = c.a;   // the click first; playback starts after it
        else m_playing = true;
        break;
    case Command::Pause:
        m_countIn = 0;
        if (m_playing) { m_playing = false; allNotesOff(); }
        break;
    case Command::Stop:
        m_countIn = 0;
        m_playing = false;
        if (c.on) m_beat = 0;
        m_eventStart = -1;
        for (PlayState& p : m_play) { p.playingSlot = -1; p.queuedSlot = -2; p.repeatSlot = -1; p.pendingAllOff = true; }
        break;
    case Command::Locate:
        m_beat = std::max(0.0, c.a);
        m_eventStart = -1;
        // Stopped, a queued launch or stop is "as soon as the song plays": it moves with the playhead
        // (0.24: a launch queued where the song last stopped used to wait for that beat to come round).
        if (!m_playing) for (PlayState& p : m_play) if (p.queuedSlot != -2) p.queuedAt = m_beat;
        allNotesOff();
        break;
    case Command::Launch:
        if (ps && clipIdAt(*td, c.slot)) {
            ps->queuedSlot = c.slot;
            ps->queuedClip = clipIdAt(*td, c.slot);
            ps->queuedAt = quantized(c.a);
            ps->overridden = true;
            ps->repeatSlot = c.on ? c.slot : -1;
            ps->repeatQuant = c.a > 0 ? c.a : 1.0;
            ps->hasSeek = false;
        }
        break;
    case Command::SeekSlot:
        if (!ps) break;
        if (ps->queuedSlot == c.slot) { ps->seekBeat = c.a; ps->hasSeek = true; }   // applied when the launch lands
        else if (ps->playingSlot == c.slot) {
            ps->slotStart = m_beat - c.a;
            const ClipData* clip = td && c.slot >= 0 && c.slot < int(td->slotClips.size()) ? td->slotClips[size_t(c.slot)].get() : nullptr;
            if (clip) ps->audioStartSeconds = m_playSeconds - (clip->secondsAt(clip->start + c.a) - clip->secondsAt(clip->start));
        }
        break;
    case Command::StopTrack:
        if (ps && (ps->playingSlot >= 0 || ps->queuedSlot >= 0)) { ps->queuedSlot = -1; ps->queuedAt = quantized(c.a); ps->repeatSlot = -1; }
        break;
    case Command::Unrepeat:
        if (ps && ps->repeatSlot >= 0) {
            if (ps->playingSlot == ps->repeatSlot && ps->queuedSlot == ps->repeatSlot) ps->queuedSlot = -2;   // drop the next retrigger
            ps->repeatSlot = -1;
        }
        break;
    case Command::BackToArrangement:
        for (PlayState& p : m_play) {
            if (p.playingSlot >= 0 || p.overridden) p.pendingAllOff = true;
            p.playingSlot = -1; p.queuedSlot = -2; p.repeatSlot = -1; p.overridden = false;
        }
        break;
    case Command::SessionTakeover:
        for (PlayState& p : m_play) p.overridden = true;
        break;
    case Command::Perform: {
        Perf* slot = nullptr;
        for (Perf& p : m_perf) if (p.trackId == c.trackId) slot = &p;
        if (!slot) for (Perf& p : m_perf) if (p.trackId == 0) { slot = &p; slot->trackId = c.trackId; break; }
        if (!slot) break;
        const float v = float(c.b);
        if (int(c.a) == 0) slot->bend = std::clamp(v, -1.0f, 1.0f);
        else if (int(c.a) == 1) slot->wheel = std::clamp(v, 0.0f, 1.0f);
        else slot->pressure = std::clamp(v, 0.0f, 1.0f);
        break;
    }
    case Command::Loop:
        m_loop = c.on; m_loopStart = c.a; m_loopLen = c.b;
        m_eventStart = -1;
        break;
    case Command::Metronome:
        m_metro = c.on;
        break;
    case Command::Capture:
        m_capturing = c.on;
        m_capPending = c.on;
        break;
    case Command::Preview:
    case Command::NoteOn:
    case Command::NoteOff:
        if (ti >= 0 && m_liveCount < 64) {
            const bool held = c.type == Command::NoteOn;
            m_live[m_liveCount++] = {ti, int(c.a), float(c.b), c.type == Command::NoteOff, held ? kHeld : int(0.3f * m_sr)};
        }
        break;
    }
    m_lastSeq = c.seq;
}

int Engine::followTarget(const TrackData& td, int current, int action) {
    const int n = int(td.slotClips.size());
    auto filled = [&td](int s) { return td.slotClips[size_t(s)] != nullptr; };
    const bool any = FollowAction(action) == FollowAction::Any;
    switch (FollowAction(action)) {
    case FollowAction::None:  return -2;
    case FollowAction::Stop:  return -1;
    case FollowAction::Again: return current;
    case FollowAction::Previous:
        for (int k = 1; k <= n; ++k) { int s = ((current - k) % n + n) % n; if (filled(s)) return s; }
        return -2;
    case FollowAction::Next:
        for (int k = 1; k <= n; ++k) { int s = (current + k) % n; if (filled(s)) return s; }
        return -2;
    case FollowAction::First:
        for (int s = 0; s < n; ++s) if (filled(s)) return s;
        return -2;
    case FollowAction::Last:
        for (int s = n - 1; s >= 0; --s) if (filled(s)) return s;
        return -2;
    case FollowAction::Any:
    case FollowAction::Other: {
        int count = 0;
        for (int s = 0; s < n; ++s) if (filled(s) && (any || s != current)) ++count;
        if (!count) return -2;
        int pick = int(m_rng.uniform() * float(count)) % count;
        for (int s = 0; s < n; ++s)
            if (filled(s) && (any || s != current) && pick-- == 0) return s;
        return -2;
    }
    }
    return -2;
}

void Engine::scheduleTrack(const TrackData& td, PlayState& ps, double from, double b0, double b1, double bps, int n, EventList& ev) {
    // Repeat launch mode: keep retriggering on the quantization grid while the button is held.
    if (ps.repeatSlot >= 0 && ps.queuedSlot == -2 && ps.playingSlot == ps.repeatSlot) {
        const double q = ps.repeatQuant;
        ps.queuedSlot = ps.repeatSlot;
        ps.queuedClip = clipIdAt(td, ps.repeatSlot);
        ps.queuedAt = (std::floor(b0 / q + 1e-9) + 1) * q;
    }
    // Follow actions: once per launch, after the follow time.
    if (ps.playingSlot >= 0 && ps.queuedSlot == -2 && !ps.followDone) {
        const ClipData* c = td.slotClips[size_t(ps.playingSlot)].get();
        if (c && c->followEnabled) {
            double at = ps.slotStart + (c->followTime > 0 ? c->followTime : c->firstPass());
            if (td.audio && !c->warp && c->sample && !c->comped() && c->followTime <= 0) {
                const double duration = std::min(c->secondsAt(c->end()), c->sample->seconds()) - c->secondsAt(c->start);
                at = b0 + (duration - (m_playSeconds - ps.audioStartSeconds)) * bps * m_sr;
            }
            if (at < b1) {
                ps.followDone = true;
                const int action = m_rng.uniform() < c->followChanceA ? c->followA : c->followB;
                const int target = followTarget(td, ps.playingSlot, action);
                if (target != -2) {
                    ps.queuedSlot = target;
                    ps.queuedClip = clipIdAt(td, target);
                    ps.queuedAt = std::max(at, b0);
                }
            }
        }
    }

    // A queued launch or stop inside this window splits it in two.
    double switchAt = -1;
    if (ps.queuedSlot != -2 && ps.queuedAt < b1) switchAt = std::max(ps.queuedAt, from);
    emitSource(td, ps, from, switchAt >= 0 ? switchAt : b1, b0, bps, n, ev);
    if (switchAt < 0) return;
    ev.add({offsetOf(switchAt, b0, bps, n), 0, 0, 0, true, false});

    const int next = ps.queuedSlot;
    const ClipData* newClip = next >= 0 && next < int(td.slotClips.size()) ? td.slotClips[size_t(next)].get() : nullptr;
    const ClipData* oldClip = ps.playingSlot >= 0 ? td.slotClips[size_t(ps.playingSlot)].get() : nullptr;
    double start = ps.queuedAt;   // a late launch keeps its phase instead of starting late
    if (newClip && newClip->legato && oldClip && oldClip->length > 0) {
        // take over the old clip's position: the new one is at that clip beat now (before its own start marker is fine)
        double pos = clipPositionAt(oldClip->start, oldClip->loopStart, oldClip->length, oldClip->loop, switchAt - ps.slotStart);
        if (!oldClip->warp && oldClip->sample && !oldClip->comped()) {
            double seconds = oldClip->secondsAt(oldClip->start) + m_playSeconds
                + (switchAt - b0) / (bps * m_sr) - ps.audioStartSeconds;
            const double loopFrom = oldClip->secondsAt(oldClip->loopStart);
            const double loopTo = std::min(oldClip->secondsAt(oldClip->end()), oldClip->sample->seconds());
            if (oldClip->loop && loopTo > loopFrom && seconds >= loopTo)
                seconds = loopFrom + std::fmod(seconds - loopFrom, loopTo - loopFrom);
            pos = oldClip->beatAt(seconds);
        }
        start = switchAt - (pos - newClip->start);
    }
    ps.playingSlot = newClip ? next : -1;
    ps.playingClip = newClip ? newClip->id : 0;
    if (newClip) {
        const double secPerBeat = 1.0 / (bps * m_sr);
        ps.audioStartSeconds = m_playSeconds + (ps.queuedAt - b0) * secPerBeat;
        if (newClip->legato && oldClip) {
            const double clipBeat = newClip->start + switchAt - start;
            ps.audioStartSeconds = m_playSeconds + (switchAt - b0) * secPerBeat
                - (newClip->secondsAt(clipBeat) - newClip->secondsAt(newClip->start));
        }
        if (ps.hasSeek) ps.audioStartSeconds -= newClip->secondsAt(newClip->start + ps.seekBeat) - newClip->secondsAt(newClip->start);
    }
    ps.slotStart = start - (ps.hasSeek && newClip ? ps.seekBeat : 0.0);   // a seek that came with the launch: the clip starts part-way
    ps.hasSeek = false;
    ps.queuedSlot = -2;
    ps.followDone = false;
    emitSource(td, ps, switchAt, b1, b0, bps, n, ev);
}

void Engine::emitSource(const TrackData& td, PlayState& ps, double from, double to, double b0, double bps, int n, EventList& ev) {
    if (to <= from) return;
    if (ps.playingSlot >= 0) {
        const ClipData* c = ps.playingSlot < int(td.slotClips.size()) ? td.slotClips[size_t(ps.playingSlot)].get() : nullptr;
        if (!c) {   // the clip was deleted while playing
            ps.playingSlot = -1;
            ev.add({offsetOf(from, b0, bps, n), 0, 0, 0, true, false});
            return;
        }
        const double once = std::max(0.0, c->end() - c->start);   // Loop off: from the start marker to the end marker
        const bool natural = td.audio && !c->warp && c->sample && !c->comped();
        const double secPerBeat = 1.0 / (bps * m_sr);
        double endAt = ps.slotStart + once;
        if (natural) {
            const double sourceEnd = std::min(c->secondsAt(c->end()), c->sample->seconds());
            const double elapsed = m_playSeconds - ps.audioStartSeconds;
            endAt = b0 + (sourceEnd - c->secondsAt(c->start) - elapsed) / secPerBeat;
        }
        if (td.audio) {   // ends at a queued switch or stop, which is known ahead of time (fade before it)
            const double hi = std::min(ps.queuedSlot != -2 ? ps.queuedAt : kNoLimit, c->loop ? kNoLimit : endAt);
            const int segment = m_segCount;
            addSegment(*c, ps.slotStart, c->start, ps.slotStart, hi, from, to);
            if (natural && m_segCount > segment) {
                AudioSeg& g = m_segs[segment];
                g.natural = true;
                g.naturalAtFrom = c->secondsAt(c->start) + m_playSeconds
                    + (g.from - b0) * secPerBeat - ps.audioStartSeconds;
            }
        } else {
            emitClip(*c, ps.slotStart, c->start, -kNoLimit, kNoLimit, from, to, b0, bps, n, ev, m_rng);
        }
        if (!c->loop && to >= endAt) ps.playingSlot = -1;
    } else if (!ps.overridden) {
        for (const Placement& p : td.arrangement) {   // a placement's first pass starts at its offset
            if (p.start >= to) break;
            if (!p.clip || p.start + p.length <= from) continue;
            if (td.audio) addSegment(*p.clip, p.start, p.offset, p.start, p.start + p.length, from, to, p.fadeIn, p.fadeOut, p.fadeInCurve, p.fadeOutCurve);
            else emitClip(*p.clip, p.start, p.offset, p.start, p.start + p.length, from, to, b0, bps, n, ev, m_rng);
        }
    }
}

// ---- audio clips -------------------------------------------------------------

namespace {

// 4-point cubic (Catmull-Rom) read of one channel at a fractional frame; silence outside.
inline float readCubic(const std::vector<float>& x, double pos) {
    const qint64 n = qint64(x.size());
    const qint64 i = qint64(std::floor(pos));
    if (i < -1 || i >= n) return 0.0f;
    const float t = float(pos - double(i));
    auto at = [&](qint64 k) { return k >= 0 && k < n ? x[size_t(k)] : 0.0f; };
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
}

constexpr double kEdgeFade  = 0.003;   // s: fade-out before a clip's end, its loop point or a switch, so audio never clicks
constexpr double kEdgeIn    = 0.0003;  // s: fade-in where a clip (or its next pass) starts: short, so downbeats keep their attack
constexpr double kGrainOut  = 0.004;   // s: Beats mode, end of a slice
constexpr double kGrainIn   = 0.0003;  // s: Beats mode, start of a slice that doesn't continue the last (short: keeps transients)

} // namespace

void Engine::addSegment(const ClipData& c, double origin, double start, double lo, double hi, double from, double to,
                        double fadeIn, double fadeOut, double inCurve, double outCurve) {
    if (m_segCount >= int(std::size(m_segs)) || !c.audio || (!c.sample && !c.comped()) || to <= from) return;
    m_segs[m_segCount++] = {&c, origin, start, lo, hi, std::max(from, lo), std::min(to, hi), std::min(fadeIn, hi - lo), std::min(fadeOut, hi - lo), inCurve, outCurve};
}

Stretcher* Engine::stretchVoice(int trackId, int clipId, double origin) {
    int free = -1;
    for (int i = 0; i < kStretchVoices; ++i) {
        StretchSlot& k = m_stretchKey[i];
        if (k.lastUse && k.trackId == trackId && k.clipId == clipId && k.origin == origin) {
            if (k.lastUse + 1 < m_blockNo) m_stretch[i].reset();
            k.lastUse = m_blockNo;
            return &m_stretch[i];
        }
        // Prefer unused/long-idle buffers over a voice that played last window.
        // A newly inserted track must not steal the first existing track's state.
        if (k.lastUse < m_blockNo && (free < 0 || k.lastUse < m_stretchKey[free].lastUse)) free = i;
    }
    if (free < 0) return nullptr;
    m_stretchKey[free] = {trackId, clipId, origin, m_blockNo};
    m_stretch[free].reset();
    return &m_stretch[free];
}

void Engine::renderAudio(int trackId, double b0, double bps, double tempo, int n, float* L, float* R) {
    const double secPerBeat = 60.0 / tempo;
    const double edge = kEdgeFade / secPerBeat, edgeIn = kEdgeIn / secPerBeat;   // in beats
    const float rate = float(tempo / 60.0 / bps);
    for (int k = 0; k < m_segCount; ++k) {
        const AudioSeg& g = m_segs[k];
        if (g.to <= g.from) continue;
        const ClipData& c = *g.clip;
        if (!c.sample && !c.comped()) continue;
        const SampleData& smp = c.sample ? *c.sample : *c.takeSamples.front();   // (a comped clip reads its takes instead)
        const double readStep = c.warp && c.repitch
            ? (c.secondsAt(1) - c.secondsAt(0)) * bps * smp.rate
            : smp.rate / rate * (c.warp ? c.ratio : 1.0);
        const auto readPlan = resample::plan(readStep);
        const double Lc = c.length, loopEnd = c.end(), firstPass = loopEnd - g.start;   // beats of the first pass: from the start marker to the end marker
        // the stretching modes: this placement's Stretcher, fed a sample at a time
        Stretcher* voice = nullptr;
        StretchSource src;
        if (c.warp && c.stretch && c.sample && !c.comped()) {
            voice = stretchVoice(trackId, c.id, g.origin);
            if (voice) src = stretchSource(c, secPerBeat);
        }
        const int s0 = g.from <= b0 ? 0 : std::clamp(int(std::ceil((g.from - b0) / bps - 1e-6)), 0, n);
        const int s1 = std::clamp(int(std::ceil((g.to - b0) / bps - 1e-6)), s0, n);
        for (int s = s0; s < s1; ++s) {
            const double b = b0 + s * bps;
            const double sinceLaunch = b - g.origin;
            if (Lc <= 0) continue;
            // the clip beat playing now: the first pass from the start marker, then round the brace (a seek may sit before the start marker)
            const double naturalRaw = g.natural ? g.naturalAtFrom + (b - g.from) * secPerBeat : 0;
            double naturalPosition = naturalRaw;
            const double naturalLoopStart = g.natural ? c.secondsAt(c.loopStart) : 0;
            const double naturalLoopEnd = g.natural ? std::min(c.secondsAt(loopEnd), smp.seconds()) : 0;
            if (g.natural && c.loop && naturalPosition >= naturalLoopEnd) {
                const double span = naturalLoopEnd - naturalLoopStart;
                if (span <= 0) continue;
                naturalPosition = naturalLoopStart + std::fmod(naturalPosition - naturalLoopStart, span);
            }
            const double local = g.natural ? c.beatAt(naturalPosition)
                : clipPositionAt(g.start, c.loopStart, Lc, c.loop, sinceLaunch);
            if (local < 0 || (!c.loop && local >= loopEnd)) continue;
            // declick: fade out before the placement's end, a switch and the loop point; fade in very briefly after
            const double sinceStart = b - g.lo + bps;   // the first sample gets a little level
            double fade = std::min({1.0, sinceStart / edgeIn, (g.hi - b) / edge});
            if (c.loop) {
                if (g.natural) {
                    fade = std::min(fade, (naturalLoopEnd - naturalPosition) / kEdgeFade);
                    if (naturalRaw >= naturalLoopEnd) fade = std::min(fade, (naturalPosition - naturalLoopStart + 1.0 / rate) / kEdgeIn);
                } else {
                    fade = std::min(fade, (loopEnd - local) / edge);
                    if (sinceLaunch >= firstPass) fade = std::min(fade, (local - c.loopStart + bps) / edgeIn);   // after a wrap, not while running into the brace
                }
            }
            if (g.fadeIn > 0) fade *= fadeGain((b - g.lo) / g.fadeIn, g.inCurve);      // the placement's fades (0.33)
            if (g.fadeOut > 0) fade *= fadeGain((g.hi - b) / g.fadeOut, g.outCurve);
            if (fade <= 0) continue;
            if (c.comped()) {   // a loop-recorded clip: from its takes, through the comp
                renderComp(c, local, fade, secPerBeat, L[s], R[s]);
                continue;
            }
            if (voice) {   // Tones / Texture / Complex / Complex Pro
                auto& key = m_stretchKey[size_t(voice - m_stretch)];
                if (c.prepared && c.preparedTempo == tempo && c.preparedRate == rate
                    && (!voice->active() || std::abs(local - key.nextBeat) > bps * 1.5)) {
                    if (std::abs(local - c.preparedStart) <= bps * 1.51) voice->copyPrepared(*c.prepared);
                    else if (c.preparedLoop && std::abs(local - c.loopStart) <= bps * 1.51) voice->copyPrepared(*c.preparedLoop);
                }
                float vl, vr;
                voice->sample(src, local, rate, vl, vr);
                key.nextBeat = local + bps;
                const float gv = float(fade) * c.gain;
                L[s] += vl * gv;
                R[s] += vr * gv;
                continue;
            }

            double t, tailOther = 0, tailBlend = 0;   // seconds into the sample
            if (!c.warp) {
                // Warp off: the file's own speed from the first marker, whatever the tempo
                t = g.natural ? naturalPosition
                    : (c.markers.empty() ? 0.0 : c.markers.front().seconds) + local * secPerBeat;
            } else if (c.repitch) {
                t = c.secondsAt(local);
            } else {
                // Beats: slices start on a grid, or at the sample's transients
                double srcStart, srcNext, sliceBeat, nextBeat;
                if (c.grain > 0) {
                    sliceBeat = std::floor(local / c.grain + 1e-9) * c.grain;
                    nextBeat = sliceBeat + c.grain;
                    srcStart = c.secondsAt(sliceBeat);
                    srcNext = c.secondsAt(nextBeat);
                } else {
                    const std::vector<double>& tr = smp.transients;
                    const double here = c.secondsAt(local);
                    const auto it = std::upper_bound(tr.begin(), tr.end(), here + 1e-9);
                    srcStart = it == tr.begin() ? 0.0 : *(it - 1);
                    srcNext = it == tr.end() ? std::max(srcStart + 1e-3, smp.seconds()) : *it;
                    sliceBeat = std::min(local, c.beatAt(srcStart));
                    nextBeat = std::max(local, c.beatAt(srcNext));
                }
                const double elapsed = (local - sliceBeat) * secPerBeat;   // real seconds into this slice
                t = srcStart + elapsed * c.ratio;
                // Keep the attack at native speed. At slower tempos sustain only the
                // latter part of the slice, never its attack or the next transient.
                const double sliceEnd = srcStart + (nextBeat - sliceBeat) * secPerBeat * c.ratio;
                if (std::abs(sliceEnd - srcNext) > 1e-4) {
                    const double untilNext = (srcNext - t) / c.ratio, untilEnd = (nextBeat - local) * secPerBeat;
                    const double span = srcNext - srcStart;
                    // Keep the interpolation/filter support clear of the next attack too.
                    const double guard = std::max(std::min(0.001, span * 0.02), (readPlan.phases ? 17.0 : 2.0) / smp.rate);
                    const double end = srcNext - guard;
                    const double begin = srcStart + std::max(span * 0.5, span - 0.080);
                    const bool sustain = c.beatsTail && span > 0.004 && sliceEnd > srcNext && end > begin + 1.0 / smp.rate;
                    if (sustain) {
                        const double length = end - begin;
                        if (c.beatsTail == 2 && t > end) {
                            const double phase = std::fmod(t - end, 2 * length);
                            t = phase < length ? end - phase : begin + phase - length;
                        } else if (c.beatsTail == 1) {
                            const double crossfade = std::min(0.004, length * 0.25);
                            if (t >= end) t = begin + crossfade + std::fmod(t - end, length - crossfade);
                            if (t > end - crossfade) {
                                tailOther = begin + t - (end - crossfade);
                                tailBlend = std::clamp((t - end + crossfade) / crossfade, 0.0, 1.0);
                            }
                        }
                    }
                    fade *= std::clamp((sustain ? untilEnd : std::min(untilNext, untilEnd)) / kGrainOut, 0.0, 1.0)
                          * std::clamp((elapsed + 1.0 / rate) / kGrainIn, 0.0, 1.0);
                    if (fade <= 0) continue;
                }
            }
            const double pos = t * smp.rate;
            const float gain = float(fade) * c.gain;
            const auto plan = c.warp && c.repitch && c.markers.size() > 2
                ? resample::plan((c.secondsAt(local + bps) - c.secondsAt(local)) * smp.rate) : readPlan;
            float vl = resample::read(smp.left, pos, plan), vr = resample::read(smp.right, pos, plan);
            if (tailBlend > 0) {
                vl += float(tailBlend) * (resample::read(smp.left, tailOther * smp.rate, plan) - vl);
                vr += float(tailBlend) * (resample::read(smp.right, tailOther * smp.rate, plan) - vr);
            }
            L[s] += vl * gain;
            R[s] += vr * gain;
        }
    }
}

// The comped clip at clip beat `local`: the segment's take, crossfaded over kCompFade with the
// neighbouring segment's take where the take changes. A take's file begins at its own clip beat.
void Engine::renderComp(const ClipData& c, double local, double fade, double secPerBeat, float& L, float& R) const {
    constexpr double kCompFade = 0.005;   // s, centred on the boundary
    const int n = int(c.segTake.size());
    int seg = int(std::upper_bound(c.segEnd.begin(), c.segEnd.end(), local) - c.segEnd.begin());
    if (seg >= n) seg = n - 1;
    auto read = [&](int take, float& l, float& r) {
        l = r = 0;
        if (take < 0 || take >= int(c.takeSamples.size())) return;
        const SampleData* ts = c.takeSamples[size_t(take)].get();
        if (!ts) return;
        const double start = c.takeStart[size_t(take)];
        const double sec = c.warp ? c.secondsAt(local) - c.secondsAt(start) : (local - start) * secPerBeat;
        if (sec < 0) return;
        const double pos = sec * ts->rate;
        l = readCubic(ts->left, pos);
        r = readCubic(ts->right, pos);
    };
    const double half = 0.5 * kCompFade / secPerBeat;   // beats on either side of a boundary
    float l0, r0;
    read(c.segTake[size_t(seg)], l0, r0);
    const double segStart = seg > 0 ? c.segEnd[size_t(seg) - 1] : 0.0;
    if (seg > 0 && local - segStart < half) {
        const float m = float(0.5 + 0.5 * (local - segStart) / half);   // this segment's weight, 0.5 at the boundary
        float l1, r1;
        read(c.segTake[size_t(seg) - 1], l1, r1);
        l0 = l1 * (1 - m) + l0 * m;
        r0 = r1 * (1 - m) + r0 * m;
    }
    if (seg + 1 < n && c.segEnd[size_t(seg)] - local < half) {
        const float m = float(0.5 + 0.5 * (c.segEnd[size_t(seg)] - local) / half);
        float l1, r1;
        read(c.segTake[size_t(seg) + 1], l1, r1);
        l0 = l0 * m + l1 * (1 - m);
        r0 = r0 * m + r1 * (1 - m);
    }
    const float gain = float(fade) * c.gain;
    L += l0 * gain;
    R += r0 * gain;
}

void Engine::clipOverrides(const TrackData& td, const PlayState& ps, double b0) {
    m_ovVolume = m_ovPan = -1;
    std::fill(m_ovSend, m_ovSend + kMaxReturns, -1.0f);
    m_ovCount = 0;
    if (!m_playing) return;
    const ClipData* c = nullptr;
    double local = -1;
    if (ps.playingSlot >= 0 && ps.playingSlot < int(td.slotClips.size())) {
        c = td.slotClips[size_t(ps.playingSlot)].get();
        if (c) local = clipPositionAt(c->start, c->loopStart, c->length, c->loop, b0 - ps.slotStart);
        if (c && td.audio && !c->warp && c->sample && !c->comped()) {
            double seconds = c->secondsAt(c->start) + m_playSeconds - ps.audioStartSeconds;
            const double from = c->secondsAt(c->loopStart), to = std::min(c->secondsAt(c->end()), c->sample->seconds());
            if (c->loop && to > from && seconds >= to) seconds = from + std::fmod(seconds - from, to - from);
            local = c->beatAt(seconds);
        }
    } else if (!ps.overridden) {
        for (const Placement& p : td.arrangement) {
            if (p.start > b0) break;
            if (p.clip && b0 < p.start + p.length) { c = p.clip.get(); local = clipPositionAt(p.offset, c->loopStart, c->length, c->loop, b0 - p.start); }
        }
    }
    if (!c || c->envs.empty() || local < 0 || (!c->loop && local >= c->end())) return;
    for (const ClipData::Env& e : c->envs) {
        const float v = curveAt(e.pts, local);
        switch (e.kind) {
        case 0: m_ovVolume = v; break;
        case 1: m_ovPan = v; break;
        case 2: if (e.index < kMaxReturns) m_ovSend[e.index] = v; break;
        case 3: if (m_ovCount < int(std::size(m_ovParams))) m_ovParams[m_ovCount++] = {e.deviceId, e.index, v}; break;
        }
    }
}

void Engine::renderBlock(float* out, int n, const float* in) {
    ++m_blockNo;   // one generation for every track in this render window
    const bool playedThisBlock = m_playing;
    const RenderState* S = m_state;
    const double tempo = S ? S->tempo : 120.0;
    const double bps = tempo / 60.0 / m_sr;
    const double b0 = m_beat, b1 = b0 + n * bps;
    const double eventFrom = m_eventStart >= 0 && m_eventStart <= b0 ? m_eventStart : b0;
    m_eventStart = -1;
    const double loopEnd = m_loopStart + m_loopLen;
    const bool wraps = m_playing && m_loop && m_loopLen > 0 && b0 < loopEnd && b1 >= loopEnd - 1e-9;
    const double winEnd = wraps ? loopEnd : b1;
    const float smooth = 1.0f - onePoleCoef(0.01f, m_sr);   // ~10 ms gain ramps
    const bool inputLive = m_inputLive.load(std::memory_order_relaxed);

    std::fill(m_mainL, m_mainL + n, 0.0f);
    std::fill(m_mainR, m_mainR + n, 0.0f);
    for (int k = 0; k < kMaxReturns; ++k) {
        std::fill(m_retL[k], m_retL[k] + n, 0.0f);
        std::fill(m_retR[k], m_retR[k] + n, 0.0f);
    }
    for (int k = 0; k < kMaxGroups; ++k) {
        std::fill(m_busL[k], m_busL[k] + n, 0.0f);
        std::fill(m_busR[k], m_busR[k] + n, 0.0f);
    }

    // Runs a device chain; automated knobs follow their envelopes at the playhead, and the
    // playing clip's envelopes (m_ovParams) win over both.
    // `keyIndex` >= 0: the track is a sidechain source; its sound before the first audio effect is copied to the Pre FX tap.
    auto runChain = [&](const TrackData& td, EventList*& ev, EventList*& spare, float* L, float* R, const Context& ctx, bool instruments, bool clipEnvs, int keyIndex = -1) {
        // A knob's value now: its own, its automation, the playing clip's envelope.
        auto valueOf = [&](const DeviceSlot& d, int p) {
            float v = d.paramAuto[p] >= 0 ? td.autos[size_t(d.paramAuto[p])].valueAt(b0) : d.params[p];
            if (clipEnvs) for (int o = 0; o < m_ovCount; ++o) if (m_ovParams[o].deviceId == d.deviceId && m_ovParams[o].param == p) v = m_ovParams[o].value;
            return v;
        };
        const float fadeStep = 1.0f / (0.012f * m_sr);   // on / off fades: ~12 ms
        // Every knob of device `di` as it plays now; a Macros (or rack macro) knob wins over everything else.
        auto paramsOf = [&](int di, float* params) {
            const DeviceSlot& d = td.devices[size_t(di)];
            std::copy(d.params, d.params + kMaxParams, params);
            for (int p = 0; p < kMaxParams; ++p) if (d.paramAuto[p] >= 0) params[p] = td.autos[size_t(d.paramAuto[p])].valueAt(b0);
            if (clipEnvs) for (int o = 0; o < m_ovCount; ++o) if (m_ovParams[o].deviceId == d.deviceId) params[m_ovParams[o].param] = m_ovParams[o].value;
            for (const TrackData::Link& l : td.links)
                if (l.dst == di) params[l.param] = std::clamp(l.lo + (l.hi - l.lo) * valueOf(td.devices[size_t(l.src)], l.macro), 0.0f, 1.0f);
        };
        // One device on `L / R`, its notes in `ev` (MIDI effects swap in what they make).
        auto runDevice = [&](int di, EventList*& evs, EventList*& spareEvs, float* bL, float* bR) {
            const DeviceSlot& d = td.devices[size_t(di)];
            if (!d.proc) return;
            float params[kMaxParams];
            paramsOf(di, params);
            Context dctx = ctx;
            dctx.sample = d.sample.get();
            dctx.slices = d.slices;
            dctx.sliceCount = d.sliceCount;
            dctx.sampleBpm = d.sampleBpm;
            dctx.pads = d.pads;
            dctx.ir = d.ir.get();
            dctx.nam = d.nam.get();
            dctx.mods = d.mods.data();
            dctx.modCount = int(d.mods.size());
            dctx.curves = d.curves;
            dctx.curveCount = d.curveCount;
            dctx.curveStride = osmium::kCurvePoints;
            if (d.keyTrack >= 0 && d.keyTrack < kMaxTracks) {   // sidechain: another track's audio at the chosen tap
                const int tapParam = sidechain::param(d.type, sidechain::Tap);
                const int tap = std::clamp(choiceIndex(d.type, tapParam, params[tapParam]), 0, 2);
                dctx.keyL = keyL(tap, d.keyTrack);
                dctx.keyR = keyR(tap, d.keyTrack);
            }
            // On / off fades rather than jumps (DeviceProcessor::onMix).
            float& mix = d.proc->onMix;
            const float target = d.enabled ? 1.0f : 0.0f;
            if (mix < 0) mix = target;
            switch (d.kind) {
            case DeviceKind::MidiEffect:
                mix = target;
                if (instruments && d.enabled) { spareEvs->clear(); d.proc->midi(*evs, *spareEvs, params, dctx, n); std::swap(evs, spareEvs); }
                break;
            case DeviceKind::Instrument:
                if (!instruments) break;
                if (mix == 0 && target == 0) break;
                if (mix == 1 && target == 1) { d.proc->render(*evs, bL, bR, n, params, dctx); break; }
                std::fill(m_fadeL, m_fadeL + n, 0.0f);
                std::fill(m_fadeR, m_fadeR + n, 0.0f);
                d.proc->render(*evs, m_fadeL, m_fadeR, n, params, dctx);
                for (int s = 0; s < n; ++s) {
                    mix = target > mix ? std::min(target, mix + fadeStep) : std::max(target, mix - fadeStep);
                    bL[s] += m_fadeL[s] * mix;
                    bR[s] += m_fadeR[s] * mix;
                }
                if (mix == 0) d.proc->panic();   // faded out: let go of the notes
                break;
            case DeviceKind::AudioEffect:
                if (mix == 0 && target == 0) break;   // bypassed
                if (mix == 1 && target == 1) { d.proc->process(bL, bR, n, params, dctx); break; }
                std::copy(bL, bL + n, m_fadeL);
                std::copy(bR, bR + n, m_fadeR);
                d.proc->process(bL, bR, n, params, dctx);
                for (int s = 0; s < n; ++s) {
                    mix = target > mix ? std::min(target, mix + fadeStep) : std::max(target, mix - fadeStep);
                    bL[s] = m_fadeL[s] + (bL[s] - m_fadeL[s]) * mix;
                    bR[s] = m_fadeR[s] + (bR[s] - m_fadeR[s]) * mix;
                }
                break;
            }
        };
        // A rack: every chain gets the notes in its zones (Instrument Rack) or the sound (Audio
        // Effect Rack, turned down by its chain zone), runs its own devices, and is mixed in at its
        // volume and pan. Chains that are off or not soloed still run (so notes end), silently.
        auto runRack = [&](int ri, int end, EventList*& evs, float* bL, float* bR) {
            const DeviceSlot& rk = td.devices[size_t(ri)];
            auto* rack = static_cast<RackProcessor*>(rk.proc.get());
            const bool instrumentRack = rk.rackType == 1;
            if (instrumentRack && !instruments) return;
            if (!rack || rk.chains == 0) return;   // no chains: silence / the dry sound
            // Switched off, an Instrument Rack goes silent and an Audio Effect Rack is bypassed: fading, like a device.
            float& mix = rack->onMix;
            const float target = rk.enabled ? 1.0f : 0.0f;
            if (mix < 0) mix = target;
            if (mix == 0 && target == 0) return;
            const bool fading = !(mix == 1 && target == 1);
            float* outL = bL;
            float* outR = bR;
            if (fading) {
                if (instrumentRack) {
                    std::fill(m_rackFadeL, m_rackFadeL + n, 0.0f); std::fill(m_rackFadeR, m_rackFadeR + n, 0.0f);
                    outL = m_rackFadeL; outR = m_rackFadeR;
                } else {
                    std::copy(bL, bL + n, m_rackFadeL); std::copy(bR, bR + n, m_rackFadeR);
                }
            }
            float rp[kMaxParams];
            paramsOf(ri, rp);
            const int select = int(std::lround(paramValue(rk.type, racks::ChainSelect, rp[racks::ChainSelect])));
            bool anySolo = false;
            for (int c = 0; c < rk.chains; ++c) anySolo |= rp[racks::param(c, racks::Solo)] >= 0.5f;
            if (!instrumentRack) {
                std::copy(bL, bL + n, m_rackInL); std::copy(bR, bR + n, m_rackInR);
                std::fill(m_rackSumL, m_rackSumL + n, 0.0f); std::fill(m_rackSumR, m_rackSumR + n, 0.0f);
            }
            for (int c = 0; c < rk.chains; ++c) {
                EventList* cev = &m_chainEvA;
                EventList* cspare = &m_chainEvB;
                if (instrumentRack) {
                    chainNotes(*evs, *cev, rk.zones[c], select, true);
                    std::fill(m_chainL, m_chainL + n, 0.0f); std::fill(m_chainR, m_chainR + n, 0.0f);
                } else {
                    cev->count = evs->count;
                    std::copy(evs->ev, evs->ev + evs->count, cev->ev);
                    std::copy(m_rackInL, m_rackInL + n, m_chainL); std::copy(m_rackInR, m_rackInR + n, m_chainR);
                    rack->chainMixer()->scale(c, m_chainL, m_chainR, n, float(rk.zones[c].select.weight(select)));
                }
                for (int i = ri + 1; i < end; ++i)
                    if (td.devices[size_t(i)].chain == c) runDevice(i, cev, cspare, m_chainL, m_chainR);
                rack->mixChain(c, m_chainL, m_chainR, instrumentRack ? outL : m_rackSumL, instrumentRack ? outR : m_rackSumR, n, rp, anySolo);
            }
            if (!instrumentRack) { std::copy(m_rackSumL, m_rackSumL + n, bL); std::copy(m_rackSumR, m_rackSumR + n, bR); }
            if (!fading) return;
            for (int s = 0; s < n; ++s) {
                mix = target > mix ? std::min(target, mix + fadeStep) : std::max(target, mix - fadeStep);
                if (instrumentRack) { bL[s] += m_rackFadeL[s] * mix; bR[s] += m_rackFadeR[s] * mix; }
                else { bL[s] = m_rackFadeL[s] + (bL[s] - m_rackFadeL[s]) * mix; bR[s] = m_rackFadeR[s] + (bR[s] - m_rackFadeR[s]) * mix; }
            }
            if (mix == 0 && instrumentRack)   // faded out: its instruments let go of their notes
                for (int i = ri + 1; i < end; ++i) if (td.devices[size_t(i)].proc) td.devices[size_t(i)].proc->panic();
        };
        // A MIDI Effect Rack: each chain takes the notes in its zones, moves them by its Transpose
        // and Velocity, runs its MIDI effects, and every chain's notes go on, merged. Off: the notes
        // pass untouched.
        auto runMidiRack = [&](int ri, int end, EventList*& evs) {
            const DeviceSlot& rk = td.devices[size_t(ri)];
            if (!instruments || !rk.proc || !rk.enabled || rk.chains == 0) return;
            rk.proc->onMix = 1;
            float rp[kMaxParams];
            paramsOf(ri, rp);
            const int select = int(std::lround(paramValue(rk.type, racks::ChainSelect, rp[racks::ChainSelect])));
            bool anySolo = false;
            for (int c = 0; c < rk.chains; ++c) anySolo |= rp[racks::param(c, racks::Solo)] >= 0.5f;
            m_rackEv.clear();
            for (int c = 0; c < rk.chains; ++c) {
                EventList* cev = &m_chainEvA;
                EventList* cspare = &m_chainEvB;
                const bool on = rp[racks::param(c, racks::On)] >= 0.5f && (!anySolo || rp[racks::param(c, racks::Solo)] >= 0.5f);
                const int transpose = int(std::lround(paramValue(rk.type, racks::param(c, racks::Transpose), rp[racks::param(c, racks::Transpose)])));
                const float velocity = float(paramValue(rk.type, racks::param(c, racks::VelocityScale), rp[racks::param(c, racks::VelocityScale)]));
                chainNotes(*evs, *cev, rk.zones[c], select, on, transpose, velocity);
                for (int i = ri + 1; i < end; ++i)
                    if (td.devices[size_t(i)].chain == c) runDevice(i, cev, cspare, m_chainL, m_chainR);
                for (int k = 0; k < cev->count; ++k) {
                    const NoteEvent& e = cev->ev[k];
                    if (e.allOff || e.noteOff) {   // the same release from two chains: once
                        bool twice = false;
                        for (int j = 0; j < m_rackEv.count && !twice; ++j) {
                            const NoteEvent& o = m_rackEv.ev[j];
                            twice = o.offset == e.offset && o.allOff == e.allOff && o.noteOff == e.noteOff && (e.allOff || o.pitch == e.pitch);
                        }
                        if (twice) continue;
                    }
                    m_rackEv.add(e);
                }
            }
            evs->count = m_rackEv.count;
            std::copy(m_rackEv.ev, m_rackEv.ev + m_rackEv.count, evs->ev);
        };
        // A Drum Rack whose pads hold chains: the pads without chains play straight out; a pad with
        // an instrument plays it (the pad's notes at C3, moved by its Transpose; another hit in its
        // choke group releases it) at the pad's volume and pan; a pad with effects runs its sound
        // (its sample's or its instrument's) through them.
        auto runDrumRack = [&](int ri, int end, EventList*& evs, float* bL, float* bR) {
            const DeviceSlot& dr = td.devices[size_t(ri)];
            if (!instruments || !dr.proc) return;
            ChainMixer* mixer = dr.proc->chainMixer();
            float& mix = dr.proc->onMix;
            const float target = dr.enabled ? 1.0f : 0.0f;
            if (mix < 0) mix = target;
            if (mix == 0 && target == 0) return;
            const bool fading = !(mix == 1 && target == 1);
            float* outL = bL;
            float* outR = bR;
            if (fading) {
                std::fill(m_rackFadeL, m_rackFadeL + n, 0.0f); std::fill(m_rackFadeR, m_rackFadeR + n, 0.0f);
                outL = m_rackFadeL; outR = m_rackFadeR;
            }
            float rp[kMaxParams];
            paramsOf(ri, rp);
            Context dctx = ctx;
            dctx.pads = dr.pads;
            dctx.padSample = ~dr.padInst;
            dctx.padPlay = ~dr.padHas;
            dr.proc->render(*evs, outL, outR, n, rp, dctx);
            const DeviceType kDrum = DeviceType::DrumRack;
            auto chokeOf = [&](int pad) { return choiceIndex(kDrum, drumrack::param(pad, drumrack::Choke), rp[drumrack::param(pad, drumrack::Choke)]); };
            for (int k = 0; k < drumrack::kPads; ++k) {
                if (!(dr.padHas >> k & 1u)) continue;
                EventList* cev = &m_chainEvA;
                EventList* cspare = &m_chainEvB;
                cev->clear();
                std::fill(m_chainL, m_chainL + n, 0.0f); std::fill(m_chainR, m_chainR + n, 0.0f);
                float gl = 1, gr = 1;   // a sample's voice has the pad's volume and pan already
                if (dr.padInst >> k & 1u) {
                    const int play = std::clamp(drumrack::kPlayNote + int(std::lround(paramValue(kDrum, drumrack::param(k, drumrack::Transpose), rp[drumrack::param(k, drumrack::Transpose)]))), 0, 127);
                    const int group = chokeOf(k);
                    for (int i = 0; i < evs->count; ++i) {
                        NoteEvent e = evs->ev[i];
                        const int pad = e.pitch - drumrack::kFirstNote;
                        if (e.allOff) { cev->add(e); continue; }
                        if (pad == k) { e.pitch = play; cev->add(e); continue; }
                        const bool sounds = pad >= 0 && pad < drumrack::kPads && ((dr.padInst >> pad & 1u) || (dr.pads[pad] && dr.pads[pad]->frames() > 1));
                        if (group > 0 && !e.noteOff && sounds && chokeOf(pad) == group) {
                            NoteEvent off;
                            off.offset = e.offset;
                            off.allOff = true;
                            cev->add(off);
                        }
                    }
                    const double g = std::pow(10.0, (paramValue(kDrum, 0, rp[0]) + paramValue(kDrum, drumrack::param(k, drumrack::Vol), rp[drumrack::param(k, drumrack::Vol)])) / 20);
                    const float pan = float(paramValue(kDrum, drumrack::param(k, drumrack::Pan), rp[drumrack::param(k, drumrack::Pan)]));
                    gl = float(g) * std::min(1.0f, 1 - pan);
                    gr = float(g) * std::min(1.0f, 1 + pan);
                } else {   // the pad's sample voices, into its chain
                    Context pctx = dctx;
                    pctx.padPlay = 1u << k;
                    dr.proc->render(*cev, m_chainL, m_chainR, n, rp, pctx);
                }
                for (int i = ri + 1; i < end; ++i)
                    if (td.devices[size_t(i)].chain == k) runDevice(i, cev, cspare, m_chainL, m_chainR);
                if (mixer) mixer->mix(k, m_chainL, m_chainR, outL, outR, n, gl, gr);
            }
            if (!fading) return;
            for (int s = 0; s < n; ++s) {
                mix = target > mix ? std::min(target, mix + fadeStep) : std::max(target, mix - fadeStep);
                bL[s] += m_rackFadeL[s] * mix;
                bR[s] += m_rackFadeR[s] * mix;
            }
            if (mix == 0) {   // faded out: let go of everything
                dr.proc->panic();
                for (int i = ri + 1; i < end; ++i) if (td.devices[size_t(i)].proc) td.devices[size_t(i)].proc->panic();
            }
        };
        bool preTapped = keyIndex < 0;
        auto preTap = [&] {   // the Pre FX key: clips and input, or the instrument's sound, before the audio effects
            std::copy(L, L + n, keyL(0, keyIndex));
            std::copy(R, R + n, keyR(0, keyIndex));
            preTapped = true;
        };
        for (int di = 0; di < int(td.devices.size()); ++di) {
            const DeviceSlot& d = td.devices[size_t(di)];
            if (d.rack != 0) continue;   // a rack runs its chains' devices
            if (!preTapped && d.kind == DeviceKind::AudioEffect) preTap();
            if (d.rackType) {
                int end = di + 1;
                while (end < int(td.devices.size()) && td.devices[size_t(end)].rack == d.deviceId) ++end;
                if (d.rackType == 3) runMidiRack(di, end, ev);
                else if (d.rackType == 4) runDrumRack(di, end, ev, L, R);
                else runRack(di, end, ev, L, R);
                di = end - 1;
                continue;
            }
            runDevice(di, ev, spare, L, R);
        }
        if (!preTapped) preTap();
    };

    int clickAt = -1;
    bool accent = false;
    if (S) {
        const Context ctx{m_sr, m_playing ? b0 : m_freeBeat, bps, tempo, m_playing};
        const int nt = std::min(int(S->tracks.size()), kMaxTracks);
        const int nr = std::min(int(S->returns.size()), kMaxReturns);

        // Fader, pan (balance) and sends of a track's buffer into `outL / outR` (Main or a group bus) and the returns.
        auto mixTrack = [&](const TrackData& td, PlayState& ps, float* outL, float* outR, int index) {
            const float vol = m_ovVolume >= 0 ? m_ovVolume : td.volAuto >= 0 ? td.autos[size_t(td.volAuto)].valueAt(b0) : td.volume;
            const float pan = m_ovPan >= 0 ? m_ovPan * 2 - 1 : td.panAuto >= 0 ? td.autos[size_t(td.panAuto)].valueAt(b0) * 2 - 1 : td.pan;
            const bool audible = td.active && (!S->anySolo || td.soloed);
            const float g = audible ? faderGain(vol) : 0.0f;
            const float targetL = g * std::min(1.0f, 1.0f - pan), targetR = g * std::min(1.0f, 1.0f + pan);
            float sendFrom[kMaxReturns], sendTo[kMaxReturns];
            for (int k = 0; k < nr; ++k) {
                const float amount = m_ovSend[k] >= 0 ? m_ovSend[k] : td.sendAuto[k] >= 0 ? td.autos[size_t(td.sendAuto[k])].valueAt(b0) : td.sends[k];
                sendFrom[k] = ps.send[k];
                sendTo[k] = audible ? amount : 0.0f;
                ps.send[k] = sendTo[k];
            }
            float peak = 0;
            for (int s = 0; s < n; ++s) {
                ps.gainL += (targetL - ps.gainL) * smooth;
                ps.gainR += (targetR - ps.gainR) * smooth;
                const float l = m_trackL[s] * ps.gainL, r = m_trackR[s] * ps.gainR;
                outL[s] += l;
                outR[s] += r;
                if (td.keySource) { keyL(2, index)[s] = l; keyR(2, index)[s] = r; }   // the Post Mix key
                peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
                const float t = float(s + 1) / float(n);
                for (int k = 0; k < nr; ++k) {
                    const float amt = sendFrom[k] + (sendTo[k] - sendFrom[k]) * t;
                    if (amt <= 0) continue;
                    if (S->returns[size_t(k)].preFader) { m_retL[k][s] += m_trackL[s] * amt; m_retR[k][s] += m_trackR[s] * amt; }
                    else { m_retL[k][s] += l * amt; m_retR[k][s] += r * amt; }
                }
            }
            raisePeak(m_trackPeak[size_t(index)], peak);
        };

        // ---- tracks, in rendering order (sidechain sources before the tracks they key; group tracks
        // wait for the second pass: their tracks fill their bus first)
        for (int oi = 0; oi < int(S->order.size()); ++oi) {
            const int i = S->order[size_t(oi)];
            if (i < 0 || i >= nt) continue;
            const TrackData& td = S->tracks[size_t(i)];
            if (td.group) continue;
            PlayState& ps = m_play[size_t(i)];

            // ---- events: pending note-offs, live / preview notes, then the clips under the playhead
            EventList* ev = &m_evA;
            EventList* spare = &m_evB;
            ev->clear();
            if (ps.pendingAllOff) { ev->add({0, 0, 0, 0, true, false}); ps.pendingAllOff = false; }
            for (int k = 0; k < m_liveCount; ++k)
                if (m_live[k].track == i) ev->add({0, m_live[k].pitch, m_live[k].velocity, m_live[k].duration, false, m_live[k].off});
            m_segCount = 0;
            if (m_playing) scheduleTrack(td, ps, eventFrom, b0, winEnd, bps, n, *ev);
            clipOverrides(td, ps, b0);

            // ---- sound: MIDI effects -> instrument -> audio effects (audio tracks: clips + input -> audio effects)
            std::fill(m_trackL, m_trackL + n, 0.0f);
            std::fill(m_trackR, m_trackR + n, 0.0f);
            if (td.audio) {
                // Monitoring replaces the track's clips: while you hear the input, you
                // don't hear the pass you recorded before, or the clip you're recording over.
                const bool monitorOn = td.monitor == int(Monitor::In) || (td.monitor == int(Monitor::Auto) && td.armed);
                if (!monitorOn) renderAudio(td.id, b0, bps, tempo, n, m_trackL, m_trackR);
                if (inputLive && in) {
                    // the input, as this track hears it (1/2 stereo, or input 1 or 2 on both sides)
                    float inPeak = 0;
                    for (int s = 0; s < n; ++s) {
                        const float l = td.inputChannel == 2 ? in[2 * s + 1] : in[2 * s];
                        const float r = td.inputChannel == 1 ? in[2 * s] : in[2 * s + 1];
                        inPeak = std::max(inPeak, std::max(std::fabs(l), std::fabs(r)));
                        if (monitorOn) { m_trackL[s] += l; m_trackR[s] += r; }
                    }
                    raisePeak(m_inputPeak[size_t(i)], inPeak);
                }
            }
            Context tctx = ctx;   // the track's performance controls reach its instruments
            if (const Perf* pf = perfFor(td.id)) { tctx.bend = pf->bend; tctx.wheel = pf->wheel; tctx.pressure = pf->pressure; }
            runChain(td, ev, spare, m_trackL, m_trackR, tctx, !td.audio, true, td.keySource ? i : -1);
            if (td.keySource) {   // the Post FX key
                std::copy(m_trackL, m_trackL + n, keyL(1, i));
                std::copy(m_trackR, m_trackR + n, keyR(1, i));
            }
            const bool toBus = td.bus >= 0 && td.bus < kMaxGroups;
            mixTrack(td, ps, toBus ? m_busL[td.bus] : m_mainL, toBus ? m_busR[td.bus] : m_mainR, i);
        }

        // ---- group tracks: their bus through their audio effects and mixer into Main
        m_ovVolume = m_ovPan = -1;
        std::fill(m_ovSend, m_ovSend + kMaxReturns, -1.0f);
        m_ovCount = 0;
        for (int i = 0; i < nt; ++i) {
            const TrackData& td = S->tracks[size_t(i)];
            if (!td.group || td.bus < 0 || td.bus >= kMaxGroups) continue;
            PlayState& ps = m_play[size_t(i)];
            std::copy(m_busL[td.bus], m_busL[td.bus] + n, m_trackL);
            std::copy(m_busR[td.bus], m_busR[td.bus] + n, m_trackR);
            EventList* ev = &m_evA;
            EventList* spare = &m_evB;
            ev->clear();
            runChain(td, ev, spare, m_trackL, m_trackR, ctx, false, false);
            // Arrangement fades act on the complete group, including its effects.
            // A Session takeover or monitored member is a live performance, with no
            // Arrangement boundary: its group remains audible at every song beat.
            bool fadeGroup = m_playing && !td.groupSpan.empty() && (td.groupFadeIn > 0 || td.groupFadeOut > 0);
            for (int member : td.groupMembers) {
                if (member < 0 || member >= nt) continue;
                const TrackData& child = S->tracks[size_t(member)];
                const bool monitored = inputLive && child.audio && (child.monitor == int(Monitor::In)
                                      || (child.monitor == int(Monitor::Auto) && child.armed));
                if (m_play[size_t(member)].overridden || monitored) { fadeGroup = false; break; }
            }
            if (fadeGroup) {
                const double length = td.groupSpan.end - td.groupSpan.start;
                const double in = std::min(td.groupFadeIn, length), out = std::min(td.groupFadeOut, length);
                for (int sample = 0; sample < n; ++sample) {
                    const double beat = b0 + sample * bps;
                    double gain = 1;
                    if (in > 0) gain *= fadeGain((beat - td.groupSpan.start) / in, td.groupFadeInCurve);
                    if (out > 0) gain *= fadeGain((td.groupSpan.end - beat) / out, td.groupFadeOutCurve);
                    m_trackL[sample] *= float(gain);
                    m_trackR[sample] *= float(gain);
                }
            }
            mixTrack(td, ps, m_mainL, m_mainR, i);
        }

        // ---- returns: audio effects only, then their own fader and pan
        for (int k = 0; k < nr; ++k) {
            const TrackData& rd = S->returns[size_t(k)];
            PlayState& ps = m_retPlay[size_t(k)];
            EventList* ev = &m_evA;
            EventList* spare = &m_evB;
            runChain(rd, ev, spare, m_retL[k], m_retR[k], ctx, false, false);
            const float vol = rd.volAuto >= 0 ? rd.autos[size_t(rd.volAuto)].valueAt(b0) : rd.volume;
            const float pan = rd.panAuto >= 0 ? rd.autos[size_t(rd.panAuto)].valueAt(b0) * 2 - 1 : rd.pan;
            const float g = rd.active ? faderGain(vol) : 0.0f;
            const float targetL = g * std::min(1.0f, 1.0f - pan), targetR = g * std::min(1.0f, 1.0f + pan);
            float peak = 0;
            for (int s = 0; s < n; ++s) {
                ps.gainL += (targetL - ps.gainL) * smooth;
                ps.gainR += (targetR - ps.gainR) * smooth;
                const float l = m_retL[k][s] * ps.gainL, r = m_retR[k][s] * ps.gainR;
                m_mainL[s] += l;
                m_mainR[s] += r;
                peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
            }
            raisePeak(m_returnPeak[size_t(k)], peak);
        }

        // ---- the Master's devices (0.21): everything mixed so far, through its chain, before the master fader
        if (!S->master.devices.empty()) {
            EventList* ev = &m_evA;
            EventList* spare = &m_evB;
            ev->clear();
            runChain(S->master, ev, spare, m_mainL, m_mainR, ctx, false, false);
        }

        // ---- metronome: a click on every beat, accented on the bar
        if (m_playing && m_metro) {
            const double k = std::ceil(eventFrom - 1e-9);
            if (k < winEnd) { clickAt = offsetOf(k, b0, bps, n); accent = std::fmod(k, 4.0) < 0.5; }
        }
        if (m_countIn > 0) {   // the count-in always clicks, on its own clock
            const double c0 = m_countTotal - m_countIn, k = std::ceil(c0 - 1e-9);
            if (k < c0 + n * bps - 1e-9 && k < m_countTotal - 1e-9) { clickAt = offsetOf(k, c0, bps, n); accent = std::fmod(k, 4.0) < 0.5; }
        }
    }

    // ---- main: fader, metronome and browser preview on top (like a cue output), soft clip, meter
    const float mg = S ? faderGain(S->mainVolume) : 0.0f;
    const float clickGain = S ? 0.35f * faderGain(S->cueVolume) : 0.0f;
    const SampleData* preview = S ? S->preview.get() : nullptr;
    if (S && S->previewSerial != m_previewPlaying) { m_previewPlaying = S->previewSerial; m_previewPos = preview ? 0 : -1; }
    const double previewStep = preview ? preview->rate / m_sr : 0;
    const float previewGain = S ? faderGain(S->cueVolume) / faderGain(0.6) * 0.7f : 0.0f;
    const float clickDecay = onePoleCoef(0.012f, m_sr);
    float peak = 0;
    for (int s = 0; s < n; ++s) {
        m_mainGain += (mg - m_mainGain) * smooth;
        float l = m_mainL[s] * m_mainGain, r = m_mainR[s] * m_mainGain;
        if (s == clickAt) { m_clickEnv = 1; m_clickPhase = 0; m_clickFreq = accent ? 1760.0f : 1320.0f; }
        if (m_clickEnv > 1e-4f) {
            const float c = std::sin(kTwoPi * m_clickPhase) * m_clickEnv * clickGain;
            m_clickPhase += m_clickFreq / m_sr;
            if (m_clickPhase >= 1) m_clickPhase -= 1;
            m_clickEnv *= clickDecay;
            l += c; r += c;
        }
        if (preview && m_previewPos >= 0) {
            if (m_previewPos >= double(preview->frames())) m_previewPos = -1;
            else {
                l += resample::read(preview->left, m_previewPos, previewStep) * previewGain;
                r += resample::read(preview->right, m_previewPos, previewStep) * previewGain;
                m_previewPos += previewStep;
            }
        }
        l = softClip(l);
        r = softClip(r);
        out[2 * s] = l;
        out[2 * s + 1] = r;
        peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
    }
    raisePeak(m_mainPeak, peak);

    // ---- advance the clocks
    m_liveCount = 0;
    m_freeBeat += n * bps;
    if (m_playing) m_playSeconds += double(n) / m_sr;
    if (m_countIn > 0) {
        m_countIn -= n * bps;
        if (m_countIn <= 1e-6) { m_countIn = 0; m_playing = true; }   // the next block plays from the start beat
    }
    if (playedThisBlock) {
        m_beat = b1;
        if (wraps) {
            // Carry fractional samples instead of accumulating a rounding error on every
            // pass. eventFrom includes the tiny head interval next time, so notes and the
            // metronome on the loop start still sound on its first available sample.
            m_beat = m_loopStart + std::max(0.0, b1 - loopEnd);
            m_eventStart = m_loopStart;
            for (PlayState& ps : m_play) {
                if (ps.playingSlot >= 0) {
                    // A global timeline wrap does not retrigger a Session clip or its DSP.
                    for (StretchSlot& k : m_stretchKey)
                        if (k.lastUse && k.trackId == ps.trackId && k.clipId == ps.playingClip && k.origin == ps.slotStart) k.origin -= m_loopLen;
                    ps.slotStart -= m_loopLen;
                }
                if (ps.queuedSlot != -2 && ps.queuedAt >= loopEnd - 1e-9) ps.queuedAt -= m_loopLen;
                if (ps.playingSlot < 0 && !ps.overridden) ps.pendingAllOff = true;   // arrangement notes end at the loop end
            }
        }
    }
}

} // namespace sessions::audio
