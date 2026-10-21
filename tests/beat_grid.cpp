#include "core/BeatGrid.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
constexpr double pi = 3.14159265358979323846;
double knownBeat(double seconds, double duration, bool varying) {
    const double after = std::max(0.0, seconds - 30);
    return 174 * seconds / 60 + (varying ? 8 * after * after / (120 * (duration - 30)) : 0);
}
SampleData drums(double rate, double duration, bool varying, double silentIntro = 0, double gapStart = 0, double gapEnd = 0, double bridgeExtraBeats = 0, bool strongPulse = false) {
    SampleData sample;
    sample.rate = rate;
    sample.left.resize(size_t(std::lround(duration * rate)));
    std::mt19937 random(74191);
    std::uniform_real_distribution<double> noise(-1, 1);
    const int steps = int(knownBeat(duration, duration, varying) * 4);
    for (int step = 0; step < steps; ++step) {
        const double target = step * .25;
        double time = target * 60 / 174;
        for (int iteration = 0; iteration < 6; ++iteration) {
            const double bpm = 174 + (varying ? 8 * std::max(0.0, time - 30) / (duration - 30) : 0);
            const double bridge = gapEnd > gapStart ? std::clamp((time - gapStart) / (gapEnd - gapStart), 0.0, 1.0) : 0;
            time -= (knownBeat(time, duration, varying) + bridgeExtraBeats * bridge - target) * 60 / bpm;
        }
        if (time < silentIntro || (time >= gapStart && time < gapEnd)) continue;
        const int begin = int(std::lround(time * rate));
        const bool kick = step % 16 == 0 || step % 16 == 10;
        const bool snare = step % 16 == 4 || step % 16 == 12;
        for (int i = 0; i < int(.085 * rate) && begin + i < int(sample.left.size()); ++i) {
            const double t = i / rate;
            double value = (strongPulse ? .02 : .13) * noise(random) * std::exp(-t * 180);
            if (kick) value += .65 * std::sin(2 * pi * 65 * t) * std::exp(-t * 35);
            if (snare) value += .45 * noise(random) * std::exp(-t * 60);
            sample.left[size_t(begin + i)] += float(value);
        }
    }
    sample.right = sample.left;
    return sample;
}
double beatAt(const tempo::TimingGrid& grid, double time) {
    int index = 0;
    while (index + 2 < grid.points.size() && grid.points[index + 1].seconds < time) ++index;
    const auto a = grid.points[index], b = grid.points[index + 1];
    return a.beat + (b.beat - a.beat) * (time - a.seconds) / (b.seconds - a.seconds);
}
bool monotonic(const tempo::TimingGrid& grid) {
    if (grid.points.size() < 2 || grid.points.front().beat != 0 || grid.points.front().seconds != 0) return false;
    for (int i = 1; i < grid.points.size(); ++i)
        if (!std::isfinite(grid.points[i].beat) || grid.points[i].beat <= grid.points[i - 1].beat
            || grid.points[i].seconds <= grid.points[i - 1].seconds) return false;
    return true;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    auto steady = drums(8000, 180, false);
    const auto constant = tempo::trackGrid(steady, 174);
    check(!constant.variable && constant.points.size() == 2, "steady long drums retain a constant source grid");
    check(constant.points.back().seconds == steady.seconds() && constant.points.back().beat == steady.seconds() * 174 / 60,
          "constant fallback retains exact source duration and accepted source tempo");

    auto varying = drums(8000, 180, true);
    const auto tracked = tempo::trackGrid(varying, 174);
    std::printf("varying: variable %d, confidence %.4f, markers %d\n", tracked.variable, tracked.confidence, int(tracked.points.size()));
    check(tracked.variable && tracked.confidence > .2 && tracked.points.size() > 2, "sustained acceleration has independent rhythmic support for a variable grid");
    check(monotonic(tracked) && tracked.points.back().seconds == varying.seconds(), "adaptive grid preserves zero and exact endpoint with strictly increasing beats and times");
    double worst = 0, fixedWorst = 0;
    for (double time = 12; time < 174; time += 3.25) {
        worst = std::max(worst, std::abs(beatAt(tracked, time) - knownBeat(time, 180, true)) * 60 / 174);
        fixedWorst = std::max(fixedWorst, std::abs(time * 174 / 60 - knownBeat(time, 180, true)) * 60 / 174);
    }
    std::printf("known acceleration: adaptive worst %.4f ms; constant worst %.2f ms\n", worst * 1000, fixedWorst * 1000);
    check(worst < .020 && fixedWorst > 2, "broad adaptive timing predicts held-out beats within 20 ms where a fixed tempo drifts seconds");
    check(tracked.points.size() < 100, "slow musical tempo changes do not generate markers for individual transients");

    auto inverted = varying;
    for (float& value : inverted.right) value = -value;
    const auto stereo = tempo::trackGrid(inverted, 174);
    bool same = stereo.variable == tracked.variable && stereo.points.size() == tracked.points.size();
    for (int i = 0; same && i < tracked.points.size(); ++i)
        same = std::abs(stereo.points[i].seconds - tracked.points[i].seconds) < 1e-9 && std::abs(stereo.points[i].beat - tracked.points[i].beat) < 1e-9;
    check(same, "anti-phase stereo cannot erase or change the rhythmic grid");

    const auto introSample = drums(8000, 180, true, 12);
    const auto intro = tempo::trackGrid(introSample, 174);
    check(intro.variable && monotonic(intro), "silent intro leaves enough later evidence to track sustained acceleration");
    check(intro.points.front().beat == 0 && intro.points.front().seconds == 0 && std::abs(beatAt(intro, 12) - 34.8) < .03,
          "silent intro is preserved instead of moving the first audible hit to beat zero");

    const auto gappedSample = drums(8000, 180, true, 0, 92, 116);
    const auto gapped = tempo::trackGrid(gappedSample, 174);
    check(monotonic(gapped), "a recording with a drumless bridge always retains a valid monotonic map");
    check(!gapped.variable, "a sixteenth-note-only pulse abstains when the beat count through a silent bridge is ambiguous");

    const auto phaseBridgeSample = drums(8000, 180, true, 0, 92, 116, .25, true);
    const auto phaseBridge = tempo::trackGrid(phaseBridgeSample, 174);
    std::printf("bridge: variable %d, predicted %.6f, expected %.6f\n", phaseBridge.variable, beatAt(phaseBridge, 160), knownBeat(160, 180, true) + .25);
    check(phaseBridge.variable && std::abs(beatAt(phaseBridge, 160) - knownBeat(160, 180, true) - .25) * 60 / 174 < .025,
          "eighth-note phase resolves a concealed sixteenth-note count error across a drumless bridge");

    const auto highRateSample = drums(44100, 120, true);
    const auto highRate = tempo::trackGrid(highRateSample, 174);
    check(highRate.variable && std::abs(beatAt(highRate, 108) - knownBeat(108, 120, true)) * 60 / 174 < .020,
          "adaptive timing remains accurate at a non-integral analysis-hop sample rate");

    SampleData quiet = varying;
    for (float& value : quiet.left) value *= .001f;
    for (float& value : quiet.right) value *= .001f;
    const auto quietGrid = tempo::trackGrid(quiet, 174);
    check(quietGrid.variable && std::abs(beatAt(quietGrid, 160) - beatAt(tracked, 160)) < .005,
          "turning rhythmic audio down does not change the inferred performance tempo");

    SampleData silence;
    silence.rate = 8000; silence.left.resize(180 * 8000); silence.right = silence.left;
    const auto silent = tempo::trackGrid(silence, 174);
    check(!silent.variable && silent.points.size() == 2, "silence never fabricates changing tempo markers");
    std::mt19937 random(5524);
    std::uniform_real_distribution<float> noise(-.001f, .001f);
    for (float& value : silence.left) value = noise(random);
    silence.right = silence.left;
    check(!tempo::trackGrid(silence, 174).variable, "weak unpitched noise cannot supply an adaptive beat grid");

    auto shortSample = drums(8000, 20, false);
    check(!tempo::trackGrid(shortSample, 174).variable, "short loops retain their accepted constant grid");
    auto tail = drums(8000, 180, true, 0, 25, 181);
    check(!tempo::trackGrid(tail, 174).variable, "a long silent tail does not extrapolate a variable performance from a short opening");
    check(tempo::trackGrid(varying, 0).points.isEmpty() && tempo::trackGrid(varying, std::nan("")).points.isEmpty(),
          "an adaptive grid requires an independently accepted finite source tempo");
    auto invalid = varying; invalid.right.pop_back();
    check(tempo::trackGrid(invalid, 174).points.isEmpty(), "invalid channel frame counts are rejected safely");

    std::printf("beat_grid: %d ok, %d failures\n", checks - failures, failures);
    return failures ? 1 : 0;
}
