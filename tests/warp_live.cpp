// Audio timing regressions for live Session playback. Generated inputs make the
// source tempo and transient positions exact; no audio device or user library is used.
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Sample.h"
#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace sessions;
using sessions::audio::Command;
namespace {
constexpr int rate = 48000;
constexpr double pi = 3.14159265358979323846;
int checks = 0, failures = 0;
#define CHECK(condition, text) do { ++checks; if (condition) std::printf("ok: %s\n", text); else { ++failures; std::printf("FAIL: %s\n", text); } } while (0)

SamplePtr pulses(double bpm, int beats, int sourceRate = rate) {
    auto s = std::make_shared<SampleData>();
    s->rate = sourceRate;
    s->left.resize(size_t(std::lround(beats * 60.0 / bpm * sourceRate)), 0.0f);
    s->transients.reserve(size_t(beats));
    for (int beat = 0; beat < beats; ++beat) {
        const double seconds = beat * 60.0 / bpm;
        s->transients.push_back(seconds);
        const size_t at = size_t(std::lround(seconds * sourceRate));
        // A 2 ms pulse with a sharp leading edge: onset timing and preserved
        // duration can be measured independently of the stretching algorithm.
        for (int j = 0; j < sourceRate / 500 && at + size_t(j) < s->left.size(); ++j)
            s->left[at + size_t(j)] = 0.4f;
    }
    s->right = s->left;
    return s;
}
SamplePtr tone(double seconds = 4) {
    auto s = std::make_shared<SampleData>();
    s->rate = rate;
    s->left.resize(size_t(std::lround(seconds * rate)));
    for (size_t i = 0; i < s->left.size(); ++i)
        s->left[i] = float(0.15 * std::sin(2 * pi * 173.0 * i / rate)
                         + 0.07 * std::sin(2 * pi * 347.0 * i / rate));
    s->right = s->left;
    s->transients = {0};
    return s;
}
ClipPtr addAudio(Project& p, const SamplePtr& sample, double bpm, WarpMode mode, double gain = 0) {
    if (p.scenes.isEmpty()) p.scenes = {"1"};
    p.addTrack("Audio", QColor("#6098b8"));
    auto& track = p.tracks.last();
    track.type = TrackType::Audio;
    track.monitor = Monitor::Off;
    auto c = p.newClip("Timing source", sample->seconds() * bpm / 60.0);
    c->samplePath = "/generated/live-warp.wav";
    c->sample = sample;
    c->detectedBpm = bpm;
    c->warpMarkers = {{0, 0}, {c->length, sample->seconds()}};
    c->warpMode = mode;
    c->gain = gain;
    c->warp = true;
    c->loop = true;
    track.clipSlots[0] = c;
    return c;
}
void launchAll(Project& p, audio::Engine& e) {
    e.send({Command::Stop, -1, -1, 0, 0, true});
    for (const auto& t : p.tracks) e.send({Command::Launch, t.id, 0, 0.0});
    e.send({Command::Play});
}
std::vector<float> render(audio::Engine& e, int frames, int block = 511) {
    std::vector<float> out(size_t(frames) * 2);
    for (int done = 0; done < frames;) {
        const int n = std::min(block, frames - done);
        e.process(out.data() + size_t(done) * 2, n);
        done += n;
    }
    return out;
}
int firstAbove(const std::vector<float>& x, int first, int last, float threshold) {
    for (int i = std::max(0, first); i < std::min(last, int(x.size() / 2)); ++i)
        if (std::abs(x[size_t(i) * 2]) > threshold) return i;
    return -1;
}
double rms(const std::vector<float>& x, int first, int last) {
    double sum = 0;
    for (int i = first; i < last; ++i) sum += double(x[size_t(i) * 2]) * x[size_t(i) * 2];
    return std::sqrt(sum / std::max(1, last - first));
}
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QCoreApplication app(argc, argv);

    // Source BPM is independent of the project. A 44.1 kHz, 176 BPM drum loop
    // must land on every 120 BPM beat through both pitch-preserving slices and
    // direct rate playback, including its second loop pass.
    for (WarpMode mode : {WarpMode::Beats, WarpMode::Repitch}) {
        Project p;
        addAudio(p, pulses(176, 4, 44100), 176, mode);
        audio::Engine e(&p);
        launchAll(p, e);
        const auto out = render(e, 120000);
        bool timing = true;
        for (int beat = 1; beat < 5; ++beat) {
            const int at = beat * 24000;
            int onset = -1;
            // Ignore cubic interpolation's tiny negative pre-ring before a
            // positive pulse; measure the leading positive edge itself.
            for (int i = at - 4; i < at + 8; ++i)
                if (out[size_t(i) * 2] > 0.001f) { onset = i; break; }
            // The four-tap resampler can interpolate a positive edge one
            // source sample early (two output samples at this slowdown).
            const int interpolationTolerance = mode == WarpMode::Repitch ? 2 : 1;
            timing &= onset >= at - interpolationTolerance && onset <= at + 1;
        }
        CHECK(timing, mode == WarpMode::Beats
              ? "176 BPM Beats clips at 44.1 kHz land on every 120 BPM project beat and loop downbeat"
              : "176 BPM Re-Pitch clips at 44.1 kHz land on every 120 BPM project beat and loop downbeat");
        CHECK(rms(out, 23900, 24100) > 0.02, "Warped drums retain an audible transient at the target beat");
    }

    // An adaptive source map must reach the callback intact, including points
    // beyond index 32. Its 40 unequal beat intervals have changing source slopes.
    // Place pulses at their midpoints: those expected project positions are
    // independent of the engine's interpolation and also exercise segment interiors.
    {
        constexpr int sourceRate = 44100;
        constexpr double projectTempo = 240, loopBeats = 24;
        QVector<WarpMarker> markers{{0, 0}};
        for (int interval = 0; interval < 40; ++interval) {
            const double beat = markers.last().beat + (interval % 2 ? 0.8 : 0.4);
            markers.push_back({beat, 0.25 * beat + 0.005 * beat * beat});
        }
        auto source = std::make_shared<SampleData>();
        source->rate = sourceRate;
        source->left.resize(size_t(std::lround(markers.last().seconds * sourceRate)), 0);
        std::vector<double> pulseBeats{0}, pulseSeconds{0};
        for (int i = 1; i < markers.size(); ++i) {
            pulseBeats.push_back((markers[i - 1].beat + markers[i].beat) / 2);
            pulseSeconds.push_back((markers[i - 1].seconds + markers[i].seconds) / 2);
        }
        for (double seconds : pulseSeconds) {
            const size_t begin = size_t(std::lround(seconds * sourceRate));
            source->transients.push_back(double(begin) / sourceRate);
            for (int i = 0; i < sourceRate / 500 && begin + size_t(i) < source->left.size(); ++i)
                source->left[begin + size_t(i)] = 0.4f;
        }
        source->right = source->left;
        for (float& value : source->right) value = -value;
        for (WarpMode mode : {WarpMode::Beats, WarpMode::Repitch}) {
            auto run = [&](int block) {
                Project p;
                p.tempo = projectTempo;
                auto c = addAudio(p, source, 120, mode);
                c->warpMarkers = markers;
                c->length = loopBeats;
                c->grain = 0;
                audio::Engine e(&p);
                launchAll(p, e);
                return render(e, 12 * rate, block);
            };
            const auto out = run(511), otherBlock = run(997);
            for (int pass = 0; pass < 2; ++pass) {
                bool regionTiming[3] = {true, true, true};
                for (double beat : pulseBeats) {
                    const int target = int(std::lround((pass * loopBeats + beat) * 60 / projectTempo * rate));
                    const int onset = firstAbove(out, target - 12, target + 20, 0.025f);
                    const int tolerance = beat == 0 ? 5 : 3; // short launch/slice declick and fractional source-frame rounding
                    regionTiming[std::min(2, int(beat / 8))] &= onset >= target - 2 && onset <= target + tolerance;
                }
                CHECK(regionTiming[0], "41-marker Session warping lands early pulses and loop downbeats on both passes");
                CHECK(regionTiming[1], "41-marker Session warping lands interpolated middle pulses on both passes");
                CHECK(regionTiming[2], "41-marker Session warping retains late map segments beyond marker 32 on both passes");
            }
            bool stereo = true;
            double blockError = 0;
            for (size_t i = 0; i < out.size(); i += 2) {
                stereo &= std::isfinite(out[i]) && std::isfinite(out[i + 1]) && std::abs(out[i] + out[i + 1]) < 1e-7;
                blockError = std::max({blockError, std::abs(double(out[i] - otherBlock[i])), std::abs(double(out[i + 1] - otherBlock[i + 1]))});
            }
            CHECK(stereo, "Variable Session warping preserves anti-phase 44.1 kHz stereo through the complete map");
            CHECK(blockError < 1e-6, "Variable Session warp timing is independent of callback boundaries");
        }
    }

    // The last count-in fragment must not advance the first musical window.
    {
        Project p;
        addAudio(p, pulses(176, 4), 176, WarpMode::Repitch);
        audio::Engine e(&p);
        e.send({Command::Launch, p.tracks[0].id, 0, 4.0});
        e.send({Command::Play, -1, -1, 1.0});
        render(e, 24000, 997);
        CHECK(std::abs(e.beat()) < 1e-9 && e.playing(), "Count-in ends exactly at the launch beat without advancing through its last callback fragment");
        const auto first = render(e, 128, 128);
        CHECK(firstAbove(first, 0, 16, 0.01f) >= 0, "A counted-in clip keeps its first drum attack");
    }

    // Both command delivery and audio segmentation must obey the same musical
    // clock even when a bar boundary falls inside an arbitrary callback size.
    {
        Project p;
        p.tempo = 137.3;
        const auto source = tone(8);
        addAudio(p, source, 176, WarpMode::Repitch);
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        const int prefix = 12345;
        render(e, prefix, 997);
        e.send({Command::Launch, p.tracks[0].id, 0, 4.0});
        const int boundary = int(std::ceil(4 * 60.0 / p.tempo * rate - 1e-8));
        const auto out = render(e, boundary - prefix + 4800, 997);
        const int expected = boundary - prefix;
        const int first = firstAbove(out, 0, expected + 32, 1e-6f);
        CHECK(first >= expected && first <= expected + 1, "Quantized Session audio launches within one sample of an interior bar boundary");
        e.send({Command::StopTrack, p.tracks[0].id, -1, 4.0});
        const int stopAt = int(std::ceil(8 * 60.0 / p.tempo * rate - 1e-8));
        const int already = boundary + 4800;
        const auto tail = render(e, stopAt - already + 128, 997);
        bool silent = true;
        for (int i = stopAt - already; i < int(tail.size() / 2); ++i)
            silent &= tail[size_t(i) * 2] == 0.0f && tail[size_t(i) * 2 + 1] == 0.0f;
        CHECK(silent && e.trackStatus(0).playingSlot == -1, "Quantized Session stop emits no audio after its target sample");
    }

    // An Arrangement loop must not reset an independently running Session
    // voice or lose fractional sample time on every pass.
    for (WarpMode mode : {WarpMode::Repitch, WarpMode::Tones, WarpMode::Complex}) {
        auto run = [&](bool globalLoop, int block, double& finalBeat) {
            Project p;
            p.tempo = 137.3;
            addAudio(p, tone(8), 176, mode);
            audio::Engine e(&p);
            launchAll(p, e);
            if (globalLoop) e.send({Command::Loop, -1, -1, 0, 0.37, true});
            auto out = render(e, rate * 3, block);
            finalBeat = e.beat();
            return out;
        };
        double baselineBeat = 0, loopBeat = 0;
        const auto baseline = run(false, 511, baselineBeat);
        const auto looped = run(true, 997, loopBeat);
        double error = 0, signal = 0;
        for (int i = 4800; i < rate * 3; ++i) {
            const double a = baseline[size_t(i) * 2], b = looped[size_t(i) * 2];
            error += (a - b) * (a - b);
            signal += a * a;
        }
        const double relativeError = std::sqrt(error / std::max(signal, 1e-20));
        std::printf("      global-loop mode %s: relative audio error %.8f, phase %.10f expected %.10f\n",
                    warpModeName(mode), relativeError, loopBeat, std::fmod(baselineBeat, 0.37));
        CHECK(std::abs(loopBeat - std::fmod(baselineBeat, 0.37)) < 137.3 / 60.0 / rate,
              "Repeated fractional-sample Arrangement loops retain the real-time beat phase");
        CHECK(relativeError < 0.002, "An unrelated Arrangement loop preserves the running Session waveform and stretcher state");
    }

    // Exact-sample loop lengths need the same rounding tolerance as segment
    // bounds: an extra frame must not become a hole in a running Session clip.
    {
        auto run = [&](bool loop) {
            Project p;
            addAudio(p, tone(4), 176, WarpMode::Repitch);
            audio::Engine e(&p);
            launchAll(p, e);
            if (loop) e.send({Command::Loop, -1, -1, 0, 0.25, true});
            return render(e, rate * 2, loop ? 997 : 511);
        };
        const auto plain = run(false), looped = run(true);
        double error = 0;
        for (size_t i = 0; i < plain.size(); ++i) error = std::max(error, std::abs(double(plain[i] - looped[i])));
        CHECK(error < 1e-6, "Exact-sample global loop boundaries introduce no holes into running Session audio");
    }

    // A project tempo change changes the duration of future beats, preserving
    // the source mapping and the clip position already reached.
    {
        Project p;
        auto clip = addAudio(p, pulses(176, 16), 176, WarpMode::Beats);
        audio::Engine e(&p);
        launchAll(p, e);
        render(e, 36000); // beat 1.5 at 120 BPM
        const double originalLength = clip->length, originalSourceAt4 = clip->secondsAt(4);
        p.setTempo(90);
        e.publish();
        const auto out = render(e, 90000, 997);
        bool timing = true;
        for (int beat = 2; beat <= 4; ++beat) {
            const int at = int(std::lround((beat - 1.5) * 60.0 / 90.0 * rate));
            const int onset = firstAbove(out, at - 4, at + 8, 0.01f);
            timing &= onset >= at - 1 && onset <= at + 1;
        }
        CHECK(clip->length == originalLength && clip->secondsAt(4) == originalSourceAt4,
              "Live project tempo changes preserve source BPM, beat markers and musical loop length");
        CHECK(timing, "Running Session drums follow the new tempo from their current clip beat");
    }

    // Warp Off follows physical sample time. Changing the project tempo or
    // looping its Arrangement must not seek the file, retime its end, or restart it.
    {
        auto run = [&](bool changeTempo, bool globalLoop, int block, int& playingSlot, bool envelope = false) {
            Project p;
            auto c = addAudio(p, tone(4), 176, WarpMode::Repitch);
            c->warp = false;
            c->loop = false;
            if (envelope) c->envelopes = {{target::volume(), {{0, 1}, {c->length, 0.2}}}};
            audio::Engine e(&p);
            launchAll(p, e);
            if (globalLoop) e.send({Command::Loop, -1, -1, 0, 0.37, true});
            render(e, 36000, block); // 0.75 seconds, regardless of the source BPM
            if (changeTempo) { p.setTempo(90); e.publish(); }
            auto out = render(e, 180000, block);
            playingSlot = e.trackStatus(0).playingSlot;
            return out;
        };
        int baselineSlot = 0, changedSlot = 0, loopedSlot = 0;
        const auto baseline = run(false, false, 511, baselineSlot);
        const auto changed = run(true, false, 997, changedSlot);
        const auto looped = run(true, true, 997, loopedSlot);
        double tempoError = 0, loopError = 0;
        for (size_t i = 0; i < baseline.size(); ++i) {
            tempoError = std::max(tempoError, std::abs(double(baseline[i] - changed[i])));
            loopError = std::max(loopError, std::abs(double(baseline[i] - looped[i])));
        }
        CHECK(tempoError < 1e-5, "Warp Off preserves the exact running sample position through a live project-tempo change");
        CHECK(loopError < 1e-5, "Warp Off playback is independent of unrelated Arrangement loops and callback sizes");
        CHECK(baselineSlot == -1 && changedSlot == -1 && loopedSlot == -1
              && rms(changed, 151000, 154000) > 0.02 && rms(changed, 157000, 180000) == 0,
              "Unwarped Session one-shots finish after their physical file duration at every project tempo");
        const auto envBase = run(false, false, 511, baselineSlot, true), envChanged = run(true, false, 511, changedSlot, true);
        double envelopeError = 0;
        for (size_t i = 0; i < envBase.size(); ++i) envelopeError = std::max(envelopeError, std::abs(double(envBase[i] - envChanged[i])));
        CHECK(envelopeError < 1e-5, "Native-speed clip envelopes follow source position through project tempo changes");
    }

    {
        Project p;
        auto c = addAudio(p, tone(1), 176, WarpMode::Repitch);
        c->warp = false; c->loop = false;
        c->followEnabled = true; c->followA = FollowAction::Again; c->followChanceA = 1;
        audio::Engine e(&p);
        launchAll(p, e);
        render(e, 60000);
        const auto status = e.trackStatus(0);
        CHECK(status.playingSlot == 0 && std::abs(status.clipBeat - 0.25 * 176.0 / 60) < 1e-7,
              "Default follow actions retrigger native-speed clips at their physical end");
    }

    // Native-speed feedback drives the same source position in the grid and
    // waveform editor. Warp toggles transfer that position between the clocks.
    {
        Project p;
        auto c = addAudio(p, tone(8), 176, WarpMode::Repitch);
        audio::Engine e(&p);
        launchAll(p, e);
        render(e, 36000);
        const double warpedBeat = e.trackStatus(0).clipBeat;
        c->warp = false;
        emit p.clipChanged();
        e.publish();
        render(e, 1);
        const auto off = e.trackStatus(0);
        CHECK(std::abs(off.clipBeat - (warpedBeat + 176.0 / 60 / rate)) < 1e-7,
              "Switching Warp off preserves the current source position before continuing at native speed");
        render(e, 12000);
        const auto native = e.trackStatus(0);
        auto& track = p.tracks[0];
        track.playingSlot = native.playingSlot;
        track.playingClipBeat = native.clipBeat;
        track.playingClipPass = native.clipPass;
        double overviewBeat = -1;
        const auto overviewClip = p.playingClip(0, e.beat(), &overviewBeat);
        CHECK(overviewClip == c && std::abs(p.clipPosition(c, e.beat()) - native.clipBeat) < 1e-10
              && std::abs(overviewBeat - native.clipBeat) < 1e-10,
              "Waveform, envelope and overview playheads use the engine's native-speed source position");
        c->warp = true;
        emit p.clipChanged();
        e.publish();
        render(e, 1);
        const auto on = e.trackStatus(0);
        CHECK(std::abs(on.clipBeat - (native.clipBeat + 2.0 / rate)) < 1e-7,
              "Switching Warp on preserves the current source position before following project beats");
    }
    {
        Project p;
        auto c = addAudio(p, tone(1), 176, WarpMode::Repitch);
        c->warp = false;
        audio::Engine e(&p);
        launchAll(p, e);
        render(e, 60000);
        const auto status = e.trackStatus(0);
        CHECK(status.clipPass == 2 && std::abs(status.clipBeat - 0.25 * 176.0 / 60) < 1e-7,
              "Native-speed loop feedback reports the actual source position and completed pass count");
    }

    // Slowing a transient slice must preserve its attack while giving the
    // player an explicit choice between a gate and a sustained tail.
    {
        auto source = std::make_shared<SampleData>();
        source->rate = rate;
        source->left.resize(rate * 2, 0.12f);
        for (int beat = 0; beat < 4; ++beat) {
            source->transients.push_back(beat * 0.5);
            std::fill(source->left.begin() + beat * rate / 2, source->left.begin() + beat * rate / 2 + 96, 0.4f);
        }
        source->right = source->left;
        for (float& x : source->right) x = -x;
        auto run = [&](int tail, double tempo, int block) {
            Project p;
            p.tempo = tempo;
            auto c = addAudio(p, source, 120, WarpMode::Beats);
            c->beatsTail = tail;
            audio::Engine e(&p);
            launchAll(p, e);
            return render(e, rate * 3, block);
        };
        const auto gate = run(0, 60, 511);
        CHECK(rms(gate, 34000, 43000) < 1e-8, "Beats Gate retains intentional silence after a slowed slice ends");
        const auto neutral = run(0, 120, 511);
        for (int tail : {1, 2}) {
            const auto out = run(tail, 60, 511);
            CHECK(rms(out, 34000, 43000) > 0.08, "Beats Sustain and Bounce fill slower-tempo tails without replaying the attack");
            bool clean = true, stereo = true;
            for (int i = 10000; i < rate * 3; ++i) {
                clean &= std::isfinite(out[size_t(i) * 2]) && std::abs(out[size_t(i) * 2]) < 0.45;
                stereo &= std::abs(out[size_t(i) * 2] + out[size_t(i) * 2 + 1]) < 1e-7;
            }
            CHECK(clean && stereo, "Extended Beats tails remain bounded and preserve the original stereo polarity");
            const auto unchanged = run(tail, 120, 511), otherBlock = run(tail, 60, 997);
            double unityError = 0, blockError = 0;
            for (size_t i = 0; i < neutral.size(); ++i) {
                unityError = std::max(unityError, std::abs(double(neutral[i] - unchanged[i])));
                blockError = std::max(blockError, std::abs(double(out[i] - otherBlock[i])));
            }
            CHECK(unityError < 1e-7, "Tail controls leave original-tempo drums unchanged");
            CHECK(blockError < 1e-6, "Sustained Beats tails render independently of callback block size");
            const int attack = firstAbove(out, rate - 4, rate + 16, 0.2f);
            CHECK(attack >= rate && attack <= rate + 10, "Sustaining a drum tail keeps the next attack at its target beat");
        }
    }

    // Every supported audio track needs persistent pitch-preserving state.
    // Compare a quiet 32-track mix with 32 copies of an isolated reference;
    // levels stay below the output limiter so the comparison is linear.
    {
        constexpr int count = 32, frames = 24000;
        const auto sample = tone(4);
        auto mix = [&](int tracks) {
            Project p;
            p.tempo = 120;
            for (int i = 0; i < tracks; ++i) addAudio(p, sample, 176, WarpMode::Complex, -32);
            audio::Engine e(&p);
            launchAll(p, e);
            return render(e, frames, 511);
        };
        const auto single = mix(1), many = mix(count);
        double error = 0, signal = 0;
        bool finite = true;
        for (int i = 4800; i < frames; ++i) {
            const double expected = single[size_t(i) * 2] * count;
            const double actual = many[size_t(i) * 2];
            error += (expected - actual) * (expected - actual);
            signal += expected * expected;
            finite &= std::isfinite(actual);
        }
        const double relativeError = std::sqrt(error / std::max(signal, 1e-20));
        std::printf("      32-track persistent stretch relative error %.8f\n", relativeError);
        CHECK(finite && rms(many, 4800, frames) > 0.01, "32 simultaneous pitch-preserving Session clips render finite, audible audio");
        CHECK(relativeError < 0.001, "32 warped tracks retain independent stretcher state without hidden mode fallback or voice stealing");

        auto editDuringPlayback = [&](bool insert) {
            Project p;
            for (int i = 0; i < count; ++i) addAudio(p, sample, 176, WarpMode::Complex, -32);
            audio::Engine e(&p);
            launchAll(p, e);
            render(e, 12000);
            if (insert) {
                // This inaudible source still needs its own stretcher. Placing
                // it first must use spare capacity rather than evicting every
                // old voice before its track is visited in the new order.
                addAudio(p, sample, 176, WarpMode::Complex, -300);
                p.tracks.move(p.tracks.size() - 1, 0);
                e.publish();
                e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
            }
            return render(e, 12000);
        };
        const auto unchanged = editDuringPlayback(false), inserted = editDuringPlayback(true);
        double editError = 0, editSignal = 0;
        for (size_t i = 0; i < unchanged.size(); ++i) {
            const double difference = unchanged[i] - inserted[i];
            editError += difference * difference;
            editSignal += double(unchanged[i]) * unchanged[i];
        }
        const double relativeEditError = std::sqrt(editError / std::max(editSignal, 1e-20));
        std::printf("      insert-before-active-tracks relative error %.8f\n", relativeEditError);
        CHECK(relativeEditError < 0.001, "Inserting a new warped track preserves active voices and uses spare stretcher capacity");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
