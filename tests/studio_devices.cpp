#include "audio/Convolver.h"
#include "audio/Dsp.h"
#include "audio/Engine.h"
#include "audio/ProductionEffects.h"
#include "core/Sample.h"
#include "core/SampleZones.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/devices/DeviceBody.h"
#include <QApplication>
#include <QFile>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUndoStack>
#include <cmath>
#include <cstdio>
using namespace sessions;
using T = DeviceType;
static int checks = 0, failures = 0;
#define CHECK(c, m)                                                                                                    \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(c)) {                                                                                                    \
            ++failures;                                                                                                \
            std::printf("FAIL: %s\n", m);                                                                              \
        }                                                                                                              \
    } while (0)
static std::vector<float> defaults(T t) {
    std::vector<float> p;
    for (const auto& v : specOf(t)->params)
        p.push_back(v.def);
    return p;
}
static float pos(T t, int i, double v) {
    const auto& s = specOf(t)->params[i];
    return s.kind == ParamKind::Exp ? std::log(v / s.lo) / std::log(s.hi / s.lo) : (v - s.lo) / (s.hi - s.lo);
}
static std::vector<float> tone(double hz, double amplitude = .2, int frames = 96000) {
    std::vector<float> v(frames);
    for (int i = 0; i < frames; ++i)
        v[i] = amplitude * std::sin(dsp::kTwoPi * hz * i / 48000.);
    return v;
}
static std::vector<float> process(T t, const std::vector<float>& in, std::vector<float> p,
                                  const audio::Context& ctx = {}) {
    auto proc = audio::createProcessor(t, 48000);
    std::vector<float> result(in.size());
    float l[128], r[128];
    for (size_t at = 0; at < in.size(); at += 128) {
        int n = std::min(size_t(128), in.size() - at);
        std::copy_n(in.data() + at, n, l);
        std::copy_n(l, n, r);
        proc->process(l, r, n, p.data(), ctx);
        std::copy_n(l, n, result.data() + at);
    }
    return result;
}
static double component(const std::vector<float>& v, double hz, int from = 24000) {
    double re = 0, im = 0;
    for (int i = from; i < int(v.size()); ++i) {
        double ph = dsp::kTwoPi * hz * i / 48000;
        re += v[i] * std::cos(ph);
        im += v[i] * std::sin(ph);
    }
    return 2 * std::hypot(re, im) / (v.size() - from);
}
static double rms(const std::vector<float>& v, int start = 24000) {
    double e = 0;
    for (int i = start; i < int(v.size()); ++i)
        e += v[i] * v[i];
    return std::sqrt(e / (v.size() - start));
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    deviceSpecs();
    auto in = tone(440);
    auto p = defaults(T::Shifter);
    auto unshifted = process(T::Shifter, in, p);
    CHECK(std::abs(component(unshifted, 440) - .2) < .012, "Neutral spectral processing preserves tone amplitude");
    p[pitchfx::Pitch] = pos(T::Shifter, pitchfx::Pitch, 12);
    p[pitchfx::Preserve] = 0;
    auto up = process(T::Shifter, in, p);
    CHECK(component(up, 880) > .08 && component(up, 440) < .015, "Pitch shift transposes an octave");
    p[pitchfx::Pitch] = pos(T::Shifter, pitchfx::Pitch, -12);
    auto down = process(T::Shifter, in, p);
    CHECK(component(down, 220) > .08, "Pitch shift transposes downward");
    auto voiced = tone(200, .02);
    for (int h = 2; h <= 24; ++h) {
        double amplitude = .005 + .08 * std::exp(-std::pow((h * 200. - 1200) / 500, 2));
        for (int i = 0; i < int(voiced.size()); ++i)
            voiced[i] += amplitude * std::sin(dsp::kTwoPi * h * 200 * i / 48000.);
    }
    p = defaults(T::Shifter);
    auto natural = process(T::Shifter, voiced, p);
    p[pitchfx::Formant] = pos(T::Shifter, pitchfx::Formant, 12);
    auto formanted = process(T::Shifter, voiced, p);
    double difference = 0, total = 0;
    for (int h = 1; h <= 24; ++h) {
        double a = component(natural, h * 200), b = component(formanted, h * 200);
        difference += std::abs(a - b);
        total += a;
    }
    CHECK(difference > total * .15 && component(formanted, 200) > .0005,
          "Formant shifting changes harmonic balance while retaining the fundamental");
    p = defaults(T::Shifter);
    p[pitchfx::Mode] = choicePosition(1, 3);
    p[pitchfx::Frequency] = pos(T::Shifter, pitchfx::Frequency, 200);
    auto frequency = process(T::Shifter, tone(1000), p);
    CHECK(component(frequency, 1200) > .15 && component(frequency, 800) < .015,
          "Frequency shifter selects the upper sideband");
    p[pitchfx::Mode] = choicePosition(2, 3);
    auto ring = process(T::Shifter, tone(1000), p);
    CHECK(component(ring, 1200) > .08 && component(ring, 800) > .08, "Ring modulation creates both sidebands");
    p = defaults(T::PitchCorrector);
    auto corrected = process(T::PitchCorrector, tone(450), p);
    CHECK(component(corrected, 440) > .08 && component(corrected, 450) < .03,
          "Pitch corrector tunes monophonic input to nearest note");
    p[pitchfx::Strength] = 0;
    auto noCorrection = process(T::PitchCorrector, tone(450), p);
    CHECK(component(noCorrection, 450) > .15, "Zero strength preserves original pitch");
    auto detector = audio::createProcessor(T::PitchCorrector, 48000);
    p = defaults(T::PitchCorrector);
    float l[128]{}, r[128]{};
    audio::Context ctx;
    for (int b = 0; b < 80; ++b)
        detector->process(l, r, 128, p.data(), ctx);
    CHECK(detector->scope[0].load() == 0, "Pitch detector rejects silence");
    auto ir = std::make_shared<SampleData>();
    ir->rate = 48000;
    ir->left.resize(48000);
    ir->right.resize(48000);
    ir->left[0] = ir->right[0] = .7f;
    ir->left[24000] = ir->right[24000] = .5f;
    auto cabinet = audio::makeIrKernel(ir, 48000), longIr = audio::makeIrKernel(ir, 48000, 3000);
    CHECK(cabinet->seconds < .18 && longIr->seconds > .99, "Room convolution retains audio beyond cabinet IR limit");
    ctx.ir = longIr.get();
    auto impulse = std::vector<float>(96000);
    impulse[0] = 1;
    p = defaults(T::HybridReverb);
    p[hybrid::Routing] = choicePosition(1, 4);
    p[hybrid::Mix] = 1;
    auto room = process(T::HybridReverb, impulse, p, ctx);
    CHECK(std::any_of(room.begin() + 24128, room.begin() + 24180, [](float x) { return std::abs(x) > .1; }),
          "Convolution renders late room reflection");
    p = defaults(T::HybridReverb);
    p[hybrid::Mix] = 1;
    ctx.ir = nullptr;
    auto tail = process(T::HybridReverb, impulse, p, ctx);
    CHECK(rms(tail, 12000) > .000001, "Algorithmic reverb generates a tail");
    p[hybrid::PreDelay] = pos(T::HybridReverb, hybrid::PreDelay, 200);
    auto delayed = process(T::HybridReverb, impulse, p, ctx);
    CHECK(std::all_of(delayed.begin(), delayed.begin() + 9600, [](float f) { return std::abs(f) < 1e-8; }),
          "Reverb predelay delays wet signal");
    auto frozen = audio::createProcessor(T::HybridReverb, 48000);
    auto freezeParams = defaults(T::HybridReverb);
    freezeParams[hybrid::Mix] = 1;
    double earlyEnergy = 0, lateEnergy = 0;
    for (int b = 0; b < 700; ++b) {
        std::fill_n(l, 128, 0);
        std::fill_n(r, 128, 0);
        if (!b)
            l[0] = r[0] = 1;
        if (b == 100)
            freezeParams[hybrid::Freeze] = 1;
        frozen->process(l, r, 128, freezeParams.data(), ctx);
        for (float v : l) {
            if (b >= 150 && b < 300)
                earlyEnergy += v * v;
            if (b >= 550)
                lateEnergy += v * v;
        }
    }
    CHECK(earlyEnergy > 1e-10 && lateEnergy > earlyEnergy * .75 && lateEnergy < earlyEnergy * 1.3,
          "Freeze sustains the feedback field without damping it away");
    auto loop = audio::createProcessor(T::Looper, 48000);
    p = defaults(T::Looper);
    p[looper::State] = choicePosition(1, 4);
    p[looper::Bars] = 0;
    for (int b = 0; b < 8; ++b) {
        for (int i = 0; i < 128; ++i)
            l[i] = r[i] = float((b * 128 + i) % 31) / 31 * .1f;
        loop->process(l, r, 128, p.data(), ctx);
    }
    p[looper::State] = choicePosition(2, 4);
    std::fill_n(l, 128, 0);
    std::fill_n(r, 128, 0);
    loop->process(l, r, 128, p.data(), ctx);
    CHECK(loop->scope[1].load() == 1024, "Looper records free-length loop");
    CHECK(l[30] > .09 && l[31] == 0, "Looper plays recorded samples");
    p[looper::State] = choicePosition(3, 4);
    for (int b = 0; b < 8; ++b) {
        std::fill_n(l, 128, .1f);
        std::fill_n(r, 128, .1f);
        loop->process(l, r, 128, p.data(), ctx);
    }
    p[looper::State] = choicePosition(2, 4);
    p[looper::Undo] = 1;
    std::fill_n(l, 128, 0);
    std::fill_n(r, 128, 0);
    loop->process(l, r, 128, p.data(), ctx);
    CHECK(*std::max_element(l, l + 128) < .101, "Undo restores prior loop without overdub");
    auto capture = std::make_shared<audio::DeviceCapture>();
    capture->stereo.resize(2048);
    loop->retainCapture(capture);
    CHECK(loop->requestCapture(capture.get()), "Loop accepts export request");
    loop->process(l, r, 128, p.data(), ctx);
    CHECK(capture->state.load() == 1 && capture->frames == 1024, "Loop snapshot completes in preallocated storage");
    p[looper::Clear] = 1;
    loop->process(l, r, 128, p.data(), ctx);
    CHECK(loop->scope[1].load() == 0, "Clear removes recorded loop");
    p[looper::RecordTrigger] = 1;
    p[looper::State] = choicePosition(2, 4);
    loop->process(l, r, 128, p.data(), ctx);
    CHECK(loop->scope[2].load() == 1, "Dedicated controller Record trigger starts recording");
    p[looper::PlayTrigger] = 1;
    loop->process(l, r, 128, p.data(), ctx);
    CHECK(loop->scope[2].load() == 2 && loop->scope[1].load() == 128,
          "Dedicated controller Play trigger closes the recording");
    {
        Project factory;
        auto saved = factory.makeDevice("Looper", DeviceKind::AudioEffect);
        saved.params[looper::State].value = choicePosition(1, 4);
        saved.params[looper::RecordTrigger].value = 1;
        saved.params[looper::Undo].value = 1;
        auto loaded = setfile::deviceFromJson(setfile::deviceToJson(saved));
        CHECK(loaded.params[looper::State].value == 0 && loaded.params[looper::RecordTrigger].value == 0 &&
                  loaded.params[looper::Undo].value == 0,
              "Saved loops reopen stopped without retriggering Record or Undo");
    }
    auto sample = std::make_shared<SampleData>();
    sample->rate = 48000;
    sample->left = tone(440, .3, 48000);
    sample->right = sample->left;
    SampleZone zone;
    zone.sample = sample;
    zone.keyLow = zone.keyHigh = zone.root = 69;
    zone.loop = true;
    zone.loopEnd = 48000;
    audio::Context zctx;
    zctx.sampleZones = &zone;
    zctx.sampleZoneCount = 1;
    auto instrument = audio::createProcessor(T::Multisampler, 48000);
    auto mp = defaults(T::Multisampler);
    audio::EventList notes;
    notes.add({0, 69, 1, audio::kHeld});
    double energy = 0;
    for (int b = 0; b < 40; ++b) {
        std::fill_n(l, 128, 0);
        std::fill_n(r, 128, 0);
        instrument->render(notes, l, r, 128, mp.data(), zctx);
        notes.clear();
        for (float f : l)
            energy += f * f;
    }
    CHECK(energy > 100, "Multisampler plays matching zone");
    instrument->panic();
    notes.add({0, 60, 1, audio::kHeld});
    std::fill_n(l, 128, 0);
    std::fill_n(r, 128, 0);
    instrument->render(notes, l, r, 128, mp.data(), zctx);
    CHECK(std::all_of(l, l + 128, [](float f) { return f == 0; }), "Key zones reject out-of-range notes");
    instrument->panic();
    zone.velocityLow = 100;
    notes.clear();
    notes.add({0, 69, .3f, audio::kHeld});
    instrument->render(notes, l, r, 128, mp.data(), zctx);
    CHECK(instrument->scope[0].load() == 0, "Velocity layers reject quiet notes");
    zone.velocityLow = 1;
    SampleZone zones[] = {zone, zone};
    zones[0].sequenceLength = zones[1].sequenceLength = 2;
    zones[1].sequencePosition = 2;
    zones[0].pan = -1;
    zones[1].pan = 1;
    zctx.sampleZones = zones;
    zctx.sampleZoneCount = 2;
    instrument->panic();
    notes.clear();
    notes.add({0, 69, 1, audio::kHeld});
    std::fill_n(l, 128, 0);
    std::fill_n(r, 128, 0);
    instrument->render(notes, l, r, 128, mp.data(), zctx);
    CHECK(std::all_of(r, r + 128, [](float f) { return f == 0; }) && instrument->scope[0].load() == 1,
          "First round robin selects first zone");
    notes.clear();
    notes.add({0, 69, 1, audio::kHeld});
    std::fill_n(l, 128, 0);
    std::fill_n(r, 128, 0);
    instrument->render(notes, l, r, 128, mp.data(), zctx);
    CHECK(instrument->scope[0].load() == 2 && std::any_of(r, r + 128, [](float f) { return std::abs(f) > 0; }),
          "Second round robin selects second zone");
    QTemporaryDir temp;
    std::vector<float> stereo(96000);
    for (int i = 0; i < 48000; ++i)
        stereo[size_t(i) * 2] = stereo[size_t(i) * 2 + 1] = sample->left[i];
    QString error;
    QString samplePath = temp.filePath("tone A4.wav");
    CHECK(samples::writeWav(samplePath, stereo.data(), 48000, 48000, samples::WavFormat::Float32, &error),
          "Test sample writes");
    QFile sfz(temp.filePath("test.sfz"));
    CHECK(sfz.open(QIODevice::WriteOnly), "SFZ test opens");
    sfz.write("<global> volume=-3 pitch_keycenter=60 lokey=0 hikey=127\n<group> key=a4 seq_length=2\n<region> "
              "seq_position=1 sample=tone A4.wav\n<region> "
              "seq_position=2 pan=100 sample=\"tone A4.wav\"\n");
    sfz.close();
    QVector<SampleZone> imported;
    CHECK(samplezones::readSfz(sfz.fileName(), imported, &error) && imported.size() == 2,
          "SFZ reads quoted/unquoted sample names");
    CHECK(imported.size() == 2 && imported[0].root == 69 && imported[0].keyLow == 69 && imported[1].gain == -3 &&
              imported[1].sequencePosition == 2 && imported[1].pan == 1,
          "SFZ inherits global/group mapping");
    Project project;
    project.scenes = {"1"};
    project.addTrack("Samples", theme::accent);
    project.tracks[0].devices = {project.makeDevice("Multisampler", DeviceKind::Instrument)};
    auto& d = project.tracks[0].devices[0];
    for (auto& z : imported)
        z.sample = samples::load(z.samplePath);
    d.sampleZones = imported;
    auto restored = setfile::deviceFromJson(setfile::deviceToJson(d));
    CHECK(restored.sampleZones == d.sampleZones && restored.sampleZones[0].sample,
          "Zones and sample resources persist");
    audio::Engine engine(&project);
    Transport transport(&project, &engine);
    devui::DeviceContext dc{&project, &transport, 0, project.tracks[0].id, d.id, T::Multisampler};
    auto* body = devui::createBody(dc, nullptr);
    auto* editor = body->createEditor(nullptr);
    editor->show();
    app.processEvents();
    auto* table = editor->findChild<QTableWidget*>("sampleZoneTable");
    CHECK(table && table->rowCount() == 2, "Expanded zone editor shows imported regions");
    table->item(0, 6)->setText("-9");
    app.processEvents();
    CHECK(project.tracks[0].devices[0].sampleZones[0].gain == -9, "Zone editor changes gain");
    project.undoStack()->undo();
    app.processEvents();
    CHECK(project.tracks[0].devices[0].sampleZones[0].gain == -3, "Undo restores zone edit");
    if (argc > 1)
        editor->grab().save(QString(argv[1]) + "/multisampler-editor.png");
    delete editor;
    delete body;
    Project routed;
    routed.scenes = {"1"};
    routed.addTrack("Audio", theme::accent);
    routed.tracks[0].type = TrackType::Audio;
    routed.tracks[0].monitor = Monitor::In;
    routed.tracks[0].devices = {routed.makeDevice("LFO", DeviceKind::AudioEffect),
                                routed.makeDevice("Utility", DeviceKind::AudioEffect)};
    routed.tracks[0].devices[0].params[modulator::Depth].value = 0;
    routed.tracks[0].devices[0].params[modulator::Offset].value = 1;
    routed.tracks[0].devices[0].maps = {{0, routed.tracks[0].devices[1].id, 0, .5, .6}};
    audio::Engine routedEngine(&routed);
    routedEngine.setInputWanted(true);
    routedEngine.publish();
    float input[256], output[256];
    std::fill_n(input, 256, .01f);
    auto settle = [&] {
        for (int b = 0; b < 120; ++b) {
            routedEngine.pushInput(input, 128);
            routedEngine.process(output, 128);
        }
        return output[200];
    };
    double modulated = settle();
    CHECK(std::abs(modulated / .01 - dsp::dbToGain(7)) < .03, "Engine applies LFO mapping to effect parameter");
    routed.tracks[0].devices[0].maps.push_back({0, 0, 0, .2, .2});
    routedEngine.publish();
    double ducked = settle();
    CHECK(std::abs(ducked / modulated - std::pow(.2 / .75, 2)) < .003, "Engine applies modulator to mixer volume");
    routed.tracks[0].devices[0].enabled = false;
    routedEngine.publish();
    CHECK(std::abs(settle() - .01) < .0002, "Bypassing modulator releases device and mixer controls");
    routed.master.devices = {routed.makeDevice("LFO", DeviceKind::AudioEffect)};
    routed.master.devices[0].params[modulator::Depth].value = 0;
    routed.master.devices[0].params[modulator::Offset].value = 1;
    routed.master.devices[0].maps = {{0, 0, 0, .375, .375}};
    routedEngine.publish();
    CHECK(std::abs(settle() - .0025) < .0001, "Modulator controls Master volume");
    Project undoLoop;
    undoLoop.scenes = {"1"};
    undoLoop.addTrack("Loop", theme::accent);
    undoLoop.tracks[0].devices = {undoLoop.makeDevice("Looper", DeviceKind::AudioEffect)};
    {
        EditScope command(&undoLoop, "Record");
        undoLoop.tracks[0].devices[0].params[looper::State].value = choicePosition(1, 4);
        undoLoop.tracks[0].devices[0].params[looper::RecordTrigger].value = 1;
    }
    CHECK(undoLoop.undoStack()->count() == 0, "Looper commands do not create project undo entries");
    {
        EditScope gain(&undoLoop, "Loop gain");
        undoLoop.tracks[0].devices[0].params[looper::Output].value = .7;
    }
    undoLoop.tracks[0].devices[0].params[looper::State].value = 0;
    undoLoop.tracks[0].devices[0].params[looper::StopTrigger].value = 1;
    undoLoop.undoStack()->undo();
    CHECK(undoLoop.tracks[0].devices[0].params[looper::State].value == 0 &&
              undoLoop.tracks[0].devices[0].params[looper::StopTrigger].value == 1,
          "Undoing a parameter edit cannot retrigger Record");
    {
        EditScope remove(&undoLoop, "Remove Looper");
        undoLoop.tracks[0].devices.clear();
    }
    undoLoop.undoStack()->undo();
    CHECK(undoLoop.tracks[0].devices[0].params[looper::State].value == 0 &&
              undoLoop.tracks[0].devices[0].params[looper::RecordTrigger].value == 0,
          "Restoring a deleted Looper clears transport commands");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
