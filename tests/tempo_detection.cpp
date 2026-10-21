#include "core/Tempo.h"
#include <QCoreApplication>
#include <cmath>
#include <cstdio>
#include <random>

using namespace sessions;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}

// A long, syncopated performance with a deliberately arbitrary tail. Its source grid
// cannot be recovered by rounding the file's duration to bars. The old integer-period
// comb reads 134 as 89.33 and 174 as 116: three-eighth notes win over the actual beat.
SampleData breakbeat(double bpm, int bars = 20, double rate = 12000) {
    SampleData s;
    s.rate = rate;
    s.channels = 2;
    const double beat = 60 / bpm;
    s.left.resize(size_t(std::lround(bars * 4 * beat * rate)) + size_t(rate * 0.3241), 0);
    std::mt19937 rng(2417);
    std::uniform_real_distribution<float> random(-1, 1);
    for (int step = 0; step < bars * 16; ++step) {
        const int at = int(std::lround(step * beat * 0.25 * rate));
        const int phase = step % 16;
        const bool kick = phase == 0 || phase == 10 || (step / 16 % 2 && phase == 14);
        for (int i = 0; i < int(0.09 * rate); ++i) {
            const double t = i / rate;
            float x = 0.07f * random(rng) * float(std::exp(-150 * t));
            if (kick) x += 0.7f * float(std::sin(2 * M_PI * (60 * t + 14 * t * std::exp(-60 * t))) * std::exp(-28 * t));
            if (phase == 4 || phase == 12) x += 0.5f * random(rng) * float(std::exp(-45 * t));
            s.left[size_t(at + i)] += x;
        }
    }
    s.right = s.left;
    return s;
}

SampleData texture(double modulationHz) {
    SampleData s;
    s.rate = 12000;
    s.left.resize(50 * 12000);
    for (size_t i = 0; i < s.left.size(); ++i) {
        const double t = i / s.rate;
        const double phase = modulationHz * t - modulationHz * 0.3 * std::cos(2 * M_PI * 0.071 * t) / (2 * M_PI * 0.071);
        const double envelope = 0.45 + 0.35 * std::sin(2 * M_PI * phase);
        s.left[i] = float(envelope * (0.5 * std::sin(2 * M_PI * 110 * t) + 0.2 * std::sin(2 * M_PI * 331 * t)));
    }
    s.right = s.left;
    return s;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    for (double bpm : {134.0, 174.0, 178.0, 212.0, 239.0}) {
        auto s = breakbeat(bpm);
        const auto estimate = tempo::detect(s);
        std::printf("long tailed break %.0f: %.6f BPM, confidence %.3f\n", bpm, estimate.bpm, estimate.confidence);
        check(std::abs(estimate.bpm - bpm) < 0.12, "a long tailed performance rejects non-octave rhythmic aliases");
        check(estimate.confidence > 0.15 && !estimate.fromMetadata, "a rhythmic estimate records acoustic confidence and provenance");
    }
    for (double rate : {22050.0, 44100.0}) {
        const auto estimate = tempo::detect(breakbeat(174, 12, rate));
        check(std::abs(estimate.bpm - 174) < 0.12, "fractional metrical candidate scoring remains accurate at other sample rates");
    }
    auto s = breakbeat(174);
    const auto reference = tempo::detect(s);
    for (float gain : {0.01f, 2.0f}) {
        auto scaled = s;
        for (float& v : scaled.left) v *= gain;
        for (float& v : scaled.right) v *= gain;
        const auto estimate = tempo::detect(scaled);
        check(std::abs(estimate.bpm - reference.bpm) < 0.001, "normalization keeps source tempo invariant to recording level");
    }
    for (double modulation : {2.3, 3.85, 4.0}) {
        const auto estimate = tempo::detect(texture(modulation));
        std::printf("drifting texture %.2f Hz: %.6f BPM, confidence %.3f\n", modulation, estimate.bpm, estimate.confidence);
        check(estimate.bpm == 0, "drifting timbre modulation is not a confident stable source beat");
    }
    const auto tagged = tempo::detect(s, 90, "/pack/Source_177.25bpm.wav");
    check(tagged.bpm == 177.25 && tagged.confidence == 1 && tagged.fromMetadata,
          "explicit source BPM remains authoritative even when its audio suggests another value");
    const auto mapped = tempo::syncTo(s, 120, 0, "/pack/Source_177.25bpm.wav");
    check(mapped.detected == 177.25 && mapped.confidence == 1 && mapped.fromMetadata,
          "clip import preserves source-tempo metadata provenance");
    const auto acoustic = tempo::syncTo(s, 120);
    check(std::abs(acoustic.detected - reference.bpm) < 1e-9 && acoustic.confidence == reference.confidence && !acoustic.fromMetadata,
          "clip import carries the same acoustic confidence without a second independent estimate");
    const auto supplied = tempo::syncTo(s, 120, 178);
    check(supplied.detected == 178 && supplied.confidence == 1 && !supplied.fromMetadata,
          "supplied source tempo is retained without claiming filename metadata");
    for (double bpm : {40.0, 240.0}) {
        const auto estimate = tempo::detect(breakbeat(bpm, 12));
        check(estimate.bpm == 0 || (estimate.bpm >= 40 && estimate.bpm <= 240),
              "acoustic refinement cannot escape the advertised tempo search range");
    }
    // There is a brief beat at the start but no stable source grid across the file.
    auto brief = breakbeat(174, 2);
    brief.left.resize(60 * 12000, 0);
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    for (size_t i = size_t(10 * brief.rate); i < brief.left.size(); ++i) brief.left[i] = noise(rng);
    brief.right = brief.left;
    check(tempo::detect(brief).bpm == 0, "one short rhythmic passage does not invent a grid for a long unrelated recording");
    std::printf("tempo_detection: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
