#include "audio/Dsp.h"
#include "audio/Engine.h"
#include "audio/ProductionEffects.h"
#include "core/Presets.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/devices/DeviceBody.h"
#include <QApplication>
#include <QDir>
#include <QTemporaryDir>
#include <QUndoStack>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
using namespace sessions;
using T = DeviceType;
static int checks = 0, failures = 0;
static bool audit = false;
static size_t allocations = 0;
void* operator new(std::size_t n) {
    if (audit)
        ++allocations;
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    return ::operator new(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
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
static double render(T t, std::vector<float> p, double hz, double amplitude = .1, double sr = 48000, int channel = 0) {
    auto effect = audio::createProcessor(t, sr);
    float l[128], r[128];
    double sum = 0;
    int count = 0;
    audio::Context ctx;
    for (int b = 0; b < 600; ++b) {
        for (int i = 0; i < 128; ++i) {
            float x = amplitude * std::sin(2 * dsp::kPi * hz * (b * 128 + i) / sr);
            l[i] = x;
            r[i] = channel == 1 ? -x : x;
        }
        effect->process(l, r, 128, p.data(), ctx);
        if (b > 300)
            for (int i = 0; i < 128; ++i) {
                sum += l[i] * l[i];
                ++count;
            }
    }
    return std::sqrt(sum / count);
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    deviceSpecs();
    const T types[] = {T::ParametricEQ,     T::Spectrum, T::MultibandDynamics, T::Lfo,
                       T::EnvelopeFollower, T::Shaper,   T::Shifter,           T::PitchCorrector,
                       T::HybridReverb,     T::Looper,   T::Multisampler};
    for (T t : types) {
        auto p = defaults(t);
        CHECK(p.size() <= audio::kMaxParams, "Parameters fit the engine");
        auto effect = audio::createProcessor(t, 48000);
        CHECK(effect != nullptr, "Processor exists");
        Project factory;
        Device d = factory.makeDevice(specOf(t)->name, DeviceKind::AudioEffect);
        auto restored = setfile::deviceFromJson(setfile::deviceToJson(d));
        CHECK(restored.name == d.name && restored.params.size() == d.params.size(),
              "Device persists through set format");
        float l[128]{}, r[128]{};
        audio::Context ctx;
        allocations = 0;
        audit = true;
        for (int b = 0; b < 40; ++b) {
            std::fill_n(l, 128, .01f);
            std::fill_n(r, 128, -.01f);
            effect->process(l, r, 128, p.data(), ctx);
        }
        effect->panic();
        audit = false;
        CHECK(allocations == 0, "Rendering and panic allocate no memory");
        CHECK(std::all_of(l, l + 128, [](float x) { return std::isfinite(x); }), "Output stays finite");
    }
    CHECK(paramCount(T::EQ) == 10, "Legacy EQ layout stays intact");
    auto eq = defaults(T::ParametricEQ);
    CHECK(std::abs(render(T::ParametricEQ, eq, 997) / (.1 / std::sqrt(2.)) - 1) < .001,
          "Neutral parametric EQ preserves amplitude");
    namespace pe = parametric;
    eq[pe::param(3, pe::Frequency)] = pos(T::ParametricEQ, pe::param(3, pe::Frequency), 997);
    eq[pe::param(3, pe::Gain)] = pos(T::ParametricEQ, pe::param(3, pe::Gain), 12);
    CHECK(std::abs(render(T::ParametricEQ, eq, 997) / (.1 / std::sqrt(2.)) - dsp::dbToGain(12)) < .015,
          "Bell applies specified gain");
    eq[pe::param(3, pe::Channel)] = choicePosition(3, 5);
    CHECK(std::abs(render(T::ParametricEQ, eq, 997, .1, 48000, 1) / (.1 / std::sqrt(2.)) - 1) < .002,
          "Mid processing preserves side signal");
    eq[pe::param(3, pe::Channel)] = choicePosition(4, 5);
    CHECK(std::abs(render(T::ParametricEQ, eq, 997) / (.1 / std::sqrt(2.)) - 1) < .002,
          "Side processing preserves mono material");
    eq[pe::param(3, pe::Channel)] = 0;
    eq[pe::param(3, pe::Type)] = choicePosition(5, 7);
    CHECK(render(T::ParametricEQ, eq, 997) < .00002, "Notch rejects center frequency");
    for (int sr : {32000, 44100, 48000, 96000})
        for (int type = 0; type < 7; ++type) {
            auto p = eq;
            p[pe::param(3, pe::Type)] = choicePosition(type, 7);
            p[pe::param(3, pe::Frequency)] = 1;
            p[pe::param(3, pe::Q)] = 1;
            CHECK(std::isfinite(render(T::ParametricEQ, p, 12000, .1, sr)),
                  "Filters remain finite across sample rates");
        }
    auto spectrum = audio::createProcessor(T::Spectrum, 48000);
    auto sp = defaults(T::Spectrum);
    float l[128], r[128];
    audio::Context ctx;
    for (int b = 0; b < 100; ++b) {
        for (int i = 0; i < 128; ++i) {
            l[i] = .5f * std::sin(dsp::kTwoPi * 1000 * (b * 128 + i) / 48000.f);
            r[i] = -l[i];
        }
        float before = l[60];
        spectrum->process(l, r, 128, sp.data(), ctx);
        CHECK(l[60] == before && r[60] == -before, "Spectrum passes audio unchanged");
    }
    float maximum = -120;
    int bin = 0;
    for (int b = 0; b < 256; ++b)
        if (spectrum->spectrum(b, true) > maximum) {
            maximum = spectrum->spectrum(b, true);
            bin = b;
        }
    double hz = 20 * std::pow(1000., bin / 255.);
    CHECK(hz > 950 && hz < 1050 && maximum > -8 && maximum < -4, "Spectrum detects anti-phase tone at correct level");
    sp[1] = 1;
    float held = spectrum->spectrum(bin, true);
    for (int b = 0; b < 50; ++b) {
        std::fill_n(l, 128, 0);
        std::fill_n(r, 128, 0);
        spectrum->process(l, r, 128, sp.data(), ctx);
    }
    CHECK(spectrum->spectrum(bin, true) == held, "Hold freezes spectrum");
    auto mb = defaults(T::MultibandDynamics);
    for (double f : {50, 120, 500, 2500, 9000})
        CHECK(std::abs(render(T::MultibandDynamics, mb, f) / (.1 / std::sqrt(2.)) - 1) < .004,
              "Crossovers reconstruct flat amplitude");
    namespace md = multiband;
    mb[md::param(1, md::AboveRatio)] = pos(T::MultibandDynamics, md::param(1, md::AboveRatio), 8);
    mb[md::param(1, md::AboveThreshold)] = pos(T::MultibandDynamics, md::param(1, md::AboveThreshold), -30);
    CHECK(render(T::MultibandDynamics, mb, 700, .6) <
              render(T::MultibandDynamics, defaults(T::MultibandDynamics), 700, .6) * .5,
          "Compression controls loud material");
    mb = defaults(T::MultibandDynamics);
    mb[md::param(1, md::BelowRatio)] = pos(T::MultibandDynamics, md::param(1, md::BelowRatio), .5);
    CHECK(render(T::MultibandDynamics, mb, 700, .001) >
              render(T::MultibandDynamics, defaults(T::MultibandDynamics), 700, .001) * 2,
          "Upward compression raises quiet material");
    auto mod = audio::createProcessor(T::Lfo, 48000);
    auto mp = defaults(T::Lfo);
    mp[modulator::Depth] = .3;
    mp[modulator::Offset] = .2;
    ctx.playing = true;
    ctx.beat = 0;
    ctx.tempo = 120;
    mod->process(l, r, 128, mp.data(), ctx);
    CHECK(mod->modulationValue() >= .2 && mod->modulationValue() <= .201, "LFO starts at offset");
    ctx.beat = 2;
    mod->process(l, r, 128, mp.data(), ctx);
    CHECK(mod->modulationValue() > .499 && mod->modulationValue() < .501, "LFO sync follows beat");
    auto shaper = audio::createProcessor(T::Shaper, 48000);
    auto shp = defaults(T::Shaper);
    CurvePoint curve[] = {{0, .8}, {1, .8}};
    int counts[] = {2, 0};
    ctx.curves = curve;
    ctx.curveCount = counts;
    shaper->process(l, r, 128, shp.data(), ctx);
    CHECK(std::abs(shaper->modulationValue() - .8) < 1e-6, "Shaper reads drawn curve");
    auto follower = audio::createProcessor(T::EnvelopeFollower, 48000);
    auto fp = defaults(T::EnvelopeFollower);
    for (int b = 0; b < 100; ++b) {
        std::fill_n(l, 128, .5f);
        std::fill_n(r, 128, .5f);
        follower->process(l, r, 128, fp.data(), ctx);
    }
    CHECK(std::abs(follower->modulationValue() - .5) < .001, "Follower tracks amplitude");
    float key[128];
    std::fill_n(key, 128, .25f);
    ctx.keyL = ctx.keyR = key;
    for (int b = 0; b < 500; ++b) {
        std::fill_n(l, 128, 0);
        std::fill_n(r, 128, 0);
        follower->process(l, r, 128, fp.data(), ctx);
    }
    CHECK(std::abs(follower->modulationValue() - .25) < .001 && l[0] == 0,
          "External follower key controls modulation without changing audio");
    ctx.keyL = ctx.keyR = nullptr;
    ctx.curves = nullptr;
    ctx.curveCount = nullptr;
    shaper->panic();
    shaper->process(l, r, 128, shp.data(), ctx);
    CHECK(shaper->modulationValue() > .5 && shaper->modulationValue() < .502,
          "Empty Shaper follows the displayed rising ramp");
    Project project;
    project.scenes = {"1"};
    project.addTrack("Audio", theme::accent);
    project.tracks[0].type = TrackType::Audio;
    project.tracks[0].monitor = Monitor::In;
    project.tracks[0].devices = {project.makeDevice("LFO", DeviceKind::AudioEffect),
                                 project.makeDevice("Utility", DeviceKind::AudioEffect)};
    Device source = project.tracks[0].devices[0];
    source.params[modulator::Depth].value = 0;
    source.params[modulator::Offset].value = 1;
    source.maps = {{0, project.tracks[0].devices[1].id, 0, .5, .6}, {0, 0, 0, .2, .2}};
    auto restored = setfile::deviceFromJson(setfile::deviceToJson(source));
    CHECK(restored.maps.size() == 2, "Device and mixer mappings persist");
    project.tracks[0].devices[0] = source;
    normalizeRacks(project.tracks[0].devices);
    CHECK(project.tracks[0].devices[0].maps.size() == 2, "Mixer maps survive normalization");
    source.name = "Shaper";
    source.curves.resize(1);
    for (int i = 0; i <= 32; ++i)
        source.curves[0].push_back({i / 32., i / 32.});
    restored = setfile::deviceFromJson(setfile::deviceToJson(source));
    CHECK(restored.curves[0].size() == 33, "Shaper saves its endpoint");
    source.name = "LFO";
    audio::Engine engine(&project);
    Transport transport(&project, &engine);
    QTemporaryDir temp;
    QString error;
    Project::DeviceBlocks blocks{{source, project.tracks[0].devices[1]}};
    CHECK(presets::save(temp.filePath("mod.sessionspreset"), "Mod", blocks, &error), "Modulator preset saves");
    Project::DeviceBlocks loaded;
    CHECK(presets::read(temp.filePath("mod.sessionspreset"), loaded, nullptr, &error) && loaded[0][0].maps.size() == 2,
          "Preset keeps device and mixer mappings");
    QString captures = argc > 1 ? argv[1] : QString();
    if (!captures.isEmpty())
        QDir().mkpath(captures);
    for (T t : types) {
        project.tracks[0].devices = {project.makeDevice(specOf(t)->name, DeviceKind::AudioEffect)};
        const int id = project.tracks[0].devices[0].id;
        devui::DeviceContext dc{&project, &transport, 0, project.tracks[0].id, id, t};
        auto* body = devui::createBody(dc, nullptr);
        body->show();
        app.processEvents();
        if (body->minimumSizeHint().height() > 192)
            std::printf("PANEL %s min %d\n", specOf(t)->name, body->minimumSizeHint().height());
        CHECK(body->height() == 192 && body->minimumSizeHint().height() <= 192, "Panel fits standard height");
        for (auto* child : body->findChildren<QWidget*>())
            if (child->isVisible()) {
                QPoint at = child->mapTo(body, QPoint());
                CHECK(at.y() >= 0 && at.y() + child->height() <= body->height() + 1, "Controls fit panel height");
            }
        if (!captures.isEmpty()) {
            body->grab().save(captures + "/" + QString(specOf(t)->name).replace(' ', '-') + "-dark.png");
            theme::apply(theme::Kind::Light);
            app.processEvents();
            body->grab().save(captures + "/" + QString(specOf(t)->name).replace(' ', '-') + "-light.png");
            theme::apply(theme::Kind::Dark);
        }
        delete body;
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
