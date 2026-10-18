// Factory-bank audit and optional reproducible preset/reel export. No sound driver needed.
#include "core/OsmiumPresets.h"
#include "core/Presets.h"
#include "core/SetFile.h"
#include "core/Sample.h"
#include "audio/Devices.h"
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
static Render render(const Device& d, bool pad, int macro = -1, double tempo = 174, bool allMacros = false, bool preview = false) {
    constexpr int sr = 48000; const double beatSeconds = 60 / tempo;
    const double seconds = pad ? 4.5 : 2.4;
    const int frames = int(seconds * sr); auto processor = audio::createProcessor(DeviceType::Osmium, sr);
    float params[audio::kMaxParams]{}; for (int i = 0; i < d.params.size(); ++i) params[i] = float(d.params[i].value);
    if (macro >= 0) params[mo::macro(macro)] = 1;
    if (allMacros) for (int i = 0; i < 8; ++i) params[mo::macro(i)] = 1;
    CurvePoint points[mo::kCurves * mo::kCurvePoints]{}; int counts[mo::kCurves]{};
    for (int c = 0; c < std::min(int(d.curves.size()), mo::kCurves); ++c) {
        counts[c] = int(d.curves[c].size()); for (int i = 0; i < counts[c]; ++i) points[c * mo::kCurvePoints + i] = d.curves[c][i];
    }
    struct Note { int at, pitch, length; }; std::vector<Note> notes;
    if (pad) { for (int pitch : {57, 60, 64, 67}) notes.push_back({0, pitch, int(2.65 * sr)}); }
    else if (preview) {
        const bool lead = QString(d.name).contains("Lead");
        const int root = lead ? 60 : 33;
        const double at[] = {0, .75, 1.5, 2.5, 3.25, 4}; const int interval[] = {0, 0, 7, 3, 0, -2};
        for (int i = 0; i < 6; ++i) notes.push_back({int(at[i] * beatSeconds * sr), root + interval[i], int((i == 5 ? 1.3 : .65) * beatSeconds * sr)});
    } else notes.push_back({0, QString(d.name).contains("Lead") ? 60 : 33, int(1.6 * sr)});
    Render out; out.stereo.resize(size_t(frames) * 2); size_t note = 0; double energy = 0, side = 0, tail = 0;
    audio::Context ctx; ctx.sampleRate = sr; ctx.tempo = tempo; ctx.beatsPerSample = tempo / (60 * sr); ctx.playing = true;
    ctx.mods = d.mods.constData(); ctx.modCount = int(d.mods.size()); ctx.curves = points; ctx.curveCount = counts;
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
static double difference(const Render& a, const Render& b) { double sum = 0; for (size_t i = 0; i < a.stereo.size(); ++i) { const double d = a.stereo[i] - b.stereo[i]; sum += d * d; } return std::sqrt(sum / a.stereo.size()); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); QCoreApplication::setApplicationName("Sessions");
    const auto& bank = osmium::factoryPresets(); CHECK(bank.size() == 40, QString("Exactly 40 new DnB presets"));
    QString destination; if (argc > 1) { destination = QString::fromLocal8Bit(argv[1]); QDir().mkpath(destination + "/Presets"); }
    Project project; QSet<QString> names; QMap<QString, int> categories;
    QString report = "name,category,peak_dbfs,rms_dbfs,all_macros_peak_dbfs,side_rms,tail_rms\n";
    QString guide = "# Osmium — Drum & Bass\n\n40 factory presets at 174 BPM. Search **Osmium** in Browser → Sounds or type a name in Osmium's patch selector.\n\n"
        "Eight macros, in order: **Tone · Warp · Drive · Width · Space · Echo · Body · Air**. Body adds an octave below; Space/Echo add wet signal while preserving the dry attack. Subs start centered with no reverb or delay. Their Width macro adds optional gentle chorus.\n\n"
        "Pads: hold open chords around MIDI 52–72 (E2–C4 in Sessions). Leads: MIDI 55–79. Bass/subs: MIDI 28–43 (E0–G1), then transpose to the track. All rhythmic movement follows project tempo.\n\n"
        "The audition reel contains each preset in the order below, with four-note pad chords and short bass/lead phrases. It uses a shared playback gain; individual patches are not peak-normalized.\n\n| # | Start | Category | Preset | Use |\n|---|---|---|---|---|\n";
    std::vector<float> reel; int index = 0;
    for (const auto& preset : bank) {
        const QString name = QString::fromUtf8(preset.name), category = QString::fromUtf8(preset.category); ++categories[category]; ++index;
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
        const bool pad = category == "Pad"; const auto dry = render(d, pad); const auto full = render(d, pad, -1, 174, true);
        CHECK(dry.finite && dry.rms > .003 && dry.peak > .015 && dry.peak < .96, name + ": audible, finite, unclipped default");
        CHECK(full.finite && full.peak < .99, name + ": all macros at maximum stay finite and below full scale");
        if (category == "Sub") CHECK(dry.side < dry.rms * .001 + 1e-7, name + ": centered low end without wet effects");
        for (int macro = 0; macro < 8; ++macro) {
            const auto changed = render(d, pad, macro);
            CHECK(changed.finite && changed.peak < .99 && difference(dry, changed) > dry.rms * .0001, name + ": Macro " + QString::number(macro + 1) + " is connected and changes the audio");
        }
        const auto slower = render(d, pad, -1, 170);
        CHECK(slower.finite && slower.peak < .96, name + ": stable at 170 BPM");
        const double db = 20 * std::log10(std::max(1e-9, dry.peak)), rms = 20 * std::log10(std::max(1e-9, dry.rms)), maxDb = 20 * std::log10(std::max(1e-9, full.peak));
        std::printf("%02d %-32s peak %6.1f dB  rms %6.1f dB  macros %6.1f dB\n", index, preset.name, db, rms, maxDb);
        std::fflush(stdout);
        report += QString("%1,%2,%3,%4,%5,%6,%7\n").arg(name, category).arg(db, 0, 'f', 2).arg(rms, 0, 'f', 2).arg(maxDb, 0, 'f', 2).arg(dry.side, 0, 'g', 5).arg(dry.tail, 0, 'g', 5);
        const int start = int(reel.size() / 96000);
        const QString time = QString("%1:%2").arg(start / 60).arg(start % 60, 2, 10, QChar('0'));
        guide += QString("| %1 | %2 | %3 | %4 | %5 |\n").arg(index).arg(time, category, name, preset.description);
        if (!destination.isEmpty()) {
            QString error; const QString file = destination + "/Presets/" + name + ".sessionspreset";
            CHECK(presets::save(file, name, {{portable}}, &error), name + ": export " + error);
            Project::DeviceBlocks blocks; CHECK(presets::read(file, blocks, nullptr, &error) && blocks.size() == 1 && setfile::deviceToJson(blocks[0][0]) == setfile::deviceToJson(portable), name + ": exported preset reads identically");
            const auto phrase = render(d, pad, -1, 174, false, true);
            // One shared 0.8 gain leaves headroom; a short fade only removes render-boundary clicks.
            const size_t frames = phrase.stereo.size() / 2;
            for (size_t i = 0; i < frames; ++i) {
                const double fade = std::min({1.0, double(i) / 240, double(frames - 1 - i) / 1200});
                reel.push_back(float(phrase.stereo[i * 2] * .8 * fade)); reel.push_back(float(phrase.stereo[i * 2 + 1] * .8 * fade));
            }
            reel.insert(reel.end(), 4800 * 2, 0);
        }
    }
    for (const QString category : {"Pad", "Lead", "Sub", "Reese", "Wobble"}) CHECK(categories.value(category) == 8, category + ": eight presets");
    if (!destination.isEmpty()) {
        QFile csv(destination + "/levels.csv"); csv.open(QIODevice::WriteOnly); csv.write(report.toUtf8());
        QFile doc(destination + "/Osmium DnB.md"); doc.open(QIODevice::WriteOnly); doc.write(guide.toUtf8());
        QString error; CHECK(samples::writeWav(destination + "/Osmium DnB - Audition.wav", reel.data(), qint64(reel.size() / 2), 48000, samples::WavFormat::Int16, &error), "Audition export: " + error);
    }
    std::printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
