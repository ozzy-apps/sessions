#include "DeviceSpecs.h"
#include "Scales.h"
#include "OsmiumPresets.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace sessions {

namespace {

using K = DeviceKind;
using T = DeviceType;
using P = ParamKind;
using U = Unit;

// ---- parameter builders (defaults are knob positions; ne / nl turn a real default into one)

double ne(double real, double lo, double hi) { return std::log(real / lo) / std::log(hi / lo); }
double nl(double real, double lo, double hi) { return (real - lo) / (hi - lo); }

ParamSpec lin(const char* n, double lo, double hi, double def, U u) { return {n, P::Linear, lo, hi, def, u, {}, {}}; }
ParamSpec expo(const char* n, double lo, double hi, double def, U u) { return {n, P::Exp, lo, hi, def, u, {}, {}}; }
ParamSpec whole(const char* n, double lo, double hi, double def, U u) { return {n, P::Int, lo, hi, def, u, {}, {}}; }
ParamSpec pct(const char* n, double def) { return {n, P::Linear, 0, 1, def, U::Percent, {}, {}}; }
ParamSpec onOff(const char* n, bool def) { return {n, P::Toggle, 0, 1, def ? 1.0 : 0.0, U::None, {}, {}}; }
ParamSpec pick(const char* n, std::vector<const char*> labels, int def, std::vector<double> values = {}) {
    const int count = int(labels.size());
    return {n, P::Choice, 0, 0, choicePosition(def, count), U::None, std::move(labels), std::move(values)};
}
ParamSpec volume(const char* n = "Volume") { return lin(n, -24, 6, nl(0, -24, 6), U::Db); }
ParamSpec dial(const char* n, double def) { return lin(n, 0, 10, def / 10, U::Dial); }   // a 0 - 10 amp / pedal knob

// shared lists
const std::vector<const char*> kFmRatioNames = {"x0.5", "x1", "x1.5", "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x11"};
const std::vector<double>      kFmRatios     = {0.5, 1, 1.5, 2, 3, 4, 5, 6, 7, 9, 11};
const std::vector<const char*> kDelayNames   = {"1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2", "1/2 dot", "1 bar"};
const std::vector<double>      kDelayBeats   = {0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4};
const std::vector<const char*> kSyncNames    = {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32"};
const std::vector<double>      kSyncBeats    = {16, 8, 4, 2, 1, 0.5, 0.25, 0.125};
const std::vector<const char*> kWaves        = {"Saw", "Square", "Triangle", "Sine"};
const std::vector<const char*> kTables       = {"Basic", "Harmonics", "Formant", "Pulse", "Sub", "Growl", "Metallic", "Digital"};   // audio/Wavetables.cpp, in order
const std::vector<const char*> kLfoRateNames = {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "Free"};
const std::vector<double>      kLfoRateBeats = {16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 0};   // 0: free-running at the Hz knob
const std::vector<const char*> kLfoShapes    = {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random"};
const std::vector<const char*> kNoteNames    = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::vector<ParamSpec> drumParams(double kick, double snare, double hats) {
    return {expo("Kick", 0.12, 1.2, kick, U::Seconds), expo("Snare", 0.06, 0.45, snare, U::Seconds),
            expo("Hats", 0.02, 0.3, hats, U::Seconds), lin("Tune", -12, 12, 0.5, U::Semitones), volume()};
}

// Drum Rack: Volume, Velocity, then each pad's Volume, Pan, Transpose, Decay and Choke group.
// The hats (F#1, G#1, A#1) share choke group 1, like a real hi-hat.
std::vector<ParamSpec> drumRackParams() {
    static std::vector<std::string> names;   // ParamSpec keeps the name's pointer
    if (names.empty()) {
        names.reserve(drumrack::kPads * drumrack::kPerPad);
        for (int k = 0; k < drumrack::kPads; ++k) {
            const int note = drumrack::kFirstNote + k;
            const std::string pad = std::string(noteName(note)) + std::to_string(note / 12 - 2);
            for (const char* knob : {" Vol", " Pan", " Transp", " Decay", " Choke"}) names.push_back(pad + knob);
        }
    }
    std::vector<ParamSpec> p = {volume(), pct("Velocity", 1)};
    for (int k = 0; k < drumrack::kPads; ++k) {
        const auto name = [&](int knob) { return names[size_t(k * drumrack::kPerPad + knob)].c_str(); };
        const bool hat = k == 6 || k == 8 || k == 10;
        p.push_back(lin(name(0), -24, 6, nl(0, -24, 6), U::Db));
        p.push_back(lin(name(1), -1, 1, 0.5, U::Pan));
        p.push_back(whole(name(2), -24, 24, 0.5, U::Semitones));
        p.push_back(expo(name(3), 0.02, 10, 1, U::Seconds));
        p.push_back(pick(name(4), {"None", "1", "2", "3", "4"}, hat ? 1 : 0));
    }
    return p;
}

// Drum Studio (see drumstudio:: in DeviceSpecs.h): the globals, then five knobs per drum.
std::vector<ParamSpec> drumStudioParams() {
    using namespace drumstudio;
    static std::vector<std::string> names;   // ParamSpec keeps the name's pointer
    if (names.empty())
        for (int d = 0; d < kDrums; ++d)
            for (const char* knob : {" Level", " Pan", " Tune", " Decay", " Tone"}) names.push_back(std::string(drumName(d)) + knob);
    std::vector<ParamSpec> p = {
        pick("Kit", {kitName(0), kitName(1), kitName(2), kitName(3), kitName(4), kitName(5)}, 0),
        volume(), lin("Overheads", -60, 6, nl(-6, -60, 6), U::Db), lin("Room", -60, 6, nl(-18, -60, 6), U::Db), pct("Room Size", 0.45),
        pct("Bleed", 0.25), pct("Width", 0.8), pct("Velocity", 0.6), pct("Wires", 0.6), pct("Beater", 0.5)};
    for (int d = 0; d < kDrums; ++d) {
        const auto name = [&](int knob) { return names[size_t(d * kPerDrum + knob)].c_str(); };
        p.push_back(lin(name(0), -24, 6, nl(0, -24, 6), U::Db));
        p.push_back(lin(name(1), -1, 1, 0.5, U::Pan));
        p.push_back(lin(name(2), -12, 12, 0.5, U::Semitones));
        p.push_back(expo(name(3), 0.25, 4, 0.5, U::Times));
        p.push_back(pct(name(4), 0.5));
    }
    return p;
}

// Monolith (see monolith:: in DeviceSpecs.h): the knobs in index order.
std::vector<ParamSpec> monolithParams() {
    using namespace monolith;
    static std::vector<std::string> names;   // ParamSpec keeps the name's pointer
    auto nm = [](const std::string& s) { names.push_back(s); return names.back().c_str(); };
    if (names.empty()) names.reserve(size_t(kParams));
    std::vector<ParamSpec> p = {
        volume(), expo("Glide", 0.001, 2, 0, U::Seconds), pick("Voice", {"Poly", "Mono", "Legato"}, 0),
        pick("Unison", {"1", "2", "3", "4"}, 0, {1, 2, 3, 4}), lin("Detune", 0, 50, 0.3, U::Cents), pct("Spread", 0.6),
        pct("Vel > Amp", 0.5), pct("Key > Cutoff", 0)};
    for (int o = 0; o < kOscs; ++o) {
        const std::string n = "Osc" + std::to_string(o + 1) + " ";
        p.push_back(pick(nm(n + "Table"), kTables, o == 1 ? 4 : 0));
        p.push_back(pct(nm(n + "Pos"), o == 0 ? 0.66 : 0.2));
        p.push_back(pick(nm(n + "Mode"), {"Bend", "Formant", "Sync"}, 0));
        p.push_back(pct(nm(n + "Intensity"), 0));
        p.push_back(whole(nm(n + "Pitch"), -36, 36, o == 1 ? nl(-12, -36, 36) : 0.5, U::Semitones));
        p.push_back(lin(nm(n + "Fine"), -100, 100, 0.5, U::Cents));
        p.push_back(pct(nm(n + "Level"), o == 0 ? 0.8 : o == 1 ? 0.5 : 0.0));
        p.push_back(pct(nm(n + "Filter"), 0));
    }
    p.push_back(whole("Mod Pitch", -36, 36, 0.5, U::Semitones));
    p.push_back(pct("Mod Phase", 0));
    p.push_back(pct("Mod Ring", 0));
    p.push_back(pct("Noise", 0));
    p.push_back(pct("Noise Color", 0.5));
    p.push_back(pct("Feedback", 0));
    for (int f = 0; f < kFilters; ++f) {
        const std::string n = "F" + std::to_string(f + 1) + " ";
        p.push_back(pick(nm(n + "Type"), {"LP 12", "LP 24", "HP", "BP", "Notch", "Comb", "Scream"}, f == 0 ? 1 : 0));
        p.push_back(expo(nm(n + "Cutoff"), 30, 18000, ne(f == 0 ? 800 : 2000, 30, 18000), U::Hz));
        p.push_back(pct(nm(n + "Reso"), f == 0 ? 0.3 : 0.2));
    }
    p.push_back(pct("Routing", 0));
    p.push_back(pct("F Mix", 0));
    p.push_back(pick("Insert", {"Off", "Tanh", "Soft", "Hard", "Fold", "Bits"}, 0));
    p.push_back(pct("Insert Amt", 0.3));
    for (int e = 0; e < kEnvs; ++e) {
        const std::string n = "Env" + std::to_string(e + 1) + " ";
        p.push_back(expo(nm(n + "A"), 0.001, 5, ne(e == 0 ? 0.003 : 0.004, 0.001, 5), U::Seconds));
        p.push_back(expo(nm(n + "D"), 0.005, 8, ne(e == 0 ? 0.5 : 0.3, 0.005, 8), U::Seconds));
        p.push_back(pct(nm(n + "S"), e == 0 ? 0.8 : 0.0));
        p.push_back(expo(nm(n + "R"), 0.005, 8, ne(e == 0 ? 0.15 : 0.2, 0.005, 8), U::Seconds));
    }
    for (int l = 0; l < kLfos; ++l) {
        const std::string n = "LFO" + std::to_string(l + 1) + " ";
        p.push_back(pick(nm(n + "Rate"), kLfoRateNames, l == 0 ? 5 : 4, kLfoRateBeats));
        p.push_back(expo(nm(n + "Hz"), 0.05, 30, ne(2, 0.05, 30), U::Hz));
        p.push_back(pick(nm(n + "Shape"), kLfoShapes, l == 0 ? 0 : 1));
    }
    for (int st = 0; st < kSteppers; ++st) {
        const std::string n = "Step" + std::to_string(st + 1) + " ";
        p.push_back(pick(nm(n + "Rate"), kSyncNames, st == 0 ? 6 : 5, kSyncBeats));
        p.push_back(whole(nm(n + "Steps"), 1, 16, 1, U::None));
        p.push_back(pct(nm(n + "Glide"), 0));
        for (int i = 0; i < kSteps; ++i) p.push_back(lin(nm(n + (i < 9 ? "0" : "") + std::to_string(i + 1)), -1, 1, 0.5, U::Percent));
    }
    for (int m = 0; m < kMacros; ++m) p.push_back(pct(nm("Macro " + std::to_string(m + 1)), 0));
    // appended in 0.17
    p.push_back(whole("Bend Range", 0, 24, nl(2, 0, 24), U::Semitones));
    p.push_back(onOff("Restart", false));
    for (int l = 0; l < kLfos; ++l) p.push_back(onOff(nm("LFO" + std::to_string(l + 1) + " Retrig"), false));
    for (int l = 0; l < kLfos; ++l) p.push_back(expo(nm("LFO" + std::to_string(l + 1) + " Fade"), 0.001, 5, 0, U::Seconds));
    for (int st = 0; st < kSteppers; ++st) p.push_back(onOff(nm("Step" + std::to_string(st + 1) + " Retrig"), false));
    // appended in 0.20
    p.push_back(pct("Sub Level", 0));
    p.push_back(pick("Sub Oct", {"-1", "-2"}, 0, {1, 2}));
    p.push_back(pick("Sub Wave", {"Sine", "Square", "Triangle"}, 0));
    p.push_back(pct("F Drive", 0));
    p.push_back(pick("Mod Wave", {"Sine", "Triangle", "Saw", "Square"}, 0));
    for (int l = 0; l < kLfos; ++l) p.push_back(pick(nm("LFO" + std::to_string(l + 1) + " Mode"), {"Loop", "One-shot"}, 0));
    p.push_back(expo("Chorus Rate", 0.05, 8, ne(0.6, 0.05, 8), U::Hz));
    p.push_back(pct("Chorus Depth", 0.5));
    p.push_back(pct("Chorus Mix", 0));
    p.push_back(pick("Delay Time", kDelayNames, 2, kDelayBeats));
    p.push_back(pct("Delay Feedback", 0.35));
    p.push_back(expo("Delay Tone", 500, 12000, ne(4000, 500, 12000), U::Hz));
    p.push_back(pct("Delay Mix", 0));
    p.push_back(pct("Reverb Size", 0.5));
    p.push_back(pct("Reverb Mix", 0));
    p.push_back(lin("EQ Low", -12, 12, 0.5, U::Db));
    p.push_back(lin("EQ High", -12, 12, 0.5, U::Db));
    // appended in 0.21: LFO 3 and 4, Macros 5 .. 8, the envelopes' Curve and Loop
    for (int l = kLfos; l < kLfosAll; ++l) {
        const std::string n = "LFO" + std::to_string(l + 1) + " ";
        p.push_back(pick(nm(n + "Rate"), kLfoRateNames, l == 2 ? 6 : 3, kLfoRateBeats));
        p.push_back(expo(nm(n + "Hz"), 0.05, 30, ne(2, 0.05, 30), U::Hz));
        p.push_back(pick(nm(n + "Shape"), kLfoShapes, l == 2 ? 2 : 0));
        p.push_back(onOff(nm(n + "Retrig"), false));
        p.push_back(expo(nm(n + "Fade"), 0.001, 5, 0, U::Seconds));
        p.push_back(pick(nm(n + "Mode"), {"Loop", "One-shot"}, 0));
    }
    for (int m = kMacros; m < kMacrosAll; ++m) p.push_back(pct(nm("Macro " + std::to_string(m + 1)), 0));
    for (int e = 0; e < kEnvs; ++e) p.push_back(lin(nm("Env" + std::to_string(e + 1) + " Curve"), -1, 1, 0.5, U::Percent));
    for (int e = 0; e < kEnvs; ++e) p.push_back(onOff(nm("Env" + std::to_string(e + 1) + " Loop"), false));
    for (int c = 0; c < kCurves; ++c) {   // the drawable curves (0.21)
        const std::string n = "Curve" + std::to_string(c + 1) + " ";
        p.push_back(pick(nm(n + "Rate"), kLfoRateNames, 2, kLfoRateBeats));   // 1 bar
        p.push_back(pick(nm(n + "Mode"), {"Loop", "One-shot"}, 0));
        p.push_back(onOff(nm(n + "Retrig"), false));
        p.push_back(pct(nm(n + "Smooth"), 0));
    }
    return p;
}

std::vector<ParamSpec> macroParams() {
    return {pct("Macro 1", 0), pct("Macro 2", 0), pct("Macro 3", 0), pct("Macro 4", 0),
            pct("Macro 5", 0), pct("Macro 6", 0), pct("Macro 7", 0), pct("Macro 8", 0)};
}

// Racks: eight macros, four knobs per chain, then the Chain selector. A MIDI Effect Rack's
// chains have Transpose and Velocity where the others have Volume and Pan.
std::vector<ParamSpec> rackParams(bool midi = false) {
    static std::vector<std::string> names[2];   // ParamSpec keeps the name's pointer
    std::vector<std::string>& nm = names[midi ? 1 : 0];
    if (nm.empty()) {
        for (int c = 0; c < racks::kChains; ++c)
            for (const char* knob : {midi ? " Transpose" : " Volume", midi ? " Velocity" : " Pan", " On", " Solo"}) nm.push_back("Chain " + std::to_string(c + 1) + knob);
    }
    std::vector<ParamSpec> p = macroParams();
    for (int c = 0; c < racks::kChains; ++c) {
        const auto name = [&](int knob) { return nm[size_t(c * racks::kPerChain + knob)].c_str(); };
        if (midi) {
            p.push_back(whole(name(racks::Transpose), -48, 48, 0.5, U::Semitones));
            p.push_back(lin(name(racks::VelocityScale), 0, 2, 0.5, U::Percent));
        } else {
            p.push_back({name(racks::Volume), P::FaderDb, 0, 1, 0.75, U::Db, {}, {}});
            p.push_back(lin(name(racks::Pan), -1, 1, 0.5, U::Pan));
        }
        p.push_back(onOff(name(racks::On), true));
        p.push_back(onOff(name(racks::Solo), false));
    }
    p.push_back(whole("Chain", 0, racks::kZoneMax, 0, U::None));   // racks::ChainSelect
    return p;
}

std::vector<const char*> scaleNames() {
    std::vector<const char*> n;
    for (const ScaleDef& s : kScales) n.push_back(s.name);
    return n;
}

struct Preset { const char* name; const char* base; std::vector<std::pair<int, double>> values; std::vector<ModRoute> mods = {}; };

// The Monolith's Sounds: knob values and modulation routes.
std::vector<Preset> monolithPresets() {
    using namespace monolith;
    auto tbl = [](int i) { return choicePosition(i, int(kTables.size())); };
    auto st = [](double semis) { return nl(semis, -36, 36); };
    auto cut = [](double hz) { return ne(hz, 30, 18000); };
    auto sec = [](double s, double lo, double hi) { return ne(s, lo, hi); };
    const double legato = choicePosition(2, 3), mono = choicePosition(1, 3);
    const double scream = choicePosition(6, 7), lp24 = choicePosition(1, 7), lp12 = 0, bp = choicePosition(3, 7);
    const double tanhIns = choicePosition(1, 6), softIns = choicePosition(2, 6), hardIns = choicePosition(3, 6), foldIns = choicePosition(4, 6);
    auto lfoRate = [](int i) { return choicePosition(i, int(kLfoRateNames.size())); };
    auto stepRate = [](int i) { return choicePosition(i, int(kSyncNames.size())); };
    auto steps = [](int n) { return nl(n, 1, 16); };
    auto sv = [](double v) { return 0.5 + 0.5 * v; };   // a step's value, -1..1
    return {
        {"Growl Bass", "Monolith", {
            {oparam(0, Table), tbl(5)}, {oparam(0, Position), 0.35}, {oparam(0, Intensity), 0.3}, {oparam(0, Level), 0.85},
            {oparam(1, Table), tbl(4)}, {oparam(1, Position), 0.2}, {oparam(1, Level), 0.6}, {oparam(1, Pitch), st(-12)},
            {fparam(0, FType), scream}, {fparam(0, FCutoff), cut(260)}, {fparam(0, FReso), 0.55},
            {InsertType, tanhIns}, {InsertAmount, 0.35}, {Feedback, 0.15},
            {VoiceMode, legato}, {Glide, sec(0.05, 0.001, 2)},
            {eparam(0, D), sec(0.4, 0.005, 8)}, {eparam(0, S), 0.85}, {eparam(0, R), sec(0.12, 0.005, 8)},
            {eparam(1, D), sec(0.25, 0.005, 8)}, {eparam(1, S), 0.1},
            {lparam(0, LRate), lfoRate(5)}},
         {{SrcLfo1, oparam(0, Position), 0.35}, {SrcLfo1, fparam(0, FCutoff), 0.2}, {SrcEnv2, fparam(0, FCutoff), 0.35},
          {SrcMacro1, fparam(0, FCutoff), 0.3}, {SrcMacro2, oparam(0, Intensity), 0.5}}},
        {"Wobble Sub", "Monolith", {
            {oparam(0, Table), tbl(4)}, {oparam(0, Position), 0.5}, {oparam(0, Level), 0.9},
            {oparam(1, Table), tbl(0)}, {oparam(1, Position), 0.66}, {oparam(1, Level), 0.5}, {oparam(1, Pitch), st(0)},
            {fparam(0, FType), lp24}, {fparam(0, FCutoff), cut(150)}, {fparam(0, FReso), 0.45},
            {InsertType, tanhIns}, {InsertAmount, 0.25}, {VoiceMode, mono}, {eparam(0, S), 0.9},
            {lparam(0, LRate), lfoRate(4)}, {lparam(0, LShape), choicePosition(1, 6)}},
         {{SrcLfo1, fparam(0, FCutoff), 0.45}, {SrcMacro1, fparam(0, FCutoff), 0.3}, {SrcMacro2, oparam(1, Level), 0.5}}},
        {"Reese Monolith", "Monolith", {
            {oparam(0, Table), tbl(0)}, {oparam(0, Position), 0.66}, {oparam(0, Level), 0.7}, {oparam(0, Fine), nl(9, -100, 100)},
            {oparam(1, Table), tbl(0)}, {oparam(1, Position), 0.66}, {oparam(1, Level), 0.7}, {oparam(1, Fine), nl(-9, -100, 100)}, {oparam(1, Pitch), st(0)},
            {oparam(2, Table), tbl(4)}, {oparam(2, Position), 0}, {oparam(2, Level), 0.5}, {oparam(2, Pitch), st(-12)},
            {Unison, choicePosition(1, 4)}, {Detune, nl(18, 0, 50)}, {Spread, 0.8},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(900)}, {fparam(0, FReso), 0.15},
            {InsertType, tanhIns}, {InsertAmount, 0.2}, {eparam(1, D), sec(0.6, 0.005, 8)}, {eparam(1, S), 0.3}},
         {{SrcEnv2, fparam(0, FCutoff), 0.25}, {SrcMacro1, fparam(0, FCutoff), 0.35}}},
        {"Neuro Step", "Monolith", {
            {oparam(0, Table), tbl(6)}, {oparam(0, Position), 0.4}, {oparam(0, Level), 0.8},
            {oparam(1, Table), tbl(5)}, {oparam(1, Position), 0.6}, {oparam(1, Level), 0.6}, {oparam(1, Pitch), st(-12)},
            {oparam(1, Mode), choicePosition(2, 3)}, {oparam(1, Intensity), 0.2},
            {fparam(0, FType), scream}, {fparam(0, FCutoff), cut(400)}, {fparam(0, FReso), 0.5},
            {fparam(1, FType), bp}, {fparam(1, FCutoff), cut(1200)}, {fparam(1, FReso), 0.6}, {FRouting, 0.5}, {FMix, 0.5},
            {ModPitch, st(7)}, {ModRing, 0.25}, {InsertType, foldIns}, {InsertAmount, 0.3}, {VoiceMode, mono}, {Glide, sec(0.02, 0.001, 2)},
            {sparam(0, SRate), stepRate(6)}, {sparam(0, SCount), steps(8)}, {sparam(0, SGlide), 0.2},
            {step(0, 0), sv(0.9)}, {step(0, 1), sv(-0.6)}, {step(0, 2), sv(0.4)}, {step(0, 3), sv(-0.9)},
            {step(0, 4), sv(0.7)}, {step(0, 5), sv(0.0)}, {step(0, 6), sv(-0.4)}, {step(0, 7), sv(0.95)},
            {sparam(1, SRate), stepRate(5)}, {sparam(1, SCount), steps(4)},
            {step(1, 0), sv(-1)}, {step(1, 1), sv(0.2)}, {step(1, 2), sv(0.8)}, {step(1, 3), sv(-0.3)}},
         {{SrcStep1, oparam(0, Position), 0.5}, {SrcStep1, fparam(0, FCutoff), 0.3}, {SrcStep2, oparam(1, Intensity), 0.4},
          {SrcMacro1, fparam(1, FCutoff), 0.4}}},
        {"Formant Talk", "Monolith", {
            {oparam(0, Table), tbl(2)}, {oparam(0, Position), 0.3}, {oparam(0, Mode), choicePosition(1, 3)}, {oparam(0, Intensity), 0.4}, {oparam(0, Level), 0.8},
            {oparam(1, Table), tbl(4)}, {oparam(1, Level), 0.5}, {oparam(1, Pitch), st(-12)},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(2500)}, {fparam(0, FReso), 0.2},
            {lparam(0, LRate), lfoRate(3)}, {lparam(1, LRate), lfoRate(8)}, {lparam(1, LHz), ne(0.35, 0.05, 30)}, {lparam(1, LShape), choicePosition(1, 6)},
            {VoiceMode, legato}, {InsertType, softIns}, {InsertAmount, 0.2}, {eparam(1, D), sec(0.5, 0.005, 8)}},
         {{SrcLfo1, oparam(0, Position), 0.5}, {SrcLfo2, oparam(0, Intensity), 0.3}, {SrcEnv2, oparam(0, Position), -0.3},
          {SrcMacro1, oparam(0, Intensity), 0.5}}},
        {"808 Sub", "Monolith", {
            {oparam(0, Table), tbl(4)}, {oparam(0, Position), 0}, {oparam(0, Level), 1.0}, {oparam(1, Level), 0},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(1500)}, {fparam(0, FReso), 0},
            {eparam(0, A), sec(0.002, 0.001, 5)}, {eparam(0, D), sec(1.2, 0.005, 8)}, {eparam(0, S), 0}, {eparam(0, R), sec(0.3, 0.005, 8)},
            {eparam(1, A), sec(0.001, 0.001, 5)}, {eparam(1, D), sec(0.06, 0.005, 8)}, {eparam(1, S), 0}, {eparam(1, R), sec(0.05, 0.005, 8)},
            {InsertType, tanhIns}, {InsertAmount, 0.45}, {VoiceMode, mono}},
         {{SrcEnv2, oparam(0, Pitch), 0.25}, {SrcMacro1, InsertAmount, 0.5}}},
        {"Sync Bass", "Monolith", {
            {oparam(0, Table), tbl(0)}, {oparam(0, Position), 0.66}, {oparam(0, Mode), choicePosition(2, 3)}, {oparam(0, Intensity), 0.1}, {oparam(0, Level), 0.8},
            {oparam(1, Table), tbl(4)}, {oparam(1, Level), 0.5}, {oparam(1, Pitch), st(-12)},
            {fparam(0, FType), lp24}, {fparam(0, FCutoff), cut(1800)}, {fparam(0, FReso), 0.2},
            {InsertType, hardIns}, {InsertAmount, 0.2}, {VoiceMode, mono}, {eparam(1, D), sec(0.35, 0.005, 8)}, {eparam(1, S), 0}},
         {{SrcEnv2, oparam(0, Intensity), 0.6}, {SrcMacro1, oparam(0, Intensity), 0.4}}},
        // ---- 0.20: with the sub, the filter drive, one-shot LFOs and the FX
        {"Dub Wobble", "Monolith", {
            {oparam(0, Table), tbl(5)}, {oparam(0, Position), 0.45}, {oparam(0, Level), 0.8},
            {oparam(1, Table), tbl(0)}, {oparam(1, Position), 0.66}, {oparam(1, Level), 0.45}, {oparam(1, Pitch), st(0)}, {oparam(1, Fine), nl(7, -100, 100)},
            {SubLevel, 0.7}, {FDrive, 0.3},
            {fparam(0, FType), lp24}, {fparam(0, FCutoff), cut(220)}, {fparam(0, FReso), 0.5},
            {InsertType, tanhIns}, {InsertAmount, 0.3}, {VoiceMode, mono}, {eparam(0, S), 0.9},
            {lparam(0, LRate), lfoRate(5)}, {lparam(0, LShape), choicePosition(1, 6)},
            {fx(DelayTime), choicePosition(2, 8)}, {fx(DelayFeedback), 0.3}, {fx(DelayMix), 0.15}, {fx(EqLow), nl(3, -12, 12)}},
         {{SrcLfo1, fparam(0, FCutoff), 0.5}, {SrcMacro1, fparam(0, FCutoff), 0.3}, {SrcMacro2, FDrive, 0.5}, {SrcWheel, oparam(0, Position), 0.4}}},
        {"Neuro Reese", "Monolith", {
            {oparam(0, Table), tbl(0)}, {oparam(0, Position), 0.66}, {oparam(0, Level), 0.7}, {oparam(0, Fine), nl(12, -100, 100)},
            {oparam(1, Table), tbl(6)}, {oparam(1, Position), 0.4}, {oparam(1, Level), 0.6}, {oparam(1, Fine), nl(-12, -100, 100)}, {oparam(1, Pitch), st(0)},
            {SubLevel, 0.5}, {Unison, choicePosition(1, 4)}, {Detune, nl(14, 0, 50)}, {Spread, 0.7}, {FDrive, 0.5},
            {fparam(0, FType), scream}, {fparam(0, FCutoff), cut(350)}, {fparam(0, FReso), 0.45},
            {fparam(1, FType), bp}, {fparam(1, FCutoff), cut(1500)}, {fparam(1, FReso), 0.5}, {FRouting, 0.6}, {FMix, 0.4},
            {InsertType, foldIns}, {InsertAmount, 0.25}, {eparam(1, D), sec(0.5, 0.005, 8)}, {eparam(1, S), 0.2},
            {fx(ChorusMix), 0.3}, {fx(ChorusDepth), 0.4}, {fx(EqLow), nl(2, -12, 12)}, {fx(EqHigh), nl(2, -12, 12)}},
         {{SrcEnv2, fparam(0, FCutoff), 0.3}, {SrcEnv2, fparam(1, FCutoff), 0.25}, {SrcMacro1, fparam(0, FCutoff), 0.4}, {SrcMacro2, FDrive, 0.5}}},
        {"Talking Growl", "Monolith", {
            {oparam(0, Table), tbl(5)}, {oparam(0, Position), 0.5}, {oparam(0, Mode), choicePosition(1, 3)}, {oparam(0, Intensity), 0.35}, {oparam(0, Level), 0.85},
            {oparam(1, Table), tbl(4)}, {oparam(1, Level), 0.5}, {oparam(1, Pitch), st(-12)}, {SubLevel, 0.3},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(600)}, {fparam(0, FReso), 0.3},
            {fparam(1, FType), bp}, {fparam(1, FCutoff), cut(900)}, {fparam(1, FReso), 0.7}, {FRouting, 0.8}, {FMix, 0.6},
            {VoiceMode, legato}, {Glide, sec(0.04, 0.001, 2)}, {InsertType, softIns}, {InsertAmount, 0.3},
            {lparam(0, LRate), lfoRate(4)}, {lparam(0, LShape), 0}, {lparam(1, LRate), lfoRate(6)}, {lparam(1, LShape), choicePosition(1, 6)},
            {fx(DelayTime), choicePosition(3, 8)}, {fx(DelayFeedback), 0.25}, {fx(DelayMix), 0.12}},
         {{SrcLfo1, oparam(0, Intensity), 0.35}, {SrcLfo1, fparam(1, FCutoff), 0.3}, {SrcLfo2, oparam(0, Position), 0.2},
          {SrcMacro1, oparam(0, Intensity), 0.5}, {SrcMacro2, fparam(1, FCutoff), 0.4}, {SrcPressure, oparam(0, Intensity), 0.3}}},
        {"Supersaw Lead", "Monolith", {
            {oparam(0, Table), tbl(0)}, {oparam(0, Position), 0.66}, {oparam(0, Level), 0.7},
            {oparam(1, Table), tbl(0)}, {oparam(1, Position), 0.66}, {oparam(1, Level), 0.6}, {oparam(1, Pitch), st(0)}, {oparam(1, Fine), nl(-6, -100, 100)},
            {oparam(2, Table), tbl(0)}, {oparam(2, Position), 0.66}, {oparam(2, Level), 0.5}, {oparam(2, Pitch), st(12)},
            {Unison, choicePosition(3, 4)}, {Detune, nl(28, 0, 50)}, {Spread, 1.0},
            {fparam(0, FType), choicePosition(2, 7)}, {fparam(0, FCutoff), cut(160)}, {fparam(0, FReso), 0.1},
            {fparam(1, FType), lp12}, {fparam(1, FCutoff), cut(6000)}, {FRouting, 1.0}, {FMix, 1.0},
            {eparam(0, A), sec(0.02, 0.001, 5)}, {eparam(0, R), sec(0.4, 0.005, 8)},
            {fx(ChorusMix), 0.45}, {fx(ChorusDepth), 0.6}, {fx(DelayTime), choicePosition(2, 8)}, {fx(DelayFeedback), 0.4}, {fx(DelayMix), 0.25},
            {fx(ReverbSize), 0.6}, {fx(ReverbMix), 0.25}, {fx(EqHigh), nl(3, -12, 12)}},
         {{SrcMacro1, fparam(1, FCutoff), -0.4}, {SrcMacro2, fx(DelayMix), 0.5}, {SrcWheel, Detune, 0.5}}},
        {"Glass Pad", "Monolith", {
            {oparam(0, Table), tbl(1)}, {oparam(0, Position), 0.3}, {oparam(0, Mode), choicePosition(1, 3)}, {oparam(0, Intensity), 0.15}, {oparam(0, Level), 0.6},
            {oparam(1, Table), tbl(2)}, {oparam(1, Position), 0.5}, {oparam(1, Level), 0.5}, {oparam(1, Pitch), st(12)}, {oparam(1, Fine), nl(5, -100, 100)},
            {Unison, choicePosition(2, 4)}, {Detune, nl(10, 0, 50)}, {Spread, 0.9},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(2500)}, {fparam(0, FReso), 0.1},
            {eparam(0, A), sec(0.9, 0.001, 5)}, {eparam(0, D), sec(1.0, 0.005, 8)}, {eparam(0, S), 0.8}, {eparam(0, R), sec(1.8, 0.005, 8)},
            {lparam(0, LRate), lfoRate(1)}, {lparam(1, LRate), lfoRate(8)}, {lparam(1, LHz), ne(0.2, 0.05, 30)},
            {fx(ChorusMix), 0.5}, {fx(ChorusDepth), 0.5}, {fx(ChorusRate), ne(0.3, 0.05, 8)}, {fx(ReverbSize), 0.85}, {fx(ReverbMix), 0.45}, {fx(EqHigh), nl(2, -12, 12)}},
         {{SrcLfo1, oparam(0, Position), 0.3}, {SrcLfo2, oparam(1, Position), 0.25}, {SrcMacro1, fparam(0, FCutoff), 0.3}, {SrcMacro2, fx(ReverbMix), 0.4}}},
        {"Acid Monolith", "Monolith", {
            {oparam(0, Table), tbl(0)}, {oparam(0, Position), 0.66}, {oparam(0, Level), 0.85}, {oparam(1, Level), 0},
            {fparam(0, FType), lp24}, {fparam(0, FCutoff), cut(300)}, {fparam(0, FReso), 0.72}, {FDrive, 0.35},
            {VoiceMode, legato}, {Glide, sec(0.06, 0.001, 2)}, {InsertType, tanhIns}, {InsertAmount, 0.4},
            {eparam(0, S), 0.9}, {eparam(0, R), sec(0.08, 0.005, 8)}, {eparam(1, A), sec(0.002, 0.001, 5)}, {eparam(1, D), sec(0.22, 0.005, 8)}, {eparam(1, S), 0},
            {sparam(0, SRate), stepRate(6)}, {sparam(0, SCount), steps(8)},
            {step(0, 0), sv(0.6)}, {step(0, 1), sv(-0.4)}, {step(0, 2), sv(0.2)}, {step(0, 3), sv(0.9)}, {step(0, 4), sv(-0.6)}, {step(0, 5), sv(0.4)}, {step(0, 6), sv(-0.2)}, {step(0, 7), sv(0.7)},
            {fx(DelayTime), choicePosition(2, 8)}, {fx(DelayFeedback), 0.35}, {fx(DelayMix), 0.18}},
         {{SrcEnv2, fparam(0, FCutoff), 0.45}, {SrcVelocity, fparam(0, FCutoff), 0.2}, {SrcStep1, fparam(0, FCutoff), 0.15}, {SrcMacro1, fparam(0, FCutoff), 0.4}, {SrcMacro2, fparam(0, FReso), 0.3}}},
        {"Hoover", "Monolith", {
            {oparam(0, Table), tbl(3)}, {oparam(0, Position), 0.5}, {oparam(0, Mode), choicePosition(2, 3)}, {oparam(0, Intensity), 0.25}, {oparam(0, Level), 0.7},
            {oparam(1, Table), tbl(0)}, {oparam(1, Position), 0.66}, {oparam(1, Level), 0.6}, {oparam(1, Pitch), st(-12)},
            {oparam(2, Table), tbl(0)}, {oparam(2, Position), 0.66}, {oparam(2, Level), 0.5}, {oparam(2, Pitch), st(0)}, {oparam(2, Fine), nl(15, -100, 100)},
            {Unison, choicePosition(3, 4)}, {Detune, nl(22, 0, 50)}, {Spread, 0.9},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(3500)}, {fparam(0, FReso), 0.15},
            {eparam(2, A), sec(0.001, 0.001, 5)}, {eparam(2, D), sec(0.35, 0.005, 8)}, {eparam(2, S), 0},
            {fx(ChorusMix), 0.4}, {fx(ChorusDepth), 0.6}, {fx(ReverbSize), 0.5}, {fx(ReverbMix), 0.2}},
         {{SrcEnv3, oparam(0, Pitch), -0.12}, {SrcEnv3, oparam(1, Pitch), -0.12}, {SrcEnv3, oparam(2, Pitch), -0.12}, {SrcMacro1, oparam(0, Intensity), 0.4}}},
        {"Stab", "Monolith", {
            {oparam(0, Table), tbl(6)}, {oparam(0, Position), 0.45}, {oparam(0, Level), 0.75},
            {oparam(1, Table), tbl(3)}, {oparam(1, Position), 0.4}, {oparam(1, Level), 0.5}, {oparam(1, Pitch), st(0)}, {oparam(1, Fine), nl(8, -100, 100)},
            {fparam(0, FType), lp24}, {fparam(0, FCutoff), cut(900)}, {fparam(0, FReso), 0.35}, {FDrive, 0.2},
            {eparam(0, D), sec(0.35, 0.005, 8)}, {eparam(0, S), 0}, {eparam(0, R), sec(0.3, 0.005, 8)}, {eparam(1, D), sec(0.18, 0.005, 8)}, {eparam(1, S), 0},
            {fx(DelayTime), choicePosition(3, 8)}, {fx(DelayFeedback), 0.5}, {fx(DelayMix), 0.3}, {fx(ReverbSize), 0.4}, {fx(ReverbMix), 0.2}},
         {{SrcEnv2, fparam(0, FCutoff), 0.5}, {SrcVelocity, fparam(0, FCutoff), 0.25}, {SrcMacro1, fx(DelayFeedback), 0.4}}},
        {"Deep Sub", "Monolith", {
            {oparam(0, Table), tbl(4)}, {oparam(0, Position), 0.1}, {oparam(0, Level), 0.9}, {oparam(1, Level), 0},
            {SubLevel, 0.8}, {SubWave, 0}, {FDrive, 0.15},
            {fparam(0, FType), lp12}, {fparam(0, FCutoff), cut(200)}, {fparam(0, FReso), 0.05},
            {VoiceMode, mono}, {Glide, sec(0.03, 0.001, 2)}, {OscRestart, 1},
            {eparam(0, A), sec(0.004, 0.001, 5)}, {eparam(0, S), 1.0}, {eparam(0, R), sec(0.12, 0.005, 8)},
            {InsertType, tanhIns}, {InsertAmount, 0.2}, {fx(EqLow), nl(4, -12, 12)}},
         {{SrcMacro1, fx(EqLow), 0.3}, {SrcMacro2, oparam(0, Position), 0.5}}},
    };
}

std::vector<DeviceSpec> build() {
    const auto monoParams = monolithParams();
    auto osmiumParams = monoParams;
    for (const char* name : {"Env1 Attack Bend", "Env1 Decay Bend", "Env1 Release Bend",
                             "Env2 Attack Bend", "Env2 Decay Bend", "Env2 Release Bend",
                             "Env3 Attack Bend", "Env3 Decay Bend", "Env3 Release Bend",
                             "Env4 Attack Bend", "Env4 Decay Bend", "Env4 Release Bend"})
        osmiumParams.push_back(lin(name, -1, 1, 0.5, U::None));
    std::vector<DeviceSpec> s = {
        // ---------------------------------------------------------------- instruments
        {"Instrument Rack", T::InstrumentRack, K::Instrument, rackParams(),
         "Instruments side by side: each chain has its own instrument and effects, and plays the notes in its key and velocity zones (all of them, "
         "unless you set zones). The Chain selector picks chains by their chain zones. Eight macros turn knobs inside."},
        {"Drum Rack", T::DrumRack, K::Instrument, drumRackParams(),
         "Sixteen pads, C1 to D#2, each playing a sample: drop your own on a pad. Each pad has its own volume, pan, pitch, decay and choke group. "
         "Drop an instrument on a pad and it plays that pad (at C3), drop effects on a pad and they process just that pad."},
        {"Drum Kit", T::DrumKit, K::Instrument, drumParams(0.5, 0.5, 0.4),
         "A synthesized drum kit on the General MIDI notes: kick C1, snare D1, clap D#1, hats F#1 / A#1, toms, cymbals."},
        {"808 Kit", T::Drum808, K::Instrument, drumParams(0.65, 0.4, 0.5),
         "Boomy kick, snappy snare and thin hats in the style of the classic drum machine."},
        {"Acoustic Kit", T::DrumAcoustic, K::Instrument, drumParams(0.4, 0.6, 0.45),
         "A drier, more natural kit: shorter kick, noisier snare and hats."},
        {"Drum Studio", T::DrumStudio, K::Instrument, drumStudioParams(),
         "An acoustic drum kit, played like a real one: six kits, each drum with its own level, pan, tuning, decay and tone, overhead and room mics, "
         "and a library of grooves in every style to drag into your clips. Plays the General MIDI drum notes (kick C1, snare D1, hats F#1 / A#1, toms, cymbals)."},
        {"Analog Synth", T::AnalogSynth, K::Instrument, {
            expo("Cutoff", 40, 16000, 0.4, U::Hz), pct("Reso", 0.3), lin("Filter Env", 0, 6, 0.5, U::Octaves),
            lin("Detune", 0, 50, 0.25, U::Cents),
            pick("Osc 1", kWaves, 0), pick("Osc 2", kWaves, 0), pick("Octave 2", {"-1", "0", "+1"}, 1, {-1, 0, 1}),
            pct("Mix", 0.5), pct("Noise", 0), pick("Filter", {"LP", "HP", "BP"}, 0), pct("Key Track", 0.5),
            expo("F Attack", 0.001, 5, 0, U::Seconds), expo("F Decay", 0.005, 5, ne(0.3, 0.005, 5), U::Seconds),
            pct("F Sustain", 0), expo("F Release", 0.005, 5, ne(0.2, 0.005, 5), U::Seconds),
            expo("Attack", 0.001, 5, ne(0.003, 0.001, 5), U::Seconds), expo("Decay", 0.005, 5, ne(0.6, 0.005, 5), U::Seconds),
            pct("Sustain", 0.6), expo("Release", 0.005, 5, ne(0.18, 0.005, 5), U::Seconds),
            expo("Glide", 0.001, 1, 0, U::Seconds), volume()},
         "Two oscillators and noise into a resonant filter with its own envelope: the classic subtractive synth."},
        {"FM Synth", T::FMSynth, K::Instrument, {
            pick("B Ratio", kFmRatioNames, 1, kFmRatios), pct("B Level", 0.3),
            expo("Attack", 0.001, 2, 0, U::Seconds), expo("Decay", 0.08, 6, 0.5, U::Seconds),
            pick("Algorithm", {"B > A", "C > B > A", "D > C > B > A", "B + C > A", "B + C + D > A", "B > A, D > C", "A + B + C + D"}, 0),
            pct("Sustain", 0.08), expo("Release", 0.01, 8, ne(0.4, 0.01, 8), U::Seconds),
            expo("B Decay", 0.01, 8, ne(0.41, 0.01, 8), U::Seconds), pct("B Sustain", 0.15),
            pick("C Ratio", kFmRatioNames, 3, kFmRatios), pct("C Level", 0), expo("C Decay", 0.01, 8, ne(0.5, 0.01, 8), U::Seconds),
            pick("D Ratio", kFmRatioNames, 4, kFmRatios), pct("D Level", 0), expo("D Decay", 0.01, 8, ne(0.5, 0.01, 8), U::Seconds),
            pct("Feedback", 0), pick("A Ratio", kFmRatioNames, 1, kFmRatios), volume(), pct("Vel > Mod", 0.6)},
         "Four sine operators (A to D) modulating each other in one of seven arrangements: bells, keys, basses, metallic tones."},
        {"Wavetable Synth", T::WavetableSynth, K::Instrument, {
            pct("Position", 0.3), expo("Cutoff", 80, 14000, 0.55, U::Hz), lin("Spread", 0, 35, 0.5, U::Cents),
            expo("Attack", 0.002, 3, 0.55, U::Seconds),
            pick("Table", {"Basic", "Harmonics", "Formant", "Pulse"}, 0), pct("Reso", 0.08), pick("Filter", {"LP", "HP", "BP"}, 0),
            expo("Decay", 0.01, 8, ne(0.8, 0.01, 8), U::Seconds), pct("Sustain", 0.75), expo("Release", 0.01, 8, ne(0.3, 0.01, 8), U::Seconds),
            lin("Filter Env", -6, 6, 0.5, U::Octaves), pct("Sub", 0), pick("Unison", {"1", "3", "5"}, 1, {1, 3, 5}), volume()},
         "Scans through a table of waveforms; the Position knob morphs between them. Unison voices spread in stereo."},
        {"Sampler", T::Sampler, K::Instrument, {
            lin("Tune", -24, 24, 0.5, U::Semitones), expo("Attack", 0.001, 1, 0, U::Seconds), expo("Release", 0.05, 4, 0.4, U::Seconds),
            {"Volume", P::FaderDb, 0, 1, 0.75, U::Db, {}, {}},
            pct("Start", 0), pct("End", 1), onOff("Loop", false), expo("Cutoff", 30, 20000, 1, U::Hz), pct("Reso", 0),
            expo("Decay", 0.01, 10, 1, U::Seconds), pct("Sustain", 1), whole("Root", 24, 96, nl(60, 24, 96), U::Note), pct("Velocity", 1)},
         "Plays a sample across the keyboard from its root note. Drop a sample on it; set where it starts, ends and loops."},
        {"Organ", T::Organ, K::Instrument, {
            whole("16'", 0, 8, 1, U::None), whole("5 1/3'", 0, 8, 1, U::None), whole("8'", 0, 8, 1, U::None),
            whole("4'", 0, 8, 0, U::None), whole("2 2/3'", 0, 8, 0, U::None), whole("2'", 0, 8, 0, U::None),
            whole("1 3/5'", 0, 8, 0, U::None), whole("1 1/3'", 0, 8, 0, U::None), whole("1'", 0, 8, 0, U::None),
            onOff("Percussion", false), pick("Perc Harm", {"2nd", "3rd"}, 1), pick("Perc Decay", {"Fast", "Slow"}, 0),
            pct("Click", 0.3), pick("Vibrato", {"Off", "V1", "V2", "V3", "C1", "C2", "C3"}, 0),
            pick("Rotary", {"Off", "Slow", "Fast"}, 0), pct("Drive", 0.2), volume()},
         "A tonewheel organ: pull the drawbars for each footage, add percussion, vibrato / chorus and the rotary speaker."},
        {"Bass", T::Bass, K::Instrument, {
            pick("Wave", {"Saw", "Square"}, 0), expo("Cutoff", 40, 8000, ne(400, 40, 8000), U::Hz), pct("Reso", 0.5),
            pct("Env Mod", 0.5), expo("Decay", 0.05, 2, ne(0.3, 0.05, 2), U::Seconds), pct("Accent", 0.4),
            expo("Glide", 0.001, 0.5, ne(0.06, 0.001, 0.5), U::Seconds), pct("Sub", 0.3), pct("Drive", 0.3), volume()},
         "A monophonic bass synth: notes that overlap slide into each other, hard-played notes get an accent. Ready-made: Acid Line and Square Bass in Browser → Sounds."},
        {"Osmium", T::Osmium, K::Instrument, osmiumParams,
         "Three wavetable oscillators, dual filters and a visual modulation workspace. Open Editor for draggable green modulation ranges, "
         "four envelopes with independent segment bends, four LFOs, two curved phrase editors, two step sequencers and eight macros. "
         "Select a source and click a knob's + slot, or drag a source onto it. Drag its green ring to set the range."},
        {"Monolith", T::Monolith, K::Instrument, monoParams,
         "The big synth, for meaty, growling basses and much more: three wavetable oscillators (Bend / Formant / Sync warping), a sub oscillator, a modulation oscillator, noise and "
         "feedback into two driven filters (Scream for growl), an insert drive, four envelopes, two LFOs (loop or one-shot), two step sequencers, four macros, "
         "then a chorus, a synced delay, a reverb and an EQ. The drawers beside the main panel open the envelopes, LFOs, steppers, modulation, voice and FX. "
         "Drag a source onto any knob to modulate it. Ready-made Sounds: Growl Bass, Dub Wobble, Supersaw Lead, Glass Pad … in Browser → Sounds."},
        {"Electric", T::Electric, K::Instrument, {
            pct("Tine", 0.5), pct("Hammer", 0.4), expo("Decay", 0.5, 12, ne(4, 0.5, 12), U::Seconds),
            expo("Release", 0.02, 2, ne(0.25, 0.02, 2), U::Seconds), expo("Trem Rate", 0.5, 12, ne(4, 0.5, 12), U::Hz),
            pct("Trem Depth", 0.3), pct("Drive", 0.1), volume()},
         "An electric piano: struck tines with a bell-like attack, the amp's tremolo and a little drive."},

        // ---------------------------------------------------------------- audio effects
        {"EQ", T::EQ, K::AudioEffect, {
            lin("Low", -15, 15, 0.5, U::Db), lin("Mid", -15, 15, 0.5, U::Db), lin("High", -15, 15, 0.5, U::Db),
            lin("Output", -12, 12, 0.5, U::Db),
            expo("Low Freq", 30, 1000, ne(120, 30, 1000), U::Hz), expo("Mid Freq", 100, 12000, ne(1000, 100, 12000), U::Hz),
            expo("Mid Q", 0.3, 8, ne(0.7, 0.3, 8), U::None), expo("High Freq", 1000, 18000, ne(6000, 1000, 18000), U::Hz),
            expo("Low Cut", 20, 2000, 0, U::Hz), expo("High Cut", 1000, 20000, 1, U::Hz)},
         "A low shelf, a bell and a high shelf, plus low and high cut. Drag the dots in the display."},
        {"Compressor", T::Compressor, K::AudioEffect, {
            lin("Threshold", -40, 0, 0.65, U::Db), expo("Ratio", 1, 20, 0.35, U::Ratio), expo("Attack", 0.1, 100, 0.35, U::Ms),
            expo("Release", 10, 1000, 0.45, U::Ms), lin("Knee", 0, 18, nl(6, 0, 18), U::Db), onOff("Makeup", true),
            lin("Output", -24, 24, 0.5, U::Db), pct("Dry/Wet", 1),
            pick("Key Tap", {"Pre FX", "Post FX", "Post Mix"}, 1), lin("Key Gain", -24, 24, 0.5, U::Db),
            expo("Key HP", 20, 2000, 0, U::Hz), expo("Key LP", 1000, 20000, 1, U::Hz), onOff("Listen", false)},
         "Turns down whatever goes over the threshold, by the ratio. The meter shows how much it's taking off. "
         "Sidechain: key it from another track (a kick ducking the bass), shape the key, Listen to it."},
        {"Saturator", T::Saturator, K::AudioEffect, {
            lin("Drive", 0, 24, 0.3, U::Db), expo("Tone", 800, 20000, 0.6, U::Hz), pct("Dry/Wet", 0.6), lin("Output", -12, 12, 0.5, U::Db),
            pick("Type", {"Analog", "Soft Sine", "Hard Clip", "Fold", "Digital"}, 0)},
         "Warmth to grit: drives the signal into a waveshaper. The display shows the curve."},
        {"Auto Filter", T::AutoFilter, K::AudioEffect, {
            expo("Freq", 30, 18000, 0.6, U::Hz), pct("Reso", 0.3), pct("LFO", 0.3), pct("Dry/Wet", 1),
            pick("Filter", {"LP", "HP", "BP", "Notch"}, 0), pick("Rate", kSyncNames, 2, kSyncBeats),
            pick("Shape", {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random"}, 0), lin("Env", -1, 1, 0.5, U::Percent),
            pick("Key Tap", {"Pre FX", "Post FX", "Post Mix"}, 1), lin("Key Gain", -24, 24, 0.5, U::Db),
            expo("Key HP", 20, 2000, 0, U::Hz), expo("Key LP", 1000, 20000, 1, U::Hz), onOff("Listen", false)},
         "A resonant filter that moves: an LFO in time with the song, or a level (Env): the input's own, or another track's through the sidechain."},
        {"Delay", T::Delay, K::AudioEffect, {
            pick("Time", kDelayNames, 2, kDelayBeats), lin("Feedback", 0, 0.95, 0.35, U::Percent), expo("Filter", 300, 12000, 0.6, U::Hz),
            pct("Dry/Wet", 0.25), onOff("Ping Pong", true), onOff("Link", true), pick("Time R", kDelayNames, 2, kDelayBeats),
            expo("Low Cut", 20, 2000, ne(120, 20, 2000), U::Hz)},
         "Echoes in time with the song, bouncing left and right in Ping Pong. The repeats get darker through the filter."},
        {"Reverb", T::Reverb, K::AudioEffect, {
            pct("Size", 0.5), pct("Decay", 0.5), pct("Damping", 0.5), pct("Dry/Wet", 0.25),
            expo("Pre-Delay", 0.5, 250, 0, U::Ms), expo("Low Cut", 20, 1000, 0, U::Hz), expo("High Cut", 1000, 20000, 1, U::Hz),
            pct("Width", 1), onOff("Freeze", false)},
         "A room around the sound: size, how long it rings, how dark it gets. Freeze holds the tail forever."},
        {"Chorus", T::Chorus, K::AudioEffect, {
            expo("Rate", 0.05, 8, ne(0.6, 0.05, 8), U::Hz), pct("Depth", 0.4), lin("Feedback", 0, 0.9, 0, U::Percent), pct("Dry/Wet", 0.5),
            pick("Mode", {"Chorus", "Ensemble", "Vibrato"}, 0), expo("Delay", 1, 30, ne(8, 1, 30), U::Ms), pct("Width", 1)},
         "Thickens a sound with slightly detuned, moving copies. Ensemble uses three; Vibrato only the moving copy."},
        {"Phaser", T::Phaser, K::AudioEffect, {
            expo("Rate", 0.02, 10, ne(0.3, 0.02, 10), U::Hz), pct("Amount", 0.6), lin("Feedback", -0.95, 0.95, nl(0.5, -0.95, 0.95), U::Percent),
            pct("Dry/Wet", 0.5), pick("Mode", {"Phaser", "Flanger"}, 0), expo("Center", 100, 8000, ne(800, 100, 8000), U::Hz),
            pick("Stages", {"2", "4", "8", "12"}, 1, {2, 4, 8, 12}), lin("Spread", 0, 180, 0.5, U::Degrees)},
         "Sweeping notches through the spectrum (Phaser) or a short swept delay (Flanger)."},
        {"Utility", T::Utility, K::AudioEffect, {
            lin("Gain", -35, 35, 0.5, U::Db), lin("Pan", -1, 1, 0.5, U::Pan), lin("Width", 0, 4, 0.25, U::Percent),
            onOff("Mono", false), onOff("Invert L", false), onOff("Invert R", false), onOff("Mute", false)},
         "Level, balance and stereo width, mono, phase flips and mute: the small fixes."},
        {"Auto Pan", T::AutoPan, K::AudioEffect, {
            pct("Amount", 0.5), pick("Rate", kSyncNames, 4, kSyncBeats), pick("Shape", {"Sine", "Triangle", "Square", "Random"}, 0),
            lin("Phase", 0, 360, 0.5, U::Degrees), lin("Offset", 0, 360, 0, U::Degrees)},
         "Moves the sound between left and right in time with the song (Phase 0: tremolo)."},
        {"Redux", T::Redux, K::AudioEffect, {
            whole("Bits", 1, 16, nl(8, 1, 16), U::Bits), expo("Rate", 200, 48000, 1, U::Hz), pct("Dry/Wet", 1), lin("Output", -12, 12, 0.5, U::Db)},
         "Lo-fi: fewer bits and a lower sample rate."},
        {"Gate", T::Gate, K::AudioEffect, {
            lin("Threshold", -70, 0, nl(-40, -70, 0), U::Db), expo("Attack", 0.02, 50, ne(0.1, 0.02, 50), U::Ms),
            expo("Hold", 1, 500, ne(10, 1, 500), U::Ms), expo("Release", 1, 2000, ne(80, 1, 2000), U::Ms),
            lin("Floor", -80, 0, 0, U::Db), lin("Return", 0, 12, nl(3, 0, 12), U::Db),
            pick("Key Tap", {"Pre FX", "Post FX", "Post Mix"}, 1), lin("Key Gain", -24, 24, 0.5, U::Db),
            expo("Key HP", 20, 2000, 0, U::Hz), expo("Key LP", 1000, 20000, 1, U::Hz), onOff("Listen", false)},
         "Silences the signal below the threshold: cleans up noise, tightens drums. Keyed from another track (sidechain) it opens to that track's hits instead."},
        {"Limiter", T::Limiter, K::AudioEffect, {
            lin("Gain", 0, 24, 0, U::Db), lin("Ceiling", -12, 0, nl(-0.3, -12, 0), U::Db), expo("Release", 10, 1000, ne(100, 10, 1000), U::Ms)},
         "Keeps peaks under the ceiling, so you can turn the track up without clipping."},
        {"Audio Effect Rack", T::AudioEffectRack, K::AudioEffect, rackParams(),
         "Effects in parallel: the sound goes through every chain at once and the results are mixed. An empty chain passes the sound through dry. "
         "The Chain selector fades chains in and out by their chain zones."},
        // ---------------------------------------------------------------- guitar and bass
        {"Tuner", T::Tuner, K::AudioEffect, {
            whole("Reference", 410, 480, nl(440, 410, 480), U::Hz), onOff("Mute", false), pick("View", {"Needle", "Strobe"}, 0)},
         "Tunes a guitar, a bass or anything with a pitch: the note, how many cents off, and a trace of the last few seconds. Mute silences the track while you tune."},
        {"Guitar Amp", T::GuitarAmp, K::AudioEffect, {
            pick("Head", {"Clean", "Crunch", "Lead", "Metal", "Bass"}, 1), dial("Gain", 5), dial("Bass", 5), dial("Mid", 5),
            dial("Treble", 5), dial("Presence", 5), dial("Volume", 5),
            pick("Cab", {"Off", "1x12 Open", "2x12 Combo", "4x12 Closed", "4x12 Vintage", "8x10 Bass", "Loaded IR"}, 3),
            pick("Mic", {"On Axis", "Off Axis", "Far"}, 0), lin("Output", -24, 12, nl(0, -24, 12), U::Db), pct("Dry/Wet", 1)},
         "A guitar (or bass) amp: five heads from clean to metal, the tone stack, presence and the power amp's Volume, into a cabinet. Load your own cabinet impulse response (IR) with Load IR or by dropping a WAV on it."},
        {"Overdrive", T::Overdrive, K::AudioEffect, {dial("Drive", 5), dial("Tone", 5), dial("Level", 5), pick("Clip", {"808", "Asym"}, 0)},
         "The green screamer: the lows stay clean, the mids clip softly in the op-amp's feedback loop, and the amp after it sings. "
         "Clip: the 808's matched diodes, or a lopsided pair for even harmonics."},
        {"Distortion", T::Distortion, K::AudioEffect, {dial("Distortion", 6), dial("Filter", 4), dial("Volume", 5), pick("Clip", {"Silicon", "LED", "Germanium"}, 0)},
         "The rat: a hard-clipping box with a treble-leaning gain stage. Filter turns up to darken it; Clip picks silicon, LEDs (louder, more open) or germanium (softer, squashed)."},
        {"Metal Distortion", T::MetalDistortion, K::AudioEffect, {
            dial("Gain", 7), dial("Low", 5), dial("Mid", 5), expo("Mid Freq", 200, 4000, ne(800, 200, 4000), U::Hz), dial("High", 5.5),
            dial("Level", 5), expo("Tight", 40, 400, ne(120, 40, 400), U::Hz)},
         "High gain in a box: two clipping stages, a sweepable mid and Tight to keep palm mutes crisp."},
        {"Chainsaw", T::Chainsaw, K::AudioEffect, {dial("Gain", 8), dial("Low", 10), dial("High", 10), dial("Level", 5)},
         "The Swedish death metal pedal: everything on 10, a buzzing low end and a sawing upper mid."},
        {"Metal Boost", T::MetalBoost, K::AudioEffect, {dial("Drive", 1.5), dial("Tone", 6), dial("Level", 7.5), expo("Tight", 60, 500, ne(200, 60, 500), U::Hz)},
         "The metal player's overdrive: little drive, lots of level, the lows cut before the amp so high gain stays tight."},
        {"Fuzz", T::Fuzz, K::AudioEffect, {dial("Sustain", 7), dial("Tone", 5), dial("Volume", 5), dial("Bias", 7)},
         "Two transistors squaring the signal up: thick, woolly sustain with the big muff's scooped tone. "
         "Bias starves the second transistor: turn it down for a gated, sputtering fuzz."},
        {"Crunch", T::Crunch, K::AudioEffect, {dial("Gain", 5.5), dial("Tone", 5.5), dial("Volume", 5), onOff("Bright", false), dial("Presence", 5)},
         "A plexi in a box: two triode-like stages whose bias sags with your playing, crunchy chords at low gain, rock leads at high. "
         "Bright adds bite before the gain, Presence on top after it."},
        {"Twang", T::Twang, K::AudioEffect, {dial("Dwell", 5), dial("Tone", 5.5), dial("Mix", 3.5), dial("Boost", 2)},
         "Spring reverb and a clean treble boost: surf drip, country spank. Dwell drives the springs."},
        {"Phase Pedal", T::PhasePedal, K::AudioEffect, {expo("Speed", 0.1, 8, ne(0.6, 0.1, 8), U::Hz), onOff("Script", false), dial("Depth", 10)},
         "The orange four-stage phaser: a sweep of notches through the mids. Script is the smoother, older voicing; Depth how far the sweep goes."},
        {"Chorus Pedal", T::ChorusPedal, K::AudioEffect, {expo("Rate", 0.1, 6, ne(0.8, 0.1, 6), U::Hz), dial("Depth", 5), onOff("Stereo", true), pct("Mix", 0.5)},
         "A warm bucket-brigade chorus. Stereo spreads the two sweeps across the sides; Mix at 100 % is a vibrato."},
        {"Wah", T::Wah, K::AudioEffect, {
            pct("Pedal", 0.5), pick("Mode", {"Manual", "Auto"}, 0), dial("Sensitivity", 5), dial("Q", 5), pick("Range", {"Guitar", "Bass"}, 0)},
         "A wah: map Pedal to an expression pedal (MIDI Map), or set Mode to Auto and let your picking open it."},
        {"NAM", T::Nam, K::AudioEffect, {
            lin("Input", -24, 24, 0.5, U::Db), lin("Output", -24, 12, nl(0, -24, 12), U::Db), pct("Mix", 1), lin("Gate", -90, -30, 0, U::Db),
            onOff("Normalize", false), onOff("IR", true), pct("IR Mix", 1), pick("Quality", {"Full", "Light"}, 0)},
         "Neural Amp Modeler: plays a .nam capture of a real amp or pedal (from ToneHunt and the like) through its neural network. "
         "Drop a capture on it or press Load. Many captures are amp-only: load a cabinet IR in its IR slot."},
        {"Cabinet", T::Cabinet, K::AudioEffect, {
            onOff("IR", true), pct("Mix", 1), lin("Gain", -24, 12, nl(0, -24, 12), U::Db),
            expo("Low Cut", 20, 1000, 0, U::Hz), expo("High Cut", 1000, 20000, 1, U::Hz)},
         "A speaker cabinet (or any room) as an impulse response, on its own: for a NAM capture without its speaker, a bass DI, or a sound from elsewhere. "
         "Drop an IR from the browser's Impulse Responses (the amp's own cabinets are there to start with), or a WAV; ◀ ▶ step through your library."},
        {"Macros", T::Macros, K::AudioEffect, macroParams(),
         "Eight macro knobs. Map each to any knobs of the track's other devices, with a range: one move turns them all."},

        // ---------------------------------------------------------------- MIDI effects
        {"Arpeggiator", T::Arpeggiator, K::MidiEffect, {
            pick("Rate", {"1/32", "1/16", "1/8", "1/4", "1/2"}, 1, {0.125, 0.25, 0.5, 1, 2}),
            lin("Gate", 0.1, 2.0, 0.5, U::Percent), pick("Steps", {"1 oct", "2 oct", "3 oct", "4 oct"}, 0, {1, 2, 3, 4}),
            pick("Style", {"Up", "Down", "Up/Down", "Random", "Played"}, 0)},
         "Plays the notes you hold one after another, in time with the song."},
        {"Chord", T::Chord, K::MidiEffect, {
            whole("Shift 1", -12, 12, nl(4, -12, 12), U::Semitones), whole("Shift 2", -12, 12, nl(7, -12, 12), U::Semitones),
            whole("Shift 3", -12, 12, 0.5, U::Semitones), pct("Vel", 1),
            whole("Shift 4", -12, 12, 0.5, U::Semitones), whole("Shift 5", -12, 12, 0.5, U::Semitones)},
         "Every note you play becomes a chord: up to five extra notes, each shifted by semitones."},
        {"Scale", T::Scale, K::MidiEffect, {
            pick("Root", kNoteNames, 0), pick("Scale", scaleNames(), 0), whole("Transpose", -24, 24, 0.5, U::Semitones)},
         "Moves every note to the nearest note of the scale, so you can't play a wrong one."},
        {"Velocity", T::Velocity, K::MidiEffect, {
            pct("Out Hi", 1), pct("Out Low", 0), lin("Drive", -1, 1, 0.5, U::Percent), pct("Random", 0)},
         "Reshapes how hard notes are played: squeeze the range, bend the curve, add a little randomness."},
        {"MIDI Effect Rack", T::MidiEffectRack, K::MidiEffect, rackParams(true),
         "MIDI effects side by side: each chain gets the notes in its key and velocity zones, transposes them, runs its own MIDI effects, "
         "and the chains' notes all go on to the instrument. Split a keyboard, layer an octave, arpeggiate only the low notes."},
    };

    // Presets: a device with other defaults (the browser's Sounds).
    std::vector<Preset> presets = {
        {"Sub Bass", "Analog Synth", {{0, 0.12}, {1, 0.0}, {2, 0.1}, {3, 0.0}}},
        {"Pluck Lead", "Analog Synth", {{0, 0.3}, {1, 0.45}, {2, 0.8}, {3, 0.35}}},
        {"Glass Keys", "FM Synth", {{0, 0.5}, {1, 0.25}, {2, 0.0}, {3, 0.65}}},
        {"Warm Pad", "Wavetable Synth", {{0, 0.45}, {1, 0.4}, {2, 0.7}, {3, 0.7}}},
        {"Tonewheel Jazz", "Organ", {{0, 1}, {1, 1}, {2, 1}, {3, 0}, {9, 1}, {13, choicePosition(6, 7)}, {14, choicePosition(1, 3)}}},
        {"Rock Organ", "Organ", {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {12, 0.5}, {13, choicePosition(3, 7)}, {14, 1}, {15, 0.6}}},
        {"Acid Line", "Bass", {{1, ne(300, 40, 8000)}, {2, 0.8}, {3, 0.7}, {5, 0.7}, {8, 0.5}}},
        {"Square Bass", "Bass", {{0, 1}, {1, ne(250, 40, 8000)}, {2, 0.3}, {3, 0.35}, {7, 0.5}, {8, 0.15}}},
        {"Suitcase Keys", "Electric", {{1, 0.55}, {4, ne(5, 0.5, 12)}, {5, 0.55}, {6, 0.25}}},
        {"Tubular Bells", "FM Synth", {{0, choicePosition(4, 11)}, {1, 0.35}, {3, ne(4, 0.08, 6)}, {5, 0}, {6, ne(2, 0.01, 8)}, {7, ne(3, 0.01, 8)}, {8, 0}}},
        {"Reese Bass", "Analog Synth", {{0, ne(600, 40, 16000)}, {1, 0.15}, {2, 0.05}, {3, 0.7}, {17, 1}, {18, ne(0.15, 0.005, 5)}}},
        {"Saw Lead", "Analog Synth", {{0, ne(3000, 40, 16000)}, {1, 0.35}, {5, choicePosition(1, 4)}, {6, 1}, {7, 0.35}, {19, ne(0.05, 0.001, 1)}}},
        {"Formant Choir", "Wavetable Synth", {{0, 0.4}, {1, 0.75}, {2, 0.6}, {3, ne(0.4, 0.002, 3)}, {4, choicePosition(2, 4)}, {9, ne(1.2, 0.01, 8)}}},
        {"Hollow Lead", "Wavetable Synth", {{0, 0.5}, {1, 0.7}, {3, 0}, {4, 1}, {10, 0.5 + 2.0 / 12}, {12, 0}}},
    };
    for (const Preset& p : monolithPresets()) presets.push_back(p);
    // Ready-made racks: the rack with the recipe's name, help and macro positions (the chains
    // come from the recipe when one is added: Project::addDeviceToSelected).
    for (const RackRecipe& r : rackRecipes()) {
        for (const DeviceSpec& base : s) {
            if (QLatin1String(base.name) != QLatin1String(r.rack)) continue;
            DeviceSpec copy = base;
            copy.name = r.name;
            copy.help = r.help;
            for (const auto& [i, v] : r.macros) if (i < int(copy.params.size())) copy.params[size_t(i)].def = v;
            s.push_back(copy);
            break;
        }
    }
    for (const Preset& p : presets) {
        for (const DeviceSpec& base : s) {
            if (QLatin1String(base.name) != QLatin1String(p.base)) continue;
            DeviceSpec copy = base;
            copy.name = p.name;
            for (const auto& [i, v] : p.values) if (i < int(copy.params.size())) copy.params[size_t(i)].def = v;
            s.push_back(copy);
            break;
        }
    }
    const DeviceSpec osmiumBase = *std::find_if(s.begin(), s.end(), [](const DeviceSpec& spec) { return spec.type == T::Osmium; });
    for (const auto& patch : osmium::factoryPresets()) {
        DeviceSpec copy = osmiumBase; copy.name = patch.name; copy.help = patch.description;
        for (int i = 0; i < osmium::kParams; ++i) copy.params[size_t(i)].def = patch.values[size_t(i)];
        s.push_back(std::move(copy));
    }
    return s;
}

double expMap(double v, double lo, double hi) { return lo * std::pow(hi / lo, v); }

QString hz(double f)   {
    if (f < 10) return QString("%1 Hz").arg(f, 0, 'f', 2);
    if (f < 100) return QString("%1 Hz").arg(f, 0, 'f', 1);
    return f < 1000 ? QString("%1 Hz").arg(f, 0, 'f', 0) : QString("%1 kHz").arg(f / 1000, 0, 'f', f < 10000 ? 2 : 1);
}
QString secs(double s) { return s < 1 ? QString("%1 ms").arg(s * 1000, 0, 'f', s < 0.01 ? 1 : 0) : QString("%1 s").arg(s, 0, 'f', 2); }
QString ms(double m)   { return m < 10 ? QString("%1 ms").arg(m, 0, 'f', 1) : m < 1000 ? QString("%1 ms").arg(m, 0, 'f', 0) : QString("%1 s").arg(m / 1000, 0, 'f', 2); }
QString db(double d)   { return QString("%1%2 dB").arg(d > 0.05 ? "+" : "").arg(d, 0, 'f', 1); }
QString pct(double v)  { return QString("%1 %").arg(v * 100, 0, 'f', 0); }
QString semis(double s) { long r = std::lround(s); return r == 0 ? QString("0 st") : QString("%1%2 st").arg(r > 0 ? "+" : "").arg(r); }

} // namespace

namespace monolith {
const char* sourceName(int s) {
    static const char* names[] = {"Env 1", "Env 2", "Env 3", "Env 4", "LFO 1", "LFO 2", "Step 1", "Step 2",
                                  "Macro 1", "Macro 2", "Macro 3", "Macro 4", "Velocity", "Key", "Random",
                                  "Wheel", "Pressure", "Bend", "LFO 3", "LFO 4", "Macro 5", "Macro 6", "Macro 7", "Macro 8", "Curve 1", "Curve 2"};
    return s >= 0 && s < kSources ? names[s] : "?";
}
bool sourceBipolar(int s) { return (s >= SrcLfo1 && s <= SrcStep2) || s == SrcKey || s == SrcRandom || s == SrcBend || s == SrcLfo3 || s == SrcLfo4; }
QString sectionName(int i) {
    if (i < kGlobals) return "Voice";
    if (i < ModPitch) return QString("Osc %1").arg((i - kOscBase) / kPerOsc + 1);
    if (i <= ModRing) return "Mod Osc";
    if (i <= NoiseColor) return "Noise";
    if (i == Feedback) return "Filters";
    if (i < FRouting) return QString("Filter %1").arg((i - kFilterBase) / kPerFilter + 1);
    if (i < kEnvBase) return "Filters";
    if (i < kLfoBase) return QString("Env %1").arg((i - kEnvBase) / kPerEnv + 1);
    if (i < kStepBase) return QString("LFO %1").arg((i - kLfoBase) / kPerLfo + 1);
    if (i < kMacroBase) return QString("Step %1").arg((i - kStepBase) / kPerStepper + 1);
    if (i < BendRange) return "Macros";
    if (i <= OscRestart) return "Voice";
    if (i < kLfoFadeBase) return QString("LFO %1").arg(i - kLfoRetrigBase + 1);
    if (i < kStepRetrigBase) return QString("LFO %1").arg(i - kLfoFadeBase + 1);
    if (i < kParams17) return QString("Step %1").arg(i - kStepRetrigBase + 1);
    if (i <= SubWave) return "Sub";
    if (i == FDrive) return "Filters";
    if (i == ModOscWave) return "Mod Osc";
    if (i < kFxBase) return QString("LFO %1").arg(i - kLfoModeBase + 1);
    if (i < fx(DelayTime)) return "Chorus";
    if (i < fx(ReverbSize)) return "Delay";
    if (i < fx(EqLow)) return "Reverb";
    if (i < kParams20) return "EQ";
    if (i < kMacro2Base) return QString("LFO %1").arg(kLfos + (i - kLfo2Base) / kPerLfo2 + 1);
    if (i < kEnvCurveBase) return "Macros";
    if (i < kEnvLoopBase) return QString("Env %1").arg(i - kEnvCurveBase + 1);
    if (i < kCurveBase) return QString("Env %1").arg(i - kEnvLoopBase + 1);
    return QString("Curve %1").arg((i - kCurveBase) / kPerCurve + 1);
}
}

const std::vector<ModRoute>& presetMods(const QString& name) {
    static const std::vector<ModRoute> none;
    static const std::vector<Preset> presets = monolithPresets();
    for (const Preset& p : presets) if (name == QLatin1String(p.name)) return p.mods;
    return none;
}

bool isRackType(DeviceType type) { return type == DeviceType::InstrumentRack || type == DeviceType::AudioEffectRack || type == DeviceType::MidiEffectRack; }

namespace sidechain {
bool supports(DeviceType type) { return type == DeviceType::Compressor || type == DeviceType::Gate || type == DeviceType::AutoFilter; }
int base(DeviceType type) {
    switch (type) {
    case DeviceType::Compressor: return 8;
    case DeviceType::AutoFilter: return 8;
    case DeviceType::Gate:       return 6;
    default: return -1;
    }
}
}
bool isGuitarType(DeviceType type) { return type >= DeviceType::Tuner && type <= DeviceType::Cabinet; }
bool holdsIr(DeviceType type) { return type == DeviceType::GuitarAmp || type == DeviceType::Nam || type == DeviceType::Cabinet; }

namespace drumstudio {
const char* drumName(int drum) {
    static const char* names[kDrums] = {"Kick", "Snare", "Hi-Hat", "Tom 1", "Tom 2", "Floor Tom", "Ride", "Crash"};
    return drum >= 0 && drum < kDrums ? names[drum] : "";
}
const char* kitName(int kit) {
    static const char* names[kKits] = {"Studio Rock", "Vintage", "Tight Pop", "Metal", "Jazz", "Big Room"};
    return kit >= 0 && kit < kKits ? names[kit] : "";
}
int drumFor(int pitch) {
    switch (pitch) {
    case 35: case 36: return Kick;
    case 37: case 38: case 39: case 40: return Snare;
    case 42: case 44: case 46: return HiHat;
    case 48: case 50: return Tom1;
    case 45: case 47: return Tom2;
    case 41: case 43: return Tom3;
    case 51: case 53: case 59: return Ride;
    case 49: case 52: case 55: case 57: return Crash;
    default: return -1;
    }
}
int noteFor(int drum) {
    static const int notes[kDrums] = {36, 38, 42, 48, 45, 41, 51, 49};
    return drum >= 0 && drum < kDrums ? notes[drum] : 36;
}
}

const std::vector<RackRecipe>& rackRecipes() {
    using R = RackRecipe;
    static const std::vector<RackRecipe> r = {
        {"Layered Keys", "Instrument Rack",
         {{"Keys", {{"Electric"}}, 0}, {"Pad", {{"Warm Pad"}, {"Chorus"}}, -9}},
         {{0, 1, -1, racks::param(1, racks::Volume), 0.0, 0.85}, {1, 1, 0, 1, 0.25, 0.85}, {2, 0, 0, 5, 0.0, 0.8}},
         {{0, 0.66}, {1, 0.5}, {2, 0.3}},
         "An electric piano with a pad underneath. Macro 1 brings the pad in, 2 opens it up, 3 is the piano's tremolo."},
        {"Lead Stack", "Instrument Rack",
         {{"Lead", {{"Saw Lead"}}, 0}, {"Sub", {{"Sub Bass"}}, -4}},
         {{0, 1, -1, racks::param(1, racks::Volume), 0.0, 0.85}, {1, 0, 0, 0, 0.15, 0.85}},
         {{0, 0.75}, {1, 0.55}},
         "A saw lead on top of a sine sub. Macro 1 is the sub's level, 2 the lead's brightness."},
        {"Parallel Crush", "Audio Effect Rack",
         {{"Dry", {}, 0}, {"Crushed", {{"Redux"}, {"Saturator", {{2, 1.0}}}}, -6}},
         {{0, 1, -1, racks::param(1, racks::Volume), 0.0, 0.85}, {1, 1, 0, 0, 0.5, 0.15}},
         {{0, 0.6}, {1, 0.4}},
         "The dry sound with a crushed copy mixed in. Macro 1 is how much crush, 2 how few bits."},
        {"Wide Space", "Audio Effect Rack",
         {{"Dry", {}, 0}, {"Echo", {{"Delay", {{3, 1.0}}}}, -6}, {"Hall", {{"Reverb", {{3, 1.0}, {0, 0.75}, {1, 0.7}}}}, -6}},
         {{0, 1, -1, racks::param(1, racks::Volume), 0.0, 0.85}, {1, 2, -1, racks::param(2, racks::Volume), 0.0, 0.85}},
         {{0, 0.6}, {1, 0.6}},
         "Echoes and a hall on their own chains next to the dry sound. Macro 1 is the echo level, 2 the hall."},
        {"Bass and Keys Split", "Instrument Rack",
         {{"Bass", {{"Sub Bass"}}, -2, 0, 54}, {"Keys", {{"Electric"}}, 0, 55, 127}},
         {{0, 0, -1, racks::param(0, racks::Volume), 0.0, 0.85}, {1, 1, -1, racks::param(1, racks::Volume), 0.0, 0.85}},
         {{0, 0.82}, {1, 0.88}},
         "A keyboard split at G3: a sub bass below, an electric piano from G3 up (key zones). Macro 1 is the bass level, 2 the keys."},
        // MIDI Effect Racks: notes into chains, chains' notes merged
        {"Octave Layers", "MIDI Effect Rack",
         {{"Played", {}}, {"Octave Up", {}, 0, 0, 127, 12, 0.7}, {"Octave Down", {}, 0, 0, 127, -12, 0.8}},
         {{0, 1, -1, racks::param(1, racks::On), 0.0, 1.0}, {1, 2, -1, racks::param(2, racks::On), 0.0, 1.0}},
         {{0, 1.0}, {1, 0.0}},
         "Every note also plays an octave up (softer) and, with Macro 2, an octave down. Put it before any instrument."},
        {"Split Arp", "MIDI Effect Rack",
         {{"Arp Low", {{"Arpeggiator"}}, 0, 0, 59}, {"Played High", {}, 0, 60, 127}},
         {{0, 0, 0, 0, 0.0, 1.0}, {1, 0, 0, 2, 0.0, 1.0}},
         {{0, 0.25}, {1, 0.0}},
         "Notes below C3 are arpeggiated, notes from C3 up play as you hold them (key zones). Macro 1 is the arp's rate, 2 its octaves."},
        // guitar rigs: a pedal or two into the amp, on one chain (devices in series)
        {"Metal Rig", "Audio Effect Rack",
         {{"Rig", {{"Gate", {{0, (-52.0 + 70) / 70}}}, {"Metal Boost"},
                   {"Guitar Amp", {{guitaramp::Head, choicePosition(3, guitaramp::kHeads)}, {guitaramp::Gain, 0.65}, {guitaramp::Bass, 0.6}, {guitaramp::Mid, 0.42},
                                   {guitaramp::Treble, 0.62}, {guitaramp::Presence, 0.6}, {guitaramp::Cab, choicePosition(3, guitaramp::kCabs)}}}}, 0}},
         {{0, 0, 2, guitaramp::Gain, 0.3, 0.9}, {1, 0, 2, guitaramp::Mid, 0.2, 0.8}, {2, 0, 1, 3, 0.0, 1.0}},
         {{0, 0.58}, {1, 0.37}, {2, 0.37}},
         "Gate, a tight boost and the Metal head into a closed 4x12. Macro 1 is gain, 2 mids, 3 how tight the low end is.", true},
        {"Swedish Chainsaw", "Audio Effect Rack",
         {{"Rig", {{"Chainsaw"}, {"Guitar Amp", {{guitaramp::Head, choicePosition(1, guitaramp::kHeads)}, {guitaramp::Gain, 0.3},
                                                {guitaramp::Cab, choicePosition(4, guitaramp::kCabs)}}}}, 0}},
         {{0, 0, 0, 0, 0.5, 1.0}, {1, 0, 1, guitaramp::Treble, 0.3, 0.8}},
         {{0, 0.6}, {1, 0.5}},
         "The Chainsaw pedal on 10 into a crunchy amp and a vintage 4x12: buzzsaw riffs. Macro 1 is the pedal's gain, 2 the amp's treble.", true},
        {"Blues Crunch", "Audio Effect Rack",
         {{"Rig", {{"Overdrive", {{0, 0.3}, {2, 0.65}}}, {"Guitar Amp", {{guitaramp::Head, choicePosition(1, guitaramp::kHeads)}, {guitaramp::Gain, 0.45},
                                                                       {guitaramp::Cab, choicePosition(2, guitaramp::kCabs)}}}}, 0}},
         {{0, 0, 0, 0, 0.0, 0.8}, {1, 0, 1, guitaramp::Gain, 0.2, 0.8}},
         {{0, 0.37}, {1, 0.42}},
         "A green overdrive pushing a crunchy combo. Macro 1 is the overdrive, 2 the amp's gain.", true},
        {"Clean Twang", "Audio Effect Rack",
         {{"Rig", {{"Twang", {{2, 0.3}}}, {"Guitar Amp", {{guitaramp::Head, choicePosition(0, guitaramp::kHeads)}, {guitaramp::Gain, 0.3},
                                                          {guitaramp::Treble, 0.65}, {guitaramp::Cab, choicePosition(1, guitaramp::kCabs)}}}}, 0}},
         {{0, 0, 0, 2, 0.0, 0.8}, {1, 0, 1, guitaramp::Gain, 0.1, 0.6}},
         {{0, 0.37}, {1, 0.4}},
         "Sparkling clean through an open 1x12, with spring reverb. Macro 1 is the springs, 2 the amp's gain.", true},
        {"Bass Amp", "Audio Effect Rack",
         {{"Rig", {{"Guitar Amp", {{guitaramp::Head, choicePosition(4, guitaramp::kHeads)}, {guitaramp::Gain, 0.4},
                                   {guitaramp::Cab, choicePosition(5, guitaramp::kCabs)}}}}, 0}},
         {{0, 0, 0, guitaramp::Gain, 0.1, 0.8}, {1, 0, 0, guitaramp::Mid, 0.2, 0.8}},
         {{0, 0.43}, {1, 0.5}},
         "The Bass head into an 8x10: warm, round, a little grit when you dig in. Macro 1 is the grit, 2 the mids.", true},
    };
    (void)sizeof(R);
    return r;
}

const RackRecipe* rackRecipe(const QString& name) {
    for (const RackRecipe& r : rackRecipes()) if (name == QLatin1String(r.name)) return &r;
    return nullptr;
}

const std::vector<DeviceSpec>& deviceSpecs() {
    static const std::vector<DeviceSpec> s = build();
    return s;
}

const DeviceSpec* deviceSpec(const QString& name) {
    for (const DeviceSpec& s : deviceSpecs())
        if (name == QLatin1String(s.name)) return &s;
    return nullptr;
}

const DeviceSpec* specOf(DeviceType type) {
    for (const DeviceSpec& s : deviceSpecs()) if (s.type == type) return &s;   // devices come before presets
    return nullptr;
}

DeviceType deviceType(const QString& name) {
    const DeviceSpec* s = deviceSpec(name);
    return s ? s->type : DeviceType::Unknown;
}

int paramCount(DeviceType type) {
    const DeviceSpec* s = specOf(type);
    return s ? int(s->params.size()) : 0;
}

double choicePosition(int index, int count) { return count > 1 ? double(index) / (count - 1) : 0.0; }

int choiceIndex(const ParamSpec& p, double v) {
    const int n = int(p.choices.size());
    return std::clamp(int(std::lround(std::clamp(v, 0.0, 1.0) * (n - 1))), 0, std::max(0, n - 1));
}

int choiceIndex(DeviceType type, int i, double v) {
    const DeviceSpec* s = specOf(type);
    if (!s || i < 0 || i >= int(s->params.size())) return 0;
    return choiceIndex(s->params[size_t(i)], v);
}

double paramValue(const ParamSpec& p, double v) {
    v = std::clamp(v, 0.0, 1.0);
    switch (p.kind) {
    case ParamKind::Linear:  return p.lo + v * (p.hi - p.lo);
    case ParamKind::Exp:     return expMap(v, p.lo, p.hi);
    case ParamKind::Int:     return double(std::lround(p.lo + v * (p.hi - p.lo)));
    case ParamKind::Toggle:  return v >= 0.5 ? 1.0 : 0.0;
    case ParamKind::FaderDb: return 40 * std::log10(std::max(v, 1e-4) / 0.75);   // the faders' law
    case ParamKind::Choice: {
        const int c = choiceIndex(p, v);
        return c < int(p.values.size()) ? p.values[size_t(c)] : double(c);
    }
    }
    return v;
}

double paramValue(DeviceType type, int i, double v) {
    const DeviceSpec* s = specOf(type);
    if (!s || i < 0 || i >= int(s->params.size())) return std::clamp(v, 0.0, 1.0);
    return paramValue(s->params[size_t(i)], v);
}

QString paramText(DeviceType type, int i, double v) {
    const DeviceSpec* s = specOf(type);
    if (!s || i < 0 || i >= int(s->params.size())) return pct(v);
    const ParamSpec& p = s->params[size_t(i)];
    const double x = paramValue(type, i, v);
    if (p.kind == ParamKind::Choice) return p.choices.empty() ? QString() : QString(p.choices[size_t(choiceIndex(type, i, v))]);
    if (p.kind == ParamKind::Toggle) return x > 0.5 ? "On" : "Off";
    if (p.kind == ParamKind::FaderDb && v < 0.002) return "-inf dB";
    switch (p.unit) {
    case Unit::Hz:        return hz(x);
    case Unit::Seconds:   return secs(x);
    case Unit::Ms:        return ms(x);
    case Unit::Db:        return x <= -79.9 && p.lo <= -80 ? QString("-inf dB") : db(x);
    case Unit::Percent:   return pct(x);
    case Unit::Semitones: return semis(x);
    case Unit::Cents:     return QString("%1 ct").arg(x, 0, 'f', 0);
    case Unit::Octaves:   return QString("%1%2 oct").arg(x > 0.05 ? "+" : "").arg(x, 0, 'f', 1);
    case Unit::Ratio:     return QString("%1 : 1").arg(x, 0, 'f', 1);
    case Unit::Times:     return QString("x %1").arg(x, 0, 'g', 3);
    case Unit::Degrees:   return QString("%1°").arg(x, 0, 'f', 0);
    case Unit::Pan: {
        const long l = std::lround(x * 50);
        return l == 0 ? QString("C") : QString("%1%2").arg(std::abs(l)).arg(l < 0 ? "L" : "R");
    }
    case Unit::Bits:      return QString("%1 bit").arg(int(x));
    case Unit::Note:      return QString("%1%2").arg(noteName(int(x))).arg(int(x) / 12 - 2);
    case Unit::Dial:      return QString::number(x, 'f', 1);
    case Unit::None:      return std::abs(x - std::round(x)) < 1e-9 ? QString::number(int(std::round(x))) : QString::number(x, 'f', 2);
    }
    return pct(v);
}

} // namespace sessions
