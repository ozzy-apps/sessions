// Native audio playback: Warp may be disabled while pitch, sample position,
// source-relative envelopes and launch behavior remain editable and audible.
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Sample.h"
#include "core/Transport.h"
#include <QCoreApplication>
#include <QThread>
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
constexpr double sourceBpm = 176;
int checks = 0, failures = 0;
#define CHECK(condition, description) do { ++checks; if (condition) std::printf("ok: %s\n", description); else { ++failures; std::printf("FAIL: %s\n", description); } } while (0)

SamplePtr tone(double seconds = 4, double frequency = 220) {
    auto sample = std::make_shared<SampleData>();
    sample->rate = rate;
    sample->left.resize(size_t(std::lround(seconds * rate)));
    for (size_t i = 0; i < sample->left.size(); ++i)
        sample->left[i] = float(0.2 * std::sin(2 * pi * frequency * i / rate));
    sample->right = sample->left;
    for (float& value : sample->right) value = -value;
    sample->transients = {0};
    return sample;
}
ClipPtr addClip(Project& project, const SamplePtr& sample, WarpMode mode = WarpMode::ComplexPro) {
    if (project.scenes.isEmpty()) project.scenes = {"1", "2"};
    project.addTrack("Native audio", QColor("#5ab3ca"));
    auto& track = project.tracks.last();
    track.type = TrackType::Audio;
    track.monitor = Monitor::Off;
    auto clip = project.newClip("Native source", sample->seconds() * sourceBpm / 60.0);
    clip->samplePath = "/generated/native-warp-off.wav";
    clip->sample = sample;
    clip->detectedBpm = sourceBpm;
    clip->warpMarkers = {{0, 0}, {clip->length, sample->seconds()}};
    clip->warp = false;
    clip->warpMode = mode;
    clip->loop = false;
    track.clipSlots[0] = clip;
    return clip;
}
void launch(Project& project, audio::Engine& engine) {
    engine.send({Command::Stop, -1, -1, 0, 0, true});
    engine.send({Command::Launch, project.tracks[0].id, 0, 0.0});
    engine.send({Command::Play});
}
std::vector<float> render(audio::Engine& engine, int frames, int block = 511) {
    std::vector<float> output(size_t(frames) * 2);
    for (int done = 0; done < frames;) {
        const int n = std::min(block, frames - done);
        engine.process(output.data() + size_t(done) * 2, n);
        done += n;
    }
    return output;
}
double frequency(const std::vector<float>& output, int first, int last) {
    int crossings = 0;
    for (int i = first + 1; i < last; ++i)
        crossings += output[size_t(i - 1) * 2] < 0 && output[size_t(i) * 2] >= 0;
    return double(crossings) * rate / (last - first);
}
double rms(const std::vector<float>& output, int first, int last) {
    double sum = 0;
    for (int i = first; i < last; ++i) sum += double(output[size_t(i) * 2]) * output[size_t(i) * 2];
    return std::sqrt(sum / std::max(1, last - first));
}
double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double result = 0;
    for (size_t i = 0; i < a.size(); ++i) result = std::max(result, std::abs(double(a[i] - b[i])));
    return result;
}
void tick() {
    // Drive Transport's actual 16 ms mirror, with a deterministic manually
    // rendered audio clock and no audio device or event-loop playback thread.
    QThread::msleep(20);
    QCoreApplication::processEvents();
}
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QCoreApplication app(argc, argv);

    // Every algorithm is dormant while Warp is disabled, including Re-Pitch.
    // Transposition has the same audible rate/pitch behavior for all choices.
    std::vector<float> reference;
    for (WarpMode mode : {WarpMode::Beats, WarpMode::Tones, WarpMode::Texture,
                          WarpMode::Repitch, WarpMode::Complex, WarpMode::ComplexPro}) {
        Project project;
        project.tempo = 137.3;
        auto clip = addClip(project, tone(), mode);
        clip->transpose = 12;
        audio::Engine engine(&project);
        launch(project, engine);
        auto output = render(engine, rate * 5 / 2, 997);
        std::printf("      selected method: %s\n", warpModeName(mode));
        CHECK(std::abs(frequency(output, rate / 4, rate * 3 / 4) - 440) < 3,
              "Warp Off transposes a 220 Hz source up one octave with every saved method");
        CHECK(engine.trackStatus(0).playingSlot == -1 && rms(output, rate * 3 / 2, rate * 19 / 10) > 0.08
              && rms(output, rate * 21 / 10, rate * 5 / 2) == 0,
              "An octave-up native one-shot ends in two seconds, independently of project tempo");
        bool stereo = true;
        for (size_t i = 0; i < output.size(); i += 2)
            stereo &= std::isfinite(output[i]) && std::abs(output[i] + output[i + 1]) < 1e-7;
        CHECK(stereo, "Native pitch playback preserves stereo polarity and finite output");
        if (reference.empty()) reference = output;
        else CHECK(difference(reference, output) < 1e-7, "Selecting another dormant Warp algorithm leaves native audio unchanged");
        CHECK(!clip->warp && clip->warpMode == mode && clip->transpose == 12,
              "Native pitch playback retains Warp Off, the selected algorithm and stored transpose");
    }

    for (const auto& pitch : std::vector<std::pair<int, double>>{{-12, 0}, {0, 50}, {0, -50}, {7, 25}}) {
        Project project;
        auto clip = addClip(project, tone(8));
        clip->transpose = pitch.first;
        clip->detune = pitch.second;
        const double ratio = std::exp2((pitch.first + pitch.second / 100.0) / 12.0);
        audio::Engine engine(&project);
        launch(project, engine);
        const auto output = render(engine, rate);
        CHECK(std::abs(frequency(output, rate / 5, rate * 4 / 5) - 220 * ratio) < 3,
              "Native transpose and cent detune combine into the expected audible pitch");
        CHECK(std::abs(engine.trackStatus(0).clipBeat - ratio * sourceBpm / 60) < 1e-7,
              "Native feedback follows source position at the selected pitch rate");
    }

    // Tempo and global timeline loops must never alter native pitch or clock.
    {
        auto run = [](bool changeTempo, bool loop, int block) {
            Project project;
            auto clip = addClip(project, tone(8));
            clip->transpose = -7;
            clip->detune = 31;
            clip->envelopes = {{target::volume(), {{0, 1}, {clip->length, 0.1}}}};
            audio::Engine engine(&project);
            launch(project, engine);
            if (loop) engine.send({Command::Loop, -1, -1, 0, 0.37, true});
            render(engine, rate * 3 / 4, block);
            if (changeTempo) { project.setTempo(79); engine.publish(); }
            const auto output = render(engine, rate, block);
            return std::make_pair(output, engine.trackStatus(0).clipBeat);
        };
        const auto plain = run(false, false, 511), tempo = run(true, false, 997), loop = run(true, true, 997);
        CHECK(difference(plain.first, tempo.first) < 1e-5,
              "Native pitched playback and source envelopes survive a live project tempo change unchanged");
        CHECK(difference(plain.first, loop.first) < 1e-5,
              "Native pitched playback survives unrelated Arrangement loops and callback sizes unchanged");
        CHECK(std::abs(plain.second - tempo.second) < 1e-9 && std::abs(plain.second - loop.second) < 1e-9,
              "Native pitched cursor feedback remains independent of project tempo and Arrangement loop phase");
    }

    // Moving the pitch control during playback advances from the existing
    // source position rather than retroactively applying the rate to the launch.
    {
        Project project;
        auto clip = addClip(project, tone(8));
        audio::Engine engine(&project);
        launch(project, engine);
        render(engine, rate * 3 / 4);
        clip->transpose = 12;
        emit project.clipChanged();
        engine.publish();
        render(engine, 1);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - (0.75 + 2.0 / rate) * sourceBpm / 60) < 1e-7,
              "A live octave edit preserves the source position already reached");
        const auto transposed = render(engine, rate / 4 - 1);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - 1.25 * sourceBpm / 60) < 1e-7,
              "After a live edit the native source advances at the new pitch rate");
        CHECK(std::abs(frequency(transposed, 1000, 11000) - 440) < 5,
              "A live octave edit audibly changes the running tone to the new pitch");
        clip->detune = 50;
        emit project.clipChanged();
        engine.publish();
        render(engine, 1);
        const double ratio = std::exp2(12.5 / 12.0);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - (1.25 + ratio / rate) * sourceBpm / 60) < 1e-7,
              "Live detune edits preserve source position before applying their new rate");
        const auto detuned = render(engine, rate / 4);
        CHECK(std::abs(frequency(detuned, 1000, 11000) - 220 * ratio) < 5,
              "A live detune edit audibly changes the running tone by the selected cents");
        const double before = engine.trackStatus(0).clipBeat;
        clip->warpMode = WarpMode::Repitch;
        emit project.clipChanged();
        engine.publish();
        render(engine, 1);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - before - ratio / rate * sourceBpm / 60) < 1e-7,
              "Changing the saved method to Re-Pitch while Warp Off does not reset the native clock");
    }

    {
        Project project;
        auto clip = addClip(project, tone(8), WarpMode::Repitch);
        clip->transpose = 12;
        clip->warp = true;
        audio::Engine engine(&project);
        launch(project, engine);
        render(engine, rate / 2);
        const double before = engine.trackStatus(0).clipBeat;
        clip->warp = false;
        emit project.clipChanged();
        engine.publish();
        render(engine, 1);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - before - 2.0 / rate * sourceBpm / 60) < 1e-7,
              "Switching Warp Off preserves source position before applying native transpose");
        render(engine, rate / 4);
        const double native = engine.trackStatus(0).clipBeat;
        clip->warp = true;
        emit project.clipChanged();
        engine.publish();
        render(engine, 1);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - native - 2.0 / rate) < 1e-7,
              "Switching Warp On preserves the pitched native position before following project beats");
    }

    for (bool follow : {false, true}) {
        Project project;
        auto clip = addClip(project, tone(1));
        clip->transpose = 12;
        clip->loop = !follow;
        clip->followEnabled = follow;
        clip->followA = FollowAction::Again;
        clip->followChanceA = 1;
        audio::Engine engine(&project);
        launch(project, engine);
        render(engine, rate * 5 / 8);
        const auto status = engine.trackStatus(0);
        CHECK(status.playingSlot == 0 && std::abs(status.clipBeat - 0.25 * sourceBpm / 60) < 1e-7,
              "Native octave-up loops and default follow actions repeat at their half-second physical end");
        if (!follow) CHECK(status.clipPass == 2, "Pitched native loop feedback counts completed passes at the audible loop boundary");
    }

    // SeekSlot offsets are source positions, so their launch origin must be
    // converted through the selected rate both before and after launching.
    for (bool queued : {false, true}) {
        Project project;
        auto clip = addClip(project, tone(8));
        clip->start = 1;
        clip->transpose = 12;
        audio::Engine engine(&project);
        launch(project, engine);
        if (!queued) render(engine, rate / 8);
        engine.send({Command::SeekSlot, project.tracks[0].id, 0, 2.0});
        render(engine, rate / 8);
        CHECK(std::abs(engine.trackStatus(0).clipBeat - (3 + 0.25 * sourceBpm / 60)) < 1e-7,
              "Queued and running native seeks retain the requested source offset under transpose");
    }

    {
        Project project;
        auto first = addClip(project, tone(8));
        first->transpose = 12;
        auto second = project.newClip("Legato native source", first->length);
        *second = *first;
        second->id += 1000;
        second->transpose = -12;
        second->legato = true;
        project.tracks[0].clipSlots[1] = second;
        audio::Engine engine(&project);
        launch(project, engine);
        render(engine, rate * 3 / 8);
        const double before = engine.trackStatus(0).clipBeat;
        engine.send({Command::Launch, project.tracks[0].id, 1, 0.0});
        render(engine, 1);
        CHECK(engine.trackStatus(0).playingSlot == 1
              && std::abs(engine.trackStatus(0).clipBeat - before - 0.5 / rate * sourceBpm / 60) < 1e-7,
              "Native legato launches transfer the audible source position between differently pitched clips");
    }

    // Arrangement offsets and comp boundaries must advance on the same native
    // source clock, rather than selecting regions on a project-tempo clock.
    for (bool arrangement : {false, true}) {
        Project project;
        project.tempo = 93;
        auto clip = addClip(project, tone(2, 330));
        clip->transpose = 12;
        clip->takes = {{"/generated/take-a.wav", tone(2, 220), "Take 1", 0},
                       {clip->samplePath, clip->sample, "Take 2", 0}};
        clip->useTake(0, sourceBpm / 60, 0); // switch after one source second / half a playback second
        if (arrangement) project.tracks[0].arrangement.push_back({0, 8, 0, clip});
        audio::Engine engine(&project);
        if (arrangement) engine.send({Command::Play});
        else launch(project, engine);
        const auto output = render(engine, rate * 5 / 4);
        CHECK(std::abs(frequency(output, rate / 8, rate * 3 / 8) - 440) < 5
              && std::abs(frequency(output, rate * 5 / 8, rate * 7 / 8) - 660) < 5,
              "Native Session and Arrangement comps change takes at their pitched source boundary");
        CHECK(rms(output, rate * 11 / 10, rate * 5 / 4) == 0,
              "Native comp one-shots stop after their pitched source duration at an unrelated project tempo");
    }

    {
        Project project;
        project.tempo = 93;
        auto clip = addClip(project, tone(4));
        clip->transpose = -12;
        clip->loop = true;
        project.tracks[0].arrangement.push_back({0, 12, 2, clip});
        audio::Engine engine(&project);
        engine.send({Command::Play});
        const auto output = render(engine, rate);
        CHECK(std::abs(frequency(output, rate / 5, rate * 4 / 5) - 110) < 3,
              "Native Arrangement clips apply transpose independently of the saved Warp method");
        const double seconds = clip->secondsAt(2) + 0.5;
        CHECK(std::abs(output[size_t(rate - 1) * 2] - 0.2 * std::sin(2 * pi * 220 * (seconds - 0.5 / rate))) < 0.002,
              "Native Arrangement placement offsets refer to source markers before the pitch-rate clock advances");
    }

    // Recording a currently playing native clip must start at the source
    // position heard there, including later loop passes and edited pitch.
    for (double transpose : {-12.0, 12.0}) {
        Project project;
        project.tempo = 93;
        auto clip = addClip(project, tone(1));
        clip->start = 0.5;
        clip->transpose = transpose;
        clip->loop = true;
        const double ratio = std::exp2(transpose / 12.0);
        audio::Engine engine(&project);
        Transport transport(&project, &engine);
        transport.setLaunchQuantization(0);
        transport.launchSlot(0, 0);
        render(engine, rate * 5 / 8);
        tick();
        const double atRecord = project.tracks[0].playingClipBeat;
        const double expectedSeconds = std::fmod(clip->secondsAt(clip->start) + 0.625 * ratio, 1.0);
        CHECK(std::abs(atRecord - expectedSeconds * sourceBpm / 60) < 1e-7,
              "Transport's visible native cursor follows the pitched source clock and loop pass");
        transport.setArrangementRecord(true);
        const double recordBeat = transport.beat();
        render(engine, rate / 8);
        tick();
        CHECK(transport.takes().size() == 1
              && std::abs(transport.takes()[0].sourceOffset - atRecord) < 1e-7,
              "Starting Arrangement recording mid-native clip captures its audible source offset");
        render(engine, rate / 8);
        tick();
        transport.setArrangementRecord(false);
        CHECK(project.tracks[0].arrangement.size() == 1
              && std::abs(project.tracks[0].arrangement[0].offset - atRecord) < 1e-7
              && std::abs(project.tracks[0].arrangement[0].start - recordBeat) < 1e-7,
              "Recorded native placements retain the exact source offset and timeline record start");
        const auto recorded = project.tracks[0].arrangement[0].clip;
        CHECK(recorded != clip && !recorded->warp && recorded->transpose == transpose
              && recorded->warpMode == clip->warpMode,
              "Recording preserves native playback, transpose and the selected dormant Warp algorithm");
        transport.stop();
        engine.publish();
        render(engine, 1);
        engine.send({Command::Locate, -1, -1, recordBeat});
        engine.send({Command::BackToArrangement});
        engine.send({Command::Play});
        const auto replay = render(engine, rate / 5);
        CHECK(std::abs(frequency(replay, 1000, 8000) - 220 * ratio) < 8,
              "A recorded native placement replays at the audible pitch of the original Session clip");
        const int sampleFrame = 2400;
        const double sourceSeconds = expectedSeconds + double(sampleFrame) / rate * ratio;
        CHECK(std::abs(replay[size_t(sampleFrame) * 2] - 0.2 * std::sin(2 * pi * 220 * sourceSeconds)) < 0.002,
              "A recorded native placement resumes the source audio heard at record start");
    }

    for (bool loop : {false, true}) {
        Project project;
        project.tempo = 93;
        auto clip = addClip(project, tone(4));
        clip->transpose = 12;
        clip->loop = loop;
        project.tracks[0].clipSlots[0].reset();
        project.tracks[0].arrangement.push_back({3, 12, 2, clip});
        audio::Engine engine(&project);
        Transport transport(&project, &engine);
        transport.playClipFrom(clip, 4);
        const double expected = 3 + (clip->secondsAt(4) - clip->secondsAt(2)) / 2 * project.tempo / 60;
        CHECK(std::abs(transport.startMarker() - expected) < 1e-7,
              "Clicking a native Arrangement waveform seeks by source time and pitch instead of clip beats");
        if (loop) {
            transport.playClipFrom(clip, 1);
            const double wrapped = 3 + (clip->secondsAt(clip->loopEnd()) - clip->secondsAt(2) + clip->secondsAt(1)) / 2 * project.tempo / 60;
            CHECK(std::abs(transport.startMarker() - wrapped) < 1e-7,
                  "Seeking before a native placement's source offset finds the next audible loop pass");
        } else {
            transport.playClipFrom(clip, 1);
            CHECK(std::abs(transport.startMarker() - expected) < 1e-7,
                  "A native one-shot waveform cannot seek to source material before the placement offset");
        }
    }

    for (double transpose : {-12.0, 12.0}) for (bool loop : {false, true}) {
        Project project;
        project.tempo = 93;
        auto clip = addClip(project, tone(4));
        clip->transpose = transpose;
        clip->loop = loop;
        project.tracks[0].clipSlots[0].reset();
        project.tracks[0].arrangement.push_back({3, 12, 2, clip});
        const double ratio = std::exp2(transpose / 12.0);
        const double now = 3 + 0.625 * project.tempo / 60;
        const double expected = clip->beatAt(clip->secondsAt(2) + 0.625 * ratio);
        double local = -1;
        CHECK(project.playingClip(0, now, &local) == clip
              && std::abs(local - expected) < 1e-7
              && std::abs(project.clipPosition(clip, now) - expected) < 1e-7,
              "Arrangement waveform and overview feedback follow the native pitched source offset");
        const double firstDuration = (clip->sample->seconds() - clip->secondsAt(2)) / ratio;
        const double afterEnd = 3 + (firstDuration + 0.1) * project.tempo / 60;
        if (loop) {
            const double wrapped = 0.1 * ratio * sourceBpm / 60;
            CHECK(project.playingClip(0, afterEnd, &local) == clip
                  && std::abs(local - wrapped) < 1e-7
                  && std::abs(project.clipPosition(clip, afterEnd) - wrapped) < 1e-7,
                  "Arrangement waveform and overview feedback wrap at the audible native loop boundary");
        } else {
            CHECK(!project.playingClip(0, afterEnd) && project.clipPosition(clip, afterEnd) < 0,
                  "Native Arrangement one-shot feedback ends with the audio before a longer placement ends");
        }
    }

    // Live Arrangement playback retains its already reached source position
    // when the pitch or project tempo changes; intentional timeline jumps reset
    // that clock to the selected placement position.
    {
        Project project;
        project.tempo = 93;
        auto clip = addClip(project, tone(8, 223));
        clip->transpose = -12;
        project.tracks[0].arrangement.push_back({0, 40, 2, clip});
        const int trackId = project.tracks[0].id;
        audio::Engine engine(&project);
        Transport transport(&project, &engine);
        engine.send({Command::Play});
        render(engine, 35088); // exactly 0.731 seconds
        tick();
        const double sourceBefore = clip->secondsAt(2) + 0.731 * 0.5;
        CHECK(std::abs(engine.trackStatus(0).clipBeat - clip->beatAt(sourceBefore)) < 1e-7,
              "The Arrangement runtime clock reports the native source position already reached");
        clip->transpose = 12;
        clip->detune = 25;
        project.setTempo(147);
        emit project.clipChanged();
        QCoreApplication::processEvents(); // consume the UI's coalesced publish before advancing its audio snapshot
        engine.publish();
        const double ratio = std::exp2(12.25 / 12.0);
        const auto changed = render(engine, 1);
        CHECK(std::abs(changed[0] - 0.2 * std::sin(2 * pi * 223 * sourceBefore)) < 0.002,
              "A live Arrangement pitch and tempo edit emits the next sample from the existing source phase");
        CHECK(std::abs(engine.trackStatus(0).clipBeat - clip->beatAt(sourceBefore + ratio / rate)) < 1e-7,
              "A live Arrangement edit advances future source time at the new pitch without a seek");
        tick();
        double local = -1;
        CHECK(project.playingClip(0, transport.beat(), &local) == clip
              && std::abs(local - engine.trackStatus(0).clipBeat) < 1e-7
              && std::abs(project.clipPosition(clip, transport.beat()) - local) < 1e-7,
              "Visible Arrangement waveform and overview markers mirror the continuous audio clock after live edits");
        project.addTrack("Inserted track", QColor("#698392"));
        project.tracks.move(1, 0);
        engine.publish();
        render(engine, 1);
        const int index = project.indexOfTrack(trackId);
        CHECK(index == 1 && std::abs(engine.trackStatus(index).clipBeat - clip->beatAt(sourceBefore + 2 * ratio / rate)) < 1e-7,
              "Reordering tracks preserves each native Arrangement clock by stable track identity");
        const double locatedBeat = 0.25 * project.tempo / 60;
        engine.send({Command::Locate, -1, -1, locatedBeat});
        const auto located = render(engine, 1);
        const double locatedSource = clip->secondsAt(2) + 0.25 * ratio;
        CHECK(std::abs(located[0] - 0.2 * std::sin(2 * pi * 223 * locatedSource)) < 0.002
              && std::abs(engine.trackStatus(index).clipBeat - clip->beatAt(locatedSource + ratio / rate)) < 1e-7,
              "An intentional Arrangement locate resets native playback to the selected timeline source position");
    }

    {
        Project project;
        auto clip = addClip(project, tone(8, 223), WarpMode::Repitch);
        clip->transpose = -12;
        project.tracks[0].arrangement.push_back({0, 40, 2, clip});
        audio::Engine engine(&project);
        engine.send({Command::Play});
        render(engine, 35088);
        const double nativeSeconds = clip->secondsAt(engine.trackStatus(0).clipBeat);
        clip->warp = true;
        emit project.clipChanged();
        engine.publish();
        const auto warped = render(engine, 1);
        CHECK(std::abs(warped[0] - 0.2 * std::sin(2 * pi * 223 * nativeSeconds)) < 0.002
              && std::abs(engine.trackStatus(0).clipBeat - clip->beatAt(nativeSeconds) - 2.0 / rate) < 1e-7,
              "Enabling Warp in the Arrangement retains the reached source phase before following project beats");
        const double warpedSeconds = clip->secondsAt(engine.trackStatus(0).clipBeat);
        clip->warp = false;
        clip->transpose = 12;
        emit project.clipChanged();
        engine.publish();
        const auto native = render(engine, 1);
        CHECK(std::abs(native[0] - 0.2 * std::sin(2 * pi * 223 * warpedSeconds)) < 0.002
              && std::abs(engine.trackStatus(0).clipBeat - clip->beatAt(warpedSeconds + 2.0 / rate)) < 1e-7,
              "Disabling Warp in the Arrangement retains source phase before applying the native pitch rate");
    }

    {
        Project project;
        auto clip = addClip(project, tone(4, 223));
        clip->transpose = 12;
        project.tracks[0].arrangement.push_back({0, 8, 2, clip});
        audio::Engine engine(&project);
        engine.send({Command::Loop, -1, -1, 0, 0.5, true});
        render(engine, rate / 5); // settle the mixer gain ramp before comparing identical loop passes
        engine.send({Command::Play});
        const auto output = render(engine, rate / 4 + 128, 997);
        double error = 0;
        for (int i = 0; i < 128; ++i)
            error = std::max(error, std::abs(double(output[size_t(i) * 2] - output[size_t(rate / 4 + i) * 2])));
        CHECK(error < 1e-6, "An intentional Arrangement loop resets native source playback at the audible loop start");
    }

    for (Command::Type stopKind : {Command::Pause, Command::Stop}) {
        Project project;
        auto clip = addClip(project, tone(8, 223));
        clip->transpose = -12;
        project.tracks[0].arrangement.push_back({0, 40, 2, clip});
        audio::Engine engine(&project);
        engine.send({Command::Play});
        render(engine, 35088);
        clip->transpose = 12;
        project.setTempo(147);
        emit project.clipChanged();
        engine.publish();
        render(engine, 1000);
        const double source = clip->secondsAt(engine.trackStatus(0).clipBeat);
        engine.send({stopKind}); // ordinary Stop does not request a rewind
        const auto silence = render(engine, 1000);
        CHECK(rms(silence, 100, 1000) == 0,
              "Pausing or stopping a native Arrangement holds audio while preserving the reached source clock");
        engine.send({Command::Play});
        const auto resumed = render(engine, 1);
        CHECK(std::abs(resumed[0] - 0.2 * std::sin(2 * pi * 223 * source)) < 0.002
              && std::abs(engine.trackStatus(0).clipBeat - clip->beatAt(source + 2.0 / rate)) < 1e-7,
              "Continuing after Pause or ordinary Stop resumes the retained phase after live native pitch and tempo edits");
    }

    {
        Project project;
        auto clip = addClip(project, tone(4));
        clip->start = 2; // Session start must not clamp an Arrangement's earlier placement offset
        clip->transpose = 12;
        project.tracks[0].arrangement.push_back({0, 8, 0, clip});
        audio::Engine engine(&project);
        Transport transport(&project, &engine);
        engine.send({Command::Play});
        render(engine, 1000);
        tick();
        const double expected = 2000.0 / rate * sourceBpm / 60;
        CHECK(std::abs(project.clipPosition(clip, transport.beat()) - expected) < 1e-7,
              "Arrangement source cursor uses placement offset zero independently of the later Session start marker");
        project.tracks[0].playingSourceClipId = clip->id + 1;
        project.tracks[0].playingClipBeat = 999;
        CHECK(std::abs(project.clipPosition(clip, transport.beat()) - expected) < 1e-7,
              "Source cursor feedback from a different clip cannot leak across an Arrangement placement boundary");
    }

    {
        Project project;
        auto clip = addClip(project, tone(4, 223));
        clip->transpose = 12;
        project.tracks[0].arrangement.push_back({0, 1, 2, clip});
        project.tracks[0].arrangement.push_back({1, 8, 1, clip});
        audio::Engine engine(&project);
        render(engine, rate / 5);
        engine.send({Command::Play});
        render(engine, rate / 2);
        const auto next = render(engine, 128);
        const double source = clip->secondsAt(1) + 64.0 / rate * 2;
        CHECK(std::abs(next[128] - 0.2 * std::sin(2 * pi * 223 * source)) < 0.002,
              "Adjacent placements sharing one clip reset the native clock to the new placement's own source offset");
    }

    {
        Project project;
        auto clip = addClip(project, tone(1));
        clip->transpose = 12;
        project.tracks[0].arrangement.push_back({0, 8, 0, clip});
        audio::Engine engine(&project);
        Transport transport(&project, &engine);
        engine.send({Command::Play});
        render(engine, rate * 2 / 5); // reached source second 0.8
        tick();
        clip->transpose = -12;
        emit project.clipChanged();
        QCoreApplication::processEvents();
        engine.publish();
        const auto end = render(engine, rate / 2); // source second 1.05, although the static formula would report 0.45
        tick();
        CHECK(rms(end, rate * 9 / 20, rate / 2) == 0 && engine.trackStatus(0).clipPass == -1,
              "A native Arrangement one-shot ends on the continuous source clock after a live downward pitch edit");
        CHECK(project.clipPosition(clip, transport.beat()) < 0 && !project.playingClip(0, transport.beat()),
              "The visible cursor stays ended instead of reverting to a stale musical-time source formula");
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
