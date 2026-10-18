// Group-bus regressions: generated source audio, no audio device or user library.
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Sample.h"
#include "core/SetFile.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace sessions;
using sessions::audio::Command;
namespace {
constexpr int rate = 48000, framesPerBeat = 24000;
int checks = 0, failures = 0;
#define CHECK(condition, text) do { ++checks; if (condition) std::printf("ok: %s\n", text); else { ++failures; std::printf("FAIL: %s\n", text); } } while (0)

SamplePtr source() {
    auto s = std::make_shared<SampleData>();
    s->rate = rate;
    s->left.assign(size_t(8 * framesPerBeat), 0.025f);
    s->right.assign(s->left.size(), 0.015f);
    return s;
}

// Two children overlap, start on different beats and finish at the same edge.
// Their pan and stereo values make accidental channel/member omissions visible.
int grouped(Project& p) {
    p.scenes = {"1"};
    for (int i = 0; i < 2; ++i) {
        p.addTrack(QString("Member %1").arg(i + 1), QColor("#7e9db2"));
        Track& t = p.tracks.last();
        t.type = TrackType::Audio;
        t.monitor = Monitor::Off;
        t.pan = i ? 0.2 : -0.2;
        const ClipPtr c = p.newClip("Generated group source", 8);
        c->sample = source();
        c->samplePath = "/generated/group-fades.wav";
        c->warp = true;
        c->warpMode = WarpMode::Repitch;
        c->warpMarkers = {{0, 0}, {8, 4}};
        c->loop = true;
        t.clipSlots[0] = c;
        t.arrangement = {{i ? 4.0 : 2.0, i ? 6.0 : 8.0, 0, c}};
    }
    return p.groupTracks({0, 1});
}

std::vector<float> render(Project& p, bool session = false, int block = 511, bool monitored = false, bool stopped = false) {
    audio::Engine e(&p);
    if (monitored) e.setInputWanted(true);
    e.send({Command::Stop, -1, -1, 0, 0, true});
    if (session) for (const Track& t : p.tracks) if (!t.isGroup()) e.send({Command::Launch, t.id, 0, 0});
    if (!stopped) e.send({Command::Play});
    const int frames = 11 * framesPerBeat;
    std::vector<float> out(size_t(frames) * 2);
    std::vector<float> input(size_t(block) * 2, 0.025f);
    for (int pos = 0; pos < frames; pos += block)
        e.process(out.data() + size_t(pos) * 2, std::min(block, frames - pos), monitored ? input.data() : nullptr);
    return out;
}

double expected(double beat, const Track& group, ArrangementSpan span) {
    double g = 1;
    const double length = span.end - span.start;
    if (group.groupFadeIn > 0) g *= fadeGain((beat - span.start) / std::min(length, group.groupFadeIn), group.groupFadeInCurve);
    if (group.groupFadeOut > 0) g *= fadeGain((span.end - beat) / std::min(length, group.groupFadeOut), group.groupFadeOutCurve);
    return g;
}

double maxError(const std::vector<float>& actual, const std::vector<float>& reference, const Track& group, ArrangementSpan span) {
    double error = 0;
    for (size_t i = 0; i < actual.size() / 2; ++i) {
        const double gain = expected(double(i) / framesPerBeat, group, span);
        for (int channel = 0; channel < 2; ++channel)
            error = std::max(error, std::abs(actual[2 * i + size_t(channel)] - reference[2 * i + size_t(channel)] * gain));
    }
    return error;
}

double difference(const std::vector<float>& a, const std::vector<float>& b) {
    double error = 0;
    for (size_t i = 0; i < a.size(); ++i) error = std::max(error, std::abs(double(a[i]) - b[i]));
    return error;
}
}

int main(int argc, char** argv) {
    QTemporaryDir dir;
    qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
    qputenv("XDG_DATA_HOME", dir.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QCoreApplication app(argc, argv);

    Project p;
    const int g = grouped(p);
    CHECK(g == 0 && p.membersOf(g).size() == 2, "group creates a bus before both selected members");
    CHECK(p.acceptsDevice(g, DeviceKind::AudioEffect) && !p.acceptsDevice(g, DeviceKind::Instrument)
          && !p.acceptsDevice(g, DeviceKind::MidiEffect), "group device chains accept audio processors and reject instruments or MIDI effects");
    const ArrangementSpan span = p.groupArrangementSpan(g);
    CHECK(span.start == 2 && span.end == 10, "group span covers the outer member Arrangement edges");
    CHECK(p.groupArrangementSpan(1).empty() && p.groupArrangementSpan(-1).empty(), "regular and missing tracks have no group span");
    p.setFolded(g, true);
    CHECK(p.groupArrangementSpan(g).start == 2 && p.groupArrangementSpan(g).end == 10, "folding keeps member audio and fade span intact");
    p.tracks[2].arrangement.push_back({20, 8, 0, nullptr});
    CHECK(p.groupArrangementSpan(g).end == 10, "empty placements do not extend a group fade");
    p.tracks[2].arrangement.removeLast();
    p.tracks[2].arrangement[0].length = 8;
    CHECK(p.groupArrangementSpan(g).end == 12, "trimming or extending a member updates the group boundary");
    p.tracks[2].arrangement[0].length = 6;

    const auto dry = render(p);
    CHECK(dry[size_t(6 * framesPerBeat) * 2] > 0.01f && dry[size_t(6 * framesPerBeat) * 2 + 1] > 0.005f, "both stereo channels of the group are audible");
    p.tracks[g].groupFadeIn = 2;
    p.tracks[g].groupFadeOut = 2;
    auto faded = render(p);
    CHECK(maxError(faded, dry, p.tracks[g], span) < 1e-6, "linear group fades multiply the entire stereo bus at each sample");
    CHECK(std::abs(faded[size_t(3 * framesPerBeat) * 2] / dry[size_t(3 * framesPerBeat) * 2] - 0.5f) < 1e-5,
          "fade-in is anchored to the earliest child, rather than song beat zero");
    CHECK(std::abs(faded[size_t(9 * framesPerBeat) * 2] / dry[size_t(9 * framesPerBeat) * 2] - 0.5f) < 1e-5,
          "fade-out is anchored to the latest child edge");
    CHECK(difference(faded, render(p, false, 128)) < 1e-6, "group fades are independent of audio callback size");
    p.tracks[g].groupFadeInCurve = 0.2;
    p.tracks[g].groupFadeOutCurve = 0.8;
    faded = render(p);
    CHECK(maxError(faded, dry, p.tracks[g], span) < 1e-6, "group curve nodes use the same response as clip fade curves");
    p.tracks[g].groupFadeIn = 100;
    p.tracks[g].groupFadeOut = 100;
    CHECK(maxError(render(p), dry, p.tracks[g], span) < 1e-6, "overlapping and oversized fades clamp safely to a shortened group span");
    p.tracks[g].groupFadeIn = 2;
    p.tracks[g].groupFadeOut = 2;

    const auto sessionFaded = render(p, true);
    p.tracks[g].groupFadeIn = p.tracks[g].groupFadeOut = 0;
    CHECK(difference(sessionFaded, render(p, true)) < 1e-6, "Session takeover bypasses Arrangement group fades before and after the group span");
    Device utility = p.makeDevice("Utility", DeviceKind::AudioEffect);
    utility.params[0].value = 0.4; // -7 dB: every child must pass through this one device.
    p.tracks[g].devices = {utility};
    const auto effectDry = render(p);
    CHECK(effectDry[size_t(6 * framesPerBeat) * 2] < dry[size_t(6 * framesPerBeat) * 2] * 0.6,
          "an audio effect on the group processes all member tracks");
    p.tracks[g].groupFadeIn = p.tracks[g].groupFadeOut = 2;
    CHECK(maxError(render(p), effectDry, p.tracks[g], span) < 1e-6, "group fades also multiply the group's processed output");
    p.tracks[g].active = false;
    const auto muted = render(p);
    CHECK(std::all_of(muted.begin(), muted.end(), [](float x) { return x == 0; }), "group mute silences all grouped members");
    p.tracks[g].active = true;

    p.tracks[1].monitor = Monitor::In;
    const auto monitoredFaded = render(p, false, 511, true);
    const auto stoppedFaded = render(p, false, 511, true, true);
    p.tracks[g].groupFadeIn = p.tracks[g].groupFadeOut = 0;
    CHECK(difference(monitoredFaded, render(p, false, 511, true)) < 1e-6
          && monitoredFaded[size_t(framesPerBeat) * 2] > 0.005f, "monitored live audio bypasses group fades outside the Arrangement span");
    CHECK(difference(stoppedFaded, render(p, false, 511, true, true)) < 1e-6
          && stoppedFaded[size_t(framesPerBeat) * 2] > 0.005f, "a stopped transport never suppresses monitored group audio with Arrangement fades");
    p.tracks[1].monitor = Monitor::Off;
    p.tracks[g].groupFadeIn = p.tracks[g].groupFadeOut = 2;

    const EditState before = p.captureEditState();
    const int undoBefore = p.undoStack()->count();
    p.beginEdit("Group Fades");
    p.tracks[g].groupFadeIn = 1.25;
    p.tracks[g].groupFadeOut = 3.5;
    p.tracks[g].groupFadeInCurve = 0.3;
    p.tracks[g].groupFadeOutCurve = 0.7;
    p.endEdit();
    const EditState changed = p.captureEditState();
    CHECK(!(changed == before) && p.undoStack()->count() == undoBefore + 1, "a group fade drag forms exactly one undo edit");
    p.undoStack()->undo();
    CHECK(p.captureEditState() == before, "undo restores both group fade lengths and curve nodes");
    p.undoStack()->redo();
    CHECK(p.captureEditState() == changed, "redo restores the edited group fade");
    p.ungroup(g);
    p.undoStack()->undo();
    CHECK(p.captureEditState() == changed && p.tracks[g].groupFadeOut == 3.5, "ungroup undo restores the bus effects and fades together");

    setfile::Extras extras, loadedExtras;
    EditState loaded;
    QString error;
    const QByteArray json = setfile::toJson(p.captureEditState(), extras);
    CHECK(setfile::fromJson(json, loaded, loadedExtras, &error) && loaded == changed,
          "set files round-trip group fade lengths, curves, devices and membership");
    QJsonObject document = QJsonDocument::fromJson(json).object();
    QJsonArray tracks = document.value("tracks").toArray();
    QJsonObject group = tracks[0].toObject();
    group.remove("groupFades"); tracks[0] = group; document.insert("tracks", tracks);
    CHECK(setfile::fromJson(QJsonDocument(document).toJson(), loaded, loadedExtras, &error)
          && loaded.tracks[0].groupFadeIn == 0 && loaded.tracks[0].groupFadeOut == 0
          && loaded.tracks[0].groupFadeInCurve == 0.5, "older sets without group fades keep their original unfaded bus");
    group.insert("groupFades", QJsonObject{{"in", -2}, {"out", 1.5}, {"inCurve", 5}, {"outCurve", -3}});
    tracks[0] = group; document.insert("tracks", tracks);
    CHECK(setfile::fromJson(QJsonDocument(document).toJson(), loaded, loadedExtras, &error)
          && loaded.tracks[0].groupFadeIn == 0 && loaded.tracks[0].groupFadeOut == 1.5
          && loaded.tracks[0].groupFadeInCurve == 0.98 && loaded.tracks[0].groupFadeOutCurve == 0.02,
          "loading sanitizes negative group fade lengths and invalid curve ranges");

    // Empty groups have no Arrangement boundary and must still pass Session audio.
    for (Track& child : p.tracks) if (!child.isGroup()) child.arrangement.clear();
    CHECK(p.groupArrangementSpan(g).empty(), "a group without Arrangement clips has no fade span");
    p.tracks[g].groupFadeIn = p.tracks[g].groupFadeOut = 4;
    const auto emptySession = render(p, true);
    CHECK(emptySession[size_t(6 * framesPerBeat) * 2] > 0.005f, "stored group fades never silence a Session-only group");

    std::printf("group_fades: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
