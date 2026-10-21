#pragma once
// DeviceSpecs: every device the browser can load, as data. Each device lists its
// parameters: a name, how a knob's 0..1 position maps to real units (linear, exponential,
// whole numbers, a list of choices, on / off, the fader law), a unit for the value text
// and a default. Shared by the model (new devices and their defaults), the device panels
// (labels and value text) and the audio engine (DSP), so the number you read is the one
// you hear.
//
// Compatibility: a device's existing parameters keep their index and meaning; new ones
// are appended. Sets saved with fewer parameters get the defaults for the rest.

#include "Model.h"
#include <QString>
#include <vector>

namespace sessions {

enum class DeviceType {
    Unknown,
    // instruments
    InstrumentRack, DrumRack, DrumKit, Drum808, DrumAcoustic,
    AnalogSynth, FMSynth, WavetableSynth, Sampler, Organ, Bass, Electric, Monolith, DrumStudio,
    // audio effects
    EQ, Compressor, Saturator, AutoFilter, Delay, Reverb,
    Chorus, Phaser, Utility, AutoPan, Redux, Gate, Limiter, Macros, AudioEffectRack,
    // guitar and bass (audio effects)
    Tuner, GuitarAmp, Overdrive, Distortion, MetalDistortion, Chainsaw, MetalBoost, Fuzz, Crunch,
    Twang, PhasePedal, ChorusPedal, Wah, Nam, Cabinet,
    // MIDI effects
    Arpeggiator, Chord, Scale, Velocity, MidiEffectRack,
    Osmium,
    DrumBuss, // appended: existing device enum values remain stable
    StereoWidth,
};

// Drum Buss: fixed indices shared by the processor and compact panel.
namespace drumbuss {
enum Param { Trim, Comp, Drive, Type, Crunch, Damp, Transients, Boom, Frequency, Decay, ForceNote, BassSolo, Output, Mix, kParams };
// Round the resonator to the nearest equal-tempered MIDI note when ForceNote is enabled.
double tunedFrequency(double hz, bool forceNote);
}

// Drum Rack: Volume and Velocity, then five knobs per pad. Pad k plays on MIDI note 36 + k
// (C1 up to D#2) and holds a sample (Device::pads). A pad can also hold a chain of devices
// (Device::rack = the Drum Rack's id, Device::chain = the pad), like a rack's chain: an
// instrument there plays the pad's notes as kPlayNote (C3, moved by the pad's Transpose)
// instead of the sample, and audio effects there process the pad's sound (its sample's or its
// instrument's). Only a Drum Rack straight on a track holds chains.
namespace drumrack {
constexpr int kPads = 16, kFirstNote = 36, kPerPad = 5, kBase = 2, kPlayNote = 60;
enum PadKnob { Vol, Pan, Transpose, Decay, Choke };
constexpr int param(int pad, int knob) { return kBase + pad * kPerPad + knob; }
}

// Monolith: the big synth (after NI Massive). Three wavetable oscillators with Bend / Formant /
// Sync warping, a modulation oscillator (phase and ring), noise and feedback into two filters
// (parallel to series; Scream for growl), an insert drive, four envelopes, two LFOs, two 16-step
// sequencers and four macros. Every one of them can turn any knob through the device's
// modulation routes (Device::mods), by a share of the knob's range.
namespace monolith {
constexpr int kOscs = 3, kFilters = 2, kEnvs = 4, kLfos = 2, kSteppers = 2, kSteps = 16, kMacros = 4, kMaxRoutes = 32;
enum Global   { Volume, Glide, VoiceMode, Unison, Detune, Spread, VelAmp, KeyTrack, kGlobals };
enum OscKnob  { Table, Position, Mode, Intensity, Pitch, Fine, Level, FilterMix, kPerOsc };
enum FilterKnob { FType, FCutoff, FReso, kPerFilter };
enum EnvKnob  { A, D, S, R, kPerEnv };
enum LfoKnob  { LRate, LHz, LShape, kPerLfo };
enum StepKnob { SRate, SCount, SGlide, SStep0, kPerStepper = 3 + kSteps };
constexpr int kOscBase = kGlobals;                                   // 8
constexpr int ModPitch = kOscBase + kOscs * kPerOsc, ModPhase = ModPitch + 1, ModRing = ModPitch + 2;   // 32..34
constexpr int NoiseLevel = ModRing + 1, NoiseColor = ModRing + 2;   // 35, 36
constexpr int Feedback = NoiseColor + 1;                             // 37
constexpr int kFilterBase = Feedback + 1;                            // 38..43
constexpr int FRouting = kFilterBase + kFilters * kPerFilter, FMix = FRouting + 1;   // 44, 45
constexpr int InsertType = FMix + 1, InsertAmount = InsertType + 1; // 46, 47
constexpr int kEnvBase = InsertAmount + 1;                           // 48..63
constexpr int kLfoBase = kEnvBase + kEnvs * kPerEnv;                 // 64..69
constexpr int kStepBase = kLfoBase + kLfos * kPerLfo;                // 70..107
constexpr int kMacroBase = kStepBase + kSteppers * kPerStepper;      // 108..111
// Appended in 0.17 (older sets get the defaults): the pitch bend range, oscillator phase restart,
// per-LFO retrigger and fade-in, per-stepper retrigger.
constexpr int BendRange = kMacroBase + kMacros;                      // 112
constexpr int OscRestart = BendRange + 1;                            // 113
constexpr int kLfoRetrigBase = OscRestart + 1;                       // 114, 115
constexpr int kLfoFadeBase = kLfoRetrigBase + kLfos;                 // 116, 117
constexpr int kStepRetrigBase = kLfoFadeBase + kLfos;                // 118, 119
constexpr int kParams17 = kStepRetrigBase + kSteppers;               // 120: where 0.17 ended
// Appended in 0.20 (older sets get the defaults): the sub oscillator (an octave or two under
// oscillator 1: level, octave, wave), drive into the filters, the modulation oscillator's wave,
// each LFO's mode (Loop, or One-shot: it runs once from the note and holds, an extra envelope),
// and the FX after the voices: a Dimension chorus, a synced ping-pong delay, a reverb, an EQ.
constexpr int SubLevel = kParams17, SubOct = SubLevel + 1, SubWave = SubLevel + 2;   // 120 .. 122
constexpr int FDrive = SubWave + 1;                                  // 123
constexpr int ModOscWave = FDrive + 1;                               // 124
constexpr int kLfoModeBase = ModOscWave + 1;                         // 125, 126
constexpr int kFxBase = kLfoModeBase + kLfos;                        // 127
enum FxKnob { ChorusRate, ChorusDepth, ChorusMix, DelayTime, DelayFeedback, DelayTone, DelayMix, ReverbSize, ReverbMix, EqLow, EqHigh, kFx };
constexpr int fx(FxKnob k)                     { return kFxBase + k; }
constexpr int kParams20 = kFxBase + kFx;                             // 138: where 0.20 ended
// Appended in 0.21 (older sets get the defaults), after Massive: LFO 3 and LFO 4 (each with its
// Rate, Hz, Shape, Retrig, Fade and Mode in one block), Macros 5 .. 8, and for each envelope a
// Curve (-1: logarithmic, fast then slow; 0: as before; +1: exponential, slow then fast) and Loop
// (while the note holds, the envelope runs its attack and decay again and again).
constexpr int kLfosAll = 4, kMacrosAll = 8;
enum Lfo2Knob { L2Rate, L2Hz, L2Shape, L2Retrig, L2Fade, L2Mode, kPerLfo2 };
constexpr int kLfo2Base = kParams20;                                 // 138 .. 149
constexpr int kMacro2Base = kLfo2Base + (kLfosAll - kLfos) * kPerLfo2;   // 150 .. 153
constexpr int kEnvCurveBase = kMacro2Base + (kMacrosAll - kMacros);  // 154 .. 157
constexpr int kEnvLoopBase = kEnvCurveBase + kEnvs;                  // 158 .. 161
// Also 0.21: two drawable curves (Device::curves, a list of points each: the Curve drawer): the
// phrase tool. Each has a synced length (Rate), Loop or One-shot, Retrig (from each note, else
// with the song) and Smooth (rounds the corners). They're sources like the LFOs (SrcCurve1 / 2).
constexpr int kCurves = 2, kCurvePoints = 32;
enum CurveKnob { CRate, CMode, CRetrig, CSmooth, kPerCurve };
constexpr int kCurveBase = kEnvLoopBase + kEnvs;                     // 162 .. 169
constexpr int kParams = kCurveBase + kCurves * kPerCurve;            // 170
constexpr int cparam(int c, CurveKnob k)       { return kCurveBase + c * kPerCurve + k; }
constexpr int oparam(int osc, OscKnob k)       { return kOscBase + osc * kPerOsc + k; }
constexpr int fparam(int f, FilterKnob k)      { return kFilterBase + f * kPerFilter + k; }
constexpr int eparam(int e, EnvKnob k)         { return kEnvBase + e * kPerEnv + k; }
constexpr int ecurve(int e)                    { return kEnvCurveBase + e; }
constexpr int eloop(int e)                     { return kEnvLoopBase + e; }
// The LFO knobs, for all four: LFO 1 and 2 in their 0.13 / 0.17 / 0.20 places, LFO 3 and 4 in the 0.21 block.
constexpr int l2param(int l, Lfo2Knob k)       { return kLfo2Base + (l - kLfos) * kPerLfo2 + k; }
constexpr int lparam(int l, LfoKnob k)         { return l < kLfos ? kLfoBase + l * kPerLfo + k : l2param(l, k == LRate ? L2Rate : k == LHz ? L2Hz : L2Shape); }
constexpr int sparam(int s, StepKnob k)        { return kStepBase + s * kPerStepper + k; }
constexpr int step(int s, int i)               { return sparam(s, SStep0) + i; }
constexpr int macro(int m)                     { return m < kMacros ? kMacroBase + m : kMacro2Base + (m - kMacros); }
constexpr int lretrig(int l)                   { return l < kLfos ? kLfoRetrigBase + l : l2param(l, L2Retrig); }
constexpr int lfade(int l)                     { return l < kLfos ? kLfoFadeBase + l : l2param(l, L2Fade); }
constexpr int sretrig(int s)                   { return kStepRetrigBase + s; }
constexpr int lmode(int l)                     { return l < kLfos ? kLfoModeBase + l : l2param(l, L2Mode); }
// Modulation sources (ModRoute::source). Envelopes, macros, velocity, key, the mod wheel and
// pressure are 0..1; LFOs, steppers, Random and pitch bend swing -1..1. Wheel, Pressure and
// Bend come from the keyboard (Context::wheel / pressure / bend). LFO 3 / 4 and Macros 5 .. 8
// were appended in 0.21 (sources are never renumbered: sets refer to them by number).
enum Source { SrcEnv1, SrcEnv2, SrcEnv3, SrcEnv4, SrcLfo1, SrcLfo2, SrcStep1, SrcStep2,
              SrcMacro1, SrcMacro2, SrcMacro3, SrcMacro4, SrcVelocity, SrcKey, SrcRandom,
              SrcWheel, SrcPressure, SrcBend, SrcLfo3, SrcLfo4, SrcMacro5, SrcMacro6, SrcMacro7, SrcMacro8, SrcCurve1, SrcCurve2, kSources };
constexpr int curveOfSource(int s) { return s == SrcCurve1 || s == SrcCurve2 ? s - SrcCurve1 : -1; }
constexpr int lfoOfSource(int s)   { return s == SrcLfo1 || s == SrcLfo2 ? s - SrcLfo1 : s == SrcLfo3 || s == SrcLfo4 ? 2 + s - SrcLfo3 : -1; }
constexpr int macroOfSource(int s) { return s >= SrcMacro1 && s <= SrcMacro4 ? s - SrcMacro1 : s >= SrcMacro5 && s <= SrcMacro8 ? 4 + s - SrcMacro5 : -1; }
const char* sourceName(int source);
bool        sourceBipolar(int source);
QString     sectionName(int param);   // "Osc 1", "Filter 2", "Env 3", … for menus
}
// The modulation routes a Sound (preset) of the Monolith comes with.
const std::vector<ModRoute>& presetMods(const QString& name);

// Osmium preserves the original Monolith indices and appends its own controls.
namespace osmium {
constexpr int kCurvePoints = 257; // 256 drawing intervals plus the final endpoint.
constexpr int kEnvBendBase = monolith::kParams;
constexpr int bend(int env, int segment) { return kEnvBendBase + env * 3 + segment; }
constexpr int kLegacyParams = kEnvBendBase + monolith::kEnvs * 3; // 182, through 0.38
constexpr int kLegacyWavetableCount = 8, kWavetableCount = 48;
constexpr int kTableExtensionBase = kLegacyParams;
constexpr int tableExtension(int osc) { return kTableExtensionBase + osc; }
constexpr int kWarpExtensionBase = kTableExtensionBase + monolith::kOscs;
constexpr int warpExtension(int osc) { return kWarpExtensionBase + osc; }
constexpr int kFoldBase = kWarpExtensionBase + monolith::kOscs;
constexpr int fold(int osc) { return kFoldBase + osc; }
constexpr int kParams = kFoldBase + monolith::kOscs; // 191: fits the existing audio parameter capacity
// Extension zero retains the original normalized 8-table / 3-warp selectors,
// including their automation and modulation. New choices never renumber old ones.
int tableIndex(double legacyPosition, double extensionPosition);
int warpMode(double legacyPosition, double extensionPosition); // Bend/Formant/Sync/Phase/Fold/Asym/Steps/Cross FM
}

// Sampler (reworked in 0.32, after the classic three-mode sampler): one sample, played in one of
// three modes. Classic plays it from Root across the keyboard between Start and End, looping
// from Loop Start with a crossfade; 1-Shot plays it whole from each key (Trigger) or while the
// key is held (Gate) with fades instead of an envelope; Slice cuts it at its transients, on a
// beat grid, into equal regions or where you double-click (Device::slices, seconds) and puts the
// slices on the keys from C1 up, one at a time (Mono), together (Poly) or each playing on to
// the end (Thru). A filter with its own envelope amount, an LFO onto pitch / filter / volume,
// the amplitude envelope, and Warp: with it on, the sample follows the set's tempo from its own
// (Device::sampleBpm, found when it loads; :2 / x2 and the field correct it) through the warp
// modes of the audio clips. The 0.4 parameters keep their places (0 .. 12); the rest is appended.
namespace sampler {
enum Param {
    Tune = 0, Attack, Release, Volume, Start, End, Loop, Cutoff, Reso, Decay, Sustain, Root, Velocity,   // 0 .. 12 (0.4)
    Mode,               // 13: Classic / 1-Shot / Slice
    LoopStart,          // 14: where the loop begins (a share of the sample, kept between Start and End)
    Fade,               // 15: the loop's crossfade, a share of the loop
    Snap,               // 16: Start / End / Loop Start move to zero crossings
    Voices,             // 17: 1 .. 16
    Retrig,             // 18: a key struck again while its note still sounds restarts that voice
    Trigger,            // 19: 1-Shot: Trigger (the whole sample) / Gate (stops with Fade Out when the key goes up)
    FadeIn, FadeOut,    // 20, 21: 1-Shot
    SliceBy,            // 22: Transient / Beat / Region / Manual
    Sensitivity,        // 23: Transient: how soft a hit still makes a slice
    Division,           // 24: Beat: 1/4 .. 1/32 of the sample's own tempo
    Regions,            // 25: Region: 2 .. 64 equal parts
    Playback,           // 26: Mono / Poly / Thru
    FilterOn, FilterType, FilterEnv,                           // 27 .. 29 (the envelope's amount in octaves)
    LfoOn, LfoShape, LfoRate, LfoHz, LfoPitch, LfoFilter, LfoVolume,   // 30 .. 36
    Warp, WarpMode,     // 37, 38: Re-Pitch / Tones / Texture / Complex / Complex Pro
    Detune,             // 39: cents
    kParams
};
enum Modes { Classic, OneShot, Slice };
enum SliceModes { ByTransient, ByBeat, ByRegion, ByManual };
enum PlaybackModes { Mono, Poly, Thru };
enum WarpModes { RePitch, Tones, Texture, Complex, ComplexPro };
constexpr int kMaxSlices = 128, kFirstSliceNote = 36, kMaxVoices = 16;
// Where the slices start, in seconds into the sample, between `start` and `end` (shares of the
// sample, the Start / End parameters); the first is always `start`. Transient: the sample's
// transients whose strength passes `sensitivity` (1: every one); Beat: every `divisionBeats` of
// the sample's tempo (`bpm`, or the set's `tempo` when it has none); Region: `regions` equal parts;
// Manual: the `manual` list (sorted, seconds). At most `max` (kMaxSlices); no allocation, so the
// audio thread can call it. Returns how many.
int slices(const SampleData* s, int by, double sensitivity, double divisionBeats, int regions, double bpm, double tempo,
           const double* manual, int manualCount, double start, double end, double* out, int max);
// The strength a transient needs to make a slice at `sensitivity`.
inline double transientThreshold(double sensitivity) { const double x = 1 - std::clamp(sensitivity, 0.0, 1.0); return x * x; }
}

// Macros: eight knobs, each turning any number of parameters of the track's other devices
// between a minimum and a maximum (Device::maps).
namespace macros { constexpr int kKnobs = 8; }

// Racks (Instrument Rack, Audio Effect Rack, MIDI Effect Rack): chains of devices side by side,
// all playing at once and mixed. The devices in a chain follow the rack in the track's device
// list with Device::rack = the rack's id and Device::chain = the chain's index; Device::chains
// names the chains and Device::zones says which notes (key, velocity) and which positions of the
// Chain selector each one answers to. Eight macros (mapped like the Macros device's,
// Device::maps), then four knobs per chain, then the Chain selector (0 .. 127).
// A MIDI Effect Rack's chains merge their notes instead of mixing sound: its first two chain
// knobs are Transpose (semitones) and Velocity (a scale) instead of Volume and Pan.
namespace racks {
constexpr int kMacros = 8, kChains = 8, kPerChain = 4, kBase = 8;
enum ChainKnob { Volume, Pan, On, Solo };
enum MidiChainKnob { Transpose = Volume, VelocityScale = Pan };
constexpr int param(int chain, int knob) { return kBase + chain * kPerChain + knob; }
constexpr int ChainSelect = kBase + kChains * kPerChain;   // 40
constexpr int kParams = ChainSelect + 1;
constexpr int kZoneMax = 127;                               // zones and the selector run 0 .. 127
}
bool isRackType(DeviceType type);   // the three racks (not the Drum Rack: see Model.h holdsChains)

// Sidechains: Compressor, Gate and Auto Filter can take their detector signal (the key) from
// another track (Device::sidechainTrack). Five parameters appended to each of them: which tap
// of the source (Pre FX / Post FX / Post Mix), the key's gain, high- and low-pass (the lowest
// / highest position = off), and Listen (hear the key instead of the device's output; never
// saved on, never exported).
namespace sidechain {
enum Knob { Tap, Gain, HP, LP, Listen, kKnobs };
bool supports(DeviceType type);
int  base(DeviceType type);                                     // index of Tap, or -1
inline int param(DeviceType type, Knob k) { return base(type) + k; }
}
// Tuner, Guitar Amp and the pedals: the browser's Guitar category.
bool isGuitarType(DeviceType type);

// Guitar Amp: its heads and cabinets (knob positions are choicePosition(index, count)).
namespace guitaramp {
enum Param { Head, Gain, Bass, Mid, Treble, Presence, Volume, Cab, Mic, Output, DryWet };
constexpr int kHeads = 5, kCabs = 7, kLoadedIr = 6;
}

// NAM: a Neural Amp Modeler capture (.nam, Device::modelPath / model: the amp or pedal itself)
// with a cabinet IR slot after it (Device::samplePath / sample, like the amp's Loaded IR).
// Normalize brings the capture's loudness metadata to kNormalizeTargetDb, as the NAM plugin does.
namespace namdev {
enum Param { Input, Output, Mix, Gate, Normalize, Ir, IrMix, Quality, Bass, Mid, Treble, Presence, kParams };
constexpr double kNormalizeTargetDb = -18;
}

namespace stereowidth {
enum Param { Width, MonoBass, BassCutoff, Spread, Delay, Output, Mix, Monitor, kParams };
}

// Cabinet (0.19): an impulse response on its own, for anything (a NAM without its speaker, a
// bass DI, a guitar from another amp sim, a room on a vocal). The IR is Device::samplePath /
// sample like the amp's and NAM's; built-in ones come from the amp's own cabinets
// ("builtin:ir/<cab>/<mic>", audio/Builtin.h). Low Cut all the way down and High Cut all the way up are off.
namespace cabinet {
enum Param { Ir, Mix, Gain, LowCut, HighCut, kParams };
}
// Devices that hold a cabinet IR (Guitar Amp, NAM, Cabinet): the Impulse Responses library drops onto them.
bool holdsIr(DeviceType type);

// Drum Studio (0.19): an acoustic drum kit played like a real one, in the spirit of Superior Drummer:
// three recorded AVL kits / six recipes (kick, snare, hi-hat, three toms, ride, crash), each drum with its own
// Level / Pan / Tune / Decay / Tone, mic channels (the close mics are the drums themselves,
// Overheads a bright stereo pair, Room a reverb pair with Room Size, Bleed what the close mics
// pick up of the others), and a groove library (core/Grooves.h) in its panel. GM drum notes:
// kick 35 / 36, snare 38 / 40 (rimshot), side stick 37, clap 39, hats 42 (closed) / 44 (pedal) /
// 46 (open), toms 41 / 43 (floor), 45 / 47 (mid), 48 / 50 (high), crashes 49 / 57 / 55 (splash) /
// 52 (china), ride 51 / 59, ride bell 53.
namespace drumstudio {
enum Global   { Kit, Volume, Overheads, Room, RoomSize, Bleed, Width, Velocity, Wires, Beater, kGlobals };
enum Drum     { Kick, Snare, HiHat, Tom1, Tom2, Tom3, Ride, Crash, kDrums };
enum DrumKnob { Level, Pan, Tune, Decay, Tone, kPerDrum };
constexpr int kKits = 6;
constexpr int param(int drum, int knob) { return kGlobals + drum * kPerDrum + knob; }
constexpr int kBaseParams = kGlobals + kDrums * kPerDrum; // original 50 indices preserved
constexpr int mute(int drum) { return kBaseParams + drum; }
constexpr int solo(int drum) { return kBaseParams + kDrums + drum; }
constexpr int kLegacyParams = kBaseParams + kDrums * 2; // original 66 indices preserved
enum PieceControl { Present, Source, StageX, StageY, MicModel, MicDistance, MicAngle, kPerPiece };
constexpr int piece(int drum, int control) { return kLegacyParams + drum * kPerPiece + control; }
enum StudioControl { OhModel = kLegacyParams + kDrums * kPerPiece, OhHeight, RoomModel, RoomDistance, k040Params, ContentLibrary = k040Params };
constexpr int contentSource(int drum) { return ContentLibrary + 1 + drum; }
constexpr int recordedMic(int drum) { return ContentLibrary + 1 + kDrums + drum; }
constexpr int kParams = ContentLibrary + 1 + kDrums * 2;
constexpr int kContentPacks = 6;
inline constexpr double stageX[kDrums] = {.50, .30, .12, .40, .60, .80, .85, .17};
inline constexpr double stageY[kDrums] = {.70, .58, .47, .30, .29, .57, .23, .19};
inline constexpr double stageRadius[kDrums] = {.17, .112, .10, .095, .105, .13, .135, .12};
constexpr int kMicModels = 4; // Recording (bypass), Dynamic, Condenser, Ribbon; tonal models
const char* drumName(int drum);     // "Kick", "Snare", "Hi-Hat", "Tom 1", "Tom 2", "Floor Tom", "Ride", "Crash"
const char* kitName(int kit);       // "Studio Rock", "Vintage", "Tight Pop", "Metal", "Jazz", "Big Room"
int  drumFor(int pitch);            // which Drum a note plays (-1: none)
int  noteFor(int drum);             // the drum's main note (to audition it)
}

// Ready-made racks for the browser's Sounds: chains of devices (device names may be presets),
// their levels, and macros already mapped. A map's `device` is an index into its chain's
// devices, or -1 for the rack's own chain knob `param` (e.g. racks::param(chain, Volume)).
struct RackRecipe {
    struct Dev { const char* name; std::vector<std::pair<int, double>> params = {}; };   // knob positions to set
    // keyLo / keyHi: the chain's key zone. MIDI Effect Racks: transpose and velocity (a scale) instead of volumeDb.
    struct Chain { const char* name; std::vector<Dev> devices; double volumeDb = 0; int keyLo = 0, keyHi = 127, transpose = 0; double velocity = 1; };
    struct Map { int macro; int chain; int device; int param; double lo, hi; };
    const char* name;
    const char* rack;            // "Instrument Rack", "Audio Effect Rack" or "MIDI Effect Rack"
    std::vector<Chain> chains;
    std::vector<Map> maps;
    std::vector<std::pair<int, double>> macros;   // starting macro positions
    const char* help = "";
    bool guitar = false;                          // also listed under Guitar in the browser
};
const std::vector<RackRecipe>& rackRecipes();
const RackRecipe* rackRecipe(const QString& name);

enum class ParamKind { Linear, Exp, Int, Choice, Toggle, FaderDb };
enum class Unit { None, Hz, Seconds, Ms, Db, Percent, Semitones, Cents, Octaves, Ratio, Times, Degrees, Pan, Bits, Note, Dial };

struct ParamSpec {
    const char* name;
    ParamKind   kind = ParamKind::Linear;
    double      lo = 0, hi = 1;           // real units (Linear / Exp / Int)
    double      def = 0.5;                // the knob's default position, 0..1
    Unit        unit = Unit::Percent;
    std::vector<const char*> choices;     // Choice: labels
    std::vector<double>      values;      // Choice: what each choice means (else its index)
};

struct DeviceSpec {
    const char* name;
    DeviceType  type;
    DeviceKind  kind;
    std::vector<ParamSpec> params;
    const char* help = "";                // one or two sentences for the Info View
};

const std::vector<DeviceSpec>& deviceSpecs();         // every device and preset (call once on the UI thread first)
const DeviceSpec* deviceSpec(const QString& name);   // nullptr for unknown names
const DeviceSpec* specOf(DeviceType type);            // the device itself (not a preset)
DeviceType        deviceType(const QString& name);
int               paramCount(DeviceType type);

// Knob position (0..1) -> engine units: Hz, dB, seconds, semitones, ratios, choice values.
double  paramValue(DeviceType type, int index, double v);
// The same from the spec itself: for processors that read many knobs per block (cache specOf(type) once).
double  paramValue(const ParamSpec& p, double v);
int     choiceIndex(const ParamSpec& p, double v);
QString paramText(DeviceType type, int index, double v);
int     choiceIndex(DeviceType type, int index, double v);   // for Choice parameters
double  choicePosition(int index, int count);                  // the knob position of a choice

} // namespace sessions
