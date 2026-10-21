// Extended oscillator audit: actual audio, append-only persistence, FM, folders
// and callback safety. All source material is generated in memory.
#include "audio/Devices.h"
#include "core/DeviceSpecs.h"
#include "core/SetFile.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

using namespace sessions;
namespace mo = monolith;
namespace os = osmium;

namespace {
int checks = 0, failures = 0;
std::atomic<bool> watchAllocations{false};
std::atomic<int> allocations{0};

void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
}

std::vector<float> defaults() {
    Project project;
    const auto device = project.makeDevice("Osmium", DeviceKind::Instrument);
    std::vector<float> params(audio::kMaxParams);
    for (int i = 0; i < device.params.size(); ++i) params[i] = device.params[i].value;
    return params;
}

std::vector<float> simple() {
    auto params = defaults();
    params[mo::OscRestart] = 1;
    params[mo::Volume] = .65;
    params[mo::VelAmp] = 0;
    params[mo::eparam(0, mo::A)] = 0;
    params[mo::eparam(0, mo::R)] = .1;
    for (int osc = 0; osc < 3; ++osc) {
        params[mo::oparam(osc, mo::Table)] = 0;
        params[mo::oparam(osc, mo::Position)] = 0;
        params[mo::oparam(osc, mo::Level)] = osc == 0 ? .55 : 0;
        params[mo::oparam(osc, mo::Pitch)] = .5;
        params[mo::oparam(osc, mo::Fine)] = .5;
    }
    for (int filter = 0; filter < 2; ++filter) {
        params[mo::fparam(filter, mo::FCutoff)] = 1;
        params[mo::fparam(filter, mo::FReso)] = 0;
    }
    return params;
}

struct Render {
    std::vector<float> audio;
    double rms = 0, peak = 0, tail = 0, dc = 0;
    bool finite = true;
};

Render render(const std::vector<float>& params, int pitch = 48, int rate = 48000,
              int block = 128, const QVector<ModRoute>& mods = {}, float bend = 0,
              double seconds = .5, bool held = false) {
    auto processor = audio::createProcessor(DeviceType::Osmium, rate);
    audio::Context context;
    context.sampleRate = rate;
    context.tempo = 174;
    context.beatsPerSample = 174. / (60 * rate);
    context.playing = true;
    context.mods = mods.constData();
    context.modCount = mods.size();
    context.bend = bend;
    const int frames = int(rate * seconds);
    Render out;
    out.audio.resize(size_t(frames) * 2);
    double sum = 0, tail = 0, dc = 0;
    for (int offset = 0; offset < frames; offset += block) {
        const int count = std::min(block, frames - offset);
        float left[audio::kMaxBlock]{}, right[audio::kMaxBlock]{};
        audio::EventList events;
        if (!offset) events.add({0, pitch, .8f, held ? frames + rate : rate / 5});
        context.beat = offset * context.beatsPerSample;
        watchAllocations = true;
        processor->render(events, left, right, count, params.data(), context);
        watchAllocations = false;
        for (int i = 0; i < count; ++i) {
            out.finite &= std::isfinite(left[i]) && std::isfinite(right[i]);
            out.peak = std::max(out.peak, double(std::max(std::abs(left[i]), std::abs(right[i]))));
            const double energy = (double(left[i]) * left[i] + double(right[i]) * right[i]) * .5;
            sum += energy;
            out.audio[size_t(offset + i) * 2] = left[i];
            out.audio[size_t(offset + i) * 2 + 1] = right[i];
            if (offset + i > frames - rate / 20) tail += energy;
            if (offset + i >= frames - rate / 2) dc += (double(left[i]) + right[i]) * .5;
        }
    }
    out.rms = std::sqrt(sum / frames);
    out.tail = std::sqrt(tail / (rate / 20));
    out.dc = dc / (rate / 2);
    return out;
}

double difference(const Render& a, const Render& b) {
    double sum = 0;
    for (size_t i = 0; i < a.audio.size(); ++i) {
        const double delta = a.audio[i] - b.audio[i];
        sum += delta * delta;
    }
    return std::sqrt(sum / a.audio.size());
}
} // namespace

void* operator new(std::size_t size) {
    if (watchAllocations.load(std::memory_order_relaxed)) ++allocations;
    if (void* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    QCoreApplication app(argc, argv);
    const auto* spec = specOf(DeviceType::Osmium);
    const auto* legacy = specOf(DeviceType::Monolith);
    check(os::kParams == 191 && os::kParams <= audio::kMaxParams && legacy->params.size() == 170,
          "new oscillator controls fit existing audio capacity without expanding Monolith");
    bool original = true;
    for (int i = 0; i < 170; ++i) {
        const auto& a = spec->params[i];
        const auto& b = legacy->params[i];
        original &= QString(a.name) == b.name && a.kind == b.kind && a.lo == b.lo && a.hi == b.hi &&
                    a.def == b.def && a.choices == b.choices;
    }
    check(original, "all original Monolith parameter names, mappings, choices and defaults retain their indices in Osmium");
    for (int index = 0; index < 48; ++index) {
        const double oldPosition = index < 8 ? double(index) / 7 : 0;
        const double extension = index < 8 ? 0 : double(index - 7) / 40;
        check(os::tableIndex(oldPosition, extension) == index,
              "all forty-eight table selections resolve without renormalizing legacy automation");
    }
    for (int mode = 0; mode < 8; ++mode) {
        const double oldPosition = mode < 3 ? double(mode) / 2 : 0;
        const double extension = mode < 3 ? 0 : double(mode - 2) / 5;
        check(os::warpMode(oldPosition, extension) == mode, "all eight warp modes resolve through append-only selectors");
    }
    bool safeDefaults = true;
    for (int i = os::kLegacyParams; i < os::kParams; ++i) safeDefaults &= spec->params[i].def == 0;
    check(safeDefaults, "new table banks, warp shapes and folders default to Legacy or exact bypass");

    Project project;
    Device device = project.makeDevice("Osmium", DeviceKind::Instrument);
    device.params[os::tableExtension(0)].value = 1;
    device.params[os::warpExtension(1)].value = 1;
    device.params[os::fold(2)].value = .67;
    device.mods = {{mo::SrcLfo4, os::fold(0), .7}};
    auto restored = setfile::deviceFromJson(setfile::deviceToJson(device));
    check(restored.params.size() == os::kParams && restored.params[os::tableExtension(0)].value == 1 &&
          restored.params[os::warpExtension(1)].value == 1 && restored.params[os::fold(2)].value == .67 && restored.mods == device.mods,
          "new table, FM and fold parameters and routes survive set persistence");
    auto json = setfile::deviceToJson(project.makeDevice("Osmium", DeviceKind::Instrument));
    auto values = json.value("params").toArray();
    while (values.size() > os::kLegacyParams) values.removeLast();
    json.insert("params", values);
    restored = setfile::deviceFromJson(json);
    bool safeLegacy = restored.params.size() == os::kParams;
    for (int i = os::kLegacyParams; i < restored.params.size(); ++i) safeLegacy &= restored.params[i].value == 0;
    check(safeLegacy, "loading a pre-expansion Osmium initializes new controls to bypass");

    for (int mode = 0; mode < 8; ++mode) {
        bool bounded = true, identity = true;
        for (int i = 0; i < 997; ++i) {
            const float phase = float(i) / 997;
            identity &= audio::osmiumWarpPhase(mode, phase, 0) == phase;
            for (float amount : {.01f, .3f, .7f, 1.f}) {
                const float warped = audio::osmiumWarpPhase(mode, phase, amount);
                bounded &= std::isfinite(warped) && warped >= 0 && warped < 1;
            }
        }
        check(bounded && identity, "every phase warp is finite, stays inside its table and is an exact identity at zero");
    }
    bool odd = true, bounded = true, bypass = true;
    for (int i = -1000; i <= 1000; ++i) {
        const float input = i / 1000.f;
        bypass &= audio::osmiumFold(input, 0) == input;
        for (float amount : {.1f, .5f, 1.f}) {
            const float output = audio::osmiumFold(input, amount);
            odd &= std::abs(output + audio::osmiumFold(-input, amount)) < 3e-6f;
            bounded &= std::isfinite(output) && std::abs(output) <= 1.000001f;
        }
    }
    check(odd && bounded && bypass, "the visual wavefolder is odd, bounded and exactly bypassed at zero");

    const auto base = simple();
    const auto dry = render(base);
    check(dry.finite && dry.rms > .01 && dry.peak < 1 && dry.tail < 1e-4,
          "generated reference tone is audible, has headroom and releases cleanly");
    const auto highReference = render(base, 72);
    for (int table = 8; table < 48; ++table) {
        auto params = base;
        params[os::tableExtension(0)] = float(table - 7) / 40;
        params[mo::oparam(0, mo::Position)] = .43;
        const auto out = render(params, 72);
        check(out.finite && out.rms > .002 && out.peak < 1 && difference(out, highReference) > .001,
              "each expanded table produces actual stable, audible oscillator output at the same pitch as the reference");
    }
    for (int mode = 3; mode < 8; ++mode) {
        auto params = base;
        params[os::warpExtension(0)] = float(mode - 2) / 5;
        const auto zeroAmount = render(params);
        check(zeroAmount.audio == dry.audio, "new warp selections at zero amount preserve exact dry oscillator audio");
        params[mo::oparam(0, mo::Intensity)] = .87;
        params[mo::oparam(1, mo::Pitch)] = double(12 + 36) / 72;
        const auto out = render(params);
        const double changed = difference(out, dry);
        if (!out.finite || out.peak >= 1 || out.tail >= 1e-4 || changed <= .002)
            std::printf("Warp audit mode %d: finite %d, peak %.6f, tail %.6g, difference %.6f\n",
                        mode, out.finite, out.peak, out.tail, changed);
        check(out.finite && out.peak < 1 && out.tail < 1e-4 && changed > .002,
              "each new warp changes actual audio, retains headroom and releases cleanly");
    }
    auto folded = base;
    folded[os::fold(0)] = .7;
    const auto foldAudio = render(folded);
    check(foldAudio.finite && foldAudio.peak < 1 && difference(foldAudio, dry) > .002,
          "per-oscillator wavefolding changes actual audio through its antialiased path");
    const auto modFold = render(base, 48, 48000, 128, {{mo::SrcLfo1, os::fold(0), .8}});
    check(modFold.finite && difference(modFold, dry) > .001, "wavefold depth responds to per-voice modulation routes");
    for (int effect = 0; effect < 3; ++effect) {
        auto params = base;
        params[mo::eparam(0, mo::S)] = 1;
        if (effect == 0) {
            params[os::tableExtension(0)] = float(10 - 7) / 40; // asymmetric PWM Drift
            params[mo::oparam(0, mo::Position)] = .9;
            params[os::fold(0)] = .8;
        } else {
            params[os::warpExtension(0)] = float((effect == 1 ? 5 : 6) - 2) / 5; // Asym / Steps
            params[mo::oparam(0, mo::Intensity)] = .87;
        }
        const auto settled = render(params, 69, 48000, 128, {}, 0, 2, true);
        check(settled.finite && settled.rms > .005 && std::abs(settled.dc) < 1e-4,
              "sustained asymmetric folding and phase shaping remove DC after the blocker settles");
        std::printf("DC audit %s: settled mean %.3g, RMS %.4f\n",
                    effect == 0 ? "PWM fold" : effect == 1 ? "Asym" : "Steps", settled.dc, settled.rms);
    }

    auto fm = base;
    fm[os::warpExtension(0)] = 1;
    fm[mo::oparam(0, mo::Intensity)] = .55;
    fm[mo::oparam(1, mo::Pitch)] = .5;
    const auto samePitch = render(fm);
    fm[mo::oparam(1, mo::Pitch)] = double(19 + 36) / 72;
    const auto ratio = render(fm);
    check(difference(samePitch, ratio) > .002 && fm[mo::oparam(1, mo::Level)] == 0,
          "Cross FM follows the next oscillator's pitch even when its audible level is zero");
    fm[mo::oparam(1, mo::Position)] = .72;
    const auto color = render(fm);
    check(difference(ratio, color) > .002, "Cross FM uses the next oscillator's actual selected waveform");
    const auto smallBlock = render(folded, 48, 48000, 64);
    check(difference(smallBlock, foldAudio) < 1e-7, "folder history and oscillator phase remain continuous across callback sizes");

    auto mutual = base;
    for (int osc = 0; osc < 3; ++osc) {
        mutual[os::warpExtension(osc)] = 1;
        mutual[mo::oparam(osc, mo::Intensity)] = .35;
        mutual[mo::oparam(osc, mo::Position)] = .13 + osc * .27;
        mutual[mo::oparam(osc, mo::Level)] = .2;
        mutual[mo::oparam(osc, mo::Pitch)] = double(36 + osc * 7) / 72;
        mutual[os::tableExtension(osc)] = float(4 + osc * 13) / 40;
    }
    auto rotated = mutual;
    for (int osc = 0; osc < 3; ++osc) {
        const int source = (osc + 1) % 3;
        for (int knob = 0; knob < mo::kPerOsc; ++knob)
            rotated[mo::oparam(osc, mo::OscKnob(knob))] = mutual[mo::oparam(source, mo::OscKnob(knob))];
        rotated[os::tableExtension(osc)] = mutual[os::tableExtension(source)];
        rotated[os::warpExtension(osc)] = mutual[os::warpExtension(source)];
        rotated[os::fold(osc)] = mutual[os::fold(source)];
    }
    const auto mutualAudio = render(mutual);
    const auto rotatedAudio = render(rotated);
    check(mutualAudio.finite && rotatedAudio.finite && difference(mutualAudio, rotatedAudio) < 1e-6,
          "cyclically rotating a mutual-FM network preserves audio independently of oscillator iteration order");
    for (int rate : {44100, 48000, 96000}) {
        auto params = fm;
        for (int osc = 0; osc < 3; ++osc) {
            params[os::tableExtension(osc)] = float(13 + osc * 9) / 40;
            params[os::warpExtension(osc)] = 1;
            params[os::fold(osc)] = .8;
            params[mo::oparam(osc, mo::Intensity)] = 1;
            params[mo::oparam(osc, mo::Pitch)] = 1;
            params[mo::oparam(osc, mo::Level)] = .18;
        }
        const auto out = render(params, 127, rate);
        check(out.finite && out.peak < 2, "mutual FM plus folding remains bounded at extreme pitch and multiple sample rates");
    }
    for (int rate : {8000, 44100, 48000, 96000}) for (int wave = 0; wave < 4; ++wave) {
        auto params = base;
        params[mo::ModPitch] = 1;
        params[mo::BendRange] = 1;
        params[mo::ModPhase] = 1;
        params[mo::ModRing] = 1;
        params[mo::ModOscWave] = float(wave) / 3;
        params[mo::SubLevel] = 1;
        params[mo::SubOct] = 0;
        params[mo::SubWave] = float(std::min(wave, 2)) / 2;
        const auto out = render(params, 127, rate, 128, {}, 1);
        check(out.finite && out.peak < 2,
              "extreme modulation/sub pitches, full ring modulation and pitch bend keep all wave phases bounded");
    }
    check(allocations == 0, "extended table scanning, FM, phase shaping and folding allocate nothing in the audio callback");
    std::printf("osmium_dsp: %d checks, %d failures; callback allocations %d\n", checks, failures, allocations.load());
    return failures ? 1 : 0;
}
