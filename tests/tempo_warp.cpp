#include "core/Tempo.h"
#include "core/Model.h"
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

// Deterministic kick/snare/hat loops, including syncopated breakbeats and alternating bars.
SampleData drums(double bpm, int bars, double rate, bool broken = false) {
    SampleData s;
    s.rate = rate; s.channels = 2;
    const double beat = 60.0 / bpm;
    s.left.resize(size_t(std::lround(bars * 4 * beat * rate)), 0);
    std::mt19937 random(2417);
    std::uniform_real_distribution<float> noise(-1, 1);
    for (int step = 0; step < bars * 16; ++step) {
        const int at = int(std::lround(step * beat * 0.25 * rate));
        const int inBar = step % 16;
        for (int i = 0; i < int(0.09 * rate) && at + i < int(s.left.size()); ++i) {
            const double t = i / rate;
            float value = 0.07f * noise(random) * float(std::exp(-t * 150));
            const bool kick = broken ? inBar == 0 || inBar == 10 || (step / 16 % 2 && inBar == 14) : step % 4 == 0;
            if (kick) value += 0.7f * float(std::sin(2 * M_PI * (60 * t + 14 * t * std::exp(-t * 60))) * std::exp(-t * 28));
            if (inBar == 4 || inBar == 12) value += 0.5f * noise(random) * float(std::exp(-t * 45));
            s.left[size_t(at + i)] += value;
        }
    }
    s.right = s.left;
    return s;
}

bool family(double actual, double expected, double tolerance = 0.003) {
    if (actual <= 0) return false;
    const double octave = std::log2(actual / expected);
    return std::abs(octave - std::round(octave)) < tolerance;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    for (double rate : {32000.0, 44100.0, 48000.0, 96000.0}) {
        auto s = drums(176, 8, rate, true);
        const auto estimated = tempo::detect(s);
        std::printf("breakbeat %.0f Hz: %.6f BPM, confidence %.3f\n", rate, estimated.bpm, estimated.confidence);
        check(std::abs(estimated.bpm - 176) < 0.12 && estimated.confidence > 0.15, "fast untagged breakbeat retains its metrical tempo accurately at every sample rate");
        auto inverted = s;
        for (float& v : inverted.right) v = -v;
        const auto wide = tempo::detect(inverted);
        check(std::abs(wide.bpm - estimated.bpm) < 1e-6 && std::abs(wide.confidence - estimated.confidence) < 1e-6,
              "stereo phase cancellation cannot erase source-tempo evidence");
    }
    for (int bars : {2, 4, 8}) {
        const auto e = tempo::detect(drums(176, bars, 48000, true));
        std::printf("breakbeat %d bars: %.6f BPM, confidence %.3f\n", bars, e.bpm, e.confidence);
        check(std::abs(e.bpm - 176) < 0.15, "closed short breakbeat loops reject the three-eighth-note tempo alias");
    }
    for (double bpm : {60.0, 87.0, 96.0, 120.0, 128.0, 172.0, 176.0, 210.0}) {
        auto s = drums(bpm, 4, 48000);
        const auto estimated = tempo::detect(s);
        std::printf("quarter drums %.0f: %.6f BPM, confidence %.3f\n", bpm, estimated.bpm, estimated.confidence);
        check(family(estimated.bpm, bpm) && estimated.confidence > 0.15, "drum tempi remain accurate without imposing the project BPM");
    }
    auto loop = drums(176, 4, 48000, true);
    for (const QString& path : {QString("/pack/Break_176bpm_Dm.wav"), QString("/pack/BPM-176 Break.wav"), QString("/pack/Break_176_Dm.wav")}) {
        const auto e = tempo::detect(loop, 90, path);
        check(e.bpm == 176 && e.confidence == 1, "a valid source-tempo filename supplies exact BPM");
    }
    auto padded = loop;
    padded.left.resize(padded.left.size() + 13777, 0); padded.right = padded.left;
    const auto noId = tempo::detect(padded, 90, "/pack/176/Break_123_take.wav");
    std::printf("padded break: %.6f BPM, confidence %.3f\n", noId.bpm, noId.confidence);
    check(std::abs(noId.bpm - 123) > 1 && noId.confidence < 1, "folder numbers and unmatched sample IDs are not tempo metadata");
    const auto decimal = tempo::detect(loop, 90, "/pack/Break_175.75bpm.wav");
    check(decimal.bpm == 175.75, "fractional source BPM tags retain precision");
    auto tagged = loop;
    tagged.path = "/pack/Break_176bpm.wav";
    check(tempo::detect(tagged).bpm == 176, "decoded sample paths supply metadata to Sampler and clip analysis");

    const auto slow = tempo::syncTo(loop, 60, 176), normal = tempo::syncTo(loop, 120, 176), fast = tempo::syncTo(loop, 220, 176);
    check(slow.beats == 16 && normal.beats == 16 && fast.beats == 16 && slow.loop && normal.loop && fast.loop,
          "176 BPM four-bar clips preserve sixteen source beats across all project tempi");
    check(slow.detected == 176 && normal.detected == 176 && fast.detected == 176, "source BPM never follows the project BPM");
    const auto pathImport = tempo::syncTo(loop, 120, 0, "/pack/Break_176bpm.wav");
    check(pathImport.detected == 176 && pathImport.beats == 16 && pathImport.loop, "import's supplied source path participates in exact tempo mapping");
    for (double cut : {0.1, 0.005}) {
        auto truncated = loop;
        truncated.left.resize(truncated.left.size() - size_t(cut * truncated.rate)); truncated.right = truncated.left;
        const auto mapped = tempo::syncTo(truncated, 120, 176);
        const auto named = tempo::syncTo(truncated, 120, 0, "/pack/Break_176bpm.wav");
        check(!mapped.loop && !named.loop && mapped.detected == 176 && named.detected == 176
                  && std::abs(mapped.beats * 60 / truncated.seconds() - 176) < 1e-9 && std::abs(mapped.beats - named.beats) < 1e-9,
              "known source176 retains its exact grid when a tail is truncated instead of being stretched to a bar");
    }
    auto tiny = std::make_shared<SampleData>();
    tiny->rate = 48000;
    tiny->left.resize(960, 0.25f); tiny->right = tiny->left; // a 20 ms one-shot
    const double tinyBeats = tiny->seconds() * 176 / 60;
    const auto tinyKnown = tempo::syncTo(*tiny, 120, 176);
    const auto tinyTagged = tempo::syncTo(*tiny, 120, 0, "/pack/Hit_176bpm.wav");
    check(!tinyKnown.loop && tinyKnown.detected == 176 && std::abs(tinyKnown.beats - tinyBeats) < 1e-12,
          "a 20 ms one-shot with explicit source176 retains its actual span below a quarter beat");
    check(!tinyTagged.loop && tinyTagged.detected == 176 && std::abs(tinyTagged.beats - tinyBeats) < 1e-12,
          "a 20 ms one-shot tagged176 BPM is not stretched by a minimum clip-length assumption");
    Project tinyProject;
    tinyProject.tempo = 120;
    const auto tinyClip = tinyProject.newAudioClip("/pack/Hit_176bpm.wav", tiny);
    check(tinyClip->warp && !tinyClip->loop && std::abs(tinyClip->sampleBpm() - 176) < 1e-9
              && std::abs(tinyClip->length - tinyBeats) < 1e-12 && tinyProject.tempo == 120,
          "imported short tagged one-shots preserve source176 in their actual warp markers at project120");

    SampleData unknown;
    unknown.rate = 48000;
    unknown.left.resize(91200, 0); unknown.right = unknown.left;
    for (size_t i = 0; i < 14000; ++i) unknown.left[i] = unknown.right[i] = 0.5f * float(std::sin(2 * M_PI * 80 * i / 48000.0) * std::exp(-double(i) / 4000));
    check(tempo::detect(unknown).bpm == 0, "a hit followed by silence supplies no source tempo");
    const auto unverified = tempo::syncTo(unknown, 120);
    check(unverified.detected == 0 && !unverified.loop && std::abs(unverified.beats - 3.8) < 1e-9,
          "unknown 1.9-second audio is not silently stretched to a project bar");
    SampleData noise;
    noise.rate = 48000; noise.left.resize(96000);
    std::mt19937 rng(7); std::uniform_real_distribution<float> random(-0.3, 0.3);
    for (float& v : noise.left) v = random(rng);
    noise.right = noise.left;
    check(tempo::detect(noise).bpm == 0, "stationary noise is not assigned a made-up tempo");
    SampleData tone;
    tone.rate = 48000; tone.left.resize(192000);
    for (size_t i = 0; i < tone.left.size(); ++i) tone.left[i] = float(0.5 * std::sin(2 * M_PI * 220 * i / tone.rate));
    tone.right = tone.left;
    const auto toneTempo = tempo::detect(tone);
    std::printf("tone: %.6f BPM, confidence %.3f\n", toneTempo.bpm, toneTempo.confidence);
    check(toneTempo.bpm == 0, "a sustained tone is not rhythmic tempo evidence");

    // A long song's tail does not move the inferred source grid to a nearby bar.
    auto stem = drums(176, 20, 48000, true);
    stem.left.resize(stem.left.size() + 13333, 0); stem.right = stem.left;
    const auto song = tempo::syncTo(stem, 120, 176);
    check(!song.loop && std::abs(song.beats - stem.seconds() * 176 / 60) < 1e-9,
          "long stem tails never rewrite source BPM by rounding the entire duration to bars");
    check(tempo::detect(loop, 0).bpm == 0 && tempo::detect(loop, -1).bpm == 0, "invalid analysis durations fail safely");
    std::printf("tempo_warp: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
