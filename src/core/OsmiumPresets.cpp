#include "OsmiumPresets.h"
#include <algorithm>
#include <cmath>

namespace sessions::osmium {
namespace {
using namespace monolith;
using Patch = FactoryPreset;
using Points = QVector<CurvePoint>;
double choice(int i, int n) { return double(i) / (n - 1); }
double expPos(double value, double lo, double hi) { return std::clamp(std::log(value / lo) / std::log(hi / lo), 0.0, 1.0); }
double linPos(double value, double lo, double hi) { return std::clamp((value - lo) / (hi - lo), 0.0, 1.0); }
void route(Patch& p, int source, int param, double amount) { p.mods.push_back({source, param, amount}); }
void env(Patch& p, int e, double a, double d, double s, double r, double aBend = 0, double dBend = -0.4, double rBend = -0.4) {
    p.values[eparam(e, A)] = expPos(a, 0.001, 5); p.values[eparam(e, D)] = expPos(d, 0.005, 8);
    p.values[eparam(e, S)] = s; p.values[eparam(e, R)] = expPos(r, 0.005, 8);
    p.values[bend(e, 0)] = 0.5 + aBend * 0.5; p.values[bend(e, 1)] = 0.5 + dBend * 0.5; p.values[bend(e, 2)] = 0.5 + rBend * 0.5;
}
void osc(Patch& p, int o, int table, double position, double level, double pitch = 0, double fine = 0, int mode = 0, double warp = 0, double split = 0) {
    p.values[oparam(o, Table)] = choice(table, 8); p.values[oparam(o, Position)] = position;
    p.values[oparam(o, Level)] = level; p.values[oparam(o, Pitch)] = linPos(pitch, -36, 36); p.values[oparam(o, Fine)] = linPos(fine, -100, 100);
    p.values[oparam(o, Mode)] = choice(mode, 3); p.values[oparam(o, Intensity)] = warp; p.values[oparam(o, FilterMix)] = split;
}
void filter(Patch& p, int f, int type, double hz, double resonance) {
    p.values[fparam(f, FType)] = choice(type, 7); p.values[fparam(f, FCutoff)] = expPos(hz, 30, 18000); p.values[fparam(f, FReso)] = resonance;
}
void lfo(Patch& p, int l, int rate, int shape, bool retrigger, double hz = 0.2, double fade = 0.001) {
    p.values[lparam(l, LRate)] = choice(rate, 9); p.values[lparam(l, LHz)] = expPos(hz, 0.05, 30);
    p.values[lparam(l, LShape)] = choice(shape, 6); p.values[lretrig(l)] = retrigger;
    p.values[lfade(l)] = expPos(fade, 0.001, 5);
}
void phrase(Patch& p, int c, const Points& points, int rate = 4, bool retrigger = true, double smooth = 0.04) {
    p.curves[c] = points; p.values[cparam(c, CRate)] = choice(rate, 9); p.values[cparam(c, CRetrig)] = retrigger; p.values[cparam(c, CSmooth)] = smooth;
}
void space(Patch& p, double chorus, double reverb, double size, double delay = 0, int division = 2, double feedback = 0.32) {
    p.values[fx(ChorusMix)] = chorus; p.values[fx(ReverbMix)] = reverb; p.values[fx(ReverbSize)] = size;
    p.values[fx(DelayMix)] = delay; p.values[fx(DelayTime)] = choice(division, 8); p.values[fx(DelayFeedback)] = feedback;
}
void drive(Patch& p, double filterDrive, int type, double amount) {
    p.values[FDrive] = filterDrive; p.values[InsertType] = choice(type, 6); p.values[InsertAmount] = amount;
}
Patch base(const char* name, const char* category, const char* description) {
    Patch p{name, category, description, {}, {}, {}};
    p.values[Volume] = 0.8; p.values[VelAmp] = 0.35; p.values[BendRange] = 2.0 / 24;
    p.values[NoiseColor] = 0.6; p.values[ModPitch] = 0.5; p.values[ModOscWave] = 0;
    p.values[fx(EqLow)] = 0.5; p.values[fx(EqHigh)] = 0.5;
    p.values[fx(ChorusRate)] = expPos(0.35, 0.05, 8); p.values[fx(ChorusDepth)] = 0.55;
    p.values[fx(DelayTone)] = expPos(4500, 500, 12000); p.values[fx(DelayFeedback)] = 0.3;
    p.curves = {{{0, 0, -0.2, 1}, {0.5, 1, 0.2, 1}, {1, 0}}, {{0, 1, -0.4}, {0.5, 0.1, 0.2, 1}, {1, 1}}};
    for (int e = 0; e < 4; ++e) { env(p, e, 0.004, 0.3, e == 0 ? 0.8 : 0, 0.12); p.values[ecurve(e)] = 0.5; }
    for (int o = 0; o < 3; ++o) osc(p, o, 0, 0.65, o == 0 ? 0.7 : 0);
    for (int f = 0; f < 2; ++f) filter(p, f, f == 0 ? 1 : 0, f == 0 ? 1800 : 4500, 0.12);
    for (int l = 0; l < 4; ++l) lfo(p, l, l == 0 ? 5 : 2, l % 2, true);
    for (int c = 0; c < 2; ++c) phrase(p, c, p.curves[c], 2);
    for (int s = 0; s < 2; ++s) {
        p.values[sparam(s, SRate)] = choice(6, 8); p.values[sparam(s, SCount)] = 1; p.values[sretrig(s)] = 1;
        for (int i = 0; i < 16; ++i) p.values[step(s, i)] = 0.5;
    }
    // Same eight performance functions throughout the bank. No macro changes pitch or timing.
    route(p, SrcMacro1, fparam(0, FCutoff), 0.30); route(p, SrcMacro1, fparam(1, FCutoff), 0.18); // Tone
    route(p, SrcMacro2, oparam(0, Intensity), 0.28); route(p, SrcMacro2, oparam(1, Position), 0.18); // Warp
    route(p, SrcMacro3, FDrive, 0.42); route(p, SrcMacro3, InsertAmount, 0.20); // Drive
    route(p, SrcMacro4, Spread, 0.7); // Width (one-voice subs deliberately stay centered)
    route(p, SrcMacro5, fx(ReverbMix), 0.25); route(p, SrcMacro6, fx(DelayMix), 0.28); // Space / Echo
    route(p, SrcMacro7, SubLevel, 0.25); // Body
    route(p, SrcMacro8, NoiseLevel, 0.025); route(p, SrcMacro8, fx(EqHigh), 0.12); // Air
    route(p, SrcWheel, fparam(0, FCutoff), 0.16);
    return p;
}
Patch pad(const char* name, const char* description) {
    auto p = base(name, "Pad", description);
    p.values[Volume] = linPos(-3, -24, 6); p.values[Unison] = choice(2, 4); p.values[Detune] = 0.26; p.values[Spread] = 0.78;
    osc(p, 0, 0, 0.46, 0.52, 0, -4); osc(p, 1, 1, 0.32, 0.38, 0, 5, 0, 0, 0.25); osc(p, 2, 4, 0.1, 0.15, -12);
    filter(p, 0, 0, 2600, 0.10); filter(p, 1, 0, 6500, 0.10); p.values[FMix] = 0.25;
    env(p, 0, 0.7, 2, 0.82, 2.2, -0.25); env(p, 1, 1.4, 3.4, 0.3, 2.4);
    lfo(p, 0, 8, 0, false, 0.12, 1.2); lfo(p, 1, 8, 1, false, 0.19, 1.8);
    route(p, SrcLfo1, oparam(0, Position), 0.12); route(p, SrcLfo2, fparam(0, FCutoff), 0.07); route(p, SrcEnv2, fparam(0, FCutoff), 0.11);
    space(p, 0.28, 0.24, 0.78, 0.10, 2); drive(p, 0.04, 1, 0.04);
    return p;
}
Patch lead(const char* name, const char* description) {
    auto p = base(name, "Lead", description);
    p.values[Volume] = linPos(5, -24, 6); p.values[VoiceMode] = 1; p.values[Unison] = choice(1, 4); p.values[Spread] = 0.48; p.values[Detune] = 0.18;
    p.values[Glide] = expPos(0.055, 0.001, 2);
    osc(p, 0, 0, 0.68, 0.68); osc(p, 1, 3, 0.22, 0.32, 0, 7, 0, 0, 0.15);
    filter(p, 0, 1, 2100, 0.23); p.values[FMix] = 0.12;
    env(p, 0, 0.006, 0.32, 0.72, 0.16); env(p, 1, 0.004, 0.23, 0.12, 0.1);
    route(p, SrcEnv2, fparam(0, FCutoff), 0.26);
    space(p, 0.10, 0.11, 0.52, 0.16, 2); drive(p, 0.12, 1, 0.1);
    return p;
}
Patch sub(const char* name, const char* description) {
    auto p = base(name, "Sub", description);
    p.values[Volume] = linPos(4, -24, 6); p.values[VoiceMode] = 1; p.values[OscRestart] = 1;
    p.values[Glide] = expPos(0.028, 0.001, 2); p.values[Spread] = 0; p.values[VelAmp] = 0.2;
    osc(p, 0, 4, 0.10, 0.90); osc(p, 1, 0, 0, 0); osc(p, 2, 0, 0, 0);
    filter(p, 0, 1, 420, 0.05); env(p, 0, 0.006, 0.3, 0.92, 0.065); env(p, 1, 0.001, 0.12, 0, 0.06);
    drive(p, 0.09, 1, 0.04); space(p, 0, 0, 0.45, 0, 1, 0.22);
    // Width leaves the sub itself centered; adding a little chorus is optional.
    p.mods.removeIf([](const ModRoute& r) { return r.source == SrcMacro4; }); route(p, SrcMacro4, fx(ChorusMix), 0.16);
    return p;
}
Patch reese(const char* name, const char* description) {
    auto p = base(name, "Reese", description); p.values[Volume] = 1;
    p.values[VoiceMode] = 1; p.values[Glide] = expPos(0.055, 0.001, 2); p.values[Unison] = choice(1, 4); p.values[Spread] = 0.68; p.values[Detune] = 0.28;
    osc(p, 0, 0, 0.67, 0.55, 0, -10); osc(p, 1, 0, 0.69, 0.48, 0, 11, 0, 0.07, 0.22);
    p.values[SubLevel] = 0.2; filter(p, 0, 1, 850, 0.22); filter(p, 1, 0, 3400, 0.12); p.values[FMix] = 0.25;
    env(p, 0, 0.009, 0.6, 0.86, 0.11); env(p, 1, 0.006, 0.5, 0.2, 0.12);
    lfo(p, 0, 8, 0, false, 0.43); lfo(p, 1, 8, 1, false, 0.17);
    route(p, SrcLfo1, oparam(0, Position), 0.08); route(p, SrcLfo2, fparam(0, FCutoff), 0.07); route(p, SrcEnv2, fparam(0, FCutoff), 0.12);
    drive(p, 0.24, 1, 0.20); space(p, 0.16, 0.035, 0.35, 0.025, 1);
    return p;
}
Patch wobble(const char* name, const char* description) {
    auto p = base(name, "Wobble", description); p.values[Volume] = linPos(5, -24, 6);
    p.values[VoiceMode] = 1; p.values[Glide] = expPos(0.035, 0.001, 2); p.values[Unison] = choice(1, 4); p.values[Spread] = 0.42; p.values[Detune] = 0.16;
    osc(p, 0, 5, 0.42, 0.64, 0, -3, 0, 0.16); osc(p, 1, 4, 0.13, 0.32, 0, 4, 0, 0, 0.15);
    p.values[SubLevel] = 0.24; filter(p, 0, 1, 230, 0.25); filter(p, 1, 0, 2400, 0.16); p.values[FMix] = 0.16;
    env(p, 0, 0.005, 0.4, 0.92, 0.085); env(p, 1, 0.001, 0.18, 0.06, 0.1);
    phrase(p, 0, {{0, 0, -0.65}, {0.24, 1, -0.35}, {0.72, 0.08, 0.15, 1}, {1, 0}}, 5);
    route(p, SrcCurve1, fparam(0, FCutoff), 0.43); route(p, SrcCurve1, oparam(0, Position), 0.19);
    drive(p, 0.22, 1, 0.18); space(p, 0.06, 0.025, 0.35, 0.03, 1);
    return p;
}
// Wobble phrases (0.44; integrated from the cloud 0.39 branch): runs of wobbles drawn end to end into one curve, each run at its own
// division, so the rate changes you would otherwise automate (quarters, eighths, triplets,
// sixteenth and 1/32 rolls) play by themselves across the phrase. A run lasts `beats`; each
// wobble in it starts at `from`, reaches `to` after `turn` of its cycle and heads back to the
// next wobble's `from`. `out` / `back` bend the two halves (negative: fast start). Shape 1 is a
// smooth S (sine-like rolls), shape 2 holds each value (square chops for stutters).
constexpr double kHalf = 2, kQuarter = 1, kQuarterT = 2.0 / 3, kEighth = 0.5, kEighthDot = 0.75, kEighthT = 1.0 / 3,
                 kSixteenth = 0.25, kSixteenthT = 1.0 / 6, kThirtySecond = 0.125;
enum PhraseRate { FourBars = 0, TwoBars = 1, OneBar = 2 };   // Curve Rate choices
struct Run { double beats, division, from = 0, to = 1, turn = 0.35, out = -0.2, back = -0.25; int shape = 0; };
Run wob(double beats, double division, double depth = 1, double turn = 0.35) { return {beats, division, 0, depth, turn}; }
Run roll(double beats, double division, double depth = 1) { return {beats, division, 0, depth, 0.5, 0, 0, 1}; }
Run wheeze(double beats, double division, double depth = 1) { return {beats, division, 0, depth, 0.82, 0.45, -0.3}; }
Run chop(double beats, double division, double depth = 1, double gate = 0.5) { return {beats, division, depth, 0, gate, 0, 0, 2}; }
Run rest(double beats, double level = 0) { return {beats, beats, level, level}; }
Points runs(double length, std::initializer_list<Run> list) {
    Points points; double at = 0;
    const std::vector<Run> all(list);
    for (size_t i = 0; i < all.size(); ++i) {
        const Run& r = all[i];
        const int cycles = std::max(1, int(std::lround(r.beats / r.division)));
        const double cycle = r.beats / cycles;
        for (int k = 0; k < cycles; ++k) {
            points.push_back({(at + k * cycle) / length, r.from, r.out, r.shape});
            if (r.to != r.from) points.push_back({(at + (k + r.turn) * cycle) / length, r.to, r.back, r.shape});
        }
        at += r.beats;
        // A wobble closes on its own `from` even when the next run starts elsewhere (a chop starts open).
        const Run& next = all[(i + 1) % all.size()];
        if (r.shape != 2 && r.to != r.from && next.from != r.from) points.push_back({at / length - 1e-5, r.from, 0, r.shape});
    }
    points.push_back({1, points.isEmpty() ? 0.0 : points.first().y});
    return points;
}
// Fat wobble basses: mono with a short glide, a four-voice unison over a centred sine sub,
// drive into the filters and an insert. Voiced around F1 (MIDI 41, the sub an octave under it);
// the filters follow the keyboard, so keyFilter() gives each cutoff as heard at F1. Curve 1 is
// the wobble phrase; Curve 2 drifts the timbre over four bars so each repeat of the phrase
// differs. Velocity opens the filter a little.
void keyFilter(Patch& p, int f, int type, double hzAtF1, double resonance) { filter(p, f, type, hzAtF1 * std::exp2((60 - 41) / 12.0), resonance); }
Patch bass(const char* name, const char* category, double bpm, const char* description) {
    auto p = base(name, category, description); p.bpm = bpm;
    p.values[Volume] = linPos(5.5, -24, 6); p.values[VoiceMode] = 1; p.values[Glide] = expPos(0.03, 0.001, 2);
    p.values[Unison] = choice(3, 4); p.values[Detune] = 0.22; p.values[Spread] = 0.55; p.values[VelAmp] = 0.25; p.values[KeyTrack] = 1;
    osc(p, 0, 5, 0.45, 0.6, 0, -4, 0, 0.18); osc(p, 1, 0, 0.66, 0.42, 0, 5);
    p.values[SubLevel] = 0.38;
    keyFilter(p, 0, 1, 110, 0.3); keyFilter(p, 1, 0, 2400, 0.14); p.values[FMix] = 0;   // all through filter 1: the wobble closes everything
    env(p, 0, 0.003, 0.4, 0.96, 0.07); env(p, 1, 0.001, 0.14, 0, 0.08);
    drive(p, 0.34, 2, 0.24); space(p, 0.07, 0.02, 0.32, 0, 1);
    phrase(p, 1, {{0, .15, 0, 1}, {.3, .7, 0, 1}, {.55, .3, 0, 1}, {.8, .9, 0, 1}, {1, .15}}, FourBars, false, .3);
    route(p, SrcCurve2, oparam(0, Position), .14); route(p, SrcVelocity, fparam(0, FCutoff), .05);
    return p;
}
void stepper(Patch& p, int s, int rate, std::initializer_list<double> steps, double glide) {
    p.values[sparam(s, SRate)] = choice(rate, 8); p.values[sparam(s, SCount)] = double(steps.size() - 1) / 15; p.values[sparam(s, SGlide)] = glide;
    int i = 0; for (double v : steps) p.values[step(s, i++)] = .5 + v * .5;
}

void appendWobbleBassBank(std::vector<Patch>& bank) {
    auto add = [&](Patch p) { bank.push_back(std::move(p)); };
    // Dubstep (0.44): voiced at 140 BPM, half-time. Curve 1 changes its wobble rate across the
    // phrase; locked to the song's bars, so hold a note through the phrase or start notes anywhere.
    {
        auto p = bass("Osmium Dubstep - Gravedigger", "Dubstep", 140, "Classic heavy wobble over a centred sub. Two bars: quarters, eighths, eighth triplets, sixteenths, then a 1/32 roll.");
        phrase(p, 0, runs(8, {wob(2, kQuarter), wob(2, kEighth), wob(2, kEighthT, .9), wob(1, kSixteenth, .85), wob(1, kThirtySecond, .7, .5)}), TwoBars, false, .06);
        route(p, SrcCurve1, fparam(0, FCutoff), .52); route(p, SrcCurve1, oparam(0, Position), .18); add(p);

        p = bass("Osmium Dubstep - Yoi Machine", "Dubstep", 140, "Talking formant wobble: the vowel and filter move together. Dotted eighths, a long half-note yoi, triplets and sixteenths.");
        osc(p, 0, 2, .1, .64, 0, -3, 1, .1); osc(p, 1, 5, .3, .4, 0, 4); keyFilter(p, 0, 1, 160, .28);
        phrase(p, 0, runs(8, {wob(3, kEighthDot, 1, .3), wob(1, kQuarter, .8), wob(1, kEighthT, .9), wob(2, kHalf, 1, .55), wob(1, kSixteenth, .8)}), TwoBars, false, .08);
        route(p, SrcCurve1, oparam(0, Position), .55); route(p, SrcCurve1, fparam(0, FCutoff), .40); add(p);

        p = bass("Osmium Dubstep - Riddim Hammer", "Dubstep", 140, "Square riddim wobble with gaps: triplet bursts, rests and a 1/32 stutter. Hard clipped, centred low end.");
        osc(p, 0, 3, .15, .62, 0, -2); osc(p, 1, 0, 1, .4, 0, 3); keyFilter(p, 0, 0, 90, .38); drive(p, .4, 3, .3);
        phrase(p, 0, runs(8, {wob(1, kEighthT), rest(.5), wob(.5, kSixteenth), wob(2, kEighthT, .9), wob(1, kQuarter), wob(1, kSixteenthT, .8), rest(1), wob(1, kThirtySecond, .75)}), TwoBars, false, .05);
        route(p, SrcCurve1, fparam(0, FCutoff), .5); add(p);

        p = bass("Osmium Dubstep - Tearout", "Dubstep", 140, "Screaming sync saw through the Scream filter and a wavefolder. Rolling sixteenths with sixteenth-triplet and 1/32 bursts.");
        osc(p, 0, 0, .66, .58, 0, -6, 2, .22); osc(p, 1, 5, .6, .42, 0, 6); keyFilter(p, 0, 6, 140, .45); drive(p, .42, 4, .12); p.values[Volume] = linPos(3.3, -24, 6);
        phrase(p, 0, runs(8, {wob(1, kSixteenth), wob(1, kEighth), wob(1, kSixteenth, .9), wob(1, kThirtySecond, .8), wob(2, kSixteenthT, .9), wob(1, kEighthT), wob(1, kThirtySecond, 1, .5)}), TwoBars, false, .04);
        route(p, SrcCurve1, fparam(0, FCutoff), .48); route(p, SrcCurve1, oparam(0, Intensity), .2); add(p);

        p = bass("Osmium Dubstep - Robot Talk", "Dubstep", 140, "Metallic ring-modulated robot voice through a sweeping comb. Four bars of changing rates with a stepped vowel underneath.");
        osc(p, 0, 6, .3, .58, 0, -3); osc(p, 1, 5, .4, .4, 0, 4); p.values[ModRing] = .18; p.values[ModPitch] = linPos(7, -36, 36);
        keyFilter(p, 0, 1, 160, .3); keyFilter(p, 1, 5, 220, .55); p.values[FRouting] = 1; p.values[FMix] = .45;
        phrase(p, 0, runs(16, {wob(1, kQuarter), wob(1, kEighthT), wob(2, kEighth), wob(2, kQuarterT), wob(2, kSixteenth, .85),
                               wob(1, kEighthT), wob(1, kSixteenthT, .85), wob(2, kHalf, 1, .5), wob(2, kEighth), wob(1, kSixteenth), wob(1, kThirtySecond, .7)}), FourBars, false, .06);
        stepper(p, 0, 6, {.8, -.4, .3, -.8, .6, -.1}, .3);
        route(p, SrcCurve1, fparam(0, FCutoff), .45); route(p, SrcCurve1, fparam(1, FCutoff), .35); route(p, SrcStep1, oparam(0, Position), .2); add(p);

        p = bass("Osmium Dubstep - Deep Wub", "Dubstep", 140, "Slow, deep half-time wub with a heavy sub. Four bars: half notes, quarters, then eighths and triplets for the turnaround.");
        osc(p, 0, 5, .3, .58, 0, -3); osc(p, 1, 0, .66, .4, 0, 4); p.values[SubLevel] = .6; keyFilter(p, 0, 1, 80, .22);
        phrase(p, 0, runs(16, {wob(4, kHalf, 1, .45), wob(4, kQuarter, .9), wob(2, kHalf), wob(2, kEighth, .85), wob(2, kEighthT, .9), wob(2, kQuarter)}), FourBars, false, .1);
        route(p, SrcCurve1, fparam(0, FCutoff), .5); route(p, SrcCurve1, oparam(0, Position), .12); add(p);

        p = bass("Osmium Dubstep - Triplet Grinder", "Dubstep", 140, "Phase-modulated growl that lives on triplets: quarter, eighth and sixteenth triplets with straight quarters between.");
        osc(p, 0, 5, .55, .6, 0, -5, 0, .2); osc(p, 1, 3, .25, .4, 0, 5); p.values[ModPhase] = .08; keyFilter(p, 0, 1, 120, .32);
        phrase(p, 0, runs(8, {wob(2, kQuarterT), wob(2, kEighthT), wob(1, kSixteenthT, .85), wob(1, kEighthT), wob(1, kQuarter), wob(1, kSixteenthT, .9)}), TwoBars, false, .06);
        route(p, SrcCurve1, fparam(0, FCutoff), .5); route(p, SrcCurve1, ModPhase, .12); add(p);

        p = bass("Osmium Dubstep - Stutter Gate", "Dubstep", 140, "Chopped wobble: square gates in eighths, sixteenths, triplets and 1/32 between open quarter wobbles. The sub keeps going.");
        osc(p, 0, 5, .4, .6, 0, -4, 0, .2); osc(p, 1, 0, .66, .42, 0, 5); keyFilter(p, 0, 1, 45, .2);
        phrase(p, 0, runs(8, {chop(1, kEighth, .9, .6), chop(1, kSixteenth, .9), wob(2, kQuarter), chop(1, kSixteenthT, .85), chop(1, kThirtySecond, .8), wob(1, kEighthT), chop(1, kSixteenth, 1, .7)}), TwoBars, false, .07);
        route(p, SrcCurve1, fparam(0, FCutoff), .62); add(p);

        p = bass("Osmium Dubstep - Laser", "Dubstep", 140, "Hard-sync laser wobble: the sync sweep follows the phrase. Eighths, triplets, a long quarter, then faster sixteenths.");
        osc(p, 0, 0, .66, .58, 0, -4, 2, .1); osc(p, 1, 5, .5, .4, 0, 5); keyFilter(p, 0, 1, 200, .25); keyFilter(p, 1, 3, 1400, .4); p.values[FRouting] = 1; p.values[FMix] = .3;
        phrase(p, 0, runs(8, {wob(1, kEighth, 1, .25), wob(1, kEighthT), wob(2, kQuarter, 1, .6), wob(1, kSixteenth), wob(1, kEighth), wob(1, kSixteenthT, .9), wob(1, kThirtySecond, .8)}), TwoBars, false, .05);
        route(p, SrcCurve1, oparam(0, Intensity), .55); route(p, SrcCurve1, fparam(0, FCutoff), .42); route(p, SrcCurve1, fparam(1, FCutoff), .3); add(p);

        p = bass("Osmium Dubstep - Halftime Growl", "Dubstep", 140, "Vocal growl: the Growl table and a drifting notch move with a four-bar phrase that ends in triplets and a 1/32 roll.");
        osc(p, 0, 5, .2, .6, 0, -4, 1, .08); osc(p, 1, 2, .3, .4, 0, 4); keyFilter(p, 0, 1, 130, .3); keyFilter(p, 1, 4, 900, .35); p.values[FRouting] = 1; p.values[FMix] = .7;
        phrase(p, 0, runs(16, {wob(2, kQuarter), wob(2, kEighth), wob(1, kEighthT), wob(1, kSixteenth), wob(2, kHalf, .9, .6), wob(4, kEighth, .9),
                               wob(1, kQuarter), wob(1, kSixteenthT), wob(1, kEighthT), wob(1, kThirtySecond, .75)}), FourBars, false, .06);
        route(p, SrcCurve1, oparam(0, Position), .45); route(p, SrcCurve1, fparam(0, FCutoff), .45); route(p, SrcCurve2, fparam(1, FCutoff), .3); add(p);
    }
    // DnB Bass (0.44): voiced at 174 BPM. Same idea at drum & bass speed; Jump Up Wheeze and
    // Stab Rewind restart their one-bar phrase with every note.
    {
        auto p = bass("Osmium DnB Bass - Neuro Talker", "DnB Bass", 174, "Talking neuro bass: a sixteenth-note vowel sequence under a two-bar filter phrase of eighths, triplets and rolls.");
        osc(p, 0, 2, .2, .6, 0, -3, 1, .12); osc(p, 1, 5, .5, .4, 0, 4); keyFilter(p, 0, 1, 200, .3);
        stepper(p, 0, 6, {.9, -.3, .6, -.7, .2, .7}, .3);
        phrase(p, 0, runs(8, {wob(1, kEighth), wob(1, kSixteenth, .8), wob(2, kQuarter, 1, .3), wob(1, kEighthT), wob(1, kEighth, .9), wob(1, kSixteenthT, .8), wob(1, kThirtySecond, .7)}), TwoBars, false, .05);
        route(p, SrcCurve1, fparam(0, FCutoff), .48); route(p, SrcStep1, oparam(0, Position), .3); add(p);

        p = bass("Osmium DnB Bass - Jump Up Wheeze", "DnB Bass", 174, "Rising jump-up wheeze on a resonant filter. Each note restarts one bar: a quarter, eighths, triplets, sixteenths.");
        osc(p, 0, 3, .3, .6, 0, -4); osc(p, 1, 0, .66, .42, 0, 5); keyFilter(p, 0, 0, 150, .55);
        phrase(p, 0, runs(4, {wheeze(1, kQuarter), wheeze(1, kEighth), wheeze(1, kEighthT, .9), wheeze(1, kSixteenth, .85)}), OneBar, true, .05);
        route(p, SrcCurve1, fparam(0, FCutoff), .5); route(p, SrcCurve1, oparam(0, Position), .2); add(p);

        p = bass("Osmium DnB Bass - Roller", "DnB Bass", 174, "Rolling detuned reese wobble with smooth sine-like movement: eighths with sixteenth and triplet fills.");
        osc(p, 0, 0, .67, .54, 0, -11); osc(p, 1, 0, .69, .48, 0, 12); p.values[Detune] = .3; keyFilter(p, 0, 1, 140, .28);
        phrase(p, 0, runs(8, {roll(2, kEighth, .9), roll(1, kSixteenth, .8), roll(1, kEighth), roll(2, kEighth, .9), roll(1, kEighthT, .85), roll(1, kSixteenth, .8)}), TwoBars, false, .1);
        route(p, SrcCurve1, fparam(0, FCutoff), .45); add(p);

        p = bass("Osmium DnB Bass - Liquid Wobble", "DnB Bass", 174, "Soft round wobble for liquid rollers: quarters easing into eighths and triplets, a little chorus and space.");
        osc(p, 0, 0, .4, .6, 0, -5); osc(p, 1, 1, .4, .38, 0, 5); p.values[Unison] = choice(2, 4); p.values[Volume] = linPos(4, -24, 6); keyFilter(p, 0, 0, 120, .2);
        drive(p, .18, 1, .1); space(p, .18, .06, .45, .06, 2);
        phrase(p, 0, runs(8, {roll(2, kQuarter), roll(2, kEighth, .85), roll(1, kQuarter), roll(1, kEighthT, .8), roll(2, kHalf, .9)}), TwoBars, false, .12);
        route(p, SrcCurve1, fparam(0, FCutoff), .42); add(p);

        p = bass("Osmium DnB Bass - Foghorn", "DnB Bass", 174, "Huge pulse foghorn with a square sub. Four bars of slow swells that speed up into triplets and a 1/32 roll.");
        osc(p, 0, 3, .05, .6, 0, -6); osc(p, 1, 3, .1, .48, 0, 7); p.values[SubWave] = choice(1, 3); p.values[Volume] = 1; keyFilter(p, 0, 1, 70, .25); drive(p, .5, 1, .32);
        phrase(p, 0, runs(16, {wob(4, kHalf, 1, .6), wob(2, kQuarter), wob(2, kEighth, .85), wob(4, 4, 1, .7), wob(2, kEighthT), wob(1, kSixteenth, .8), wob(1, kThirtySecond, .7)}), FourBars, false, .1);
        route(p, SrcCurve1, fparam(0, FCutoff), .5); add(p);

        p = bass("Osmium DnB Bass - Halftime Crusher", "DnB Bass", 174, "Folded halftime bass built on triplets: quarter and eighth triplets against straight halves and quarters over four bars.");
        osc(p, 0, 5, .65, .58, 0, -4, 0, .24); osc(p, 1, 7, .35, .4, 0, 5); keyFilter(p, 0, 1, 120, .34); drive(p, .4, 4, .26);
        phrase(p, 0, runs(16, {wob(2, kQuarterT), wob(2, kQuarter), wob(2, kEighthT), wob(2, kHalf), wob(1, kSixteenthT), wob(1, kEighthT), wob(2, kEighth), wob(2, kQuarterT), wob(1, kSixteenth), wob(1, kThirtySecond, .7)}), FourBars, false, .06);
        route(p, SrcCurve1, fparam(0, FCutoff), .48); route(p, SrcCurve1, oparam(0, Position), .15); add(p);

        p = bass("Osmium DnB Bass - Growl Reese", "DnB Bass", 174, "Detuned reese with a formant growl in a band-pass layer. The vowel opens on quarters, eighths and triplets.");
        osc(p, 0, 0, .67, .52, 0, -10); osc(p, 1, 2, .3, .46, 0, 11, 1, .1); keyFilter(p, 0, 1, 150, .25); keyFilter(p, 1, 3, 600, .45); p.values[FRouting] = 1; p.values[FMix] = .45;
        phrase(p, 0, runs(8, {wob(1, kQuarter), wob(1, kEighth), wob(2, kEighthT, .9), wob(1, kSixteenth), wob(1, kEighth), wob(2, kQuarter, 1, .55)}), TwoBars, false, .06);
        route(p, SrcCurve1, oparam(1, Position), .5); route(p, SrcCurve1, fparam(1, FCutoff), .35); route(p, SrcCurve1, fparam(0, FCutoff), .4); add(p);

        p = bass("Osmium DnB Bass - Stab Rewind", "DnB Bass", 174, "Synced growl stab that ratchets up: every note restarts a bar of one quarter, two eighths, sixteenths and 1/32s.");
        osc(p, 0, 5, .5, .6, 0, -4, 2, .16); osc(p, 1, 0, .66, .42, 0, 5);
        phrase(p, 0, runs(4, {{1, kQuarter, 0, 1, .12, -.6, -.1}, wob(1, kEighth, .9), wob(1, kSixteenth, .8), wob(1, kThirtySecond, .7)}), OneBar, true, .04);
        route(p, SrcCurve1, fparam(0, FCutoff), .5); route(p, SrcEnv2, fparam(0, FCutoff), .15); add(p);

        p = bass("Osmium DnB Bass - Metal Hornet", "DnB Bass", 174, "Buzzing metallic bass through a sweeping comb. Sixteenth triplets, eighths and quarters swap places over two bars.");
        osc(p, 0, 6, .5, .58, 0, -5, 2, .1); osc(p, 1, 5, .45, .42, 0, 5); p.values[ModRing] = .1; p.values[ModPitch] = linPos(12, -36, 36);
        keyFilter(p, 0, 1, 150, .3); keyFilter(p, 1, 5, 300, .6); p.values[FRouting] = 1; p.values[FMix] = .4;
        phrase(p, 0, runs(8, {wob(1, kSixteenthT), wob(1, kEighth), wob(1, kSixteenth, .85), wob(1, kQuarter), wob(2, kEighthT), wob(1, kSixteenthT, .9), wob(1, kThirtySecond, .75)}), TwoBars, false, .05);
        route(p, SrcCurve1, fparam(0, FCutoff), .45); route(p, SrcCurve1, fparam(1, FCutoff), .4); add(p);

        p = bass("Osmium DnB Bass - Mutant Step", "DnB Bass", 174, "Digital sync bass: an eight-step sixteenth timbre sequence against a filter phrase of eighths, triplets and quarters.");
        osc(p, 0, 7, .4, .6, 0, -4, 2, .1); osc(p, 1, 5, .4, .42, 0, 5); keyFilter(p, 0, 1, 180, .32);
        stepper(p, 0, 6, {1, .2, -.6, .8, -.2, .5, -.9, .4}, .15);
        phrase(p, 0, runs(8, {wob(2, kEighth), wob(1, kSixteenthT, .85), wob(1, kQuarter), wob(1, kEighthT), wob(1, kSixteenth), wob(2, kQuarter, 1, .5)}), TwoBars, false, .05);
        route(p, SrcCurve1, fparam(0, FCutoff), .48); route(p, SrcStep1, oparam(0, Position), .25); route(p, SrcStep1, oparam(0, Intensity), .15); add(p);
    }
}

// Wavefront uses the expanded catalog without reinterpreting the original table selectors.
void wave(Patch& p, int o, int table, double position, double level, double pitch = 0,
          double fine = 0, int mode = 0, double warp = 0, double split = 0) {
    osc(p, o, 0, position, level, pitch, fine, mode, warp, split);
    p.values[osmium::tableExtension(o)] = choice(table - 7, 41);
}
void spectral(Patch& p, int o, int warp, double amount, double folding = 0) {
    p.values[osmium::warpExtension(o)] = choice(warp, 6);
    p.values[oparam(o, Intensity)] = amount;
    p.values[osmium::fold(o)] = folding;
}
void mod(Patch& p, int source, int param, double amount) {
    for (auto& r : p.mods) if (r.source == source && r.param == param) { r.amount = amount; return; }
    route(p, source, param, amount);
}
Patch keys(const char* name, const char* description) {
    auto p = base(name, "Keys", description);
    p.values[Volume] = linPos(6, -24, 6); p.values[VelAmp] = .7; p.values[KeyTrack] = .3;
    p.values[Unison] = 0; p.values[Spread] = .35; p.values[OscRestart] = 1;
    p.mods.removeIf([](const ModRoute& r) { return r.source == SrcMacro4; }); mod(p, SrcMacro4, fx(ChorusMix), .24);
    filter(p, 0, 0, 5200, .1); filter(p, 1, 0, 8500, .08); p.values[FMix] = .2;
    env(p, 0, .002, 1.0, .06, .35); env(p, 1, .001, .22, 0, .12);
    mod(p, SrcEnv2, fparam(0, FCutoff), .18); mod(p, SrcVelocity, fparam(0, FCutoff), .12);
    space(p, .1, .13, .54, .08, 2); drive(p, .025, 1, .025);
    return p;
}
Patch motion(const char* name, const char* description) {
    auto p = base(name, "Motion", description);
    p.values[Volume] = linPos(4, -24, 6); p.values[Unison] = choice(1, 4);
    p.values[Spread] = .6; p.values[Detune] = .12; p.values[OscRestart] = 1;
    filter(p, 0, 0, 1900, .16); filter(p, 1, 0, 5400, .1); p.values[FMix] = .18;
    env(p, 0, .009, .35, .88, .22); env(p, 1, .003, .3, .12, .15);
    space(p, .12, .09, .5, .12, 2); drive(p, .08, 1, .06);
    return p;
}
void appendWavefrontBank(std::vector<Patch>& bank) {
    auto add = [&](Patch p) { bank.push_back(std::move(p)); };
    // Eight pads: independent scan rates, distinct spectral cores and restrained low end.
    {
        auto p = pad("Osmium Pad - Opaline", "Velvet saw and triangle bloom. Open C3-C5 chords; Warp reveals the upper glass layer.");
        wave(p, 0, 8, .22, .49, 0, -5); wave(p, 1, 11, .35, .38, 0, 4, 0, .02, .25);
        wave(p, 2, 19, .16, .12, 12, 0, 0, 0, .65); spectral(p, 0, 2, .035, .015); filter(p, 0, 0, 3200, .08);
        env(p, 0, .55, 2.3, .8, 1.9, -.3); mod(p, SrcLfo2, oparam(2, Position), .1); space(p, .25, .23, .74, .11, 4); mod(p, SrcLfo2, osmium::fold(0), .015); add(p);
        p = pad("Osmium Pad - Polychrome", "Slow harmonic colour over a stable warm fundamental. Broad chords without a fixed fifth."); p.values[Volume] = linPos(0, -24, 6);
        wave(p, 0, 16, .12, .45, 0, -3); wave(p, 1, 15, .5, .32, 0, 5, 0, .03, .4);
        wave(p, 2, 9, .25, .15, -12); spectral(p, 0, 1, .08); phrase(p, 0, {{0,.1,0,1},{.35,.85,0,1},{.8,.25,0,1},{1,.1}}, 0, false, .28);
        mod(p, SrcCurve1, oparam(1, Position), .22); filter(p, 0, 0, 4100, .09); env(p, 0, .9, 2.5, .75, 2.2); add(p);
        p = pad("Osmium Pad - Velvet Current", "Wide vintage stack with slow opposing PWM movement. A warm bed for sparse drums.");
        wave(p, 0, 9, .18, .48, 0, -6); wave(p, 1, 10, .35, .35, 0, 6, 0, .025, .2);
        wave(p, 2, 41, .12, .12, -12); filter(p, 0, 1, 1750, .12); mod(p, SrcLfo2, oparam(1, Position), -.16);
        env(p, 0, .4, 1.7, .86, 1.4); space(p, .3, .15, .58, .06); add(p);
        p = pad("Osmium Pad - Lumen Choir", "Airy choir and whisper formants. Pressure gently opens the vowel; long held upper voicings.");
        wave(p, 0, 23, .25, .48, 0, -4); wave(p, 1, 22, .4, .33, 0, 5, 0, .04, .35);
        wave(p, 2, 11, .12, .15, -12); spectral(p, 0, 3, .05); filter(p, 0, 0, 4800, .08); p.values[NoiseLevel] = .012;
        mod(p, SrcPressure, oparam(0, Position), .2); mod(p, SrcLfo2, oparam(1, Position), -.1);
        env(p, 0, .8, 2.1, .83, 2.1); space(p, .23, .25, .82, .08, 4); add(p);
        p = pad("Osmium Pad - Airfield", "Bowed air-string canopy with a quiet reed centre. Slow attack and a smooth tonal tail.");
        wave(p, 0, 47, .28, .46, 0, -3); wave(p, 1, 44, .15, .32, 0, 4, 0, .02, .3);
        wave(p, 2, 15, .08, .12, 12); env(p, 0, 1.1, 2.5, .8, 2.0); filter(p, 0, 0, 3500, .1);
        lfo(p, 0, 8, 0, false, .085, 1.3); space(p, .29, .23, .75, .10, 4); add(p);
        p = pad("Osmium Pad - Glacier Bloom", "Icy spectral comb softens into an even-harmonic body. Tone moves from mist to bright ice.");
        wave(p, 0, 17, .11, .43, 0, -2); wave(p, 1, 15, .3, .37, 0, 3, 0, .01, .3);
        wave(p, 2, 31, .08, .12, 12); filter(p, 0, 0, 2600, .12); env(p, 1, 1.8, 2.2, .5, 1.6);
        mod(p, SrcEnv2, oparam(0, Position), .32); space(p, .2, .27, .8, .12, 2); add(p);
        p = pad("Osmium Pad - Soft Architecture", "Soft-fold triangle pad with a mellow organ halo. Fast enough to answer a chord progression.");
        wave(p, 0, 12, .14, .5, 0, -3); wave(p, 1, 46, .18, .28, 0, 4, 0, .025, .4);
        wave(p, 2, 8, .2, .13, 12); spectral(p, 0, 2, .06, .025); env(p, 0, .22, 1.4, .86, 1.1); filter(p, 0, 0, 2900, .09);
        mod(p, SrcLfo1, oparam(0, Position), .07); space(p, .18, .14, .62, .07); add(p);
        p = pad("Osmium Pad - Noctilucent", "Dark vowel pad with a slowly opening glass ribbon. Two-bar timbre movement leaves the pitch steady.");
        wave(p, 0, 20, .15, .44, 0, -4); wave(p, 1, 19, .1, .32, 0, 5, 0, .02, .5);
        wave(p, 2, 41, .12, .15, -12); spectral(p, 1, 1, .045); filter(p, 0, 1, 1600, .13); phrase(p, 0, {{0,.1,0,1},{.3,.35,0,1},{.65,.95,0,1},{1,.1}}, 1, false, .25);
        mod(p, SrcCurve1, oparam(1, Position), .35); env(p, 0, .75, 2.2, .78, 1.9); space(p, .24, .24, .78, .09, 4); add(p);
    }
    // Eight leads: varied articulation and wave families, no transposition in the macros.
    {
        auto p = lead("Osmium Lead - Arc Flash", "Focused sync sweep with a sharp envelope bite. Short legato phrases around C3-C5.");
        wave(p, 0, 13, .2, .62, 0, -3, 2, .12); wave(p, 1, 8, .3, .29, 0, 4, 0, .02, .2); spectral(p, 0, 1, .14, .025);
        mod(p, SrcEnv2, oparam(0, Position), .2); filter(p, 0, 1, 2700, .18); drive(p, .16, 1, .12); add(p);
        p = lead("Osmium Lead - Talk Circuit", "Expressive talkbox lead with a soft low octave. Wheel opens the mouth without bending pitch.");
        wave(p, 0, 24, .22, .62, 0, -2); wave(p, 1, 41, .18, .22, -12, 0, 0, .02, .15); spectral(p, 0, 3, .08);
        mod(p, SrcWheel, oparam(0, Position), .42); filter(p, 0, 0, 5200, .12); env(p, 0, .012, .35, .76, .2);
        p.values[Glide] = expPos(.075, .001, 2); space(p, .08, .1, .48, .15, 4); add(p);
        p = lead("Osmium Lead - Pixel Lance", "Bit-fold attack over a rounded pulse. A dry digital lead that cuts through dense percussion.");
        wave(p, 0, 32, .14, .56, 0, -3); wave(p, 1, 35, .28, .3, 0, 4, 0, .04, .2); spectral(p, 0, 4, .08);
        env(p, 0, .003, .21, .48, .11); filter(p, 0, 1, 3600, .14); drive(p, .1, 2, .08);
        space(p, .04, .06, .35, .1, 1); mod(p, SrcEnv2, oparam(0, Position), -.1); add(p);
        p = lead("Osmium Lead - Ion Needle", "FM-prism lead with a clean fundamental and a bright transient. Pressure adds spectral motion.");
        wave(p, 0, 36, .18, .62); wave(p, 1, 11, .25, .22, 12, 3, 0, 0, .3); p.values[ModPhase] = .025; spectral(p, 0, 5, .11);
        mod(p, SrcEnv2, oparam(0, Position), .21); mod(p, SrcPressure, oparam(0, Intensity), .12);
        filter(p, 0, 0, 4700, .13); space(p, .07, .13, .54, .17, 2); add(p);
        p = lead("Osmium Lead - Hollow Beacon", "Warm hollow-sweep lead with a singing band-pass layer. Rounded onset for melodic answers.");
        wave(p, 0, 18, .26, .62, 0, -3); wave(p, 1, 44, .2, .26, 0, 4, 0, .03, .45);
        filter(p, 0, 0, 3200, .13); filter(p, 1, 3, 1800, .22); p.values[FMix] = .24;
        env(p, 0, .025, .4, .82, .22); p.values[Glide] = expPos(.09, .001, 2); space(p, .12, .13, .58, .13, 4); add(p);
        p = lead("Osmium Lead - Sawtooth Relay", "Teeth-sweep lead with a one-bar timbre accent. Bright attack and a small stereo echo.");
        wave(p, 0, 42, .14, .56, 0, -3, 0, .07); wave(p, 1, 9, .16, .32, 0, 4, 0, 0, .2); spectral(p, 0, 2, .11, .035);
        phrase(p, 0, {{0,.1,-.5},{.12,.85,-.3},{.4,.15,0,1},{.75,.5,0,1},{1,.1}}, 2);
        mod(p, SrcCurve1, oparam(0, Position), .2); filter(p, 0, 1, 2700, .17); drive(p, .2, 1, .13); add(p);
        p = lead("Osmium Lead - Pulse Ribbon", "Animated PWM lead with opposing oscillator scans. Smooth enough for long portamento lines.");
        wave(p, 0, 10, .28, .58, 0, -4); wave(p, 1, 10, .58, .3, 0, 4, 0, .02, .25); spectral(p, 0, 1, .09);
        lfo(p, 0, 8, 0, false, .31, .25); mod(p, SrcLfo1, oparam(0, Position), .11); mod(p, SrcLfo1, oparam(1, Position), -.1);
        filter(p, 0, 0, 3400, .15); env(p, 0, .016, .4, .78, .2); space(p, .13, .11, .48, .12, 2); add(p);
        p = lead("Osmium Lead - Copper Signal", "Organ-bloom lead with a metallic octave edge. Velocity controls the bite of short notes.");
        wave(p, 0, 46, .3, .59); wave(p, 1, 27, .08, .22, 12, 2, 0, .02, .3); spectral(p, 0, 5, .06);
        mod(p, SrcVelocity, oparam(1, Level), .1); env(p, 0, .004, .3, .55, .17); filter(p, 0, 0, 4200, .12);
        drive(p, .08, 1, .06); space(p, .07, .1, .46, .14, 2); add(p);
    }
    // Eight polyphonic keys: velocity-sensitive percussive envelopes and tuned spectral bodies.
    {
        auto p = keys("Osmium Keys - Iron Petal", "Bell-edged pluck with a soft triangle body. Play open C3-C5 arpeggios.");
        wave(p, 0, 26, .16, .62); wave(p, 1, 11, .22, .22, 0, 2, 0, 0, .2); spectral(p, 0, 5, .04);
        env(p, 0, .002, 1.1, .035, .38); mod(p, SrcEnv2, oparam(0, Position), .16); filter(p, 0, 0, 6900, .09); space(p, .07, .17, .6, .12, 2); mod(p, SrcEnv2, osmium::fold(0), .04); add(p);
        p = keys("Osmium Keys - Chrome Drops", "Bright chime plucks with short repeating tails. Upper-register answers and liquid arpeggios.");
        wave(p, 0, 31, .12, .64); wave(p, 1, 19, .08, .18, 12, 0, 0, 0, .35);
        env(p, 0, .0015, .65, 0, .25); env(p, 1, .001, .12, 0, .1); filter(p, 0, 0, 7600, .08);
        space(p, .05, .13, .54, .23, 2); add(p);
        p = keys("Osmium Keys - Hollow Timber", "Rounded wood-like mallet with a short hollow knock. Strong velocity response, little wash.");
        wave(p, 0, 45, .25, .7); wave(p, 1, 41, .12, .17, -12); env(p, 0, .002, .43, 0, .16);
        filter(p, 0, 0, 3400, .12); mod(p, SrcEnv2, oparam(0, Position), .2); space(p, .025, .075, .37, .055, 1); add(p);
        p = keys("Osmium Keys - Reed Electric", "Soft reed electric keys with a warm sustained body. Velocity opens a gentle bark.");
        wave(p, 0, 44, .18, .64); wave(p, 1, 15, .12, .2, 0, 3, 0, .03, .25);
        env(p, 0, .004, 1.5, .22, .4); mod(p, SrcVelocity, oparam(0, Position), .18); filter(p, 0, 0, 3100, .1);
        space(p, .17, .09, .45, .06, 4); drive(p, .07, 1, .05); add(p);
        p = keys("Osmium Keys - Odd Ivory", "Odd-harmonic tine keys with a clear middle register. Short chords retain a dry attack.");
        wave(p, 0, 14, .2, .66); wave(p, 1, 11, .12, .17, 12, 0, 0, 0, .3);
        env(p, 0, .003, 1.2, .12, .32); filter(p, 0, 0, 4400, .09); mod(p, SrcEnv2, oparam(0, Intensity), .05);
        space(p, .14, .12, .5, .08, 2); add(p);
        p = keys("Osmium Keys - Alloy Harp", "Metallic harp pluck with a brief spectral scrape. Sparse chords and descending patterns.");
        wave(p, 0, 29, .08, .61); wave(p, 1, 8, .12, .22, 0, -2, 0, 0, .2); spectral(p, 0, 2, .035, .015);
        env(p, 0, .001, .8, 0, .23); env(p, 1, .001, .16, 0, .09); mod(p, SrcEnv2, oparam(0, Position), .22);
        filter(p, 0, 0, 5700, .11); space(p, .06, .15, .59, .16, 4); add(p);
        p = keys("Osmium Keys - Steel Pluck", "Tight steel-wire pluck with a rounded low body. Fast repeating patterns without a long tail.");
        wave(p, 0, 28, .18, .64); wave(p, 1, 45, .1, .18, -12, 0, 0, .02, .2);
        env(p, 0, .0015, .36, 0, .13); filter(p, 0, 0, 4800, .1); mod(p, SrcEnv2, oparam(0, Position), -.12);
        space(p, .04, .07, .36, .09, 1); add(p);
        p = keys("Osmium Keys - Binary Musicbox", "Digital staircase pluck with an octave glint. Soft played notes stay round; hard notes sparkle.");
        wave(p, 0, 34, .11, .6); wave(p, 1, 31, .1, .18, 12, 0, 0, 0, .4); spectral(p, 0, 4, .1);
        env(p, 0, .002, .72, .02, .25); mod(p, SrcVelocity, oparam(0, Position), .18); filter(p, 0, 0, 5800, .08);
        space(p, .08, .15, .54, .16, 2); add(p);
    }
    // Eight basses: keep the fundamental firm while the expanded upper spectra move.
    {
        auto p = reese("Osmium Bass - Faultline", "Dense Reese Core bass with opposing slow scan motion. Play C1-C2; Body reinforces the dry centre."); p.category = "Bass";
        wave(p, 0, 38, .2, .56, 0, -8); wave(p, 1, 38, .43, .4, 0, 9, 0, .04, .2); spectral(p, 0, 2, .08, .04);
        filter(p, 0, 1, 1050, .19); drive(p, .28, 1, .2); space(p, .1, .015, .3); mod(p, SrcLfo2, osmium::fold(0), .025); add(p);
        p = wobble("Osmium Bass - Rubber Valve", "Rounded growl with a curved quarter-note opening. A centered sub anchors the moving upper layer."); p.category = "Bass";
        wave(p, 0, 39, .12, .58, 0, -3, 0, .08); wave(p, 1, 41, .15, .25, 0, 2, 0, 0, .15); spectral(p, 0, 3, .13, .025);
        phrase(p, 0, {{0,.1,-.6},{.18,1,-.4},{.65,.12,0,1},{1,.1}}, 4); filter(p, 0, 1, 280, .21);
        drive(p, .19, 1, .14); add(p);
        p = sub("Osmium Bass - Acid Reservoir", "Hollow acid bass with a tight resonant envelope. Use short notes; no tempo gate is baked in."); p.category = "Bass";
        wave(p, 0, 40, .14, .78); wave(p, 1, 41, .06, .18, -12); spectral(p, 0, 2, .08, .025); filter(p, 0, 1, 380, .25);
        env(p, 0, .003, .35, .35, .07); env(p, 1, .001, .22, .025, .08); mod(p, SrcEnv2, fparam(0, FCutoff), .34);
        mod(p, SrcEnv2, oparam(0, Position), .18); drive(p, .17, 1, .1); add(p);
        p = sub("Osmium Bass - Deep Alloy", "Rounded Sub core with a quiet metallic overtone. Sustained low notes with a short, clean release."); p.category = "Bass";
        wave(p, 0, 41, .2, .78); wave(p, 1, 26, .06, .14, 12, 0, 0, .01, .1); spectral(p, 0, 5, .035);
        filter(p, 0, 1, 650, .08); env(p, 0, .007, .45, .86, .09); drive(p, .12, 1, .06); add(p);
        p = reese("Osmium Bass - Teeth Machine", "Hard-edged teeth sweep over a stable lower octave. Short envelope accents cut through a break."); p.category = "Bass";
        wave(p, 0, 42, .16, .52, 0, -5, 2, .1); wave(p, 1, 38, .2, .36, 0, 6, 0, .03, .2); spectral(p, 0, 1, .14, .035);
        filter(p, 0, 1, 1350, .2); env(p, 1, .002, .24, .05, .1); mod(p, SrcEnv2, oparam(0, Position), .24);
        drive(p, .3, 2, .14); space(p, .07, 0, .3); add(p);
        p = wobble("Osmium Bass - Neuro Bloom", "Layered neuro edge with alternating long and short vowel openings. One-bar phrase, stable sub pitch."); p.category = "Bass";
        wave(p, 0, 43, .15, .54, 0, -4, 0, .1); wave(p, 1, 21, .18, .29, 0, 5, 0, .03, .25); spectral(p, 0, 5, .09, .025);
        phrase(p, 0, {{0,.08,-.7},{.1,.9,-.4},{.38,.1,0,1},{.5,.15,-.6},{.6,.75,.25},{.9,.1},{1,.08}}, 2);
        mod(p, SrcCurve1, oparam(1, Position), -.2); filter(p, 0, 1, 330, .23); drive(p, .28, 1, .18); mod(p, SrcCurve1, osmium::fold(0), .045); add(p);
        p = reese("Osmium Bass - Nasal Pressure", "Narrow reed growl with a wider filtered upper layer. Slow movement for long bass notes."); p.category = "Bass";
        wave(p, 0, 25, .2, .55, 0, -7); wave(p, 1, 40, .2, .34, 0, 8, 0, .04, .25); spectral(p, 0, 3, .15, .03);
        filter(p, 0, 1, 900, .18); mod(p, SrcLfo1, oparam(0, Position), .14); p.values[SubLevel] = .26;
        drive(p, .23, 1, .18); space(p, .09, .01, .3); add(p);
        p = wobble("Osmium Bass - Pixel Hydraulic", "Punchy digital pulse bass with staggered eighth-note movement. The top stays dry and focused."); p.category = "Bass";
        wave(p, 0, 35, .19, .58, 0, -3); wave(p, 1, 41, .13, .25, 0, 2, 0, 0, .1); spectral(p, 0, 4, .07);
        phrase(p, 0, {{0,.9,-.5},{.35,.1,0,2},{.55,.7,-.4},{.9,.05},{1,.9}}, 3, true, .06);
        filter(p, 0, 1, 260, .2); drive(p, .23, 2, .12); space(p, .03, 0, .3, .02, 1); add(p);
    }
    // Eight motion patches: drawn phrases, step accents and filter rhythm remain tempo-locked.
    {
        auto p = motion("Osmium Motion - Comb Runner", "Spectral-comb pulse with alternating sixteenth-note accents. Hold C2-C4 notes or open fifths.");
        wave(p, 0, 17, .15, .57); wave(p, 1, 8, .2, .28, 0, 3, 0, .02, .25); spectral(p, 0, 1, .09);
        phrase(p, 0, {{0,.1,-.7},{.12,1,-.6},{.45,.12,0,2},{.55,.65,-.5},{.9,.05},{1,.1}}, 4);
        mod(p, SrcCurve1, fparam(0, FCutoff), .3); mod(p, SrcCurve1, oparam(0, Position), .23); filter(p, 0, 0, 650, .18); add(p);
        p = motion("Osmium Motion - Throat Sequencer", "Eight-step vocal pattern with gliding formant changes. Step motion shapes timbre, never note pitch.");
        wave(p, 0, 21, .38, .56); wave(p, 1, 41, .15, .24, -12, 0, 0, .01, .15);
        p.values[sparam(0, SCount)] = 7.0 / 15; p.values[sparam(0, SGlide)] = .19;
        const double throat[] = {.15,.85,.3,.6,.95,.25,.7,.4}; for (int i = 0; i < 16; ++i) p.values[step(0,i)] = throat[i%8];
        mod(p, SrcStep1, oparam(0, Position), .25); mod(p, SrcStep1, fparam(0, FCutoff), .16);
        filter(p, 0, 0, 2200, .15); space(p, .06, .07, .4, .1, 2); add(p);
        p = motion("Osmium Motion - Phase Conveyor", "Phase-lattice ripple with a smooth one-bar timbre conveyor. Gentle enough for sustained chords.");
        wave(p, 0, 33, .14, .5); wave(p, 1, 15, .3, .3, 0, 4, 0, .03, .35); spectral(p, 0, 1, .11, .02);
        phrase(p, 0, {{0,.05,0,1},{.3,.85,0,1},{.55,.3,0,1},{.8,.7,0,1},{1,.05}}, 2, false, .2);
        mod(p, SrcCurve1, oparam(0, Position), .3); mod(p, SrcCurve1, fparam(0, FCutoff), .13);
        env(p, 0, .08, .6, .82, .5); space(p, .17, .13, .58, .14, 4); add(p);
        p = motion("Osmium Motion - Shard Engine", "Hard digital shards in a syncopated bar. Fast envelope and sparse echo keep each accent distinct.");
        wave(p, 0, 37, .1, .5, 0, -2); wave(p, 1, 35, .2, .28, 0, 3, 0, .02, .2); spectral(p, 0, 2, .08, .025);
        phrase(p, 0, {{0,1,-.8},{.12,.12,0,2},{.36,.8,-.7},{.5,.08,0,2},{.74,.9,-.7},{1,1}}, 2, true, .04);
        mod(p, SrcCurve1, oparam(0, Position), .32); mod(p, SrcCurve1, fparam(0, FCutoff), .34);
        filter(p, 0, 1, 750, .16); env(p, 0, .003, .25, .9, .12); drive(p, .14, 2, .07); space(p, .04, .04, .32, .08, 1); mod(p, SrcCurve1, osmium::fold(0), .08); add(p);
        p = motion("Osmium Motion - Ribbon Gate", "Glass-ribbon pad with a soft eighth-note gate. A second slow curve changes the brightness across two bars.");
        wave(p, 0, 19, .12, .52); wave(p, 1, 11, .3, .28, 0, -3, 0, 0, .3);
        phrase(p, 0, {{0,.1,-.6},{.15,1,-.4},{.8,.12},{1,.1}}, 5); phrase(p, 1, {{0,.15,0,1},{.7,.95,0,1},{1,.15}}, 1, false, .25);
        mod(p, SrcCurve1, oparam(0, Level), .25); mod(p, SrcCurve1, fparam(0, FCutoff), .23); mod(p, SrcCurve2, oparam(0, Position), .3);
        filter(p, 0, 0, 950, .12); env(p, 0, .06, .7, .82, .6); space(p, .16, .14, .58, .13, 2); add(p);
        p = motion("Osmium Motion - Plate Ripple", "Bowed-plate shimmer with triplet accents and a warm lower layer. Useful behind half-time drums.");
        wave(p, 0, 30, .14, .47); wave(p, 1, 8, .18, .32, -12, 2, 0, .01, .25); spectral(p, 0, 5, .055);
        lfo(p, 0, 7, 1, true); mod(p, SrcLfo1, oparam(0, Position), .21); mod(p, SrcLfo1, fparam(0, FCutoff), .16);
        filter(p, 0, 0, 2700, .13); env(p, 0, .018, .7, .75, .4); space(p, .11, .16, .63, .15, 4); add(p);
        p = motion("Osmium Motion - Copper Steps", "Copper-gong ostinato with four contrasting timbre steps. Hold notes; the sequence follows project tempo.");
        wave(p, 0, 27, .2, .53); wave(p, 1, 45, .15, .26, 0, 2, 0, .01, .2); spectral(p, 0, 3, .06);
        p.values[sparam(0, SCount)] = 3.0 / 15; p.values[sparam(0, SGlide)] = .1; p.values[sparam(0, SRate)] = choice(5,8);
        const double copper[] = {.85,.25,.65,.12}; for (int i = 0; i < 16; ++i) p.values[step(0,i)] = copper[i%4];
        mod(p, SrcStep1, oparam(0, Position), .22); mod(p, SrcStep1, fparam(0, FCutoff), .18); mod(p, SrcStep1, oparam(0, Level), .13);
        filter(p, 0, 0, 2000, .14); space(p, .08, .1, .5, .14, 2); add(p);
        p = motion("Osmium Motion - Vowel Carousel", "Smooth two-bar vowel carousel with a quiet octave bloom. Slow chord movement and sustained notes.");
        wave(p, 0, 20, .16, .52); wave(p, 1, 23, .25, .27, 12, 4, 0, .02, .4);
        phrase(p, 0, {{0,.1,0,1},{.25,.7,0,1},{.5,.3,0,1},{.75,.95,0,1},{1,.1}}, 1, false, .22);
        mod(p, SrcCurve1, oparam(0, Position), .38); mod(p, SrcCurve1, oparam(1, Position), -.15);
        filter(p, 0, 0, 3800, .12); env(p, 0, .16, .9, .8, .65); space(p, .18, .16, .62, .09, 4); add(p);
    }
    // Eight textures: harmonic rather than sample-based, with deliberate beginnings and tails.
    {
        auto p = pad("Osmium Texture - Bowed Horizon", "Bowed plate and air strings form a slow cinematic swell. Hold open upper chords; p.values[Volume] = linPos(-1, -24, 6); keep the low register sparse."); p.category = "Texture";
        wave(p, 0, 30, .1, .42, 0, -3); wave(p, 1, 47, .25, .35, 0, 4, 0, .02, .4); wave(p, 2, 11, .2, .12, -12);
        env(p, 0, 1.2, 2.6, .8, 2.2); env(p, 1, 1.8, 2.7, .45, 2); filter(p, 0, 0, 3400, .1);
        mod(p, SrcEnv2, oparam(0, Position), .28); space(p, .24, .28, .84, .1, 4); add(p);
        p = pad("Osmium Texture - Whisper Matrix", "Whisper formant and phase lattice drift in opposite directions. Air without a noisy unpitched layer."); p.values[Volume] = linPos(-1, -24, 6); p.category = "Texture";
        wave(p, 0, 22, .2, .44, 0, -4); wave(p, 1, 33, .18, .3, 0, 4, 0, .02, .45); wave(p, 2, 15, .12, .13, -12);
        lfo(p, 0, 8, 0, false, .071, 1.4); lfo(p, 1, 8, 1, false, .11, 1.8); mod(p, SrcLfo2, oparam(1, Position), -.17);
        filter(p, 0, 0, 4200, .09); env(p, 0, .95, 2.2, .78, 2); space(p, .28, .26, .8, .09, 4); add(p);
        p = pad("Osmium Texture - Oxide Garden", "Soft-fold and copper spectra grow into a dark metallic bloom. Pressure adds the brighter edge."); p.category = "Texture";
        wave(p, 0, 12, .2, .48, 0, -3); wave(p, 1, 27, .1, .28, 0, 4, 0, .02, .35); wave(p, 2, 41, .12, .15, -12); spectral(p, 0, 2, .05, .02);
        filter(p, 0, 1, 1550, .16); env(p, 1, 1.3, 2.3, .5, 1.8); mod(p, SrcEnv2, oparam(1, Position), .22);
        mod(p, SrcPressure, fparam(0, FCutoff), .16); env(p, 0, .65, 2, .82, 1.8); space(p, .2, .22, .75, .06, 2); mod(p, SrcEnv2, osmium::fold(0), .06); add(p);
        p = pad("Osmium Texture - Frozen Relay", "Glass ribbon and binary staircase glint above a quiet triangle. Slowly stepped colour, no random tuning."); p.category = "Texture";
        wave(p, 0, 19, .15, .43, 0, -2); wave(p, 1, 34, .1, .25, 12, 2, 0, 0, .5); wave(p, 2, 11, .15, .19, -12);
        phrase(p, 0, {{0,.2,0,2},{.2,.65,0,2},{.45,.35,0,2},{.7,.85,0,2},{1,.2}}, 0, false, .3);
        mod(p, SrcCurve1, oparam(1, Position), .28); filter(p, 0, 0, 3300, .1); env(p, 0, .75, 2.2, .78, 2.1); space(p, .22, .29, .83, .13, 4); add(p);
        p = pad("Osmium Texture - Sunken Organ", "Low organ bloom with a hollow wood overtone. A short cinematic tail keeps slow harmony readable."); p.category = "Texture";
        wave(p, 0, 46, .18, .5, 0, -3); wave(p, 1, 45, .12, .26, 0, 3, 0, .03, .3); wave(p, 2, 41, .1, .16, -12);
        filter(p, 0, 1, 1200, .12); env(p, 0, .3, 1.6, .86, 1.2); lfo(p, 0, 8, 0, false, .14, .9);
        mod(p, SrcLfo1, oparam(0, Position), .08); space(p, .17, .16, .6, .07, 4); add(p);
        p = pad("Osmium Texture - Alloy Atmosphere", "Alloy-cluster mist over a soft warm core. The spectral layer moves slowly while the bass stays still."); p.values[Volume] = linPos(-1, -24, 6); p.category = "Texture";
        wave(p, 0, 29, .08, .37, 0, -2); wave(p, 1, 8, .17, .39, 0, 3, 0, 0, .3); wave(p, 2, 11, .1, .13, -12);
        mod(p, SrcLfo1, oparam(0, Position), .14); filter(p, 0, 0, 2900, .1); env(p, 0, 1.1, 2.3, .77, 2.1);
        space(p, .26, .27, .81, .1, 2); drive(p, .025, 1, .03); add(p);
        p = pad("Osmium Texture - Ember Choir", "Warm throat and choir air with a slow envelope opening. A breathy cinematic chord bed without pitch drift."); p.category = "Texture";
        wave(p, 0, 21, .12, .43, 0, -4); wave(p, 1, 23, .2, .32, 0, 4, 0, .02, .4); wave(p, 2, 15, .08, .15, -12);
        filter(p, 0, 0, 3100, .1); env(p, 1, 1.6, 2.4, .5, 1.8); mod(p, SrcEnv2, oparam(0, Position), .3);
        env(p, 0, .85, 2.2, .81, 2); space(p, .23, .25, .79, .08, 4); add(p);
        p = pad("Osmium Texture - Prism Wake", "Slow FM-prism flare dissolves into an even-harmonic tail. Hold upper chords and let the opening settle."); p.category = "Texture";
        wave(p, 0, 36, .08, .42, 0, -2); wave(p, 1, 15, .25, .35, 0, 3, 0, 0, .35); wave(p, 2, 47, .14, .13, 12); spectral(p, 0, 5, .07);
        phrase(p, 0, {{0,.05,-.2},{.35,.95,0,1},{1,.12}}, 0, true, .2); p.values[cparam(0,CMode)] = 1;
        mod(p, SrcCurve1, oparam(0, Position), .32); mod(p, SrcCurve1, fparam(0, FCutoff), .12);
        filter(p, 0, 0, 2600, .12); env(p, 0, 1.0, 2.5, .8, 2.2); space(p, .25, .27, .82, .11, 4); add(p);
    }
}

// Gesture Bass: authored non-looping contours, never a periodic LFO or step sequence.
// The two independent one-shots reshape harmonics, while the amp and sub remain solid.
void gesture(Patch& p, const Points& mouth, const Points& grain, int mouthRate = 3, int grainRate = 2) {
    phrase(p, 0, mouth, mouthRate, true, .075); phrase(p, 1, grain, grainRate, true, .11);
    p.values[cparam(0, CMode)] = 1; p.values[cparam(1, CMode)] = 1;
}
Patch fatBass(const char* name, const char* description, int mainTable, int colourTable, double cutoff) {
    auto p = base(name, "Gesture Bass", description);
    p.values[Volume] = linPos(5, -24, 6); p.values[VoiceMode] = choice(1, 3); // mono, retrigger even on overlaps
    p.values[Glide] = expPos(.026, .001, 2); p.values[OscRestart] = 1; p.values[VelAmp] = .28;
    p.values[Unison] = 0; p.values[Detune] = .12; p.values[Spread] = .35;
    wave(p, 0, mainTable, .16, .66, 0, -2); wave(p, 1, colourTable, .21, .34, 0, 3, 0, 0, .42);
    osc(p, 2, 4, .03, .24); p.values[SubLevel] = .22;
    filter(p, 0, 1, cutoff, .13); filter(p, 1, 0, 3100, .09); p.values[FMix] = .18;
    env(p, 0, .006, .38, .91, .12, -.25, -.55, -.6);
    env(p, 1, .003, .17, .08, .11, -.6, -.65, -.45);
    env(p, 2, .19, 1.37, .14, .23, .35, -.45, -.55);
    env(p, 3, .002, .071, 0, .08, -.6, -.7, -.5);
    mod(p, SrcCurve1, fparam(0, FCutoff), .31); mod(p, SrcCurve1, oparam(0, Position), .36);
    mod(p, SrcCurve2, oparam(1, Position), .44); mod(p, SrcCurve2, fparam(1, FCutoff), -.13);
    mod(p, SrcCurve2, fparam(0, FCutoff), -.115); // independent countergesture remains audible on dark patches
    mod(p, SrcEnv2, fparam(0, FCutoff), .12); mod(p, SrcEnv3, osmium::fold(0), .065);
    mod(p, SrcEnv4, oparam(1, Intensity), .08);
    mod(p, SrcVelocity, fparam(0, FCutoff), .055); mod(p, SrcPressure, osmium::fold(1), .14);
    mod(p, SrcRandom, oparam(1, Position), .028); // one small colour offset per note, no random clock
    p.mods.removeIf([](const ModRoute& r) { return r.source == SrcMacro4; }); mod(p, SrcMacro4, fx(ChorusMix), .18);
    drive(p, .16, 1, .10); space(p, 0, 0, .32); // optional space/echo are macros, never the default motion
    return p;
}
void appendGestureBank(std::vector<Patch>& bank) {
    auto add = [&](Patch p) { bank.push_back(std::move(p)); };
    {
        auto p = fatBass("Osmium Bass - Rift", "Thick reese with an early snarl, a late shoulder and a soft landing. Hold A1; two unequal one-shot contours.", 34, 38, 390);
        p.values[Unison] = choice(1, 4); p.values[Spread] = .42; spectral(p, 0, 3, .08, .025);
        gesture(p, {{0,.12,-.6,1},{.041,.88,.5,1},{.183,.3,-.35,1},{.417,.73,.3,1},{.594,.2,-.25,1},{.861,.42,.25,1},{1,.25}},
                   {{0,.7,.45,1},{.137,.18,-.35,1},{.392,.82,.6,1},{.731,.32,-.4,1},{1,.46}}, 3, 2); add(p);
        p = fatBass("Osmium Bass - Silt", "Round low bass with a swallowed attack and uneven liquid rebounds. Gentle velocity opens its warm upper layer.", 9, 35, 230);
        spectral(p, 0, 1, .11); p.values[SubLevel] = .29; env(p, 1, .018, .31, .04, .09, .4);
        gesture(p, {{0,.6,.55,1},{.093,.08,-.4,1},{.271,.79,.45,1},{.349,.38,-.6,1},{.683,.55,.2,1},{1,.16}},
                   {{0,.1,-.3,1},{.219,.68,.5,1},{.583,.23,-.2,1},{.917,.52,.3,1},{1,.31}}, 3, 1); add(p);
        p = fatBass("Osmium Bass - Magma Mouth", "Vocal low-mid growl with a fast jaw opening, short catch and slow closing throat. Pressure adds grain.", 20, 37, 460);
        spectral(p, 0, 3, .15, .05); spectral(p, 1, 1, .08); filter(p, 1, 3, 1700, .17);
        gesture(p, {{0,.08,-.55,1},{.027,.96,.6,1},{.148,.36,-.35,1},{.223,.69,.35,1},{.561,.21,-.3,1},{.803,.47,.6,1},{1,.18}},
                   {{0,.28,.45,1},{.086,.77,.1,1},{.313,.39,-.55,1},{.652,.89,.65,1},{1,.24}}, 3, 2); add(p);
        p = fatBass("Osmium Bass - Boulder", "Dense restrained bass with a heavy centre and three differently shaped shoulders. No amplitude chopping.", 8, 33, 275);
        p.values[SubLevel] = .3; drive(p, .23, 2, .13); osc(p, 2, 4, .06, .31);
        gesture(p, {{0,.2,-.4,1},{.063,.72,.45,1},{.287,.35,-.65,1},{.461,.62,.35,1},{.829,.29,-.2,1},{1,.37}},
                   {{0,.74,.6,1},{.163,.2,-.3,1},{.507,.61,.25,1},{.692,.3,-.4,1},{1,.42}}, 4, 2); add(p);
        p = fatBass("Osmium Bass - Tar Pit", "Dark sticky bass that strains upward before two uneven folds settle into a low held body. Hold for the full change.", 36, 21, 195);
        spectral(p, 0, 2, .09, .035); env(p, 2, .47, 1.91, .12, .2, .6);
        gesture(p, {{0,.06,.55,1},{.237,.81,.35,1},{.381,.41,-.4,1},{.623,.91,.6,1},{.754,.24,-.4,1},{1,.19}},
                   {{0,.44,-.35,1},{.117,.13,.55,1},{.428,.73,-.2,1},{.781,.26,.4,1},{1,.35}}, 2, 1); add(p);
        p = fatBass("Osmium Bass - Loose Jaw", "Rubbery talking bass with staggered jaw and throat gestures. One contour answers the other without repeating.", 22, 35, 360);
        spectral(p, 0, 1, .18); spectral(p, 1, 3, .08); mod(p, SrcCurve2, oparam(0, Intensity), .13);
        gesture(p, {{0,.13,-.5,1},{.074,.93,.5,1},{.193,.19,-.4,1},{.486,.67,.3,1},{.713,.29,-.5,1},{1,.22}},
                   {{0,.66,.5,1},{.231,.16,-.6,1},{.347,.79,.55,1},{.663,.34,-.25,1},{.908,.62,.3,1},{1,.39}}, 3, 3); add(p);
    }
    {
        auto p = fatBass("Osmium Bass - Undertow", "A broad inward swell pulls against a quicker submerged ripple. Smooth low reese for long bass notes.", 11, 34, 310);
        p.values[Unison] = choice(1, 4); p.values[Detune] = .19; p.values[Spread] = .48; env(p, 0, .018, .57, .9, .18);
        gesture(p, {{0,.64,.55,1},{.158,.12,-.35,1},{.439,.83,.5,1},{.617,.34,-.3,1},{.897,.58,.5,1},{1,.27}},
                   {{0,.17,-.55,1},{.048,.77,.4,1},{.286,.23,-.35,1},{.553,.68,.6,1},{1,.32}}, 2, 3); add(p);
        p = fatBass("Osmium Bass - Black Sap", "Warm woody bass with an offset resinous swell and a brief upper crack. The sub stays present throughout.", 46, 9, 255);
        spectral(p, 0, 2, .07, .06); p.values[SubLevel] = .27;
        gesture(p, {{0,.17,-.55,1},{.036,.71,.45,1},{.217,.27,.6,1},{.537,.89,.65,1},{.694,.22,-.4,1},{1,.3}},
                   {{0,.56,.6,1},{.171,.11,-.4,1},{.389,.7,.25,1},{.847,.21,-.45,1},{1,.43}}, 3, 2); add(p);
        p = fatBass("Osmium Bass - Broken Current", "Electrical low bass with a short stutter in colour followed by a long rounded fall. No gated volume or sequenced rhythm.", 28, 39, 450);
        spectral(p, 0, 4, .07); spectral(p, 1, 2, .035, .025); drive(p, .19, 1, .12);
        gesture(p, {{0,.09,-.5,1},{.039,.84,.35,1},{.113,.24,-.6,1},{.159,.71,.45,1},{.406,.37,-.3,1},{.781,.57,.55,1},{1,.2}},
                   {{0,.21,-.3,1},{.194,.76,.6,1},{.361,.28,-.35,1},{.629,.56,.25,1},{1,.34}}, 3, 2); add(p);
        p = fatBass("Osmium Bass - Bent Axle", "Twisted metallic reese with a quick rasp and an off-centre return. A clean bass core supports the moving edge.", 31, 38, 480);
        spectral(p, 0, 3, .11, .04); p.values[Unison] = choice(1, 4); p.values[Detune] = .16;
        gesture(p, {{0,.26,-.55,1},{.057,.91,.5,1},{.246,.22,-.35,1},{.593,.78,.6,1},{.739,.33,-.3,1},{1,.28}},
                   {{0,.8,.6,1},{.121,.19,-.3,1},{.473,.66,.5,1},{.816,.24,-.2,1},{1,.41}}, 3, 1); add(p);
        p = fatBass("Osmium Bass - Cinder Lung", "Breathy furnace growl that inhales slowly and exhales in two unequal gestures. Hold notes to hear the later bloom.", 23, 45, 330);
        spectral(p, 0, 2, .12, .04); p.values[NoiseLevel] = .006; env(p, 2, .31, 1.61, .09, .21, .55);
        gesture(p, {{0,.1,.45,1},{.191,.87,.55,1},{.337,.32,-.45,1},{.718,.76,.6,1},{.842,.25,-.25,1},{1,.18}},
                   {{0,.62,.55,1},{.284,.14,-.45,1},{.493,.78,.35,1},{.763,.33,-.35,1},{1,.39}}, 2, 2); add(p);
        p = fatBass("Osmium Bass - Gravel Throat", "Grainy mid-bass over a round bottom. A bent attack scrapes once, then an independent throat shape takes over.", 37, 30, 420);
        spectral(p, 0, 2, .09, .1); spectral(p, 1, 3, .1); filter(p, 1, 0, 2450, .12);
        gesture(p, {{0,.15,-.65,1},{.023,.96,.65,1},{.137,.29,-.3,1},{.372,.64,.4,1},{.679,.21,-.5,1},{1,.31}},
                   {{0,.12,.4,1},{.143,.53,-.4,1},{.281,.26,.45,1},{.617,.86,.55,1},{1,.28}}, 3, 2); add(p);
    }
    {
        auto p = fatBass("Osmium Bass - Velvet Bruise", "Soft heavy bass with a rounded double shoulder and quiet spectral bruising. Warm rather than abrasive.", 9, 16, 210);
        osc(p, 2, 4, .08, .34); p.values[SubLevel] = .26; spectral(p, 0, 1, .06); drive(p, .11, 2, .07);
        gesture(p, {{0,.22,-.35,1},{.109,.69,.3,1},{.354,.31,-.45,1},{.573,.59,.5,1},{.887,.22,-.2,1},{1,.28}},
                   {{0,.57,.4,1},{.197,.2,-.3,1},{.514,.72,.45,1},{.793,.36,-.2,1},{1,.4}}, 3, 1); add(p);
        p = fatBass("Osmium Bass - Deep Tissue", "Low rounded pressure with a tucked transient and a delayed organic opening. Good for sparse, sustained bass lines.", 33, 46, 185);
        p.values[SubLevel] = .32; spectral(p, 0, 2, .035, .025); env(p, 1, .009, .23, .04, .1);
        gesture(p, {{0,.48,.55,1},{.087,.13,-.4,1},{.321,.59,.45,1},{.489,.25,-.25,1},{.752,.77,.65,1},{1,.24}},
                   {{0,.13,.4,1},{.263,.73,.6,1},{.427,.22,-.35,1},{.831,.57,.4,1},{1,.33}}, 2, 1); add(p);
        p = fatBass("Osmium Bass - Iron Sap", "Heavy singing metal with a soft sub and an irregular falling formant. Small pressure movements reveal the folded layer.", 24, 36, 350);
        spectral(p, 0, 3, .09, .035); spectral(p, 1, 2, .06, .03); wave(p, 1, 36, .18, .29, -12, 0, 0, 0, .35);
        gesture(p, {{0,.16,-.5,1},{.049,.82,.6,1},{.208,.44,-.35,1},{.431,.68,.45,1},{.773,.19,-.4,1},{1,.26}},
                   {{0,.68,.55,1},{.153,.24,-.4,1},{.382,.83,.6,1},{.697,.29,-.3,1},{1,.38}}, 3, 2); add(p);
        p = fatBass("Osmium Bass - Slow Rupture", "A dark sub-backed growl opens late, catches briefly and tears into a warm sustained tail. Hold for several beats.", 38, 20, 170);
        spectral(p, 0, 3, .07, .07); p.values[SubLevel] = .28; env(p, 2, .63, 2.3, .17, .23, .6);
        gesture(p, {{0,.06,.55,1},{.283,.64,.4,1},{.391,.28,-.4,1},{.587,.94,.65,1},{.713,.42,.45,1},{1,.22}},
                   {{0,.37,.6,1},{.183,.12,-.35,1},{.521,.79,.55,1},{.864,.23,-.3,1},{1,.31}}, 1, 2); add(p);
        p = fatBass("Osmium Bass - Tectonic", "Broad low reese with two unequal pressure ridges and a late spectral shift. Stable weight under the slow motion.", 34, 11, 290);
        p.values[Unison] = choice(1, 4); p.values[Detune] = .22; p.values[Spread] = .5; spectral(p, 0, 1, .085);
        gesture(p, {{0,.19,-.4,1},{.128,.78,.5,1},{.309,.27,-.35,1},{.649,.9,.65,1},{.821,.32,-.4,1},{1,.23}},
                   {{0,.76,.55,1},{.241,.2,-.4,1},{.463,.64,.3,1},{.883,.27,-.2,1},{1,.43}}, 2, 1); add(p);
        p = fatBass("Osmium Bass - Aftershock", "A rounded impact followed by a smaller delayed snarl and a long settling shoulder. The second hit is timbre, not a volume gate.", 35, 37, 320);
        spectral(p, 0, 1, .14); env(p, 1, .002, .11, .02, .08);
        gesture(p, {{0,.2,-.6,1},{.031,.96,.65,1},{.167,.16,-.25,1},{.397,.7,.5,1},{.526,.34,-.35,1},{.849,.48,.5,1},{1,.2}},
                   {{0,.19,.4,1},{.217,.61,.55,1},{.358,.23,-.3,1},{.734,.81,.6,1},{1,.32}}, 3, 2); add(p);
    }
    {
        auto p = fatBass("Osmium Bass - Rubber Maw", "Elastic vowel bass that bends open, hesitates, then closes in an unequal second bite. Short notes retain the weight.", 21, 35, 285);
        spectral(p, 0, 1, .17, .025); filter(p, 1, 3, 2100, .14);
        gesture(p, {{0,.12,.35,1},{.081,.87,.55,1},{.237,.34,-.4,1},{.514,.76,.45,1},{.691,.23,-.35,1},{1,.31}},
                   {{0,.71,.6,1},{.129,.18,-.5,1},{.419,.79,.35,1},{.799,.29,-.25,1},{1,.42}}, 4, 3); add(p);
        p = fatBass("Osmium Bass - Crooked Root", "Woody asymmetric bass with a small opening flick, a broad crooked swell and a gentle low tail.", 47, 33, 245);
        spectral(p, 0, 3, .08, .035); drive(p, .21, 2, .11);
        gesture(p, {{0,.11,-.45,1},{.047,.61,.35,1},{.174,.26,.5,1},{.466,.88,.6,1},{.728,.32,-.45,1},{1,.2}},
                   {{0,.44,.45,1},{.209,.15,-.4,1},{.557,.74,.55,1},{.863,.24,-.25,1},{1,.36}}, 3, 2); add(p);
        p = fatBass("Osmium Bass - Molten Core", "Rich folded bass with a smooth low anchor. A fast surface curl crosses a slower internal swell.", 36, 32, 370);
        spectral(p, 0, 2, .1, .09); spectral(p, 1, 1, .09); mod(p, SrcCurve2, osmium::fold(0), .09);
        gesture(p, {{0,.17,-.55,1},{.052,.92,.55,1},{.219,.32,-.3,1},{.453,.72,.5,1},{.837,.21,-.3,1},{1,.29}},
                   {{0,.08,.45,1},{.311,.82,.6,1},{.463,.36,-.3,1},{.749,.64,.5,1},{1,.24}}, 3, 1); add(p);
        p = fatBass("Osmium Bass - Heavy Weather", "Dense rolling reese whose uneven upper swells pass over a steady low body. Slow notes let the full contour unfold.", 8, 38, 260);
        p.values[Unison] = choice(1, 4); p.values[Detune] = .24; p.values[Spread] = .46; spectral(p, 1, 3, .095);
        gesture(p, {{0,.38,.5,1},{.173,.14,-.45,1},{.417,.87,.55,1},{.568,.31,-.25,1},{.793,.66,.4,1},{1,.24}},
                   {{0,.21,-.45,1},{.073,.72,.55,1},{.294,.26,-.35,1},{.672,.83,.6,1},{1,.35}}, 2, 2); add(p);
        p = fatBass("Osmium Bass - Dark Matter", "Deep smooth bass with a brief harmonic flare and a long inward fold. Body adds weight; Tone opens the hidden edge.", 10, 34, 180);
        p.values[SubLevel] = .31; spectral(p, 0, 1, .07); env(p, 0, .009, .49, .94, .16);
        gesture(p, {{0,.13,-.6,1},{.033,.81,.6,1},{.189,.24,-.3,1},{.471,.61,.55,1},{.816,.17,-.3,1},{1,.25}},
                   {{0,.63,.6,1},{.271,.16,-.4,1},{.481,.71,.5,1},{.738,.31,-.25,1},{1,.38}}, 2, 1); add(p);
        p = fatBass("Osmium Bass - Fault Bloom", "Firm low bass with a restrained opening snap, delayed vocal bloom and an offset closing fold. Pressure roughens the bloom.", 39, 23, 305);
        spectral(p, 0, 3, .06, .025); spectral(p, 1, 1, .12); mod(p, SrcCurve2, osmium::fold(1), .07);
        gesture(p, {{0,.18,-.5,1},{.061,.73,.55,1},{.226,.21,.35,1},{.549,.92,.65,1},{.706,.34,-.35,1},{1,.23}},
                   {{0,.28,.5,1},{.173,.67,.45,1},{.364,.19,-.4,1},{.827,.78,.6,1},{1,.31}}, 3, 2); add(p);
    }
}

std::vector<Patch> buildBank() {
    std::vector<Patch> bank;
    auto add = [&](Patch p) { bank.push_back(std::move(p)); };
    // Pads: long notes/chords in C3-C5; the dry centre stays present under the space.
    {
        auto p = pad("Osmium Pad - Aurora", "Airy detuned triangle/harmonic pad. Hold C3-C5 chords; Tone opens the shimmer.");
        osc(p, 0, 0, 0.36, 0.52, 0, -5); osc(p, 1, 1, 0.28, 0.34, 12, 4, 0, 0, 0.3); space(p, 0.3, 0.3, 0.85, 0.12); add(p);
        p = pad("Osmium Pad - Cirrus", "Soft upper air over a steady low centre. Slow attack for liquid DnB breakdowns.");
        osc(p, 0, 1, 0.22, 0.5); osc(p, 1, 3, 0.08, 0.28, 12, 6, 0, 0, 0.5); p.values[NoiseLevel] = 0.025; env(p, 0, 1.3, 2.8, 0.76, 3.4); filter(p, 0, 0, 3600, 0.1); add(p);
        p = pad("Osmium Pad - Tidal Glass", "Bell-edged glass pad with a slow vowel sweep. Sustained upper chords and open voicings.");
        osc(p, 0, 6, 0.14, 0.44, 0, -3); osc(p, 1, 0, 0.32, 0.40, 0, 4); lfo(p, 0, 1, 0, false); p.values[ModPhase] = 0.025; space(p, 0.23, 0.28, 0.74, 0.17, 4); add(p);
        p = pad("Osmium Pad - Halo Choir", "Breathy formant ensemble. Voice-like movement without a hard transient; play around C3."); p.values[Volume] = linPos(-1, -24, 6);
        osc(p, 0, 2, 0.32, 0.54, 0, -6); osc(p, 1, 2, 0.57, 0.35, 0, 7, 1, 0.06, 0.25); p.values[NoiseLevel] = 0.018; filter(p, 0, 0, 4200, 0.10); route(p, SrcLfo2, oparam(1, Position), -0.16); add(p);
        p = pad("Osmium Pad - Rainlight", "Soft digital rain above a warm bed. Subtle stepped colour across a two-bar phrase.");
        osc(p, 0, 0, 0.4, 0.5); osc(p, 1, 7, 0.18, 0.24, 12, 5, 0, 0, 0.5); phrase(p, 0, {{0, .12, 0, 2}, {.25, .5, 0, 2}, {.5, .25, 0, 2}, {.75, .8, 0, 2}, {1, .12}}, 1, false, .28); route(p, SrcCurve1, oparam(1, Position), 0.2); add(p);
        p = pad("Osmium Pad - Blue Hour", "Warm dark liquid pad with a slow filter bloom. Shorter tail leaves room for drums.");
        osc(p, 0, 0, 0.56, 0.55); osc(p, 1, 1, 0.18, 0.36, 0, 3); filter(p, 0, 1, 1100, 0.16); env(p, 0, 0.38, 1.8, 0.88, 1.3); space(p, 0.22, 0.17, 0.58, 0.05); add(p);
        p = pad("Osmium Pad - Suspended", "Airy octave pad with slow rising sync colour. Long held voicings, no fifth baked into the chord.");
        osc(p, 0, 1, 0.34, 0.48); osc(p, 1, 0, 0.43, 0.34, 12, -4, 2, 0.06, 0.3); phrase(p, 0, {{0, 0, 0, 1}, {0.65, 1, 0, 1}, {1, 0}}, 0, false, 0.2); route(p, SrcCurve1, oparam(1, Intensity), 0.15); space(p, .33, .30, .82, .14); add(p);
        p = pad("Osmium Pad - Night Bloom", "Dark evolving formant pad with a low halo. Slow attack, long release, and restrained resonance."); p.values[Volume] = linPos(-1, -24, 6);
        osc(p, 0, 2, .2, .52, 0, -5); osc(p, 1, 5, .12, .30, 0, 6); filter(p, 0, 1, 1350, .18); env(p, 0, 1.0, 3, .78, 3.0); env(p, 1, 2.4, 3.6, .5, 3); route(p, SrcEnv2, oparam(0, Position), .25); add(p);
    }
    // Leads: mono/legato, C3-C5. Includes short plucked leads for liquid patterns.
    {
        auto p = lead("Osmium Lead - Vector", "Focused saw/pulse lead with a short filter bite. Legato phrases around C3-C4."); add(p);
        p = lead("Osmium Lead - Prism", "Bright glass-like lead, gentle phase modulation and dotted eighth echoes.");
        osc(p, 0, 6, .22, .65); osc(p, 1, 0, .30, .30, 12, 4); p.values[ModPhase] = .055; filter(p, 0, 0, 4600, .15); env(p, 0, .003, .30, .45, .20); add(p);
        p = lead("Osmium Lead - Signal", "Narrow nasal pulse lead. Controlled resonance cuts through a busy break.");
        osc(p, 0, 3, .48, .68); osc(p, 1, 0, .68, .25, 0, 8); filter(p, 0, 3, 1700, .30); p.values[FMix] = .25; route(p, SrcEnv2, oparam(0, Position), -.2); add(p);
        p = lead("Osmium Lead - Afterimage", "Soft plucked harmonic lead for melodic liquid DnB. Velocity and delay carry the tail.");
        p.values[VoiceMode] = 0; osc(p, 0, 1, .35, .7); osc(p, 1, 0, .31, .24, 12); env(p, 0, .003, .45, .06, .26); env(p, 1, .001, .16, 0, .12); space(p, .16, .18, .63, .24, 2); add(p);
        p = lead("Osmium Lead - Razorline", "Hard sync lead with a tight amp contour. Dry attack and short stereo repeat.");
        osc(p, 0, 0, .66, .62, 0, -3, 2, .24); osc(p, 1, 5, .3, .3, 0, 5); filter(p, 0, 1, 3200, .2); drive(p, .24, 2, .14); route(p, SrcEnv2, oparam(0, Intensity), .22); space(p, .06, .07, .36, .1, 1); add(p);
        p = lead("Osmium Lead - Glasswire", "Short metallic pluck with octave sparkle. Play C3-C5 arpeggios or two-note answers.");
        p.mods.removeIf([](const ModRoute& r) { return r.source == SrcMacro4; }); route(p, SrcMacro4, fx(ChorusMix), .28);
        p.values[VoiceMode] = 0; p.values[Unison] = 0; osc(p, 0, 6, .25, .72); osc(p, 1, 1, .21, .25, 12); env(p, 0, .001, .27, 0, .14); filter(p, 0, 0, 6500, .13); p.values[ModPhase] = .08; space(p, .07, .17, .55, .23, 2); add(p);
        p = lead("Osmium Lead - Kestrel", "Airy portamento lead, softer triangle core and a quiet octave. Hold overlapping notes.");
        osc(p, 0, 0, .4, .72); osc(p, 1, 1, .20, .27, 12, 5); p.values[Glide] = expPos(.11, .001, 2); env(p, 0, .025, .4, .85, .24); filter(p, 0, 0, 3900, .12); space(p, .17, .19, .68, .14, 4); add(p);
        p = lead("Osmium Lead - Neon Thread", "Digital legato lead with a repeating curved timbre accent. Tempo-locked, pitch stays steady.");
        osc(p, 0, 7, .28, .60); osc(p, 1, 3, .16, .30, 0, -7); phrase(p, 0, {{0, .1, -.4}, {.2, .85, 0, 1}, {.6, .25}, {1, .1}}, 4); route(p, SrcCurve1, oparam(0, Position), .24); route(p, SrcCurve1, fparam(0, FCutoff), .13); add(p);
    }
    // Subs: C1-C2 (MIDI 36-48), centered before optional effects; no reverb by default.
    {
        auto p = sub("Osmium Sub - Abyss", "Clean sustained sine/sub tone. Play C1-C2; Body adds an octave below, Tone introduces harmonics."); osc(p, 0, 4, .02, .94); filter(p, 0, 1, 240, .02); drive(p, .03, 1, .02); add(p);
        p = sub("Osmium Sub - Pressure", "Warm sustained sub with enough upper harmonics for small speakers. Short, clean release."); osc(p, 0, 4, .31, .88); filter(p, 0, 1, 580, .08); drive(p, .20, 1, .08); add(p);
        p = sub("Osmium Sub - Rubber", "Round syncopated sub with a gentle curved filter pulse. One pulse per quarter note."); osc(p, 0, 4, .25, .90); filter(p, 0, 1, 180, .10); phrase(p, 0, {{0, 1, -.45}, {.7, .08, 0, 1}, {1, 1}}, 4); route(p, SrcCurve1, fparam(0, FCutoff), .18); add(p);
        p = sub("Osmium Sub - Roundhouse", "Punchy short sub for fast repeated notes. The filter envelope adds impact without a click."); osc(p, 0, 4, .23, .93); env(p, 0, .003, .28, .24, .045); route(p, SrcEnv2, fparam(0, FCutoff), .18); filter(p, 0, 1, 180, .08); add(p);
        p = sub("Osmium Sub - Velvet", "Soft triangle/sub blend with a rounded onset. Sustained liquid bass under airy chords."); osc(p, 0, 0, .31, .82); osc(p, 1, 4, .04, .22); filter(p, 0, 1, 330, .04); env(p, 0, .018, .4, .88, .12); drive(p, .04, 1, .02); add(p);
        p = sub("Osmium Sub - Undertone", "Long smooth 808-style sub decay. No fixed pitch dive; notes retain a clear fundamental."); osc(p, 0, 4, .12, .94); env(p, 0, .004, 1.65, 0, .16, 0, -.2); filter(p, 0, 1, 290, .04); add(p);
        p = sub("Osmium Sub - Tunnel", "Dark hollow sub with an octave harmonic. Close the Tone macro for a pure low foundation."); osc(p, 0, 4, .20, .80); osc(p, 1, 0, .34, .18, 12); filter(p, 0, 1, 480, .13); drive(p, .13, 1, .07); add(p);
        p = sub("Osmium Sub - Low Orbit", "Wide-overtones option over a dry centered sine. Upper detuned layer stays quiet."); osc(p, 0, 4, .03, .90); osc(p, 1, 0, .62, .14, 12, 6); filter(p, 0, 1, 360, .05); p.values[Glide] = expPos(.07, .001, 2); add(p);
    }
    // Reeses: beating comes from detuned oscillators and slow timbral drift, not random pitch.
    {
        auto p = reese("Osmium Reese - Fracture", "Classic detuned saw reese with a firm sub foundation. Play C1-C2 and glide between notes."); add(p);
        p = reese("Osmium Reese - Concrete", "Dense, nearly dry low-mid reese. Smaller spread and soft clipping keep the centre solid."); osc(p, 0, 5, .23, .56, 0, -8); osc(p, 1, 0, .66, .44, 0, 9); filter(p, 0, 1, 1250, .18); p.values[Spread] = .40; drive(p, .38, 2, .20); space(p, .06, 0, .3); add(p);
        p = reese("Osmium Reese - Undertow", "Rounded dark reese with slow formant motion. Filtered enough to sit under a busy break."); osc(p, 0, 5, .30, .58, 0, -13); osc(p, 1, 2, .18, .34, 0, 12, 0, .1, .3); filter(p, 0, 1, 620, .22); route(p, SrcLfo1, oparam(1, Position), .16); add(p);
        p = reese("Osmium Reese - Corrosion", "Rough sync/metal reese with a curved midrange sweep. Short notes give harder accents."); osc(p, 0, 5, .37, .52, 0, -8, 2, .16); osc(p, 1, 6, .19, .36, 0, 10, 0, .08, .2); phrase(p, 0, {{0, .2, 0, 1}, {.5, .9, .3}, {1, .2}}, 2); route(p, SrcCurve1, oparam(0, Intensity), .14); filter(p, 0, 1, 1450, .20); drive(p, .3, 4, .10); add(p);
        p = reese("Osmium Reese - Black Ice", "Cold digital reese, brighter top and controlled low end. Tone exposes the granular edge."); osc(p, 0, 7, .22, .5, 0, -7); osc(p, 1, 0, .68, .46, 0, 9); filter(p, 0, 1, 1800, .18); p.values[NoiseLevel] = .008; route(p, SrcLfo2, oparam(0, Intensity), .09); add(p);
        p = reese("Osmium Reese - Viscous", "Slow, thick pulse reese. Envelope bend rounds the initial movement for longer bass notes."); osc(p, 0, 3, .17, .54, 0, -15); osc(p, 1, 3, .31, .48, 0, 13); env(p, 1, .12, .8, .35, .15, -.25); route(p, SrcEnv2, oparam(0, Position), .25); filter(p, 0, 1, 780, .27); add(p);
        p = reese("Osmium Reese - Iron Current", "Tight aggressive reese with a quiet ring-modulated metallic edge. Dry low end, little tail."); osc(p, 0, 5, .42, .58, 0, -6); osc(p, 1, 6, .12, .30, 0, 8); p.values[ModRing] = .13; p.values[ModPitch] = linPos(12, -36, 36); drive(p, .34, 1, .28); filter(p, 0, 1, 1500, .20); space(p, .10, 0, .3); add(p);
        p = reese("Osmium Reese - Horizon", "Open spacious reese for melodic drops. Wider upper harmonics and slower warm filter motion."); osc(p, 0, 0, .66, .50, 0, -9); osc(p, 1, 1, .45, .40, 0, 10, 0, 0, .3); filter(p, 0, 0, 2200, .13); p.values[Spread] = .85; space(p, .24, .08, .56, .07, 2); add(p);
    }
    // Wobbles: tempo-locked shapes designed for 170-174 BPM; varied subdivisions and contours.
    {
        auto p = wobble("Osmium Wobble - Undertow", "Deep eighth-note wobble with a curved opening and a dry sub. Start around C1-C2."); add(p);
        p = wobble("Osmium Wobble - Breakwater", "Alternating long and short filter openings over one bar. Useful for call-and-response bass lines."); phrase(p, 0, {{0, .05, -.5}, {.12, 1, -.4}, {.38, .05}, {.5, .15, -.6}, {.56, .85, -.4}, {.72, .1}, {.82, .65, -.4}, {1, .05}}, 2); osc(p, 0, 5, .35, .65, 0, -4, 0, .22); add(p);
        p = wobble("Osmium Wobble - Hydraulics", "Sixteenth-note square/hold movement with smoothed edges. Tight mechanical bass under sharp drums."); phrase(p, 0, {{0, 1, 0, 2}, {.5, .06, 0, 2}, {1, 1}}, 6, true, .08); osc(p, 0, 3, .25, .63); drive(p, .3, 2, .18); filter(p, 0, 1, 220, .3); add(p);
        p = wobble("Osmium Wobble - Lung", "Long breathing vowel wobble, one cycle per half bar. Formant and cutoff move together."); osc(p, 0, 2, .2, .62, 0, -3, 1, .09); osc(p, 1, 5, .18, .30, 0, 4); phrase(p, 0, {{0, .08, 0, 1}, {.55, 1, 0, 1}, {1, .08}}, 3); filter(p, 0, 0, 420, .2); add(p);
        p = wobble("Osmium Wobble - Torque", "Short snarling quarter-note bursts, with an envelope accent and stable low anchor."); osc(p, 0, 5, .48, .60, 0, -5, 2, .18); phrase(p, 0, {{0, .1, -.8}, {.09, 1, -.6}, {.6, .02}, {1, .1}}, 4); route(p, SrcEnv2, oparam(0, Intensity), .18); drive(p, .32, 1, .24); add(p);
        p = wobble("Osmium Wobble - Undertaker", "Heavy dark growl with a staggered one-bar phrase. Negative secondary motion changes the vowel."); osc(p, 0, 5, .5, .62, 0, -4, 0, .24); osc(p, 1, 2, .5, .34, 0, 5, 0, .12, .25); phrase(p, 0, {{0, .15, -.4}, {.16, .95, -.35}, {.5, .1, -.65}, {.63, .8, .35}, {.88, .2}, {1, .15}}, 2); route(p, SrcCurve1, oparam(1, Position), -.24); drive(p, .4, 2, .16); add(p);
        p = wobble("Osmium Wobble - Relay", "Eight-step neuro phrase with glide and sync accents. Every note retriggers the same pattern.");
        p.mods.removeIf([](const ModRoute& r) { return r.source == SrcCurve1; });
        p.values[sparam(0, SCount)] = 7.0 / 15; p.values[sparam(0, SGlide)] = .24; const double steps[] = {.9, -.4, .5, -.8, .75, .1, -.6, .3};
        for (int i = 0; i < 16; ++i) p.values[step(0, i)] = .5 + steps[i % 8] * .5;
        filter(p, 0, 1, 720, .27); osc(p, 0, 7, .30, .56, 0, -3, 2, .12); route(p, SrcStep1, fparam(0, FCutoff), .28); route(p, SrcStep1, oparam(0, Position), .18); add(p);
        p = wobble("Osmium Wobble - Seismic", "Slow heavy LFO wobble with a warm low layer. Quarter-note movement and a broad growl crest.");
        p.mods.removeIf([](const ModRoute& r) { return r.source == SrcCurve1; }); filter(p, 0, 1, 700, .28);
        lfo(p, 0, 4, 0, true); route(p, SrcLfo1, fparam(0, FCutoff), .30); route(p, SrcLfo1, oparam(0, Position), .21); osc(p, 0, 5, .42, .64, 0, -4, 0, .28); p.values[SubLevel] = .34; drive(p, .35, 1, .23); add(p);
    }
    appendWavefrontBank(bank);
    appendGestureBank(bank);
    appendWobbleBassBank(bank);
    return bank;
}
} // namespace

const std::vector<FactoryPreset>& factoryPresets() { static const auto bank = buildBank(); return bank; }
const FactoryPreset* factoryPreset(const QString& name) {
    for (const auto& p : factoryPresets()) if (name == QLatin1String(p.name)) return &p;
    return nullptr;
}
void applyPreset(Device& d, const FactoryPreset& p) {
    for (int i = 0; i < std::min(int(d.params.size()), kParams); ++i) d.params[i].value = p.values[size_t(i)];
    d.mods = p.mods; d.curves = p.curves;
}
} // namespace sessions::osmium
