#pragma once
// Sessions — core data model.
// Plain structs owned by Project; the UI reads them directly and asks
// Project to emit change signals after editing. Beats are the time unit
// everywhere (4 beats = 1 bar at 4/4).
//
// Undo: wrap an edit in EditScope (or beginEdit/endEdit around a drag).
// Project snapshots everything editable (clips, tracks with their mixer
// settings, devices and automation, scenes, tempo, locators) before and
// after, and pushes the pair onto its QUndoStack. Playback state, solo,
// arm and meters are not part of the snapshot.

#include "Sample.h"
#include "Mapping.h"
#include <QObject>
#include <QString>
#include <QColor>
#include <QVector>
#include <QHash>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <memory>

class QUndoStack;

namespace sessions::nam { struct Model; }

namespace sessions {

struct Note {
    int    pitch    = 60;    // MIDI note number
    double start    = 0.0;   // beats, relative to clip start
    double length   = 0.25;  // beats
    double velocity = 0.8;   // 0..1
    double chance   = 1.0;   // 0..1
};

// ---- automation
// Every automatable control has a target string and a 0..1 value:
//   "mixer:volume", "mixer:pan" (0.5 = centre), "mixer:send:<k>", "device:<deviceId>:<param>"
struct AutoPoint { double beat = 0; double value = 0; };
struct Envelope {
    QString            target;
    QVector<AutoPoint> points;   // sorted by beat
    double valueAt(double beat) const;   // linear between points, flat beyond the ends
};

namespace target {
inline QString volume() { return QStringLiteral("mixer:volume"); }
inline QString pan()    { return QStringLiteral("mixer:pan"); }
inline QString send(int k) { return QStringLiteral("mixer:send:%1").arg(k); }
inline QString device(int deviceId, int param) { return QStringLiteral("device:%1:%2").arg(deviceId).arg(param); }
}

enum class LaunchMode { Trigger, Gate, Toggle, Repeat };
enum class FollowAction { None, Stop, Again, Previous, Next, First, Last, Any, Other };

// Audio clips are warped: warp markers pin clip beats to times in the sample, and the
// audio between them is stretched to fit the tempo, in one of six ways (0.21):
//  - Beats: slices the audio and plays each slice at its own speed (the pitch stays;
//    transpose shifts it). Slices start at the sample's transients, or on a fixed grid
//    (Clip::grain). Drums and percussive loops.
//  - Tones: overlapping grains (Clip::grainMs), each lined up with the last one's
//    continuation, so a sustained note stays smooth. Bass, vocals, monophonic lines.
//  - Texture: grains that wander by Clip::flux: a loose, cloudy sound. Pads, noise, ambience.
//  - Re-Pitch: plays faster or slower, like a record (the pitch follows the tempo).
//  - Complex: a phase vocoder: every partial keeps its pitch while the time stretches. Full mixes.
//  - Complex Pro: the vocoder with its bins locked around each spectral peak and its phases
//    restarted at transients (clean chords, sharp hits), and Clip::formants to keep the
//    formants in place when transposing (a voice stays a voice an octave down).
// With Warp off (Clip::warp) the audio plays at its own speed from the first marker,
// whatever the tempo. The order here is the chooser's; sets save the mode by name.
enum class WarpMode { Beats, Tones, Texture, Repitch, Complex, ComplexPro };
inline const char* warpModeName(WarpMode m) {
    switch (m) {
    case WarpMode::Beats: return "Beats";
    case WarpMode::Tones: return "Tones";
    case WarpMode::Texture: return "Texture";
    case WarpMode::Repitch: return "Re-Pitch";
    case WarpMode::Complex: return "Complex";
    case WarpMode::ComplexPro: return "Complex Pro";
    }
    return "Beats";
}
struct WarpMarker { double beat = 0; double seconds = 0; };

// The warp map, shared with the audio thread: linear between markers, the end segments'
// slopes beyond them. `m` is sorted by beat (and by seconds); fewer than two markers = 120 BPM.
inline double warpSecondsAt(const WarpMarker* m, int n, double beat) {
    if (n < 2) return (n ? m[0].seconds - m[0].beat * 0.5 : 0.0) + beat * 0.5;
    int i = 0;
    while (i < n - 2 && m[i + 1].beat <= beat) ++i;
    const double span = m[i + 1].beat - m[i].beat;
    return span > 1e-12 ? m[i].seconds + (beat - m[i].beat) * (m[i + 1].seconds - m[i].seconds) / span : m[i].seconds;
}

// Takes and comping (audio clips recorded in a loop): every pass of an Arrangement loop
// recording is kept as a take, a separate file that begins at clip beat `startBeat` (0 when
// the pass started with the loop; later when recording began mid-loop). The comp says which
// take plays where: regions in clip beats, each assigned to a take; wherever no region is,
// the newest take plays. Regions never overlap; the engine crossfades where the take changes.
struct Take {
    QString   samplePath;   // the pass's WAV (absolute)
    SamplePtr sample;       // nullptr while the file is missing
    QString   name;         // "Take 1", …
    double    startBeat = 0;
};
struct CompRegion { double start = 0, end = 0; int take = 0; };

// Playback of a clip (0.23): pass 0 runs from `start` (a launched clip: Clip::start; a
// timeline placement: its offset) to the end marker, loopStart + length; with Loop on every later
// pass goes round the brace [loopStart, loopStart + length). clipPositionAt gives the clip's
// timeline position `t` beats after the launch (the caller rejects < 0, and >= the end marker
// with Loop off). forClipPasses calls f(at, from, to) for every pass overlapping [lo, hi) beats
// after the launch: `at` beats after it, the timeline plays from `from` to `to`.
inline double clipPositionAt(double start, double loopStart, double length, bool loop, double t) {
    double pos = start + t;
    const double end = loopStart + length;
    if (loop && length > 0 && pos >= end) pos = loopStart + std::fmod(pos - loopStart, length);
    return pos;
}
template <class F>
inline void forClipPasses(double start, double loopStart, double length, bool loop, double lo, double hi, F&& f) {
    const double end = loopStart + length;
    if (loop && length > 0 && start >= end) start = loopStart + std::fmod(start - loopStart, length);
    if (start >= end || hi <= 0) return;
    double at = 0, from = start;
    if (loop && length > 0 && lo > end - start) {   // skip the passes before `lo`
        const double k = std::floor((lo - (end - start)) / length);
        at = (end - start) + k * length; from = loopStart;
    }
    while (at < hi) {
        if (at + (end - from) > lo) f(at, from, end);
        if (!loop || length <= 0) break;
        at += end - from; from = loopStart;
    }
}

struct Clip {
    int             id = 0;
    QString         name;
    QColor          color;          // the clip's own colour (0.21); invalid: the track's (Project::clipColor)
    double          length = 4.0;   // the loop brace's length in beats
    bool            loop   = true;
    // The start marker and the loop brace (0.23): the clip's own timeline runs from 1.1.1 (beat 0);
    // playback begins at `start`, runs to the end marker (loopStart + length) and, with Loop on,
    // goes round the brace [loopStart, loopStart + length) from then on; with Loop off it stops at
    // the end marker. Both 0 by default: a clip plays from 1.1.1 for `length` beats as it always did.
    double          start     = 0;
    double          loopStart = 0;
    double          loopEnd() const { return loopStart + length; }
    double          positionAt(double t) const;                  // the timeline position `t` beats after a launch, or -1 when nothing plays
    double          positionFrom(double from, double t) const;   // the same for a placement whose offset is `from`
    QVector<Note>   notes;

    // Launch settings
    LaunchMode   launchMode    = LaunchMode::Trigger;
    bool         legato        = false;   // take over the playing clip's position
    double       quantization  = -1;      // beats; -1 = global launch quantization, 0 = none
    bool         followEnabled = false;
    FollowAction followA       = FollowAction::Next;
    FollowAction followB       = FollowAction::None;
    double       followChanceA = 1.0;     // 0..1; B gets the rest
    double       followTime    = 0;       // beats after launch; 0 = one loop of the clip

    // Notes: scale highlighting / folding in the note editor
    bool scaleOn   = false;
    int  scaleRoot = 0;    // 0 = C
    int  scale     = 0;    // index into Scales.h

    // Audio clips (on audio tracks): a sample instead of notes.
    QString   samplePath;            // absolute path of the audio file; empty for a MIDI clip
    SamplePtr sample;                // the decoded audio; nullptr while the file is missing
    WarpMode  warpMode  = WarpMode::Beats;
    QVector<WarpMarker> warpMarkers; // sorted by beat, at least two
    double    gain      = 0;         // dB
    int       transpose = 0;         // semitones (every mode but Re-Pitch, where the tempo sets the pitch)
    bool      reverse   = false;     // plays the sample backwards; the markers refer to the reversed audio
    bool      warp      = true;      // off: plays at the file's own speed, ignoring the tempo
    double    grain     = 0;         // Beats mode slices: 0 = at transients, else a grid in beats (0.25 = 1/16)
    int       beatsTail = 0;         // Beats: 0 gate, 1 crossfaded forward sustain, 2 reflected sustain
    double    grainMs   = 50;        // Tones / Texture: the grain size, 12 .. 256 ms
    double    flux      = 0.25;      // Texture: how far the grains wander, 0 .. 1
    double    formants  = 0;         // Complex Pro: how much the formants stay put when transposed, 0 .. 1
    // The tempo found in the audio (core/Tempo.h), 0 when unknown. sampleBpm() is the
    // editable source warp map's tempo; project tempo changes leave that map untouched.
    double    detectedBpm = 0;

    // Clip envelopes: the same targets as track automation ("device:<id>:<p>", "mixer:volume", …),
    // positions in clip beats, looping with the clip. While the clip plays they set those controls.
    QVector<Envelope> envelopes;
    const Envelope* envelope(const QString& target) const;
    Envelope*       envelope(const QString& target);

    // Takes (see Take above). With takes, `samplePath` / `sample` mirror the newest take and
    // the engine plays from the takes through the comp.
    QVector<Take>       takes;
    QVector<CompRegion> comp;     // sorted by start, inside [0, length)
    int  takeAt(double beat) const;   // the take playing at a clip beat (the newest where no region is); -1 without takes
    // Uses take `take` between `start` and `end` (clip beats): earlier regions there are cut
    // around it, neighbours with the same take are joined.
    void useTake(double start, double end, int take);
    void useWholeTake(int take);      // one region over the whole clip (the newest: no regions)
    double takeSeconds(int take, double beat) const;   // seconds into that take's file at a clip beat (< 0: before it begins)

    bool   isAudio() const { return !samplePath.isEmpty(); }
    double secondsAt(double beat) const;   // warp map: clip beat -> seconds into the (reversed) sample
    double beatAt(double seconds) const;   // and back
    double sampleBpm() const;              // the tempo at which the audio plays at its own speed
};
using ClipPtr = std::shared_ptr<Clip>;

enum class DeviceKind { Instrument, AudioEffect, MidiEffect };

struct DeviceParam {
    QString name;
    double  value = 0.5;   // 0..1 normalized; DeviceSpecs maps it to real units
};

// A Drum Rack pad: one sample, played from its start (pitch by the pad's Transpose).
// Built-in sounds have a "builtin:<kit>/<note>" path (audio/Builtin.h).
struct DrumPad {
    QString   name;         // shown on the pad: the file's name, or the built-in sound's
    QString   samplePath;   // empty: the pad is empty
    SamplePtr sample;       // nullptr while the file is missing
};

// A Macros knob turning another device's parameter: macro at 0 sets it to `lo`, at 1 to `hi`.
struct MacroMap {
    int    macro = 0;      // 0..7
    int    deviceId = 0;   // a device on the same track
    int    param = 0;
    double lo = 0, hi = 1;
};

// Monolith (the synth): one modulation route, a source (monolith::Source) turning one of
// its own parameters by `amount` of the knob's range (-1..1; bipolar sources swing both ways).
struct ModRoute {
    int    source = 0;
    int    param = 0;
    double amount = 0;
};
inline bool operator==(const ModRoute& a, const ModRoute& b) { return a.source == b.source && a.param == b.param && a.amount == b.amount; }
inline bool operator!=(const ModRoute& a, const ModRoute& b) { return !(a == b); }
// A point of one of the Monolith's drawable curves (0.21): x along the curve (0 .. 1 of its length), y its value (0 .. 1).
struct CurvePoint {
    double x = 0, y = 0;
    double bend = 0;  // outgoing segment: -1 fast start .. +1 slow start
    int shape = 0;    // 0 bent line, 1 smooth S, 2 hold
};
inline bool operator==(const CurvePoint& a, const CurvePoint& b) { return a.x == b.x && a.y == b.y && a.bend == b.bend && a.shape == b.shape; }
inline bool operator!=(const CurvePoint& a, const CurvePoint& b) { return !(a == b); }

// A rack chain's zone: the values (0 .. 127: a note, a velocity, the Chain selector) it answers
// to, lo to hi. It fades in over the first fadeLo steps above lo and out over the last fadeHi
// below hi (the fade ranges). Notes in a fade play softer (their velocity is scaled); an
// Audio Effect Rack's chain-select fades turn the chain's input down.
struct Zone {
    int lo = 0, hi = 127, fadeLo = 0, fadeHi = 0;
    double weight(int x) const {   // 0 outside, 1 inside, between in the fades
        if (x < lo || x > hi) return 0;
        double w = 1;
        if (fadeLo > 0 && x < lo + fadeLo) w = std::min(w, double(x - lo + 1) / (fadeLo + 1));
        if (fadeHi > 0 && x > hi - fadeHi) w = std::min(w, double(hi - x + 1) / (fadeHi + 1));
        return w;
    }
    Zone sane() const;   // inside 0 .. 127, lo <= hi, the fades fitting between them
    bool operator==(const Zone& o) const { return lo == o.lo && hi == o.hi && fadeLo == o.fadeLo && fadeHi == o.fadeHi; }
    bool operator!=(const Zone& o) const { return !(*this == o); }
};
// A rack chain's three zones. By default a chain takes every key and velocity, and answers to
// the Chain selector only at 0 (where it starts), so zones change nothing until you set them.
struct ChainZones {
    enum Which { Key, Velocity, Select };
    Zone key, vel{1, 127, 0, 0}, select{0, 0, 0, 0};
    Zone&       operator[](int which)       { return which == Key ? key : which == Velocity ? vel : select; }
    const Zone& operator[](int which) const { return which == Key ? key : which == Velocity ? vel : select; }
    bool operator==(const ChainZones& o) const { return key == o.key && vel == o.vel && select == o.select; }
    bool operator!=(const ChainZones& o) const { return !(*this == o); }
    bool isDefault() const { return *this == ChainZones{}; }
};

struct Device {
    int                  id = 0;   // stable across undo; the audio engine keys its DSP state on it
    QString              name;
    DeviceKind           kind = DeviceKind::AudioEffect;
    bool                 enabled = true;
    QVector<DeviceParam> params;
    QString              samplePath;   // Sampler: the loaded sample (empty: a plain tone)
    SamplePtr            sample;       // nullptr while the file is missing
    QString              modelPath;    // NAM: the .nam capture (the amp's / NAM's samplePath is their cabinet IR)
    std::shared_ptr<const nam::Model> model;   // nullptr while the file is missing or unreadable (plays dry)
    QVector<DrumPad>     pads;         // Drum Rack: 16 pads from C1 (36) up
    QVector<MacroMap>    maps;         // Macros and racks: what their macro knobs turn
    QVector<ModRoute>    mods;         // Monolith: its modulation routes (envelopes, LFOs, steppers, macros … onto its knobs)
    QVector<QVector<CurvePoint>> curves;   // Monolith: its drawable curves (monolith::kCurves lists, each sorted by x; an empty one is flat 0)
    double               sampleBpm = 0;    // Sampler (0.32): the sample's own tempo (found when it loads, or set; 0: none), what Warp stretches from
    QVector<double>      slices;           // Sampler: the slices you placed (Slice By Manual), seconds into the sample, sorted
    QStringList          chains;       // racks: the names of their chains
    QVector<ChainZones>  zones;        // racks: each chain's key / velocity / chain-select zones (as many as chains)
    int                  rack = 0;     // a device in a rack's chain (or on a Drum Rack's pad): the rack's id (0: straight on the track)
    int                  chain = 0;    // and which of its chains (a Drum Rack: the pad)
    bool                 folded = false;   // the Device View shows just its title (double-click it)
    // Compressor, Gate, Auto Filter: the track whose audio drives the detector (the key),
    // 0 = the device's own input. Which tap of it (Pre FX / Post FX / Post Mix), the key's
    // gain and filters and Listen are parameters (DeviceSpecs: sidechain::). A deleted source
    // leaves the id: the key is then silent ("Missing").
    int                  sidechainTrack = 0;
};

// Racks keep the devices of their chains right after them in the track's list, chain by
// chain, each chain in playing order. A device's "block" is the device itself, or a rack and
// everything in its chains. A Drum Rack straight on a track holds chains too, one per pad (its
// pads' instruments and effects): holdsChains. Nothing holding chains goes inside a chain, except
// a Drum Rack whose pads hold nothing (it then plays its samples, like any instrument).
bool isRack(const Device& d);                                     // Instrument / Audio Effect / MIDI Effect Rack
bool isDrumRack(const Device& d);
bool holdsChains(const Device& d);                                // a rack, or a Drum Rack (on the track itself)
int  chainCapacity(const Device& d);                              // racks: 8 chains; a Drum Rack: its 16 pads
int  deviceBlockEnd(const QVector<Device>& devices, int index);   // index just past the block starting at `index`
void normalizeRacks(QVector<Device>& devices);                    // restores that order (sets read from files)
struct RackRecipe;

// A fade's gain `t` of the way through it (0 .. 1), for a curve: the gain half-way through, 0.5
// for a straight line, less for a slow start (exponential), more for a fast one (logarithmic).
inline double fadeGain(double t, double curve) {
    t = std::clamp(t, 0.0, 1.0);
    curve = std::clamp(curve, 0.02, 0.98);
    return std::pow(t, std::log(curve) / std::log(0.5));
}

struct ArrangementClip {
    double  start  = 0.0;   // beats on the timeline
    double  length = 4.0;   // beats
    double  offset = 0.0;   // clip time at `start` (a split's second half starts mid-clip): the placement's own start marker
    ClipPtr clip;
    // Fades (0.33, audio clips): a fade-in over the first `fadeIn` beats of the placement and a
    // fade-out over its last `fadeOut` (0: none), each with its curve (fadeGain). They belong to
    // the placement, not the clip: the same clip placed twice can fade differently. The Arrangement
    // shows them as a line from the corner with a square handle (the length) and a round one (the curve).
    double  fadeIn = 0, fadeOut = 0, fadeInCurve = 0.5, fadeOutCurve = 0.5;
    double  beatOf(double clipBeat) const;   // the first timeline beat where this placement plays that clip beat, or -1
    double  fadeAt(double beat) const {      // the fades' gain at a timeline beat inside the placement
        double g = 1;
        if (fadeIn > 0) g *= fadeGain((beat - start) / fadeIn, fadeInCurve);
        if (fadeOut > 0) g *= fadeGain((start + length - beat) / fadeOut, fadeOutCurve);
        return g;
    }
};

// MIDI tracks: notes -> instrument. Audio tracks: audio clips (and the audio input), audio
// effects only. Group tracks: no clips of their own; the tracks in the group (Track::groupId)
// play into the group's audio effects and mixer instead of the Main output.
enum class TrackType { Midi, Audio, Group };

// Audio tracks: when the input is heard through the track (the Monitor switch).
// Auto: while the track is armed. In: always. Off: never (recording still works).
enum class Monitor { In, Auto, Off };

struct Track {
    int       id = 0;          // stable identity: engine state, undo, recording
    TrackType type = TrackType::Midi;
    QString   name;
    QColor    color;
    double    volume = 0.75;   // 0..1, 0.75 = 0 dB
    double    pan    = 0.0;    // -1..1
    QVector<double> sends;     // one per return track, 0..1
    bool      active = true;   // track activator (mute when false)
    bool      solo   = false;
    bool      armed  = false;
    bool      preFader = false; // return tracks: sends tap the signal before the track faders
    Monitor   monitor  = Monitor::Auto;   // audio tracks
    int       inputChannel = 0;           // audio tracks: 0 = inputs 1/2 (stereo), 1 = input 1, 2 = input 2 (mono)
    int       groupId  = 0;               // the group track this track plays into (0 = none, straight to Main)

    QVector<ClipPtr>         clipSlots;      // one per scene (nullptr = empty)
    QVector<ArrangementClip> arrangement;
    QVector<Device>          devices;
    QVector<Envelope>        automation;

    const Envelope* envelope(const QString& target) const;
    Envelope*       envelope(const QString& target);

    // runtime state (not undoable)
    QSet<QString> overriddenTargets;   // automation you've overridden by hand, until Re-Enable Automation
    int    playingSlot  = -1;   // session clip currently playing (mirrored from the engine)
    double slotStart    = 0.0;  // beat at which that clip started
    double playingClipBeat = -1; // native-speed audio position mirrored from the engine
    int    playingClipPass = 0;
    int    queuedSlot   = -2;   // -2 none, -1 stop, >=0 launch
    double queuedAt     = 0.0;
    bool   overridden   = false; // session has taken over from the arrangement
    double meter        = 0.0;   // 0..1, post-fader peak on the meter scale
    int    width        = 0;     // Session View column width in px (0 = default); saved with the set, not undoable
    bool   folded       = false; // group tracks: members hidden in the Session and Arrangement; saved, not undoable
    double inputMeter   = 0.0;   // audio tracks: the input level, 0..1 on the meter scale
    bool   isGroup() const { return type == TrackType::Group; }
};

struct Locator {
    double  beat = 0;
    QString name;
};

// Everything undo/redo restores.
struct EditState {
    struct Placed { double start, length, offset; int clip; double fadeIn = 0, fadeOut = 0, fadeInCurve = 0.5, fadeOutCurve = 0.5; };
    struct TrackState {
        int id; QString name; QColor color;
        double volume, pan; QVector<double> sends; bool active, preFader;
        QVector<int> slotClips; QVector<Placed> arrangement;
        QVector<Device> devices; QVector<Envelope> automation;
        TrackType type = TrackType::Midi;
        Monitor monitor = Monitor::Auto; int inputChannel = 0; int groupId = 0;
    };
    QMap<int, Clip>     clips;   // by clip id
    QVector<TrackState> tracks, returns;
    TrackState          master;   // the Master's devices and automation (0.21)
    QStringList         scenes;
    double              tempo = 120, mainVolume = 0.75;
    QVector<Locator>    locators;
};
bool operator==(const EditState& a, const EditState& b);

class Project : public QObject {
    Q_OBJECT
public:
    explicit Project(QObject* parent = nullptr);
    ~Project() override;

    QVector<Track>   tracks;
    QVector<Track>   returns;      // return tracks (A Reverb, B Delay in the demo)
    Track            master;       // the Master track (0.21): its devices process the whole mix before the master fader; index trackCount()
    QStringList      scenes;
    QVector<Locator> locators;     // sorted by beat
    double           mainVolume = 0.75;
    double           cueVolume  = 0.6;
    double           tempo      = 120.0;
    double           mainMeter  = 0.0;
    QVector<Mapping> mappings;     // MIDI Map / Key Map (saved with the set, not undoable)

    // Tracks and returns share one index space: tracks first, then returns.
    int          trackCount() const { return int(tracks.size() + returns.size()); }
    bool         isReturn(int i) const { return i >= tracks.size() && i < trackCount(); }
    bool         isMaster(int i) const { return i == trackCount(); }   // trackAt(trackCount()) is the Master (not counted in trackCount)
    static constexpr int kMasterId = -7;                              // Track::id of the Master: never a real track's
    QColor       clipColor(const std::shared_ptr<Clip>& clip) const;  // the clip's own colour, else its track's
    Track*       trackAt(int i);
    const Track* trackAt(int i) const;
    int          indexOfTrack(int id) const;               // -1 if gone
    int          trackOfClip(const ClipPtr& clip) const;   // owning regular track, or -1

    int   selectedTrack = 0;
    ClipPtr selectedClip;
    // Several tracks selected at once (Ctrl / Shift + click on titles), by track id; for
    // Group Tracks. Always holds the selected track while it's a regular track.
    QSet<int> selectedTrackIds;
    QList<int> selectedTrackIndexes() const;   // regular tracks, in order
    // Where new audio recordings are written (the set's Samples/Recorded folder once it's
    // saved, else ~/Music/Sessions/Recorded). Set by the window.
    QString recordFolder;

    // ---- groups (each one undo step)
    int  groupTracks(const QList<int>& indexes);   // a new group track before the first; returns its index (-1 if none)
    void ungroup(int groupIndex);                  // members go straight to Main again; the group track goes
    int  groupOf(int trackIndex) const;            // index of the group a track plays into, -1 if none
    QList<int> membersOf(int groupIndex) const;     // indexes of its tracks
    bool hiddenByFold(int trackIndex) const;       // inside a folded group
    void setFolded(int groupIndex, bool folded);   // not undoable (a view setting); saved with the set

    void loadDemo();
    void loadEmpty();   // File > New: two empty MIDI tracks, the A Reverb / B Delay returns, eight scenes
    // Replaces the whole set with `s` (File > Open): ids are kept, the id counters move past
    // the highest id in it, and runtime state (playback, solo, arm) and undo history start fresh.
    // cueVolume isn't part of EditState: set it before calling.
    void loadEditState(const EditState& s);
    ClipPtr newClip(const QString& name = "MIDI Clip", double length = 4.0);
    // An audio clip for `path` (sample may be null when the file is missing). Loops that
    // last a power-of-two number of bars at a plausible tempo are warped to whole bars and
    // loop; anything else plays at its own speed at the current tempo, once.
    ClipPtr newAudioClip(const QString& path, const SamplePtr& sample);
    ClipPtr copyClip(const ClipPtr& src);                   // new id, same contents and settings
    Device makeDevice(const QString& name, DeviceKind kind);
    // What adding `name` puts on a track: the device, or a ready-made rack with its chains' devices.
    QVector<Device> makeDeviceBlock(const QString& name, DeviceKind kind);
    bool   addDeviceToSelected(const QString& name, DeviceKind kind);   // false if the track can't take it

    // ---- putting devices somewhere, moving and copying them (each one undo step)
    // A place on a track: at the top level before the block of device `beforeId` (0: at the
    // end, -1: their natural place, the end of their kind's part of the chain), or with a
    // `rackId`, in that rack's chain `chain` (-1 or past the last: a new chain named after the
    // first device) before its device `beforeId` (0 or -1: at the end). Kinds keep their order
    // wherever they're dropped: MIDI effects, the instrument (one per track or chain: a new one
    // replaces it), audio effects.
    struct DevicePlace { int beforeId = -1; int rackId = 0; int chain = -1; };
    using DeviceBlocks = QVector<QVector<Device>>;   // each: a device, or a rack and what's in its chains
    // Puts `blocks` (devices nobody else has: makeDeviceBlock, freshCopies, a preset) on track
    // `to`. False, with `why`, and nothing changes when one of them can't go there.
    bool insertDevices(int to, const DeviceBlocks& blocks, const DevicePlace& at, const QString& label,
                       QList<int>* placed = nullptr, QString* why = nullptr);
    // Moves devices `ids` of track `from` (a rack with its chains; a device inside a rack that's
    // moving too stays in it) to track `to`, or copies them. A device leaving a rack loses the
    // rack macros that turned it, a Macros device the mappings to devices it left behind;
    // moving to another track takes their automation along. `placed`: the ids where they landed.
    bool moveDevices(int from, const QList<int>& ids, int to, const DevicePlace& at, bool copy = false,
                     QList<int>* placed = nullptr, QString* why = nullptr);
    DeviceBlocks deviceBlocks(int track, const QList<int>& ids) const;   // what moveDevices would take, in chain order
    DeviceBlocks freshCopies(const DeviceBlocks& blocks);   // new ids; racks keep their chains, macros their mappings
    // Would `blocks` fit on track `to` at `at`? (Drag feedback; no change.)
    bool canPlace(int to, const DeviceBlocks& blocks, const DevicePlace& at, QString* why = nullptr) const;
    // A new track after the last one (outside any group) for `blocks`: MIDI when one of them is
    // an instrument or a MIDI effect, else audio; named `name`. Returns its index.
    int    insertTrackFor(const DeviceBlocks& blocks, const QString& name, int after = -1);   // after: the index it follows (in that track's group); -1 = after the last, outside groups
    bool   deleteDevices(int trackIndex, const QList<int>& ids);   // racks go with their chains
    void   setDeviceFolded(int trackIndex, int deviceId, bool folded);

    // ---- racks (each one undo step; see racks:: in DeviceSpecs.h)
    // Adds a device to a rack's chain (an instrument replaces the chain's instrument). A chain
    // index past the last makes a new chain named after the device. False if it can't go there.
    bool   addDeviceToChain(int trackIndex, int rackId, int chain, const QString& name);
    int    addChain(int trackIndex, int rackId);   // a new empty chain; its index, or -1 when the rack is full
    // Deletes a chain and its devices; the later chains move up, and so do their knobs, zones,
    // automation, clip envelopes and macro mappings. On a Drum Rack: the pad's devices go (the pad stays).
    void   deleteChain(int trackIndex, int rackId, int chain);
    void   renameChain(int trackIndex, int rackId, int chain, const QString& name);
    // Moves chain `from` to position `to` (the ones between shift over), with its devices, knobs,
    // zones, automation, clip envelopes and macro mappings. False when nothing moves.
    bool   moveChain(int trackIndex, int rackId, int from, int to);
    // Sets one of a chain's zones (ChainZones::Which). `mergeKey`: a drag's steps become one undo step.
    bool   setChainZone(int trackIndex, int rackId, int chain, int which, const Zone& zone, quintptr mergeKey = 0);
    bool   deleteDevice(int trackIndex, int deviceId);   // a rack goes with its chains
    int    groupIntoRack(int trackIndex, int deviceId);  // puts a device into a new rack's chain; the rack's id, or 0
    // Several devices (top level; no racks, Macros or MIDI effects) into one chain of a new rack
    // where the first one was: an Instrument Rack when one is an instrument. The rack's id, or 0.
    int    groupIntoRack(int trackIndex, const QList<int>& deviceIds);
    bool   ungroupRack(int trackIndex, int rackId);      // a rack with at most one chain hands its devices back
    static bool chainAccepts(const Device& rack, const Device& d);   // racks and Macros don't go inside racks
    bool   acceptsDevice(int track, DeviceKind kind) const;   // returns and audio tracks: audio effects only

    // ---- sidechains (Device::sidechainTrack)
    // Can device `deviceId` on track `trackIndex` take its key from track `sourceId` (0: its own
    // input)? Sources are regular, non-group tracks other than the device's own, and the
    // routing must stay free of cycles (a key from a track that is itself keyed from this one).
    bool   canKeyFrom(int trackIndex, int deviceId, int sourceId, QString* why = nullptr) const;
    bool   setKeySource(int trackIndex, int deviceId, int sourceId);   // one undo step; false (nothing changes) when it can't
    // Does track `from` (by id) reach track `to` through sidechain keys (to is keyed from
    // from, or from something keyed from from, …)? Rendering must then do `from` first.
    bool   keyReaches(int fromId, int toId) const;

    // ---- tracks and scenes (each one undo step)
    void addTrack(const QString& name, const QColor& color);   // demo / tests: no undo step
    int  insertTrack(int at, TrackType type = TrackType::Midi);   // a new track at index `at` (0..tracks.size()); returns its index
    // A MIDI track becomes an audio track, or back (0.21: a MIDI device dropped on an audio track,
    // an audio file on a MIDI track): only while the track holds no clips and, going to audio, no
    // instrument or MIDI effects; `why` says what's in the way. One undo step.
    bool canConvertTrack(int index, TrackType to, QString* why = nullptr) const;
    bool convertTrack(int index, TrackType to, QString* why = nullptr);
    int  insertReturn();                     // a new return track at the end; returns its combined index
    // An audio track for a guitar or bass: Input 1, monitored, with a Tuner and a Guitar Amp (one undo step).
    int  insertGuitarTrack(int at);
    int  duplicateTrack(int index);          // regular tracks; clips are copied
    void deleteTrack(int index);             // tracks or returns (combined index)
    bool moveTrack(int from, int to);        // regular tracks only
    void renameTrack(int index, const QString& name);
    void setTrackColor(int index, const QColor& c);
    int  insertScene(int at);
    bool moveScene(int from, int to);        // its clips move with it, in every track
    int  duplicateScene(int index);
    void deleteScene(int index);
    void renameScene(int index, const QString& name);
    int  captureScene(int after);            // copies the playing clips into a new scene after `after`
    static const QVector<QColor>& palette();          // the colour grid (0.21): kPaletteColumns hues a row, vivid, pastel, soft, deep and muted rows, then greys
    static constexpr int kPaletteColumns = 14;

    // ---- automation helpers
    QStringList automatableTargets(int trackIndex) const;
    QString     targetName(int trackIndex, const QString& target) const;
    double      targetValue(int trackIndex, const QString& target) const;   // the static (unautomated) value, 0..1
    void        setTargetValue(int trackIndex, const QString& target, double v);
    // Call when the user moves a control by hand: if it's automated, that envelope is overridden.
    void        touchTarget(int trackIndex, const QString& target);
    // The clip playing on a track at `beat` (its Session clip, else the Arrangement's) and the
    // clip beat it's at, looped; nullptr when none is.
    ClipPtr     playingClip(int trackIndex, double beat, double* local = nullptr) const;
    double      clipPosition(const ClipPtr& clip, double beat) const;   // the clip's timeline position under the playhead (when its track plays it), or -1
    // What sets a control right now instead of its own value, or -1: a Macros knob mapped to it,
    // else the playing clip's envelope (while `playing`), else the track's automation. The
    // engine applies them in the same order.
    double      drivenValue(int trackIndex, const QString& target, double beat, bool playing) const;
    // The Macros mapping that turns a device's parameter (and that Macros device's id), if any.
    const MacroMap* macroFor(int trackIndex, int deviceId, int param, int* macrosDeviceId = nullptr) const;
    bool        anyAutomationOverridden() const;
    void        reEnableAutomation();

    bool anySolo() const;
    void setTempo(double bpm);   // re-matches warped audio clips to the new tempo (tempo::octaveFor)
    // Fits an audio clip to the set's tempo again from its audio: finds the tempo when it isn't
    // known yet, then sets the clip's length, loop and warp markers (one undo step). False when
    // the clip has no audio.
    bool syncClipToTempo(const ClipPtr& clip);
    // Every audio clip in the set (slots and the Arrangement), each once.
    QVector<ClipPtr> audioClips() const;

    // ---- undo
    QUndoStack* undoStack() const { return m_undo; }
    void beginEdit(const QString& label, quintptr mergeKey = 0);   // nests; the outermost pair records
    void endEdit();
    bool editing() const { return m_editDepth > 0; }
    EditState captureEditState() const;
    void      restoreEditState(const EditState& s);

signals:
    void structureChanged();   // tracks / scenes / slots / devices / clips added or removed
    void clipChanged();        // notes, names, lengths, launch settings
    void mixerChanged();       // volume, pan, sends, mute, solo, arm
    void deviceChanged();      // device knobs or on/off
    void automationChanged();  // envelopes edited, overridden or re-enabled
    void tempoChanged();
    void locatorsChanged();
    void selectionChanged();
    void restored();           // undo / redo replaced data: drop raw pointers and indices into it

private:
    void resizeSends();
    void clearSet();   // empties everything and forgets every clip, before a load
    void addDefaultReturns();
    void emitAllChanged();   // after replacing the whole set
    int m_nextClipId = 1;
    int m_nextDeviceId = 1;
    int m_nextTrackId = 1;
    int m_colorIndex = 0;
    QHash<int, std::weak_ptr<Clip>> m_clipRegistry;   // lets undo revive the same Clip objects
    QUndoStack* m_undo;
    int         m_editDepth = 0;
    EditState   m_editBefore;
    QString     m_editLabel;
    quintptr    m_editKey = 0;
};

// Merge keys: edits with the same non-zero key less than a second apart become
// one undo step (knob drags, typing). Recording keys merge regardless of time.
inline quintptr clipMergeKey(int clipId, int what)      { return (quintptr(1) << 40) | (quintptr(clipId) << 4) | quintptr(what); }
inline quintptr deviceMergeKey(int deviceId, int param) { return (quintptr(2) << 40) | (quintptr(deviceId) << 8) | quintptr(param & 255); }
inline quintptr mixerMergeKey(int trackId, int what)    { return (quintptr(3) << 40) | (quintptr(trackId) << 8) | quintptr(what); }
inline quintptr recordMergeKey(int take)                { return (quintptr(4) << 40) | quintptr(take); }
inline bool     isRecordMergeKey(quintptr k)            { return (k >> 40) == 4; }

// RAII helper: one undo step for whatever happens inside the scope.
class EditScope {
public:
    EditScope(Project* p, const QString& label, quintptr mergeKey = 0) : m_p(p) { m_p->beginEdit(label, mergeKey); }
    ~EditScope() { m_p->endEdit(); }
    EditScope(const EditScope&) = delete;
    EditScope& operator=(const EditScope&) = delete;
private:
    Project* m_p;
};

} // namespace sessions
