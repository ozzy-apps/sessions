#include "DeviceSpecs.h"
#include <cmath>
#include <deque>
#include <string>

namespace sessions {
namespace {
using P = ParamKind;
using U = Unit;
using T = DeviceType;
using K = DeviceKind;
const char* label(const std::string& s) {
    static std::deque<std::string> names;
    names.push_back(s);
    return names.back().c_str();
}
ParamSpec linear(const char* n, double lo, double hi, double def, U u = U::None) {
    return {n, P::Linear, lo, hi, (def - lo) / (hi - lo), u, {}, {}};
}
ParamSpec exp(const char* n, double lo, double hi, double def, U u) {
    return {n, P::Exp, lo, hi, std::log(def / lo) / std::log(hi / lo), u, {}, {}};
}
ParamSpec toggle(const char* n, bool def) {
    return {n, P::Toggle, 0, 1, def ? 1. : 0., U::None, {}, {}};
}
ParamSpec choice(const char* n, std::vector<const char*> c, int def = 0, std::vector<double> v = {}) {
    return {n, P::Choice, 0, 0, choicePosition(def, int(c.size())), U::None, std::move(c), std::move(v)};
}
} // namespace
void appendProductionSpecs(std::vector<DeviceSpec>& s) {
    std::vector<ParamSpec> eq;
    const double freqs[] = {80, 160, 320, 640, 1250, 2500, 5000, 10000};
    for (int b = 0; b < 8; ++b) {
        const std::string pre = std::to_string(b + 1) + " ";
        eq.push_back(toggle(label(pre + "On"), true));
        eq.push_back(choice(label(pre + "Type"),
                            {"Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass"},
                            b == 0   ? 1
                            : b == 7 ? 2
                                     : 0));
        eq.push_back(exp(label(pre + "Freq"), 20, 20000, freqs[b], U::Hz));
        eq.push_back(linear(label(pre + "Gain"), -24, 24, 0, U::Db));
        eq.push_back(exp(label(pre + "Q"), .1, 24, .707, U::None));
        eq.push_back(choice(label(pre + "Channel"), {"Stereo", "Left", "Right", "Mid", "Side"}));
        eq.push_back(choice(label(pre + "Slope"), {"12 dB", "24 dB", "48 dB"}));
    }
    eq.push_back(linear("Output", -24, 24, 0, U::Db));
    eq.push_back(toggle("Analyze", true));
    eq.push_back(choice("Audition", {"Off", "1", "2", "3", "4", "5", "6", "7", "8"}));
    s.push_back({"Parametric EQ", T::ParametricEQ, K::AudioEffect, eq,
                 "Eight selectable filters with band audition, stereo, left/right and mid/side processing, and "
                 "input/output spectrum analysis."});
    s.push_back({"Spectrum",
                 T::Spectrum,
                 K::AudioEffect,
                 {exp("Smoothing", .02, 2, .2, U::Seconds), toggle("Hold", false)},
                 "Real-time stereo frequency analysis. Hold freezes the display; audio passes unchanged."});
    std::vector<ParamSpec> mb = {exp("Low Cross", 40, 1000, 120, U::Hz), exp("High Cross", 1000, 16000, 2500, U::Hz),
                                 linear("Amount", 0, 1, 1, U::Percent), linear("Dry/Wet", 0, 1, 1, U::Percent),
                                 linear("Output", -24, 24, 0, U::Db)};
    for (int b = 0; b < 3; ++b) {
        const std::string pre = std::string(b == 0 ? "Low" : b == 1 ? "Mid" : "High") + " ";
        mb.push_back(linear(label(pre + "Above"), -60, 0, -18, U::Db));
        mb.push_back(exp(label(pre + "Above Ratio"), .25, 20, 1, U::Ratio));
        mb.push_back(linear(label(pre + "Below"), -80, -6, -40, U::Db));
        mb.push_back(exp(label(pre + "Below Ratio"), .25, 4, 1, U::Ratio));
        mb.push_back(exp(label(pre + "Attack"), .1, 200, 10, U::Ms));
        mb.push_back(exp(label(pre + "Release"), 5, 2000, 100, U::Ms));
        mb.push_back(linear(label(pre + "Gain"), -24, 24, 0, U::Db));
        mb.push_back(toggle(label(pre + "Solo"), false));
        mb.push_back(toggle(label(pre + "On"), true));
    }
    s.push_back({"Multiband Dynamics", T::MultibandDynamics, K::AudioEffect, mb,
                 "Three complementary bands with upward/downward compression and expansion. Below ratios under 1 raise "
                 "quiet signals; above ratios over 1 compress peaks."});
    for (T t : {T::Lfo, T::EnvelopeFollower, T::Shaper}) {
        std::vector<ParamSpec> p = {choice("Rate",
                                           {"4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "Free"},
                                           2, {16, 8, 4, 2, 1, .5, .25, .125, 0}),
                                    exp("Hz", .01, 40, 1, U::Hz),
                                    choice("Shape", {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random"}),
                                    linear("Depth", 0, 1, 1, U::Percent),
                                    linear("Offset", 0, 1, 0, U::Percent),
                                    linear("Phase", 0, 360, 0, U::Degrees),
                                    exp("Rise", .1, 1000, t == T::EnvelopeFollower ? 10 : .1, U::Ms),
                                    exp("Fall", 1, 2000, t == T::EnvelopeFollower ? 100 : 1, U::Ms),
                                    linear("Gain", -24, 24, 0, U::Db)};
        if (t == T::Shaper)
            p[modulator::Shape] = choice("Direction", {"Forward", "Reverse", "Ping Pong", "Steps", "Random", "Sine"});
        if (t == T::EnvelopeFollower) {
            p = {p[modulator::Depth], p[modulator::Offset], p[modulator::Rise], p[modulator::Fall], p[modulator::Gain]};
            p.push_back(choice("Key Tap", {"Pre FX", "Post FX", "Post Mix"}, 1));
            p.push_back(linear("Key Gain", -24, 24, 0, U::Db));
            p.push_back(exp("Key HP", 20, 2000, 20, U::Hz));
            p.push_back(exp("Key LP", 1000, 20000, 20000, U::Hz));
            p.push_back(toggle("Listen", false));
        }
        s.push_back({t == T::Lfo      ? "LFO"
                     : t == T::Shaper ? "Shaper"
                                      : "Envelope Follower",
                     t, K::AudioEffect, p,
                     "Map to device or mixer parameters on this track, then set the mapping range. Audio passes "
                     "unchanged. Bypassing releases mapped controls."});
    }
    std::vector<ParamSpec> pitch = {choice("Mode", {"Pitch", "Frequency", "Ring"}),
                                    linear("Pitch", -24, 24, 0, U::Semitones),
                                    linear("Fine", -100, 100, 0, U::Cents),
                                    linear("Formant", -12, 12, 0, U::Semitones),
                                    toggle("Preserve Formants", true),
                                    linear("Frequency", -2000, 2000, 0, U::Hz),
                                    linear("Dry/Wet", 0, 1, 1, U::Percent),
                                    linear("Output", -24, 12, 0, U::Db),
                                    choice("Root", {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}),
                                    choice("Scale", {"Chromatic", "Major", "Minor", "Pentatonic"}),
                                    linear("Strength", 0, 1, 1, U::Percent),
                                    exp("Retune", 1, 500, 30, U::Ms)};
    auto shifter = pitch;
    shifter[pitchfx::Strength].def = 0;
    s.push_back({"Shifter", T::Shifter, K::AudioEffect, shifter,
                 "Real-time pitch/formant shifting, frequency shifting and ring modulation. Pitch mode uses a "
                 "2048-sample spectral window; dry audio is aligned to it."});
    s.push_back({"Pitch Corrector", T::PitchCorrector, K::AudioEffect, pitch,
                 "Monophonic pitch correction to a root and scale with retune speed, correction strength and formant "
                 "control. Pitch tracking ignores unvoiced audio."});
    s.push_back(
        {"Hybrid Reverb",
         T::HybridReverb,
         K::AudioEffect,
         {choice("Routing", {"Algorithm", "Convolution", "Parallel", "Serial"}), linear("Blend", 0, 1, .5, U::Percent),
          exp("Decay", .2, 20, 2.5, U::Seconds), linear("Size", .25, 2, 1), exp("Pre-Delay", .1, 250, .1, U::Ms),
          linear("Damping", 0, 1, .5, U::Percent), exp("Low Cut", 20, 2000, 80, U::Hz),
          exp("High Cut", 1000, 20000, 16000, U::Hz), linear("Width", 0, 2, 1, U::Percent), toggle("Freeze", false),
          linear("Dry/Wet", 0, 1, .25, U::Percent), linear("Output", -24, 12, 0, U::Db)},
         "Algorithmic feedback-delay reverb and long impulse responses, in parallel or in series. Load a room or plate "
         "IR up to eight seconds."});
    s.push_back(
        {"Looper",
         T::Looper,
         K::AudioEffect,
         {choice("State", {"Stop", "Record", "Play", "Overdub"}),
          choice("Length", {"Free", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"}, 2, {0, 4, 8, 16, 32, 64}),
          linear("Feedback", 0, 1, 1, U::Percent), linear("Dry/Wet", 0, 1, 1, U::Percent),
          linear("Output", -24, 12, 0, U::Db), toggle("Reverse", false), toggle("Half Speed", false),
          toggle("Undo", false), toggle("Clear", false), toggle("Record Trigger", false), toggle("Play Trigger", false),
          toggle("Stop Trigger", false), toggle("Overdub Trigger", false)},
         "Record and overdub loops up to 120 seconds. Fixed lengths follow the tempo. Undo swaps the last overdub; "
         "export the loop to an audio file."});
    s.push_back(
        {"Multisampler",
         T::Multisampler,
         K::Instrument,
         {exp("Attack", .001, 5, .005, U::Seconds), exp("Decay", .01, 10, .5, U::Seconds),
          linear("Sustain", 0, 1, 1, U::Percent), exp("Release", .005, 10, .2, U::Seconds),
          linear("Volume", -36, 6, 0, U::Db), linear("Tune", -24, 24, 0, U::Semitones),
          exp("Cutoff", 20, 20000, 20000, U::Hz), exp("Resonance", .5, 8, .707, U::None), toggle("Loop", false)},
         "Sample zones with key/velocity ranges, root notes, gain, pan and round robin. Import WAV/AIFF/audio files or "
         "an SFZ mapping; edit zones in the expanded editor."});
}
} // namespace sessions
