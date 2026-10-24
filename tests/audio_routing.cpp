#include "audio/Engine.h"
#include "core/Model.h"
#include "core/SetFile.h"
#include "core/Transport.h"
#include "core/Theme.h"
#include <QDir>
#include "ui/MixerStrip.h"
#include <QApplication>
#include <QComboBox>
#include <QAbstractItemView>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
void tracks(Project& p, int count) {
    p.scenes = {"1"};
    for (int i = 0; i < count; ++i) {
        p.addTrack(QString("Audio %1").arg(i + 1), QColor("#8cb6cf"));
        p.tracks[i].type = TrackType::Audio;
        p.tracks[i].monitor = Monitor::Off;
    }
}
SamplePtr sample() {
    auto s = std::make_shared<SampleData>();
    s->rate = 48000;
    s->left.assign(96000, 0.2f);
    s->right.assign(96000, -0.1f);
    s->left[24000] = 0.6f;
    return s;
}
void sourceClip(Project& p, int index) {
    auto c = p.newClip("Source", 4);
    c->sample = sample();
    c->samplePath = "test-source.wav";
    c->warp = false;
    c->warpMarkers = {{0, 0}, {4, 2}};
    p.tracks[index].arrangement.push_back({0, 4, 0, c});
}
void process(audio::Engine& e, int count = 1) {
    float output[512];
    for (int i = 0; i < count; ++i) e.process(output, 256);
}
}
int main(int argc, char** argv) {
    QTemporaryDir temporary;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    {
        Project p; tracks(p, 3);
        audio::Engine e(&p); Transport t(&p, &e);
        MixerStrip strip(&p, &t, 0);
        auto* selector = strip.findChild<QComboBox*>("audioFrom");
        check(selector && selector->count() == 6, "selector includes stereo, mono, Master and other tracks");
        check(selector && selector->findData(p.tracks[0].id) < 0, "self input excluded");
        const int id = p.tracks[2].id;
        const int choice = selector->findData(id);
        selector->setCurrentIndex(choice);
        QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, choice));
        check(p.tracks[0].inputSource == id, "selector uses stable source identity");
        p.undoStack()->undo();
        check(p.tracks[0].inputSource == 0, "input selection undo");
        p.undoStack()->redo();
        check(p.tracks[0].inputSource == id, "input selection redo");
        QString error; setfile::Extras extras;
        check(setfile::save(temporary.filePath("routing.sessions"), p, extras, &error), "save input routing");
        Project restored;
        check(setfile::load(temporary.filePath("routing.sessions"), restored, extras, &error)
              && restored.tracks[0].inputSource == id, "input routing round trip");
        p.tracks[0].inputSource = 98765; emit p.mixerChanged();
        check(selector->currentText() == "Missing source", "missing input is visible without hardware fallback");
        p.tracks[0].inputSource = -1;
        check(p.blockedAudioInputs().contains(p.tracks[0].id), "Master monitoring blocked");
        p.tracks[0].inputSource = p.tracks[1].id;
        p.tracks[1].inputSource = p.tracks[0].id;
        p.tracks[0].monitor = p.tracks[1].monitor = Monitor::In;
        check(p.blockedAudioInputs().size() == 1, "monitoring cycle cuts one input edge");
        p.tracks[1].inputSource = 0;
        check(p.blockedAudioInputs().isEmpty(), "acyclic internal monitoring allowed");
        p.tracks[2].type = TrackType::Group;
        p.tracks[0].groupId = p.tracks[2].id;
        p.tracks[0].inputSource = p.tracks[2].id;
        check(p.blockedAudioInputs().contains(p.tracks[0].id), "group feeding its own member blocks monitoring");
        const QString capture = qEnvironmentVariable("SESSIONS_AUDIO_ROUTING_CAPTURE");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            p.tracks[0].inputSource = -1; strip.syncFromModel(); strip.resize(120, 500); strip.show();
            QApplication::processEvents(); strip.grab().save(capture + "/audio-master.png");
            selector->showPopup(); QApplication::processEvents();
            selector->view()->window()->grab().save(capture + "/audio-sources.png"); selector->hidePopup();
        }
    }
    // Destination precedes its source and group in project order: dependency sorting is essential.
    for (int source : {1, 2, -1}) {
        Project p; tracks(p, 3);
        p.tracks[2].type = TrackType::Group;
        p.tracks[1].groupId = p.tracks[2].id;
        sourceClip(p, 1);
        p.tracks[0].inputSource = source < 0 ? -1 : p.tracks[source].id;
        p.tracks[0].monitor = Monitor::In;
        audio::Engine e(&p);
        auto stream = e.recordingStream(p.tracks[0].inputSource);
        e.startCapture(); e.send({audio::Command::Play});
        process(e, 20);
        std::vector<float> captured(10240);
        const int got = stream->ring.read(captured.data(), int(captured.size()));
        check(got == int(captured.size()), "internal capture works with hardware closed");
        check(std::abs(captured[got - 2]) > 0.15f && captured[got - 1] < -0.05f, "track/group/Master capture contains stereo source audio");
        check(!e.inputOpen(), "internal recording does not open interface input");
        if (source > 0) check(e.takeInputPeak(0) > 0.15f, "internal monitoring receives source rendered later in project order");
    }
    {
        Project p; tracks(p, 3); sourceClip(p, 1);
        p.tracks[2].type = TrackType::Group; p.tracks[1].groupId = p.tracks[2].id;
        auto utility = p.makeDevice("Utility", DeviceKind::AudioEffect);
        utility.params[5].value = 1; // invert R
        p.tracks[1].devices.push_back(utility);
        p.tracks[1].volume = p.tracks[2].volume = p.mainVolume = .375;
        p.tracks[1].pan = .5;
        audio::Engine e(&p);
        auto track = e.recordingStream(p.tracks[1].id);
        auto group = e.recordingStream(p.tracks[2].id);
        auto master = e.recordingStream(-1);
        e.previewSample(sample()); // cue must not be printed in Master
        e.startCapture(); e.send({audio::Command::Play}); process(e, 80);
        std::vector<float> data(40960);
        track->ring.read(data.data(), int(data.size()));
        check(std::abs(data[data.size()-2] - .025f) < .0001f && std::abs(data.back() - .025f) < .0001f,
              "track tap follows source effects, pan and fader");
        group->ring.read(data.data(), int(data.size()));
        check(std::abs(data.back() - .00625f) < .0001f, "group tap follows group fader");
        master->ring.read(data.data(), int(data.size()));
        check(std::abs(data.back() - .0015625f) < .0001f, "Master tap follows master fader and excludes browser preview");
        p.tracks[1].active = false; e.publish(); process(e, 80);
        track->ring.read(data.data(), int(data.size()));
        check(std::abs(data.back()) < .00001f, "source mute reaches recording tap");
        p.deleteTrack(1); e.publish(); process(e, 10);
        const int got = track->ring.read(data.data(), int(data.size()));
        check(got == 5120 && std::abs(data[got-1]) < .00001f, "deleted capture source produces silence instead of stale samples");
        auto late = e.recordingStream(p.tracks[0].id);
        process(e, 4);
        check(late->firstFrame.load() == 43520 && late->ring.available() == 2048, "late capture source publishes its exact stream frame");
    }
    // Concurrent Arrangement takes must receive distinct streams and exact sample timing.
    {
        Project p; tracks(p, 4); sourceClip(p, 3);
        p.recordFolder = temporary.path();
        p.tracks[0].inputSource = p.tracks[3].id;
        p.tracks[1].inputSource = -1;
        for (int i : {0, 1, 2}) p.tracks[i].armed = true;
        audio::Engine e(&p); Transport t(&p, &e);
        t.setArrangementRecord(true); t.play();
        float input[512], output[512];
        for (float& x : input) x = 0.035f;
        for (int block = 0; block < 400; ++block) {
            e.pushInput(input, 256); e.process(output, 256);
            if (block == 180) { p.tracks[0].inputSource = 0; e.publish(); }
            if (block % 8 == 7) QTest::qWait(17);
        }
        QTest::qWait(20); t.stop();
        for (int i = 0; i < 30 && t.audioRecording(); ++i) { process(e, 8); QTest::qWait(17); }
        check(!t.audioRecording(), "concurrent takes finish");
        for (int i : {0, 1, 2}) check(!p.tracks[i].arrangement.isEmpty() && p.tracks[i].arrangement.first().clip->sample, "concurrent take saved as audio");
        if (!p.tracks[0].arrangement.isEmpty() && p.tracks[0].arrangement.first().clip->sample) {
            const auto s = p.tracks[0].arrangement.first().clip->sample;
            check(s->frames() > 24000 && s->left[24000] > 0.55f && std::abs(s->left[23999] - .2f) < .001f,
                  "internal take sample alignment has no hardware latency compensation");
            check(s->frames() > 70000 && std::abs(s->left[70000] - .2f) < .001f, "take retains original source after selector changes");
        }
        if (!p.tracks[2].arrangement.isEmpty() && p.tracks[2].arrangement.first().clip->sample)
            check(std::abs(p.tracks[2].arrangement.first().clip->sample->left[24000] - .035f) < .001f, "hardware take stays separate from internal sources");
    }
    // Long pre-roll and count-in must drain all rings before punch-in, including
    // the unused hardware stream, and keep the internal take on the musical grid.
    {
        Project p; tracks(p, 2); sourceClip(p, 1);
        p.tracks[1].arrangement[0].length = 20;
        p.tracks[0].inputSource = p.tracks[1].id; p.tracks[0].armed = true;
        p.recordFolder = temporary.path();
        audio::Engine e(&p); Transport t(&p, &e);
        t.setCountInBars(1); t.setLoop(12, 4); t.setPunchIn(true); t.setPunchOut(true);
        t.setArrangementRecord(true); t.play();
        for (int block = 0; block < 2200; ++block) { process(e); if (block % 32 == 31) QTest::qWait(17); }
        QTest::qWait(20); t.stop();
        for (int i = 0; i < 20 && t.audioRecording(); ++i) { process(e, 8); QTest::qWait(17); }
        const auto& placed = p.tracks[0].arrangement;
        check(!t.audioRecording() && placed.size() == 1 && std::abs(placed[0].start - 12) < .001
              && std::abs(placed[0].length - 4) < .001, "internal count-in and punch boundaries");
        check(!placed.isEmpty() && placed[0].clip->sample && placed[0].clip->sample->left[24000] > .55f,
              "internal punch take preserves sample timing after count-in");
        check(e.recordOverflows() == 0 && !e.inputOpen(), "long pre-roll drains source rings without opening hardware");
    }
    // Session recording from a group, ending at a bar boundary.
    {
        Project p; tracks(p, 3); sourceClip(p, 1);
        p.tracks[2].type = TrackType::Group; p.tracks[1].groupId = p.tracks[2].id;
        p.tracks[0].inputSource = p.tracks[2].id; p.tracks[0].armed = true;
        p.recordFolder = temporary.path();
        audio::Engine e(&p); Transport t(&p, &e);
        t.play();
        t.recordSlot(0, 0);
        for (int block = 0; block < 220; ++block) { process(e); if (block % 8 == 7) QTest::qWait(17); }
        t.launchSlot(0, 0);
        for (int block = 0; block < 320; ++block) { process(e); if (block % 8 == 7) QTest::qWait(17); }
        const auto clip = p.tracks[0].clipSlots[0];
        check(clip && clip->sample && clip->sample->frames() == 96000, "Session group recording ends at exact bar");
        check(clip && clip->sample && clip->sample->left[24000] > .55f, "Session group recording preserves timing");
        check(!e.inputOpen(), "Session internal recording leaves hardware closed");
    }
    std::printf("Audio routing: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
