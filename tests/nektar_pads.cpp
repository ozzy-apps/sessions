#include "audio/Engine.h"
#include "audio/MidiInput.h"
#include "core/ControlSurface.h"
#include "core/Mapping.h"
#include "core/Model.h"
#include "core/NektarPads.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/MainWindow.h"
#include "ui/Preferences.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
const QString keys = "Impact LX49+: Impact LX49+ MIDI1", controls = "Impact LX49+: Impact LX49+ MIDI2";
const QVector<NektarPads::Port> ports{
    {"32:0", keys, true}, {"32:1", controls, true}, {"28:0", "APC Key 25 MIDI 1", true}};
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", text);
    }
}
void reset() {
    QSettings s("Sessions", "Sessions");
    s.clear();
    s.setValue("midi/controllerPickup", false);
}
void project(Project& p) {
    for (int i = 0; i < 16; ++i)
        p.scenes.push_back(QString::number(i + 1));
    for (int i = 0; i < 3; ++i)
        p.addTrack(QString("Track %1").arg(i + 1), QColor("#8cb6cf"));
    p.tracks[0].clipSlots[0] = p.newClip("First", 4);
    p.tracks[0].clipSlots[8] = p.newClip("Ninth", 4);
    p.tracks[1].clipSlots[1] = p.newClip("Second track", 4);
}
bool checksum(const QByteArray& data) {
    if (data.size() < 8 || !data.startsWith(QByteArray::fromHex("f00001777f")) || uchar(data.back()) != 0xf7)
        return false;
    int sum = 0;
    for (int i = 5; i < data.size() - 1; ++i) {
        if (uchar(data[i]) > 127)
            return false;
        sum += uchar(data[i]);
    }
    return (sum & 127) == 0;
}
} // namespace
int main(int argc, char** argv) {
    QTemporaryDir temporary;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    {
        std::array<int, 8> colors;
        colors.fill(5);
        check(NektarPads::usbModeMessage(2) == QByteArray::fromHex("f00001777f010500010f010267f7"),
              "native USB setup packet matches protocol fixture");
        check(NektarPads::padModeMessage(true) == QByteArray::fromHex("f00001777f010904010001036df7"),
              "launcher pad mode packet matches fixture");
        check(NektarPads::padModeMessage(false) == QByteArray::fromHex("f00001777f010904010001026ef7"),
              "internal pad mode packet matches fixture");
        check(NektarPads::paletteMessage(colors) ==
                  QByteArray::fromHex(
                      "f00001777f010600000101050002010500030105000401050005010500060105000701050008010525f7"),
              "eight-pad yellow/green palette fixture and checksum");
        check(checksum(NektarPads::paletteMessage(colors)), "palette checksum covers all pad entries");
        check(NektarPads::padNote(0) == 60 && NektarPads::padNote(7) == 72 && NektarPads::padNote(8) == -1,
              "native pad note table and bounds");
    }
    reset();
    {
        Project p;
        project(p);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        QVector<QPair<QString, QByteArray>> sent;
        NektarPads pads(&p, &transport);
        pads.setSender([&](const QString& port, const QByteArray& data) {
            sent.push_back({port, data});
            return true;
        });
        pads.setPorts(ports);
        check(pads.connected() && pads.inputPort() == controls && pads.outputPort() == keys,
              "pair DAW input MIDI2 with output MIDI1");
        bool scoped = true, valid = true, presets = false;
        for (const auto& packet : sent) {
            scoped &= packet.first == keys;
            if (uchar(packet.second[0]) == 0xf0) {
                valid &= checksum(packet.second);
                presets |= uchar(packet.second[6]) == 1;
            }
        }
        check(scoped && valid && !presets, "initialization is output-scoped and never writes user presets");
        check(!pads.takesNote(keys, 15, 60) && !pads.takesNote("APC Key 25 MIDI 1", 15, 60),
              "keyboard and APC notes remain outside Nektar pad input");
        check(!pads.controlChange(keys, 15, 117, 127) && !pads.controlChange(controls, 0, 117, 127),
              "mode controls are source and channel scoped");
        pads.controlChange(controls, 15, 117, 1);
        pads.controlChange(controls, 15, 117, 0);
        check(pads.mode() == NektarPads::Mode::Clips, "Clips accepts firmware 1/0 button messages");
        auto lights = pads.lights(0);
        check(lights[0].palette == 5 && !lights[0].on && lights[1].palette == 3 && !lights[1].on,
              "loaded yellow and empty off");
        check(sent.contains({keys, QByteArray::fromHex("bf757f")}) &&
                  sent.contains({keys, QByteArray::fromHex("bf7600")}),
              "Clips and Scenes indicators follow pad mode");
        transport.continuePlay();
        p.tracks[0].playingSlot = 0;
        check(pads.lights(0)[0].on, "playing clip lights green");
        p.tracks[0].queuedSlot = 0;
        check(!pads.lights(0)[0].on && pads.lights(180)[0].on, "queued clip blinks between loaded and playing colours");
        p.tracks[0].queuedSlot = -2;
        p.tracks[0].playingSlot = -1;
        pads.note(controls, 15, 60, 100, true);
        check(p.tracks[0].queuedSlot == 0, "pad launches selected track clip");
        pads.note(controls, 15, 60, 0, false);
        pads.controlChange(controls, 15, 117, 127);
        pads.note(controls, 15, 62, 100, true);
        pads.note(controls, 15, 62, 0, false);
        pads.controlChange(controls, 15, 117, 0);
        check(pads.mode() == NektarPads::Mode::Clips && pads.bank() == 1,
              "hold Clips and pad selects bank without leaving launcher");
        pads.note(controls, 15, 60, 100, true);
        pads.note(controls, 15, 60, 0, false);
        check(p.tracks[0].queuedSlot == 8, "clip pad bank addresses scenes 9-16");
        int chosen = -1;
        QObject::connect(&pads, &NektarPads::clipChosen, &pads, [&](int, int scene) { chosen = scene; });
        pads.controlChange(controls, 15, 108, 1);
        pads.note(controls, 15, 64, 100, true);
        pads.note(controls, 15, 64, 0, false);
        pads.controlChange(controls, 15, 108, 0);
        check(chosen == 10 && p.tracks[0].queuedSlot == 8, "Shift-pad selects without launching");
        pads.controlChange(controls, 15, 110, 1);
        pads.controlChange(controls, 15, 110, 1);
        pads.controlChange(controls, 15, 110, 0);
        check(p.selectedTrack == 1, "Track+ follows selection and ignores repeated presses");
        pads.controlChange(controls, 15, 109, 127);
        pads.controlChange(controls, 15, 109, 0);
        check(p.selectedTrack == 0, "Track- returns selected track");
        pads.controlChange(controls, 15, 118, 127);
        pads.controlChange(controls, 15, 118, 0);
        check(pads.mode() == NektarPads::Mode::Scenes && pads.bank() == 0, "Scenes has independent bank");
        check(pads.lights(0)[0].palette == 5 && pads.lights(0)[1].palette == 5,
              "scene lights aggregate content across tracks");
        pads.note(controls, 15, 62, 100, true);
        pads.note(controls, 15, 62, 0, false);
        check(p.tracks[1].queuedSlot == 1, "scene pad launches across tracks");
        pads.controlChange(controls, 15, 118, 127);
        pads.controlChange(controls, 15, 118, 0);
        check(pads.mode() == NektarPads::Mode::Notes, "press current mode again restores performance pads");
        pads.setMode(NektarPads::Mode::Clips);
        pads.controlChange(controls, 15, 117, 1);
        pads.controlChange(controls, 15, 118, 1);
        pads.controlChange(controls, 15, 117, 0);
        pads.controlChange(controls, 15, 118, 0);
        check(pads.mode() == NektarPads::Mode::Scenes, "overlapping mode-button releases preserve the newest mode");
        sent.clear();
        pads.shutdown();
        check(sent.contains({keys, NektarPads::usbModeMessage(0)}) &&
                  sent.contains({keys, NektarPads::padModeMessage(false)}),
              "shutdown restores USB setup and normal pads");
        check(!pads.connected(), "shutdown disables further pad actions");
    }
    reset();
    {
        Project p;
        project(p);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        NektarPads pads(&p, &transport);
        QVector<QByteArray> sent;
        bool failPalette = false;
        pads.setSender([&](const QString&, const QByteArray& data) {
            sent.push_back(data);
            return !(failPalette && data.size() > 30);
        });
        pads.setPorts(ports);
        failPalette = true;
        pads.setMode(NektarPads::Mode::Clips);
        const int failed = sent.size();
        pads.refreshLights(false);
        check(sent.size() == failed, "failed output waits before retry");
        failPalette = false;
        pads.refreshLights(false, QDateTime::currentMSecsSinceEpoch() + 1100);
        check(sent.size() > failed + 8, "failed palette is resent with the whole pad state");
        sent.clear();
        pads.refreshLights(false, QDateTime::currentMSecsSinceEpoch() + 1101);
        check(sent.isEmpty(), "unchanged feedback is suppressed after successful delivery");
        auto disabled = ports;
        disabled[1].enabled = false;
        pads.setPorts(disabled);
        check(!pads.connected(), "disabled DAW input disables native feedback");
        pads.setPorts(ports);
        check(pads.connected(), "reconnection restores native profile");
        const QString otherKeys = "Impact LX61+: MIDI1", otherControls = "Impact LX61+: MIDI2";
        auto two = ports;
        two.push_back({"40:0", otherKeys, true});
        two.push_back({"40:1", otherControls, true});
        QSettings("Sessions", "Sessions").setValue("midi/controllerInput", otherKeys);
        pads.setPorts(two);
        pads.reloadSettings();
        check(pads.inputPort() == otherControls && pads.outputPort() == otherKeys,
              "explicit input selects its own paired controller");
        QSettings("Sessions", "Sessions").setValue("midi/controllerInput", "Unknown Controller");
        pads.reloadSettings();
        check(!pads.connected(), "unmatched explicit input never falls back to another device");
        QSettings("Sessions", "Sessions").setValue("midi/controller", "off");
        pads.reloadSettings();
        check(!pads.connected(), "global Profile Off restores and disables native pads");
        pads.shutdown();
    }
    reset();
    {
        Project p;
        project(p);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        NektarPads pads(&p, &transport);
        pads.setSender([](const QString&, const QByteArray&) { return true; });
        pads.setPorts(ports);
        pads.setMode(NektarPads::Mode::Clips);
        p.tracks[0].clipSlots[0]->launchMode = LaunchMode::Gate;
        pads.note(controls, 15, 60, 100, true);
        const int id = p.tracks[0].id;
        p.moveTrack(0, 1);
        p.selectedTrack = 0;
        pads.note(controls, 15, 60, 0, false);
        check(p.tracks[p.indexOfTrack(id)].queuedSlot == -1,
              "Gate release follows frozen source identity after track move");
        p.selectedTrack = p.indexOfTrack(id);
        p.tracks[p.selectedTrack].armed = true;
        pads.note(controls, 15, 62, 100, true);
        pads.note(controls, 15, 62, 0, false);
        check(transport.isRecordingSlot(p.selectedTrack, 1), "empty armed slot starts recording");
        check(pads.lights(0)[1].palette == 8 && pads.lights(0)[1].on, "recording slot lights red");
        transport.stop();
        pads.shutdown();
    }
    reset();
    {
        Project p;
        project(p);
        audio::Engine engine(&p);
        Transport transport(&p, &engine);
        p.tracks[2].type = TrackType::Group;
        p.tracks[0].groupId = p.tracks[1].groupId = p.tracks[2].id;
        p.tracks[0].clipSlots[0]->launchMode = LaunchMode::Gate;
        p.selectedTrack = 2;
        NektarPads pads(&p, &transport);
        pads.setSender([](const QString&, const QByteArray&) { return true; });
        pads.setPorts(ports);
        pads.setMode(NektarPads::Mode::Clips);
        check(pads.lights(0)[0].palette == 5 && pads.lights(0)[1].palette == 5,
              "group feedback aggregates member slots");
        pads.note(controls, 15, 60, 100, true);
        check(p.tracks[0].queuedSlot == 0 && p.tracks[2].queuedSlot == -2, "group pad launches members");
        pads.note(controls, 15, 60, 0, false);
        check(p.tracks[0].queuedSlot == -1, "group Gate release reaches its held members");
        pads.shutdown();
    }
    reset();
    {
        MainWindow window;
        window.engine()->shutdown();
        auto* pads = window.controlSurface()->pads();
        pads->setSender([](const QString&, const QByteArray&) { return true; });
        pads->setPorts(ports);
        window.project()->mappings.clear();
        window.midiInput()->inject(0xbf, 117, 127, controls);
        window.midiInput()->inject(0xbf, 117, 0, controls);
        check(pads->mode() == NektarPads::Mode::Clips, "MainWindow routes dedicated mode controls");
        const bool loop = window.transport()->loopEnabled();
        window.project()->mappings.push_back({Mapping::MidiNote, 15, 60, "transport:loop"});
        window.midiInput()->inject(0x9f, 60, 127, controls);
        window.midiInput()->inject(0x9f, 60, 0, controls);
        check(window.transport()->loopEnabled() != loop,
              "custom MIDI note mapping takes priority over Nektar launcher");
        window.project()->mappings.clear();
        window.midiInput()->inject(0x9f, 60, 127, controls);
        check(window.project()->tracks[window.project()->selectedTrack].queuedSlot == 0,
              "MainWindow routes native pads to clip launching");
        window.midiInput()->inject(0x9f, 60, 0, controls);
        PreferencesDialog preferences(window.engine(), window.midiInput(), {}, &window);
        auto* feedback = preferences.findChild<QCheckBox*>("nektarFeedback");
        auto* mode = preferences.findChild<QComboBox*>("nektarPadMode");
        check(feedback && feedback->isChecked() && mode && mode->currentIndex() == 1,
              "Preferences restores native feedback and mode");
        const QString capture = qEnvironmentVariable("SESSIONS_NEKTAR_PADS_CAPTURE");
        if (!capture.isEmpty()) {
            QDir().mkpath(capture);
            auto* sidebar = preferences.findChild<QListWidget*>("prefsSidebar");
            sidebar->setCurrentItem(sidebar->findItems("MIDI", Qt::MatchExactly).first());
            preferences.show();
            QApplication::processEvents();
            preferences.grab().save(capture + "/nektar-pads.png");
        }
        pads->shutdown();
    }
    std::printf("Nektar pads: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
