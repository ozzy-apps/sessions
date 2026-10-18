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
