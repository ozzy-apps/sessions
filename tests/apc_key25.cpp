// The APC Key 25 profile (both models), relative MIDI Map knobs, the browser preview's Stop
// and the piano roll's lit keys. Settings and data are temporary; MIDI is injected and
// the controller's lights are captured from MidiInput::sent, without hardware.
#include "audio/Engine.h"
#include "audio/MidiInput.h"
#include "core/ApcKey25.h"
#include "core/DeviceSpecs.h"
#include "core/Mapping.h"
#include "core/Model.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/Browser.h"
#include "ui/ClipView.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include "ui/MainWindow.h"
#include "ui/PianoRoll.h"
#include "ui/SessionView.h"
#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QPushButton>
#include <QMouseEvent>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
    else std::printf("ok:   %s\n", text);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
const QString mk2Keys = "APC Key 25 mk2: APC Key 25 mk2 APC Key 25 mk2 K";
const QString mk2Controls = "APC Key 25 mk2: APC Key 25 mk2 APC Key 25 mk2 C";
const QString classic = "APC Key 25: APC Key 25 MIDI 1";
QVector<ApcKey25::Port> mk2Ports() { return {{"24:0", mk2Keys}, {"24:1", mk2Controls}, {"30:0", "Other Keyboard: MIDI 1"}}; }
void settings(bool pickup = true) {
    QSettings s("Sessions", "Sessions");
    s.setValue("midi/controller", "auto");
    s.setValue("midi/controllerPickup", pickup);
    s.remove("midi/apcKnobs");
}
int paramNamed(const Device& d, const QString& name) {
    for (int i = 0; i < d.params.size(); ++i) if (d.params[i].name == name) return i;
    return -1;
}
// A set of `tracks` MIDI tracks and 6 scenes, a clip in every track's first three scenes.
void fill(Project& p, int tracks) {
    p.scenes = {"1", "2", "3", "4", "5", "6"};
    for (int i = 0; i < tracks; ++i) {
        p.addTrack(QString("Track %1").arg(i + 1), QColor::fromHsv((i * 37) % 360, 200, 220));
        for (int s = 0; s < 3; ++s) p.tracks[i].clipSlots[s] = p.newClip(QString("Clip %1-%2").arg(i + 1).arg(s + 1), 4);
    }
}
void press(ApcKey25& apc, const QString& port, int note, bool on = true) { apc.note(port, 0, note, on ? 127 : 0, on); }
void tap(ApcKey25& apc, const QString& port, int note) { press(apc, port, note, true); press(apc, port, note, false); }
void withShift(ApcKey25& apc, const QString& port, int note) { press(apc, port, ApcKey25::kShift); tap(apc, port, note); press(apc, port, ApcKey25::kShift, false); }
int recordPixels(QWidget* w, int maxX) {
    const QImage img = w->grab().toImage();
    int n = 0;
    for (int y = 0; y < img.height(); ++y) for (int x = 0; x < std::min(maxX, img.width()); ++x) if (img.pixelColor(x, y) == theme::record) ++n;
    return n;
}
}

int main(int argc, char** argv) {
    const QString shots = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();   // optional: a folder for reviewed captures
    auto capture = [&](QWidget* w, const QString& name) { if (!shots.isEmpty()) w->grab().save(shots + "/" + name); };
    QTemporaryDir temporary;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);   // the .qrc lives in the static sessions_core library
    theme::apply(theme::Kind::Dark);
    settings();

    // ---- names and ports
    check(ApcKey25::modelOf("APC Key 25: APC Key 25 MIDI 1") == ApcKey25::Model::Classic && ApcKey25::modelOf("Akai APC Key 25") == ApcKey25::Model::Classic
              && ApcKey25::modelOf(mk2Controls) == ApcKey25::Model::Mk2 && ApcKey25::modelOf("APC KEY 25 MKII: Control") == ApcKey25::Model::Mk2,
          "APC: both models are recognised by their port names");
    check(ApcKey25::modelOf("APC40 mkII: MIDI 1") == ApcKey25::Model::None && ApcKey25::modelOf("APC mini mk2: Control") == ApcKey25::Model::None
              && ApcKey25::modelOf("Impact LX49+: MIDI1") == ApcKey25::Model::None, "APC: other controllers are left alone");
    {
        Project p; fill(p, 10);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        Mapper mapper(&p, &transport);
        ApcKey25 apc(&p, &transport, &mapper);
        QVector<QPair<QString, QByteArray>> sent;
        apc.setSender([&](const QString& port, const QByteArray& m) { sent.push_back({port, m}); });
        check(!apc.connected() && sent.isEmpty(), "APC: nothing is sent before a controller is there");
        apc.setPorts(mk2Ports());
        check(apc.connected() && apc.model() == ApcKey25::Model::Mk2 && apc.controlsPort() == mk2Controls
                  && apc.role(mk2Keys) == ApcKey25::Role::Keys && apc.role(mk2Controls) == ApcKey25::Role::Controls
                  && apc.role("Other Keyboard: MIDI 1") == ApcKey25::Role::None, "APC mk2: the first port is the keys, the second the controls");
        bool allToControls = true;
        for (const auto& m : sent) allToControls &= m.first == mk2Controls && m.second.size() == 3 && (uchar(m.second[0]) & 0xF0) == 0x90;
        check(sent.size() == 40 + 8 + 5 + 2 && allToControls, "APC mk2: connecting lights every pad and button once, on the controls port");
        {
            Project q; fill(q, 2);
            Transport tq(&q, &engine); Mapper mq(&q, &tq); ApcKey25 generic(&q, &tq, &mq);
            generic.setPorts({{"20:0", "APC Key 25 mk2: APC Key 25 mk2 MIDI 1"}, {"20:1", "APC Key 25 mk2: APC Key 25 mk2 MIDI 2"}});
            check(generic.controlsPort().endsWith("MIDI 2") && generic.role("APC Key 25 mk2: APC Key 25 mk2 MIDI 1") == ApcKey25::Role::Keys,
                  "APC mk2: generic port names still find the controls port");
            ApcKey25 old(&q, &tq, &mq);
            old.setPorts({{"28:0", classic}});
            check(old.connected() && old.model() == ApcKey25::Model::Classic && old.controlsPort() == classic, "APC (2014): its one port carries the controls");
            check(old.takesNote(classic, 0, 36) && old.takesNote(classic, 0, ApcKey25::kShift) && !old.takesNote(classic, 0, 50)
                      && !old.takesNote(classic, 1, 36), "APC (2014): channel 1's pads and buttons are taken, other notes and channels play");
        }
        check(apc.takesNote(mk2Controls, 0, 50) && apc.takesNote(mk2Controls, 3, 10) && !apc.takesNote(mk2Keys, 0, 36) && !apc.takesNote(mk2Keys, 0, 60),
              "APC mk2: nothing on the controls port plays notes; the keys port plays them all");
        check(apc.isRelativeKnob(mk2Controls, 0, 48) && apc.isRelativeKnob(mk2Controls, 0, 55) && !apc.isRelativeKnob(mk2Controls, 0, 56)
                  && !apc.isRelativeKnob(mk2Keys, 0, 48) && !apc.isRelativeKnob(classic, 0, 48), "APC: only the mk2's knobs are endless");

        // ---- pads
        press(apc, mk2Controls, 32);   // top left: track 1, scene 1
        check(p.tracks[0].queuedSlot == 0 || p.tracks[0].playingSlot == 0, "APC pads: the top-left pad launches the box's first clip");
        press(apc, mk2Controls, 32, false);
        press(apc, mk2Controls, 1); press(apc, mk2Controls, 1, false);   // bottom row: scene 5 of track 2, empty
        check(p.tracks[1].queuedSlot == -1 || p.tracks[1].playingSlot == -1, "APC pads: an empty pad stops its track");
        tap(apc, mk2Controls, 32 - 8 + 2);   // second row, third column: track 3, scene 2
        check(p.tracks[2].queuedSlot == 1 || p.tracks[2].playingSlot == 1, "APC pads: rows count down from the top, columns from the left");
        p.tracks[3].clipSlots[0]->launchMode = LaunchMode::Gate;
        transport.stop(); transport.stop();
        for (auto& t : p.tracks) { t.queuedSlot = -2; t.playingSlot = -1; }
        press(apc, mk2Controls, 35);
        const bool gateOn = p.tracks[3].queuedSlot == 0 || p.tracks[3].playingSlot == 0;
        press(apc, mk2Controls, 35, false);
        check(gateOn && p.tracks[3].queuedSlot != 0, "APC pads: letting go of a Gate clip's pad releases it");
        QSignalSpy chosen(&apc, &ApcKey25::clipChosen);
        for (auto& t : p.tracks) t.queuedSlot = -2;
        press(apc, mk2Controls, ApcKey25::kShift); tap(apc, mk2Controls, 33); press(apc, mk2Controls, ApcKey25::kShift, false);
        check(chosen.size() == 1 && chosen[0][0].toInt() == 1 && chosen[0][1].toInt() == 0 && p.tracks[1].queuedSlot == -2,
              "APC pads: Shift + pad shows the clip without launching it");
        p.tracks[4].armed = true;
        tap(apc, mk2Controls, 4 + 8);   // track 5, scene 4: empty on an armed track
        check(transport.isRecordingSlot(4, 3) || p.tracks[4].clipSlots[3], "APC pads: an armed track's empty pad records into it");
        transport.stop(); transport.stop();
        p.tracks[4].armed = false;

        // ---- scenes, stop all, transport
        for (auto& t : p.tracks) { t.queuedSlot = -2; t.playingSlot = -1; }
        tap(apc, mk2Controls, ApcKey25::kSceneButton + 1);
        bool row = true;
        for (int i = 0; i < 10; ++i) row &= p.tracks[i].queuedSlot == 1 || p.tracks[i].playingSlot == 1;
        check(row, "APC scenes: a scene button launches the box's scene");
        transport.play();
        tap(apc, mk2Controls, ApcKey25::kStopAll);
        bool stopping = true;
        for (int i = 0; i < 10; ++i) stopping &= p.tracks[i].queuedSlot == -1 || p.tracks[i].playingSlot < 0;
        check(stopping, "APC: Stop All Clips stops every clip");
        transport.stop(); transport.stop();
        tap(apc, mk2Controls, ApcKey25::kPlay);
        const bool started = transport.playing();
        tap(apc, mk2Controls, ApcKey25::kPlay);
        check(started && !transport.playing(), "APC: Play starts and stops the transport");
        tap(apc, mk2Controls, ApcKey25::kRecord);
        const bool armed = transport.arrangementRecord();
        tap(apc, mk2Controls, ApcKey25::kRecord);
        check(armed && !transport.arrangementRecord(), "APC: Rec switches Arrangement Record");
        transport.stop(); transport.stop();

        // ---- the box
        QSignalSpy moved(&apc, &ApcKey25::boxChanged);
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 3);   // right
        check(apc.boxTrack() == 1 && apc.boxTracks().first() == 1 && moved.size() == 1, "APC box: Shift + the fourth track button moves it right");
        for (int i = 0; i < 4; ++i) withShift(apc, mk2Controls, ApcKey25::kTrackButton + 3);
        check(apc.boxTrack() == 2 && apc.boxTracks().last() == 9, "APC box: it stops with its last column on the last track");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 1);   // down
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 1);
        check(apc.firstScene() == 1, "APC box: down moves a scene, and stops with its last row on the last scene");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 0); withShift(apc, mk2Controls, ApcKey25::kTrackButton + 2); withShift(apc, mk2Controls, ApcKey25::kTrackButton + 2);
        check(apc.boxTrack() == 0 && apc.firstScene() == 0, "APC box: up and left move it back");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 3);
        for (auto& t : p.tracks) t.queuedSlot = -2;
        tap(apc, mk2Controls, 32);
        check(p.tracks[1].queuedSlot == 0 || p.tracks[1].playingSlot == 0, "APC box: the pads follow the box");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 2);
        transport.stop(); transport.stop();

        // ---- track buttons
        for (auto& t : p.tracks) { t.queuedSlot = -2; t.playingSlot = -1; }
        p.tracks[0].playingSlot = 0;
        tap(apc, mk2Controls, ApcKey25::kTrackButton);
        check(apc.buttons() == ApcKey25::Buttons::ClipStop && (p.tracks[0].queuedSlot == -1 || p.tracks[0].playingSlot == -1),
              "APC track buttons: Clip Stop to begin with");
        withShift(apc, mk2Controls, ApcKey25::kSceneButton + 1);
        tap(apc, mk2Controls, ApcKey25::kTrackButton + 2);
        check(apc.buttons() == ApcKey25::Buttons::Solo && p.tracks[2].solo, "APC track buttons: Shift + the second scene button: Solo");
        withShift(apc, mk2Controls, ApcKey25::kSceneButton + 2);
        tap(apc, mk2Controls, ApcKey25::kTrackButton + 3);
        check(apc.buttons() == ApcKey25::Buttons::Mute && !p.tracks[3].active, "APC track buttons (mk2): the third scene button: Mute");
        withShift(apc, mk2Controls, ApcKey25::kSceneButton + 3);
        tap(apc, mk2Controls, ApcKey25::kTrackButton + 4);
        check(apc.buttons() == ApcKey25::Buttons::Arm && p.tracks[4].armed, "APC track buttons (mk2): the fourth: Rec Arm");
        QSignalSpy picked(&apc, &ApcKey25::trackChosen);
        withShift(apc, mk2Controls, ApcKey25::kSceneButton + 4);
        tap(apc, mk2Controls, ApcKey25::kTrackButton + 5);
        check(apc.buttons() == ApcKey25::Buttons::Select && picked.size() == 1 && picked[0][0].toInt() == 5, "APC track buttons: the fifth: Select");
        {
            Project q; fill(q, 2);
            Transport tq(&q, &engine); Mapper mq(&q, &tq); ApcKey25 old(&q, &tq, &mq);
            old.setPorts({{"28:0", classic}});
            withShift(old, classic, ApcKey25::kSceneButton + 2);
            const bool arm = old.buttons() == ApcKey25::Buttons::Arm;
            withShift(old, classic, ApcKey25::kSceneButton + 3);
            check(arm && old.buttons() == ApcKey25::Buttons::Mute, "APC track buttons (2014): Rec Arm third, Mute fourth, as printed");
        }
        p.tracks[2].solo = false; p.tracks[3].active = true; p.tracks[4].armed = false;
        withShift(apc, mk2Controls, ApcKey25::kSceneButton);

        // ---- knobs
        check(apc.knobs() == ApcKey25::Knobs::Device, "APC knobs: Device to begin with");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 4);
        check(apc.knobs() == ApcKey25::Knobs::Volume && apc.knobTargets()[0] == QString("track:%1:volume").arg(p.tracks[0].id),
              "APC knobs: Shift + the fifth track button: Volume");
        p.tracks[0].volume = 0.5;
        for (int i = 0; i < 4; ++i) apc.controlChange(mk2Controls, 0, 48, 1);
        const double up = p.tracks[0].volume;
        apc.controlChange(mk2Controls, 0, 48, 126);
        check(near(up, 0.5 + 4 / 127.0) && near(p.tracks[0].volume, 0.5 + 2 / 127.0), "APC mk2 knobs: endless, a step a message, either way");
        press(apc, mk2Controls, ApcKey25::kShift);
        apc.controlChange(mk2Controls, 0, 48, 4);
        press(apc, mk2Controls, ApcKey25::kShift, false);
        check(near(p.tracks[0].volume, 0.5 + 3 / 127.0), "APC mk2 knobs: Shift turns finely");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 5);
        p.tracks[1].pan = 0;
        apc.controlChange(mk2Controls, 0, 49, 127);
        check(apc.knobs() == ApcKey25::Knobs::Pan && near(p.tracks[1].pan, -2 / 127.0), "APC knobs: Pan");
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 6);
        check(apc.knobs() == ApcKey25::Knobs::Send && apc.knobTargets()[0].isEmpty(), "APC knobs: Send (no returns: nothing to turn)");
        p.tracks[0].devices = {p.makeDevice("Osmium", DeviceKind::Instrument)};
        p.selectedTrack = 0;
        withShift(apc, mk2Controls, ApcKey25::kTrackButton + 7);
        const Device& osmium = p.tracks[0].devices[0];
        const int macro1 = paramNamed(osmium, "Macro 1"), macro8 = paramNamed(osmium, "Macro 8");
        check(apc.knobs() == ApcKey25::Knobs::Device && macro1 >= 0 && macro8 >= 0
                  && apc.knobTargets()[0] == QString("device:%1:%2:%3").arg(p.tracks[0].id).arg(osmium.id).arg(macro1)
                  && apc.knobTargets()[7].endsWith(QString(":%1").arg(macro8)), "APC knobs: Device turns Osmium's Macro 1–8");
        const double m0 = p.tracks[0].devices[0].params[macro1].value;
        for (int i = 0; i < 5; ++i) apc.controlChange(mk2Controls, 0, 48, 1);
        check(near(p.tracks[0].devices[0].params[macro1].value, std::min(1.0, m0 + 5 / 127.0)), "APC knobs: an Osmium macro steps smoothly, no jump");
        p.tracks[0].devices.push_back(p.makeDevice("Auto Filter", DeviceKind::AudioEffect));
        const int filterId = p.tracks[0].devices.last().id;
        apc.setDeviceChoice([filterId] { return filterId; });
        check(apc.knobTargets()[0] == QString("device:%1:%2:0").arg(p.tracks[0].id).arg(filterId), "APC knobs: a device without macros: its first eight parameters");
        apc.setDeviceChoice({});
        {   // the 2014 model: absolute knobs, with pickup
            Project q; fill(q, 2);
            Transport tq(&q, &engine); Mapper mq(&q, &tq); ApcKey25 old(&q, &tq, &mq);
            old.setPorts({{"28:0", classic}});
            withShift(old, classic, ApcKey25::kTrackButton + 4);
            q.tracks[0].volume = 0.5;
            old.controlChange(classic, 0, 48, 10);
            const bool held = near(q.tracks[0].volume, 0.5);
            old.controlChange(classic, 0, 48, 70);
            check(held && near(q.tracks[0].volume, 70 / 127.0), "APC (2014) knobs: absolute; with Pickup they take over at the value");
        }

        // ---- lights
        for (auto& t : p.tracks) { t.queuedSlot = -2; t.playingSlot = -1; }
        p.tracks[0].clipSlots[0]->color = QColor(255, 10, 10);
        p.tracks[1].playingSlot = 0;
        p.tracks[2].queuedSlot = 0;
        p.tracks[5].armed = true;
        const auto lit = apc.lights();
        check(lit.value(32) == qMakePair(6, 5), "APC lights (mk2): a stopped clip shows its colour at full brightness");
        check(lit.value(33).second == 21 && (lit.value(33).first == 6 || lit.value(33).first == 3), "APC lights (mk2): a playing clip pulses green");
        check(lit.value(34).second == 21 || lit.value(34).second == 0, "APC lights (mk2): a queued clip blinks green");
        check(lit.value(0) == qMakePair(0, 0) && lit.value(5) == qMakePair(1, 5), "APC lights (mk2): empty pads are dark, an armed track's glow dim red");
        check(lit.value(ApcKey25::kSceneButton).second == 1 && lit.value(ApcKey25::kSceneButton + 4).second == 0,
              "APC lights: scene buttons light for scenes with clips");
        check(lit.value(ApcKey25::kTrackButton + 1).second == 1 && lit.value(ApcKey25::kTrackButton + 3).second == 0,
              "APC lights: in Clip Stop the track buttons show the playing tracks");
        press(apc, mk2Controls, ApcKey25::kShift);
        const auto shifted = apc.lights();
        press(apc, mk2Controls, ApcKey25::kShift, false);
        check(shifted.value(ApcKey25::kTrackButton + 3).second == 1 && shifted.value(ApcKey25::kTrackButton + 2).second == 0
                  && shifted.value(ApcKey25::kTrackButton + 7).second == 1 && shifted.value(ApcKey25::kTrackButton + 4).second == 0
                  && shifted.value(ApcKey25::kSceneButton).second == 1, "APC lights: with Shift held, the box's directions and the modes");
        for (auto& t : p.tracks) { t.queuedSlot = -2; t.playingSlot = -1; t.armed = false; }   // nothing blinking or pulsing
        apc.refreshLights(true);
        sent.clear();
        apc.refreshLights();
        const int unchanged = int(sent.size());
        p.tracks[7].clipSlots[0].reset();   // the box's last column
        apc.refreshLights();
        check(unchanged == 0 && sent.size() == 1 && uchar(sent[0].second[1]) == 39 && uchar(sent[0].second[2]) == 0, "APC lights: only what changed is sent");
        sent.clear();
        apc.darken();
        bool dark = !sent.isEmpty();
        for (const auto& m : sent) dark &= m.second.size() == 3 && uchar(m.second[2]) == 0;
        check(dark && apc.lights().size() == 55, "APC lights: leaving darkens them");
        apc.setEnabled(false);
        check(!apc.connected() && apc.role(mk2Controls) == ApcKey25::Role::None && !apc.takesNote(mk2Controls, 0, 32), "APC: profile Off leaves the controller as a keyboard");
        {
            Project q; fill(q, 2);
            Transport tq(&q, &engine); Mapper mq(&q, &tq); ApcKey25 old(&q, &tq, &mq);
            old.setPorts({{"28:0", classic}});
            q.tracks[1].playingSlot = 0;
            const auto l = old.lights();
            check(l.value(32).second == 5 && l.value(33).second == 1 && l.value(34).second == 0, "APC lights (2014): amber clips, green playing, dark when empty");
        }
    }

    // ---- MIDI Map: relative knobs are learned, and stepped
    {
        check(Mapper::looksRelative({1, 1}) && Mapper::looksRelative({127, 127, 126}) && Mapper::looksRelative({1, 127})
                  && !Mapper::looksRelative({1}) && !Mapper::looksRelative({100}) && !Mapper::looksRelative({0, 1, 2, 3})
                  && !Mapper::looksRelative({5, 6, 7}) && !Mapper::looksRelative({120, 121, 122}), "MIDI Map: relative knobs are told from absolute ones");
        Project p; fill(p, 3);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        Mapper mapper(&p, &transport);
        const QString vol0 = QString("track:%1:volume").arg(p.tracks[0].id), vol1 = QString("track:%1:volume").arg(p.tracks[1].id),
                      vol2 = QString("track:%1:volume").arg(p.tracks[2].id);
        mapper.setMode(Mapper::Mode::Midi);
        mapper.select(vol0);
        mapper.controlChange(0, 48, 1, true);
        mapper.select(vol1);
        mapper.controlChange(0, 21, 1); mapper.controlChange(0, 21, 1);
        mapper.select(vol2);
        mapper.controlChange(0, 22, 100); mapper.controlChange(0, 22, 101);
        mapper.setMode(Mapper::Mode::Off);
        auto mapFor = [&](const QString& t) { for (const Mapping& m : p.mappings) if (m.target == t) return m; return Mapping{}; };
        check(mapFor(vol0).relative && mapFor(vol1).relative && !mapFor(vol2).relative && mapFor(vol2).number == 22,
              "MIDI Map: a knob that sends steps is learned as relative; one that sends positions stays absolute");
        p.tracks[0].volume = 0.5; p.tracks[1].volume = 0.25;
        for (int i = 0; i < 3; ++i) mapper.controlChange(0, 48, 1);
        mapper.controlChange(0, 21, 125);
        mapper.controlChange(0, 22, 0);
        const double low = p.tracks[2].volume;
        mapper.controlChange(0, 22, 127);
        check(near(p.tracks[0].volume, 0.5 + 3 / 127.0) && near(p.tracks[1].volume, 0.25 - 3 / 127.0) && low == 0 && p.tracks[2].volume == 1.0,
              "MIDI Map: relative maps step the value; absolute ones still set it");
        p.mappings.push_back({Mapping::MidiCC, 0, 50, QString("track:%1:pan").arg(p.tracks[2].id)});   // learned as absolute (before relative maps)
        p.tracks[2].pan = 0;
        mapper.controlChange(0, 50, 1, true);
        check(near(p.tracks[2].pan, 2 / 127.0 - 0), "MIDI Map: an APC mk2 knob mapped as absolute steps too (no jump to the end)");
        p.tracks[2].pan = 0;
        mapper.controlChange(0, 50, 127, true);
        check(near(p.tracks[2].pan, -2 / 127.0), "MIDI Map: ...and turns down from where it is");
        setfile::Extras extras;
        extras.mappings = p.mappings;
        EditState state = p.captureEditState();
        const QByteArray json = setfile::toJson(state, extras);
        EditState back; setfile::Extras backExtras;
        const bool read = setfile::fromJson(json, back, backExtras);
        int relatives = 0;
        for (const Mapping& m : backExtras.mappings) relatives += m.relative;
        check(read && backExtras.mappings.size() == p.mappings.size() && relatives == 2 && json.count("\"relative\"") == 2, "MIDI Map: relative maps are saved with the set");
        p.tempo = 120;
        p.mappings.push_back({Mapping::MidiCC, 0, 51, "tempo", true});
        mapper.controlChange(0, 51, 2);
        check(near(p.tempo, 122), "MIDI Map: a relative knob on the tempo steps a BPM at a time");
    }

    // ---- the window: APC pads launch, keys play and light the piano roll, the box shows
    settings();
    {
        QVector<QPair<QString, QByteArray>> sent;   // before the window: it darkens the pads as it closes
        MainWindow window;
        window.resize(1300, 800);
        window.show();
        QApplication::processEvents();
        window.engine()->shutdown();
        Project* p = window.project();
        auto* input = window.midiInput();
        auto* apc = window.apcKey25();
        p->mappings.clear();
        while (p->tracks.size() < 10) p->addTrack(QString("Extra %1").arg(p->tracks.size() + 1), QColor("#8cb6cf"));
        for (auto& t : p->tracks) if (!t.isGroup() && t.clipSlots.size() && !t.clipSlots[0]) t.clipSlots[0] = p->newClip("Loop", 4);
        emit p->structureChanged();
        QObject::connect(input, &audio::MidiInput::sent, &window, [&](const QString& port, const QByteArray& m) { sent.push_back({port, m}); });
        apc->setPorts(mk2Ports());
        QApplication::processEvents();
        check(apc->connected() && sent.size() >= 55 && sent.first().first == mk2Controls, "window: an APC Key 25 mk2 lights up through MIDI Out");
        ClipGrid* grid = window.sessionView()->grid();
        const QRect box = grid->controllerBoxRect();
        check(!box.isEmpty() && recordPixels(grid, grid->width()) > 0, "window: the Session View outlines the pads' box");
        int midiTrack = -1;
        for (int i = 0; i < p->tracks.size(); ++i) if (p->tracks[i].type == TrackType::Midi && midiTrack < 0) midiTrack = i;
        p->selectedTrack = midiTrack;
        PianoRoll* roll = window.clipView()->pianoRoll();
        const int col = apc->visibleTracks().indexOf(midiTrack);
        for (auto& t : p->tracks) t.queuedSlot = -2;
        if (col >= 0 && col < 8) input->inject(0x90, 32 + col, 127, mk2Controls);
        check(col >= 0 && col < 8 && (p->tracks[midiTrack].queuedSlot == 0 || p->tracks[midiTrack].playingSlot == 0) && roll->litPitches().isEmpty(),
              "window: an APC pad launches its clip and plays no note");
        if (col >= 0) input->inject(0x80, 32 + col, 0, mk2Controls);
        window.transport()->stop(); window.transport()->stop();
        input->inject(0x90, 60, 100, mk2Keys);
        input->inject(0x90, 64, 90, mk2Keys);
        const QSet<int> held = roll->litPitches();
        input->inject(0x80, 60, 0, mk2Keys);
        check(held == QSet<int>({60, 64}) && roll->litPitches() == QSet<int>({64}), "window: the APC's keys play and light their keys in the piano roll");
        input->inject(0x80, 64, 0, mk2Keys);
        const QRect before = grid->controllerBoxRect();
        input->inject(0x90, ApcKey25::kShift, 127, mk2Controls);
        input->inject(0x90, ApcKey25::kTrackButton + 3, 127, mk2Controls);
        input->inject(0x80, ApcKey25::kTrackButton + 3, 0, mk2Controls);
        input->inject(0x80, ApcKey25::kShift, 0, mk2Controls);
        QApplication::processEvents();
        check(apc->boxTrack() == 1 && grid->controllerBoxRect().left() > before.left(), "window: Shift + → moves the outline with the box");
        capture(window.sessionView(), "apc-session-box.png");

        // The Osmium case: a mapped mk2 knob turns an Osmium macro smoothly.
        const int t = p->insertTrack(p->tracks.size());
        p->tracks[t].devices = {p->makeDevice("Osmium", DeviceKind::Instrument)};
        const Device& osmium = p->tracks[t].devices[0];
        const int macro = paramNamed(osmium, "Macro 2");
        const QString target = QString("device:%1:%2:%3").arg(p->tracks[t].id).arg(osmium.id).arg(macro);
        window.setMapMode(Mapper::Mode::Midi);
        window.mapper()->select(target);
        input->inject(0xB0, 49, 1, mk2Controls);
        window.setMapMode(Mapper::Mode::Off);
        p->tracks[t].devices[0].params[macro].value = 0.4;
        QVector<double> seen;
        for (int i = 0; i < 6; ++i) { input->inject(0xB0, 49, i < 4 ? 1 : 127, mk2Controls); seen.push_back(p->tracks[t].devices[0].params[macro].value); }
        bool smooth = true;
        for (int i = 1; i < seen.size(); ++i) smooth &= std::abs(seen[i] - seen[i - 1]) < 1.5 / 127.0;
        check(!p->mappings.isEmpty() && p->mappings.last().relative && smooth && near(seen.last(), 0.4 + 2 / 127.0),
              "window: an APC mk2 knob mapped to an Osmium macro moves it in small steps (no jumping)");
        p->mappings.clear();

        // Dragging an Osmium dial while notes play (and the song runs) moves it steadily.
        p->selectedTrack = t;
        DeviceView devices(p, window.transport());
        devices.resize(1240, 720); devices.show(); devices.rebuild(); QApplication::processEvents();
        auto* openEditor = devices.findChild<QPushButton*>("osmiumOpenEditor");
        if (openEditor) openEditor->click();
        QApplication::processEvents();
        auto* editor = devices.findChild<devui::DeviceBody*>("osmiumEditor");
        const int cutoff = monolith::fparam(0, monolith::FCutoff);
        QWidget* dial = editor ? editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(cutoff)) : nullptr;
        bool steady = dial != nullptr;
        if (dial) {
            p->tracks[t].devices[0].params[cutoff].value = 0.3;
            window.transport()->play();
            const QPoint c(dial->width() / 2, 16 + dial->property("dialDiameter").toInt() / 2);
            QTest::mousePress(dial, Qt::LeftButton, Qt::NoModifier, c);
            double last = p->tracks[t].devices[0].params[cutoff].value;
            for (int i = 1; i <= 10; ++i) {
                input->inject(0x90, 48 + i, 100, mk2Keys);
                const QPoint at = c - QPoint(0, 3 * i);
                QMouseEvent move(QEvent::MouseMove, at, dial->mapToGlobal(at), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(dial, &move);
                QTest::qWait(40);   // the editor's 33 ms refresh runs between moves
                input->inject(0x80, 48 + i, 0, mk2Keys);
                const double now = p->tracks[t].devices[0].params[cutoff].value;
                steady &= std::abs((now - last) - 3 / 150.0) < 1e-6;
                last = now;
            }
            QTest::mouseRelease(dial, Qt::LeftButton, Qt::NoModifier, c - QPoint(0, 30));
            window.transport()->stop(); window.transport()->stop();
        }
        check(steady, "Osmium: dragging a dial while notes play moves it in even steps");

        // The computer keyboard and the sustain pedal light keys too, only while they're held.
        QWidget* focus = window.sessionView()->grid();
        focus->setFocus();
        if (!window.midiKeyboard()) QTest::keyClick(focus, Qt::Key_M);
        QKeyEvent pressA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "a");
        QApplication::sendEvent(focus, &pressA);
        const bool keyLit = !roll->litPitches().isEmpty();
        QKeyEvent releaseA(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier, "a");
        QApplication::sendEvent(focus, &releaseA);
        check(window.midiKeyboard() && keyLit && roll->litPitches().isEmpty(), "piano roll: a computer-keyboard key lights while held");
        QTest::keyClick(focus, Qt::Key_M);
        input->inject(0xB0, 64, 127, mk2Keys);
        input->inject(0x90, 62, 100, mk2Keys);
        const bool down = roll->litPitches().contains(62);
        input->inject(0x80, 62, 0, mk2Keys);
        const bool upWhileSustained = !roll->litPitches().contains(62);
        input->inject(0xB0, 64, 0, mk2Keys);
        check(down && upWhileSustained, "piano roll: a key goes dark when it's let go, even with the pedal down");
        ClipPtr clip;
        for (const auto& tr : p->tracks) if (tr.type == TrackType::Midi && tr.clipSlots.size() && tr.clipSlots[0]) { clip = tr.clipSlots[0]; break; }
        window.clipView()->setClip(clip);
        roll->resize(700, 400);
        roll->setHeldPitches({});
        const int none = recordPixels(roll, 33);
        QSet<int> all;
        for (int k = 0; k < 128; ++k) all.insert(k);
        roll->setHeldPitches(all);
        const int some = recordPixels(roll, 33);
        roll->setHeldPitches({36, 40, 43});   // C1 E1 G1: in view for the capture
        capture(roll, "piano-roll-held-keys.png");
        roll->setHeldPitches({});
        check(none == 0 && some > 0, "piano roll: held keys are painted in the record red");
    }

    // ---- the browser preview stops
    {
        Project p; p.scenes = {"1"}; p.addTrack("A", QColor("#8cb6cf"));
        audio::Engine engine(&p);
        auto data = std::make_shared<SampleData>();
        data->rate = engine.sampleRate();
        data->left.assign(4000, 0.5f); data->right.assign(4000, 0.5f);
        engine.publish();
        engine.previewSample(data);
        std::vector<float> out(512 * 2);
        const bool pending = engine.previewPlaying();
        engine.process(out.data(), 512);
        const bool sounding = engine.previewPlaying() && std::abs(out[1000]) > 0.01f;
        for (int i = 0; i < 12; ++i) engine.process(out.data(), 512);
        check(pending && sounding && !engine.previewPlaying(), "preview: the engine says while a preview plays, and when it ended");
        engine.previewSample(data);
        engine.process(out.data(), 512);
        engine.stopPreview();
        engine.process(out.data(), 512);
        float peak = 0;
        for (float v : out) peak = std::max(peak, std::abs(v));
        check(!engine.previewPlaying() && peak < 1e-6f, "preview: Stop silences it at once");

        Browser browser;
        browser.resize(420, 520);
        browser.show();
        QApplication::processEvents();
        bool playing = true;
        browser.setPreviewProbe([&] { return playing; });
        QSignalSpy stops(&browser, &Browser::previewStopRequested);
        auto* stop = browser.findChild<QToolButton*>("browserPreviewStop");
        auto* preview = browser.findChild<QToolButton*>("browserPreview");
        check(stop && preview && !stop->isVisible(), "browser: no Stop while nothing previews");
        browser.previewStarted("/tmp/Loop.wav");
        QApplication::processEvents();
        check(stop->isVisible() && browser.previewing() == "/tmp/Loop.wav", "browser: Stop shows while a sample previews");
        capture(&browser, "browser-preview-stop.png");
        QTest::mouseClick(stop, Qt::LeftButton);
        check(stops.size() == 1 && !stop->isVisible() && browser.previewing().isEmpty(), "browser: Stop stops it");
        browser.previewStarted("/tmp/Loop.wav");
        preview->setChecked(true);
        QTest::mouseClick(preview, Qt::LeftButton);
        check(!preview->isChecked() && stops.size() == 2 && browser.previewing().isEmpty(), "browser: turning Preview off stops it");
        QTest::mouseClick(preview, Qt::LeftButton);
        browser.previewStarted("/tmp/Loop.wav");
        auto* items = browser.findChild<QTreeWidget*>("browserItems");
        QTest::keyClick(items, Qt::Key_Escape);
        check(stops.size() == 3 && browser.previewing().isEmpty(), "browser: Esc in the list stops it");
        browser.previewStarted("/tmp/Loop.wav");
        playing = false;
        QTest::qWait(250);
        check(stops.size() == 3 && !stop->isVisible() && browser.previewing().isEmpty(), "browser: Stop goes away by itself when the sample has played out");

        // Through the window: a preview really stops, also a file still loading.
        MainWindow window;
        window.show();
        QApplication::processEvents();
        window.engine()->shutdown();
        window.engine()->publish();
        window.engine()->previewSample(data);
        window.browser()->previewStarted("/tmp/Loop.wav");
        window.engine()->process(out.data(), 64);
        const bool on = window.engine()->previewPlaying();
        window.browser()->stopPreview();
        check(on && !window.engine()->previewPlaying(), "window: the browser's Stop stops the engine's preview");
    }

    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
