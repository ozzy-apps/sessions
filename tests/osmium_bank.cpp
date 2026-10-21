// Factory-bank audit and optional reproducible preset/reel export. No sound driver needed.
#include "core/OsmiumPresets.h"
#include "core/ModulationCurve.h"
#include "core/Presets.h"
#include "core/SetFile.h"
#include "core/Sample.h"
#include "audio/Devices.h"
#include "audio/Engine.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSet>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <cstdio>
using namespace sessions;
namespace mo = monolith;
static int failures = 0, checks = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL: %s\n", qPrintable(m)); } } while(0)
struct Render { std::vector<float> stereo; double peak = 0, rms = 0, side = 0, tail = 0; bool finite = true; };
// The wobble basses (0.39): Dubstep and DnB Bass. They hold a note for two bars (their preview: four bars of three notes).
static bool isBass(const QString& category) { return category == "Dubstep" || category == "DnB Bass"; }
static bool isBassName(const QString& name) { return name.contains("Dubstep - ") || name.contains("DnB Bass - "); }
// `whole`: a bass plays its held note for two bars (the measurements); otherwise 2.4 s, as the rest of the bank.
static Render render(const Device& d, bool pad, int macro = -1, double tempo = 174, bool allMacros = false, bool preview = false, bool whole = false) {
    constexpr int sr = 48000; const double beatSeconds = 60 / tempo;
    const bool bass = isBassName(d.name);
    const double seconds = pad ? 4.5 : bass && (preview || whole) ? (preview ? 16 : 8) * beatSeconds + 0.5 : 2.4;
    const int frames = int(seconds * sr); auto processor = audio::createProcessor(DeviceType::Osmium, sr);
    float params[audio::kMaxParams]{}; for (int i = 0; i < d.params.size(); ++i) params[i] = float(d.params[i].value);
    if (macro >= 0) params[mo::macro(macro)] = 1;
    if (allMacros) for (int i = 0; i < 8; ++i) params[mo::macro(i)] = 1;
    // Osmium's own curve capacity (257 nodes a curve), as the Engine passes it.
    CurvePoint points[mo::kCurves * osmium::kCurvePoints]{}; int counts[mo::kCurves]{};
    for (int c = 0; c < std::min(int(d.curves.size()), mo::kCurves); ++c) {
        counts[c] = std::min(int(d.curves[c].size()), osmium::kCurvePoints); for (int i = 0; i < counts[c]; ++i) points[c * osmium::kCurvePoints + i] = d.curves[c][i];
    }
    struct Note { int at, pitch, length; }; std::vector<Note> notes;
    if (pad) { for (int pitch : {57, 60, 64, 67}) notes.push_back({0, pitch, int(2.65 * sr)}); }
    else if (bass && preview) {   // F1 held through two bars, then A flat and E flat for a bar each
        const double at[] = {0, 8, 12}, beats[] = {7.6, 3.6, 3.8}; const int pitch[] = {41, 44, 39};
        for (int i = 0; i < 3; ++i) notes.push_back({int(at[i] * beatSeconds * sr), pitch[i], int(beats[i] * beatSeconds * sr)});
    } else if (bass) notes.push_back({0, 41, int(7.8 * beatSeconds * sr)});
    else if (preview) {
        const bool lead = QString(d.name).contains("Lead");
        const int root = lead ? 60 : 33;
        const double at[] = {0, .75, 1.5, 2.5, 3.25, 4}; const int interval[] = {0, 0, 7, 3, 0, -2};
        for (int i = 0; i < 6; ++i) notes.push_back({int(at[i] * beatSeconds * sr), root + interval[i], int((i == 5 ? 1.3 : .65) * beatSeconds * sr)});
    } else notes.push_back({0, QString(d.name).contains("Lead") ? 60 : 33, int(1.6 * sr)});
    Render out; out.stereo.resize(size_t(frames) * 2); size_t note = 0; double energy = 0, side = 0, tail = 0;
    audio::Context ctx; ctx.sampleRate = sr; ctx.tempo = tempo; ctx.beatsPerSample = tempo / (60 * sr); ctx.playing = true;
    ctx.mods = d.mods.constData(); ctx.modCount = int(d.mods.size()); ctx.curves = points; ctx.curveCount = counts; ctx.curveStride = osmium::kCurvePoints;
    for (int offset = 0; offset < frames; offset += audio::kMaxBlock) {
        const int n = std::min(audio::kMaxBlock, frames - offset); float l[audio::kMaxBlock]{}, r[audio::kMaxBlock]{}; audio::EventList events;
        while (note < notes.size() && notes[note].at < offset + n) {
            const auto& e = notes[note++]; events.add({e.at - offset, e.pitch, .85f, e.length});
        }
        ctx.beat = offset * ctx.beatsPerSample; processor->render(events, l, r, n, params, ctx);
        for (int i = 0; i < n; ++i) {
            out.finite &= std::isfinite(l[i]) && std::isfinite(r[i]);
            out.peak = std::max(out.peak, double(std::max(std::abs(l[i]), std::abs(r[i]))));
            energy += (l[i] * l[i] + r[i] * r[i]) * .5; const double s = (l[i] - r[i]) * .5; side += s * s;
            if (offset + i > frames - sr / 4) tail += (l[i] * l[i] + r[i] * r[i]) * .5;
            out.stereo[size_t(offset + i) * 2] = l[i]; out.stereo[size_t(offset + i) * 2 + 1] = r[i];
        }
    }
    out.rms = std::sqrt(energy / frames); out.side = std::sqrt(side / frames); out.tail = std::sqrt(tail / (sr / 4)); return out;
}
// A 12 dB/octave Butterworth section (RBJ); two in a row make the 24 dB/octave band splits below.
struct Section {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    Section(bool high, double hz) {
        const double w = 2 * M_PI * hz / 48000, c = std::cos(w), alpha = std::sin(w) / std::sqrt(2.0), a0 = 1 + alpha;
        b0 = (high ? 1 + c : 1 - c) / 2 / a0; b1 = (high ? -(1 + c) : 1 - c) / a0; b2 = b0; a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
    }
    double run(double x) { const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; }
};
static double mid(const Render& r, size_t i) { return (r.stereo[i * 2] + r.stereo[i * 2 + 1]) * .5; }
// Low-end weight: the share of the level below 150 Hz.
static double lowShare(const Render& r) {
    Section a(false, 150), b(false, 150); double low = 0, all = 0;
    for (size_t i = 0; i < r.stereo.size() / 2; ++i) { const double m = mid(r, i), l = b.run(a.run(m)); low += l * l; all += m * m; }
    return std::sqrt(low / std::max(1e-12, all));
}
// How much the wobble moves the sound: the level above 500 Hz in 1/64-note windows (short enough
// for sixteenth wobbles) over the held note's first seven and a half beats, the loudest 5 % against
// the quietest 5 % (in dB).
static double wobbleDepthDb(const Render& r, double tempo) {
    Section a(true, 500), b(true, 500);
    const int window = int(60 / tempo / 16 * 48000), frames = int(7.5 * 60 / tempo * 48000);
    std::vector<double> levels; double sum = 0; int n = 0;
    for (int i = 0; i < frames && size_t(i) < r.stereo.size() / 2; ++i) {
        const double high = b.run(a.run(mid(r, size_t(i)))); sum += high * high;
        if (++n == window) { levels.push_back(std::sqrt(sum / n)); sum = 0; n = 0; }
    }
    if (levels.size() < 20) return 0;
    std::sort(levels.begin(), levels.end());
    return 20 * std::log10(std::max(1e-9, levels[levels.size() * 95 / 100]) / std::max(1e-9, levels[levels.size() * 5 / 100]));
}
// The wobble rates in a drawn phrase: how many times per beat the curve rises through the middle,
// for every beat of the phrase (its Rate in beats). 1: quarters, 2: eighths, 3: eighth triplets, 4: sixteenths ...
static QSet<int> ratesPerBeat(const QVector<CurvePoint>& curve, double beats) {
    // Sampled between the ticks, so a chop that opens on a beat counts in that beat.
    QSet<int> rates; const int perBeat = 960; double last = modcurve::value(curve.constData(), int(curve.size()), 1 - .5 / perBeat / beats);
    for (int beat = 0; beat < int(beats); ++beat) {
        int rises = 0;
        for (int i = 0; i < perBeat; ++i) {
            const double v = modcurve::value(curve.constData(), int(curve.size()), (beat + (i + .5) / perBeat) / beats);
            if (last < .35 && v >= .35) ++rises;
            last = v;
        }
        if (rises) rates.insert(rises);
    }
    return rates;
}
static double difference(const Render& a, const Render& b) {   // over the shorter of the two
    const size_t n = std::min(a.stereo.size(), b.stereo.size()); double sum = 0;
    for (size_t i = 0; i < n; ++i) { const double d = a.stereo[i] - b.stereo[i]; sum += d * d; }
    return std::sqrt(sum / std::max<size_t>(1, n));
}
// The Curve 1 phase Osmium reports (scope[14]) after rendering `beats` of a held note at `tempo`,
// with the Curve 1 Rate switched to `rateAfter` (a Rate choice) once `switchAt` beats have played:
// what automating the Rate does mid-note.
static double phaseAfterRateChange(const Device& d, double tempo, double switchAt, int rateAfter, double beats) {
    constexpr int sr = 48000; auto processor = audio::createProcessor(DeviceType::Osmium, sr);
    float params[audio::kMaxParams]{}; for (int i = 0; i < d.params.size(); ++i) params[i] = float(d.params[i].value);
    CurvePoint points[mo::kCurves * osmium::kCurvePoints]{}; int counts[mo::kCurves]{};
    for (int c = 0; c < std::min(int(d.curves.size()), mo::kCurves); ++c) {
        counts[c] = int(d.curves[c].size()); for (int i = 0; i < counts[c]; ++i) points[c * osmium::kCurvePoints + i] = d.curves[c][i];
    }
    audio::Context ctx; ctx.sampleRate = sr; ctx.tempo = tempo; ctx.beatsPerSample = tempo / (60 * sr); ctx.playing = true;
    ctx.mods = d.mods.constData(); ctx.modCount = int(d.mods.size()); ctx.curves = points; ctx.curveCount = counts; ctx.curveStride = osmium::kCurvePoints;
    const int frames = int(std::lround(beats / ctx.beatsPerSample)), switchFrame = int(std::lround(switchAt / ctx.beatsPerSample));
    for (int offset = 0; offset < frames; offset += audio::kMaxBlock) {
        const int n = std::min(audio::kMaxBlock, frames - offset); float l[audio::kMaxBlock]{}, r[audio::kMaxBlock]{}; audio::EventList events;
        if (offset == 0) events.add({0, 41, .85f, frames + sr});
        if (offset >= switchFrame) params[mo::cparam(0, mo::CRate)] = float(rateAfter) / 8;
        ctx.beat = offset * ctx.beatsPerSample; processor->render(events, l, r, n, params, ctx);
    }
    return processor->scope[14].load();
}
// The same through the whole engine: a set with Gravedigger on a track holding F1 for eight beats,
// its Curve 1 Rate automated (an automation lane) from 2 bars to 1/16 at beat 4 when `automate`.
// Returns the Curve 1 phase after `beats` (120 BPM) and the audio.
static double automatedPhase(bool automate, double beats, std::vector<float>* audio = nullptr) {
    Project p; p.scenes = {"1"}; p.addTrack("Bass", QColor("#7e9db2"));
    p.tracks[0].devices = {p.makeDevice("Osmium Dubstep - Gravedigger", DeviceKind::Instrument)};
    const int id = p.tracks[0].devices[0].id;
    const ClipPtr clip = p.newClip("F1", 8); clip->notes = {Note{41, 0, 8, .85, 1}};
    p.tracks[0].clipSlots[0] = clip; p.tracks[0].arrangement = {{0, 8, 0, clip}};
    if (automate) p.tracks[0].automation = {{target::device(id, mo::cparam(0, mo::CRate)), {{0, 1.0 / 8}, {3.999, 1.0 / 8}, {4, 6.0 / 8}}}};
    audio::Engine e(&p); e.send({audio::Command::Stop, -1, -1, 0, 0, true}); e.send({audio::Command::Play});
    const int frames = int(std::lround(beats * 24000)); std::vector<float> out(size_t(frames) * 2);
    for (int done = 0; done < frames;) { const int n = std::min(256, frames - done); e.process(out.data() + size_t(done) * 2, n); done += n; }
    if (audio) *audio = std::move(out);
    return e.deviceScope(id, 14);
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationName("Sessions");
    const auto& bank = osmium::factoryPresets(); CHECK(bank.size() == 60, QString("Exactly 60 factory presets: the 40 DnB presets and 20 wobble basses"));
    QString destination; if (argc > 1) { destination = QString::fromLocal8Bit(argv[1]); QDir().mkpath(destination + "/Presets"); QDir().mkpath(destination + "/Bass Presets"); }
    Project project; QSet<QString> names; QMap<QString, int> categories;
    QString report = "name,category,peak_dbfs,rms_dbfs,all_macros_peak_dbfs,side_rms,tail_rms\n";
    QString bassReport = "name,category,bpm,peak_dbfs,rms_dbfs,all_macros_peak_dbfs,low_share,wobble_depth_db,rates_per_beat\n";
    QString guide = "# Osmium — Drum & Bass\n\n40 factory presets at 174 BPM. Search **Osmium** in Browser → Sounds or type a name in Osmium's patch selector.\n\n"
        "Eight macros, in order: **Tone · Warp · Drive · Width · Space · Echo · Body · Air**. Body adds an octave below; Space/Echo add wet signal while preserving the dry attack. Subs start centered with no reverb or delay. Their Width macro adds optional gentle chorus.\n\n"
        "Pads: hold open chords around MIDI 52–72 (E2–C4 in Sessions). Leads: MIDI 55–79. Bass/subs: MIDI 28–43 (E0–G1), then transpose to the track. All rhythmic movement follows project tempo.\n\n"
        "The audition reel contains each preset in the order below, with four-note pad chords and short bass/lead phrases. It uses a shared playback gain; individual patches are not peak-normalized.\n\n| # | Start | Category | Preset | Use |\n|---|---|---|---|---|\n";
    QString bassGuide = "| # | Start | Category | Preset | BPM | Use |\n|---|---|---|---|---|---|\n";
    std::vector<float> reel, bassReel; int index = 0, bassIndex = 0;
    for (const auto& preset : bank) {
        const QString name = QString::fromUtf8(preset.name), category = QString::fromUtf8(preset.category); ++categories[category]; ++index;
        const bool bass = isBass(category); const double bpm = preset.bpm;
        CHECK(!names.contains(name), name + ": unique name"); names.insert(name);
        CHECK(deviceType(name) == DeviceType::Osmium, name + ": registered as an Osmium sound");
        Device d = project.makeDevice(name, DeviceKind::Instrument);
        CHECK(d.params.size() == osmium::kParams && d.mods == preset.mods && d.curves == preset.curves, name + ": browser loads its routes and curves");
        bool valid = d.mods.size() <= mo::kMaxRoutes; QSet<int> routes;
        for (int i = 0; i < d.params.size(); ++i) valid &= std::isfinite(d.params[i].value) && d.params[i].value >= 0 && d.params[i].value <= 1 && d.params[i].value == preset.values[size_t(i)];
        for (const auto& r : d.mods) { const int key = r.source * 256 + r.param; valid &= !routes.contains(key) && r.source >= 0 && r.source < mo::kSources && r.param >= 0 && r.param < osmium::kParams && std::abs(r.amount) <= 1; routes.insert(key); }
        CHECK(valid, name + ": normalized parameters and unique valid routes");
        const Device restored = setfile::deviceFromJson(setfile::deviceToJson(d));
        CHECK(setfile::deviceToJson(restored) == setfile::deviceToJson(d), name + ": complete set/preset round trip");
        Device portable = d; portable.name = "Osmium"; // works in existing 0.27/0.28 installations too
        const Device portableRead = setfile::deviceFromJson(setfile::deviceToJson(portable));
        CHECK(setfile::deviceToJson(portableRead) == setfile::deviceToJson(portable), name + ": portable bank preserves the base Osmium format");
        const bool pad = category == "Pad"; const auto dry = render(d, pad, -1, bpm, false, false, true); const auto full = render(d, pad, -1, bpm, true);
        CHECK(dry.finite && dry.rms > .003 && dry.peak > .015 && dry.peak < .96, name + ": audible, finite, unclipped default");
        CHECK(full.finite && full.peak < .99, name + ": all macros at maximum stay finite and below full scale");
        if (category == "Sub") CHECK(dry.side < dry.rms * .001 + 1e-7, name + ": centered low end without wet effects");
        for (int macro = 0; macro < 8; ++macro) {
            const auto changed = render(d, pad, macro, bpm);
            CHECK(changed.finite && changed.peak < .99 && difference(dry, changed) > dry.rms * .0001, name + ": Macro " + QString::number(macro + 1) + " is connected and changes the audio");
        }
        // The Drum & Bass bank is checked a little slower; the basses at the other genre's tempo (140 <-> 174).
        const double otherTempo = !bass ? 170 : bpm < 160 ? 174 : 140;
        const auto other = render(d, pad, -1, otherTempo);
        CHECK(other.finite && other.peak < .96, name + ": stable at " + QString::number(otherTempo) + " BPM");
        const double db = 20 * std::log10(std::max(1e-9, dry.peak)), rms = 20 * std::log10(std::max(1e-9, dry.rms)), maxDb = 20 * std::log10(std::max(1e-9, full.peak));
        std::printf("%02d %-36s peak %6.1f dB  rms %6.1f dB  macros %6.1f dB", index, preset.name, db, rms, maxDb);
        QString rateList;
        if (bass) {
            // The phrase: a valid drawn curve (sorted, 0 .. 1, within Osmium's 257 nodes) holding at least three wobble rates.
            bool drawn = d.curves.size() == mo::kCurves;
            for (const auto& curve : d.curves) {
                drawn &= curve.size() >= 2 && curve.size() <= osmium::kCurvePoints && curve.first().x == 0 && curve.last().x == 1;
                for (int i = 0; i < curve.size(); ++i)
                    drawn &= curve[i].y >= 0 && curve[i].y <= 1 && std::abs(curve[i].bend) <= 1 && curve[i].shape >= 0 && curve[i].shape <= 2 && (i == 0 || curve[i].x >= curve[i - 1].x);
            }
            CHECK(drawn, name + ": both curves are valid drawn phrases within 257 nodes");
            static const double rateBeats[] = {16, 8, 4, 2, 1, .5, .25, .125, 0};
            const double phraseBeats = rateBeats[std::clamp(int(std::lround(d.params[mo::cparam(0, mo::CRate)].value * 8)), 0, 8)];
            const QSet<int> rates = ratesPerBeat(d.curves[0], phraseBeats);
            QList<int> sorted(rates.begin(), rates.end()); std::sort(sorted.begin(), sorted.end());
            for (int r : sorted) rateList += (rateList.isEmpty() ? "" : " ") + QString::number(r);
            CHECK(rates.size() >= 3 && phraseBeats >= 4, name + ": Curve 1 is a phrase of at least a bar holding at least three wobble rates (" + rateList + " per beat)");
            bool curveRoute = false; for (const auto& r : d.mods) curveRoute |= r.source == mo::SrcCurve1 && r.amount > .3;
            CHECK(curveRoute, name + ": the phrase drives the sound (a Curve 1 route of more than 30 %)");
            const double low = lowShare(dry), depth = wobbleDepthDb(dry, bpm);
            CHECK(low > .7, name + ": fat low end (" + QString::number(low, 'f', 2) + " of the level below 150 Hz)");
            CHECK(depth > 12, name + ": the wobble is heard (" + QString::number(depth, 'f', 1) + " dB of movement above 500 Hz)");
            CHECK(d.params[mo::Unison].value > .5 && d.params[mo::SubLevel].value > .3 && d.params[mo::VoiceMode].value > 0, name + ": mono, at least three unison voices over the sub");
            std::printf("  low %.2f  wobble %4.1f dB  rates %s", low, depth, qPrintable(rateList));
            bassReport += QString("%1,%2,%3,%4,%5,%6,%7,%8,%9\n").arg(name, category).arg(bpm).arg(db, 0, 'f', 2).arg(rms, 0, 'f', 2).arg(maxDb, 0, 'f', 2).arg(low, 0, 'f', 3).arg(depth, 0, 'f', 1).arg(rateList);
        }
        std::printf("\n"); std::fflush(stdout);
        report += QString("%1,%2,%3,%4,%5,%6,%7\n").arg(name, category).arg(db, 0, 'f', 2).arg(rms, 0, 'f', 2).arg(maxDb, 0, 'f', 2).arg(dry.side, 0, 'g', 5).arg(dry.tail, 0, 'g', 5);
        std::vector<float>& target = bass ? bassReel : reel;
        const int start = int(target.size() / 96000);
        const QString time = QString("%1:%2").arg(start / 60).arg(start % 60, 2, 10, QChar('0'));
        if (bass) bassGuide += QString("| %1 | %2 | %3 | %4 | %5 | %6 |\n").arg(++bassIndex).arg(time, category, name.mid(7)).arg(bpm).arg(preset.description);
        else guide += QString("| %1 | %2 | %3 | %4 | %5 |\n").arg(index).arg(time, category, name, preset.description);
        if (!destination.isEmpty()) {
            QString error; const QString file = destination + (bass ? "/Bass Presets/" : "/Presets/") + name + ".sessionspreset";
            CHECK(presets::save(file, name, {{portable}}, &error), name + ": export " + error);
            Project::DeviceBlocks blocks; CHECK(presets::read(file, blocks, nullptr, &error) && blocks.size() == 1 && setfile::deviceToJson(blocks[0][0]) == setfile::deviceToJson(portable), name + ": exported preset reads identically");
            const auto phrase = render(d, pad, -1, bpm, false, true);
            // One shared 0.8 gain leaves headroom; a short fade only removes render-boundary clicks.
            const size_t frames = phrase.stereo.size() / 2;
            for (size_t i = 0; i < frames; ++i) {
                const double fade = std::min({1.0, double(i) / 240, double(frames - 1 - i) / 1200});
                target.push_back(float(phrase.stereo[i * 2] * .8 * fade)); target.push_back(float(phrase.stereo[i * 2 + 1] * .8 * fade));
            }
            target.insert(target.end(), 4800 * 2, 0);
        }
    }
    for (const QString category : {"Pad", "Lead", "Sub", "Reese", "Wobble"}) CHECK(categories.value(category) == 8, category + ": eight presets");
    for (const QString category : {"Dubstep", "DnB Bass"}) CHECK(categories.value(category) == 10, category + ": ten wobble basses");
    // Automating Curve 1 Rate mid-note changes the phrase length at once. Gravedigger's two-bar
    // phrase is locked to the song: after 6 beats it is 3/4 through; switched to one bar at beat 4,
    // the same 6 beats are half-way through its second pass.
    if (const auto* grave = osmium::factoryPreset("Osmium Dubstep - Gravedigger")) {
        const Device d = project.makeDevice(grave->name, DeviceKind::Instrument);
        // Rate choices: 1 = 2 bars, 2 = 1 bar, 6 = 1/16.
        const double steady = phaseAfterRateChange(d, 140, 1e9, 1, 6), toBar = phaseAfterRateChange(d, 140, 4, 2, 6), toSixteenth = phaseAfterRateChange(d, 140, 4, 6, 5.125);
        CHECK(std::abs(steady - .75) < .01, QString("Curve 1 Rate: a two-bar phrase is 3/4 through after 6 beats (%1)").arg(steady));
        CHECK(std::abs(toBar - .5) < .01, QString("Curve 1 Rate automated to 1 bar at beat 4: half-way through the bar after 6 beats (%1)").arg(toBar));
        CHECK(std::abs(toSixteenth - .5) < .02, QString("Curve 1 Rate automated to 1/16 at beat 4: the phrase repeats every sixteenth, half-way at beat 5 1/8 (%1)").arg(toSixteenth));
    } else CHECK(false, QString("Gravedigger is in the bank"));
    {
        std::vector<float> plain, automated;
        const double steady = automatedPhase(false, 5.125, &plain), switched = automatedPhase(true, 5.125, &automated);
        double before = 0, after = 0;   // how far the two renders differ before and after the switch at beat 4
        for (size_t i = 0; i < plain.size(); ++i) (i / 2 < size_t(4 * 24000) ? before : after) += std::abs(plain[i] - automated[i]);
        CHECK(std::abs(steady - 5.125 / 8) < .01 && std::abs(switched - .5) < .02 && before < 1e-3 && after > 1,
              QString("An automation lane on Curve 1 Rate switches Gravedigger from 2 bars to 1/16 at beat 4 in the engine (phase %1 -> %2; the audio changes only after the switch: %3 / %4)")
                  .arg(steady).arg(switched).arg(before).arg(after));
    }
    if (!destination.isEmpty()) {
        QFile csv(destination + "/levels.csv"); csv.open(QIODevice::WriteOnly); csv.write(report.toUtf8());
        QFile bassCsv(destination + "/bass-levels.csv"); bassCsv.open(QIODevice::WriteOnly); bassCsv.write(bassReport.toUtf8());
        QFile doc(destination + "/Osmium DnB.md"); doc.open(QIODevice::WriteOnly); doc.write(guide.toUtf8());
        QFile bassDoc(destination + "/Osmium Wobble Basses - table.md"); bassDoc.open(QIODevice::WriteOnly); bassDoc.write(bassGuide.toUtf8());
        QString error; CHECK(samples::writeWav(destination + "/Osmium DnB - Audition.wav", reel.data(), qint64(reel.size() / 2), 48000, samples::WavFormat::Int16, &error), "Audition export: " + error);
        CHECK(samples::writeWav(destination + "/Osmium Wobble Basses - Audition.wav", bassReel.data(), qint64(bassReel.size() / 2), 48000, samples::WavFormat::Int16, &error), "Bass audition export: " + error);
    }
    std::printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
