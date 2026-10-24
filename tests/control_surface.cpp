// Source-scoped control surfaces, MIDI precedence and live Session scene shortcuts.
// All settings and libraries are temporary; MIDI is injected without hardware.
#include "audio/Engine.h"
#include "audio/MidiInput.h"
#include "core/ControlSurface.h"
#include "core/DeviceSpecs.h"
#include <QDir>
#include <QListWidget>
#include "core/Mapping.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/MainWindow.h"
#include "ui/Preferences.h"
#include "ui/SessionView.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
const QString nektar = "Impact LX49+: MIDI1";
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
void settings(const QString& profile = "auto", const QString& input = {}, int bank = 0, bool pickup = false) {
    QSettings s("Sessions", "Sessions");
    s.setValue("midi/controller", profile);
    s.setValue("midi/controllerInput", input);
    s.setValue("midi/controllerBank", bank);
    s.setValue("midi/controllerPickup", pickup);
}
QByteArray mmc(int command) { QByteArray packet = QByteArray::fromHex("f07f7f0600f7"); packet[4] = char(command); return packet; }
void key(QWidget* target, int code, Qt::KeyboardModifiers mods = Qt::NoModifier, bool repeat = false) {
    const QString text = code >= Qt::Key_0 && code <= Qt::Key_9 ? QString(QChar(ushort(code))) : QString();
    QKeyEvent press(QEvent::KeyPress, code, mods, text, repeat);
    QApplication::sendEvent(target, &press);
    QKeyEvent release(QEvent::KeyRelease, code, mods, text, repeat);
    QApplication::sendEvent(target, &release);
}
void activate(QComboBox* combo, int index) {
    combo->setCurrentIndex(index);
    QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
}
}

int main(int argc, char** argv) {
    QTemporaryDir temporary;
    qputenv("XDG_CONFIG_HOME", temporary.path().toUtf8());
    qputenv("XDG_DATA_HOME", temporary.path().toUtf8());
    qputenv("SESSIONS_AUDIO", "none");
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    settings();
    {
        Project project;
        project.scenes = {"1"};
        for (int i = 0; i < 16; ++i) project.addTrack(QString("Track %1").arg(i + 1), QColor("#8cb6cf"));
        audio::Engine engine(&project);
        Transport transport(&project, &engine);
        Mapper mapper(&project, &transport);
        ControlSurface surface(&project, &transport, &mapper);
        check(surface.accepts(nektar) && surface.accepts("Nektar Impact LX61 Plus: MIDI")
                  && surface.accepts("IMPACT LX49+: MIDI 2"), "automatic profile recognizes plus-model Nektar port names");
        check(!surface.accepts("") && !surface.accepts("Other Keyboard") && !surface.accepts("Impact LX49: MIDI")
                  && !surface.accepts("LX88+: MIDI"), "automatic profile leaves unrelated and unsupported keyboards alone");
        bool volumes = true, pans = true, mutes = true, unmutes = true;
        for (int ch = 0; ch < 8; ++ch) {
            volumes &= surface.controlChange(nektar, ch, 7, 20 + ch) && near(project.tracks[ch].volume, (20 + ch) / 127.0);
            pans &= surface.controlChange(nektar, ch, 10, 70 + ch) && near(project.tracks[ch].pan, 2 * (70 + ch) / 127.0 - 1);
            mutes &= surface.controlChange(nektar, ch, 12, 127) && !project.tracks[ch].active;
            unmutes &= surface.controlChange(nektar, ch, 12, 0) && project.tracks[ch].active;
        }
        check(volumes, "GM mixer channels 1-8 control the first eight track volumes");
        check(pans, "GM mixer knobs control bipolar track pan");
        check(mutes && unmutes, "factory alternating mute values mute and unmute all eight tracks");
        check(surface.controlChange(nektar, 15, 7, 43) && near(project.mainVolume, 43 / 127.0), "the ninth fader on channel 16 controls Master volume");
        check(!surface.controlChange(nektar, 0, 1, 70) && !surface.controlChange(nektar, 0, 64, 127)
                  && !surface.controlChange(nektar, 8, 7, 70), "mod wheel, sustain and non-mixer channels remain available to instruments");
        check(!surface.controlChange(nektar, -1, 7, 40) && !surface.controlChange(nektar, 16, 7, 40)
                  && !surface.controlChange(nektar, 0, 7, 128), "malformed mixer messages cannot edit the project");
        const double beforeOther = project.tracks[0].volume;
        check(!surface.controlChange("Other Keyboard", 0, 7, 127) && near(project.tracks[0].volume, beforeOther), "another keyboard cannot move a Nektar mixer fader");
        settings("auto", {}, 8);
        surface.reloadSettings();
        check(surface.controlChange(nektar, 0, 7, 29) && near(project.tracks[8].volume, 29 / 127.0)
                  && near(project.tracks[0].volume, beforeOther), "mixer banking addresses the next eight tracks");
        settings("auto", {}, 56);
        surface.reloadSettings();
        check(surface.controlChange(nektar, 0, 7, 127) && near(project.tracks[0].volume, beforeOther), "an empty bank consumes mixer CCs without editing an unrelated track");

        settings("auto", {}, 0, true);
        surface.reloadSettings();
        project.tracks[0].volume = 0.5;
        check(surface.controlChange(nektar, 0, 7, 20) && near(project.tracks[0].volume, 0.5), "pickup prevents a mismatched physical fader from jumping the software value");
        surface.controlChange(nektar, 0, 7, 90);
        check(near(project.tracks[0].volume, 90 / 127.0), "crossing the current software value acquires pickup");
        surface.controlChange(nektar, 0, 7, 95);
        check(near(project.tracks[0].volume, 95 / 127.0), "an acquired fader continues smoothly after crossing");
        project.tracks[0].volume = 0.2;
        surface.controlChange(nektar, 0, 7, 100);
        check(near(project.tracks[0].volume, 0.2), "a manual software edit re-arms pickup");
        surface.controlChange(nektar, 0, 7, 25);
        check(near(project.tracks[0].volume, 25 / 127.0), "a fader within one MIDI step acquires the new value");

        project.tracks[0].volume = 51 / 127.0;
        project.tracks[1].volume = 0.9;
        surface.reloadSettings();
        surface.controlChange(nektar, 0, 7, 51);
        const int oldId = project.tracks[0].id;
        project.moveTrack(0, 1);
        surface.controlChange(nektar, 0, 7, 52);
        check(project.indexOfTrack(oldId) == 1 && near(project.tracks[0].volume, 0.9)
                  && near(project.tracks[1].volume, 51 / 127.0), "track reordering cannot carry a caught fader into a different track id");
        surface.controlChange(nektar, 0, 7, 114);
        check(near(project.tracks[0].volume, 114 / 127.0), "pickup can acquire the track newly occupying a mixer strip");
        settings("auto", "Custom Controller: Mixer");
        surface.reloadSettings();
        check(surface.accepts("Custom Controller: Mixer") && !surface.accepts(nektar)
                  && !surface.accepts("Custom Controller: Other"), "an explicit input override is exact and source scoped");
        settings("off", "Custom Controller: Mixer");
        surface.reloadSettings();
        check(!surface.accepts("Custom Controller: Mixer") && !surface.machineControl("Custom Controller: Mixer", 2), "Off disables both mixer and transport control");
    }

    {
        settings("auto", {}, 0, false);
        Project project; project.scenes = {"1"};
        for (int i = 0; i < 16; ++i) project.addTrack(QString("Track %1").arg(i + 1), QColor("#8cb6cf"));
        audio::Engine engine(&project); Transport transport(&project, &engine); Mapper mapper(&project, &transport);
        ControlSurface surface(&project, &transport, &mapper);
        auto cc = [&](int number, int value) { return surface.controlChange(nektar, 15, number, value); };
        auto press = [&](int number) { cc(number, 127); cc(number, 0); };
        bool mixer = true;
        for (int i = 0; i < 8; ++i) {
            cc(12 + i, 30 + i); cc(30 + i, 70 + i);
            mixer &= near(project.tracks[i].volume, (30 + i) / 127.0) && near(project.tracks[i].pan, (70 + i) / 127.0 * 2 - 1);
        }
        check(mixer, "dedicated Mixer preset faders and pan knobs work on fixed channel 16");
        cc(20, 51); check(near(project.mainVolume, 51 / 127.0), "dedicated Mixer Master fader");
        cc(21, 127); cc(21, 127);
        check(!project.tracks[0].active, "dedicated mute triggers once on repeated press packets");
        cc(21, 0); cc(21, 127); cc(21, 0);
        check(project.tracks[0].active, "dedicated mute toggles on the next press");
        press(29); check(project.tracks[0].armed, "dedicated ninth button arms selected track");
        press(110); cc(12, 43);
        check(near(project.tracks[8].volume, 43 / 127.0), "Track+ advances Mixer bank by eight");
        press(109); cc(12, 39);
        check(near(project.tracks[0].volume, 39 / 127.0), "Track- returns Mixer bank");
        project.tracks[0].devices.push_back(project.makeDevice("Monolith", DeviceKind::Instrument));
        project.tracks[0].devices.push_back(project.makeDevice("Utility", DeviceKind::AudioEffect));
        surface.setDeviceChoice([&] { return project.tracks[0].devices[1].id; });
        cc(56, 77);
        check(near(project.tracks[0].devices[1].params[0].value, 77 / 127.0), "Instrument knobs follow selected effect");
        surface.setDeviceChoice({}); cc(56, 29);
        check(near(project.tracks[0].devices[0].params[monolith::macro(0)].value, 29 / 127.0), "Instrument fallback chooses track instrument Macro 1");
        press(106); check(transport.playing(), "dedicated Play");
        press(105); check(!transport.playing(), "dedicated Stop");
        press(102); check(transport.loopEnabled(), "dedicated Cycle");
        press(107); check(transport.arrangementRecord(), "dedicated Record enables recording");
        press(107); check(!transport.arrangementRecord(), "dedicated Record toggles recording off");
        transport.setBeat(8); press(103); press(104);
        check(near(transport.beat(), 8), "dedicated rewind and forward");
        check(!surface.controlChange("Other Keyboard", 15, 12, 127), "dedicated controls preserve source isolation");
        settings("auto", {}, 0, true); surface.reloadSettings();
        project.tracks[0].volume = .5; cc(12, 10);
        check(near(project.tracks[0].volume, .5), "dedicated faders respect pickup");
        cc(12, 90); check(near(project.tracks[0].volume, 90 / 127.0), "dedicated fader pickup crosses target");
    }

    settings("auto", {}, 0, false);
    MainWindow window;
    window.show();
    QApplication::processEvents();
    window.engine()->shutdown(); // deterministic commands, with no running audio callback
    auto* project = window.project();
    auto* transport = window.transport();
    auto* input = window.midiInput();
    auto* surface = window.controlSurface();
    project->mappings.clear();
    transport->setBeat(0); // keep optimistic UI state while the engine is deliberately stopped
    project->tracks[0].volume = 0.6;
    input->inject(0xb0, 7, 38, nektar);
    check(near(project->tracks[0].volume, 38 / 127.0), "injected Nektar CCs reach the live MainWindow mixer integration");
    input->inject(0xb0, 7, 127, "Unrelated Device");
    check(near(project->tracks[0].volume, 38 / 127.0), "MainWindow preserves MIDI source isolation");
    project->mappings.push_back({Mapping::MidiCC, 0, 7, "main:cue"});
    input->inject(0xb0, 7, 83, nektar);
    check(near(project->cueVolume, 83 / 127.0) && near(project->tracks[0].volume, 38 / 127.0), "a custom MIDI Map takes priority over the automatic surface profile");
    project->mappings.clear();
    input->inject(0xbf, 12, 67, nektar);
    check(near(project->tracks[0].volume, 67 / 127.0), "dedicated Mixer messages reach MainWindow");
    project->mappings.push_back({Mapping::MidiCC, 15, 12, "main:cue"});
    input->inject(0xbf, 12, 93, nektar);
    check(near(project->cueVolume, 93 / 127.0) && near(project->tracks[0].volume, 67 / 127.0), "custom mappings override dedicated Nektar controls");
    project->mappings.clear();
    QString observedSource;
    QObject::connect(input, &audio::MidiInput::controlChange, &window, [&](int, int, int) { observedSource = input->sourceName(); });
    input->inject(0xb0, 1, 64, nektar);
    check(observedSource == nektar, "injected CC delivery retains the source name for the synchronous receiver");

    QSignalSpy mmcSpy(input, &audio::MidiInput::machineControl);
    for (const QByteArray& bad : {QByteArray::fromHex("f07f7f0602"), QByteArray::fromHex("f07e7f0602f7"),
                                 QByteArray::fromHex("f07fff0602f7"), QByteArray::fromHex("f07f7f0502f7"),
                                 QByteArray::fromHex("f07f7f0682f7"), QByteArray::fromHex("f07f7f0602f6"),
                                 QByteArray::fromHex("f07f7f0602f700")}) input->injectSysEx(bad, nektar);
    check(mmcSpy.isEmpty() && !transport->playing(), "incomplete, non-realtime, invalid-data and non-MMC SysEx packets are ignored");
    input->injectSysEx(mmc(2), "Unrelated Device");
    check(!transport->playing(), "another device's MMC cannot start Sessions");
    input->injectSysEx(mmc(2), nektar);
    check(transport->playing(), "valid source-scoped MMC Play starts the transport");
    transport->setBeat(9);
    input->injectSysEx(mmc(1), nektar);
    input->injectSysEx(mmc(1), nektar);
    check(!transport->playing() && near(transport->beat(), 9), "repeated MMC Stop does not rewind a stopped song");
    input->injectSysEx(mmc(4), nektar);
    check(near(transport->beat(), 13), "MMC Fast Forward moves forward one bar");
    input->injectSysEx(mmc(5), nektar);
    check(near(transport->beat(), 9), "MMC Rewind moves backward one bar");
    input->injectSysEx(mmc(6), nektar);
    check(transport->playing() && transport->arrangementRecord(), "MMC Record starts armed Arrangement recording and playback");
    input->injectSysEx(mmc(7), nektar);
    check(!transport->arrangementRecord() && transport->playing(), "MMC Record Exit ends recording while playback continues");
    input->injectSysEx(mmc(9), nektar);
    check(!transport->playing() && near(transport->beat(), 9), "MMC Pause stops without changing the song position");
    window.setMapMode(Mapper::Mode::Midi);
    input->injectSysEx(mmc(2), nektar);
    check(!transport->playing(), "mapping mode prevents accidental automatic transport commands");
    window.setMapMode(Mapper::Mode::Off);

    while (project->scenes.size() < 10) project->insertScene(project->scenes.size());
    for (int scene = 0; scene < 10; ++scene) project->tracks[0].clipSlots[scene] = project->newClip(QString("Scene %1").arg(scene + 1), 4);
    emit project->structureChanged();
    auto* grid = window.sessionView()->grid();
    grid->setFocus();
    QApplication::processEvents();
    auto clearQueued = [&] { for (auto& track : project->tracks) track.queuedSlot = -2; };
    bool rows = true;
    for (int scene = 0; scene < 10; ++scene) {
        clearQueued();
        key(grid, scene == 9 ? Qt::Key_0 : Qt::Key_1 + scene);
        rows &= project->tracks[0].queuedSlot == scene;
    }
    check(rows, "number-row keys 1 through 0 launch exactly Session scenes 1 through 10");
    clearQueued();
    for (Qt::KeyboardModifiers mods : {Qt::KeyboardModifiers(Qt::ShiftModifier), Qt::KeyboardModifiers(Qt::ControlModifier),
                                      Qt::KeyboardModifiers(Qt::AltModifier), Qt::KeyboardModifiers(Qt::MetaModifier),
                                      Qt::KeyboardModifiers(Qt::KeypadModifier)}) key(grid, Qt::Key_2, mods);
    check(project->tracks[0].queuedSlot == -2, "modifiers and keypad digits do not invoke default scene shortcuts");
    key(grid, Qt::Key_2, Qt::NoModifier, true);
    check(project->tracks[0].queuedSlot == -2, "auto-repeat cannot relaunch a default scene");
    project->mappings.push_back({Mapping::Key, 0, Qt::Key_1, "scene:5"});
    key(grid, Qt::Key_1);
    check(project->tracks[0].queuedSlot == 5, "a custom number-key map takes priority over its default scene");
    project->mappings.clear();
    auto typing = [&](QWidget* editor) {
        clearQueued(); editor->show(); editor->setFocus(); QApplication::processEvents();
        key(editor, Qt::Key_3); const bool untouched = project->tracks[0].queuedSlot == -2;
        editor->hide(); grid->setFocus(); return untouched;
    };
    QLineEdit line(&window);
    check(typing(&line), "typing a number in a line edit never launches a scene");
    QSpinBox spin(&window);
    check(typing(&spin), "typing a number in a spin box never launches a scene");
    QTextEdit text(&window);
    check(typing(&text), "typing a number in a text editor never launches a scene");

    settings("auto", {}, 0, true);
    surface->reloadSettings();
    PreferencesDialog preferences(window.engine(), input, {}, &window);
    QObject::connect(&preferences, &PreferencesDialog::midiControllerSettingsChanged, surface, &ControlSurface::reloadSettings);
    QSignalSpy settingsSpy(&preferences, &PreferencesDialog::midiControllerSettingsChanged);
    auto* profile = preferences.findChild<QComboBox*>("controllerProfile");
    auto* source = preferences.findChild<QComboBox*>("controllerInput");
    auto* bank = preferences.findChild<QComboBox*>("controllerBank");
    auto* pickup = preferences.findChild<QCheckBox*>("controllerPickup");
    check(profile && source && bank && pickup, "Preferences exposes profile, input, bank and pickup controls");
    if (profile && source && bank && pickup) {
        activate(profile, profile->findData("off"));
        check(!surface->accepts(nektar) && !bank->isEnabled() && !source->isEnabled(), "Preferences Off immediately disables the live profile and dependent controls");
        activate(profile, profile->findData("auto"));
        check(surface->accepts(nektar) && bank->isEnabled() && source->isEnabled(), "Preferences Auto immediately restores the live profile");
        source->addItem("Test Mixer", "Test Mixer"); activate(source, source->findData("Test Mixer"));
        check(surface->accepts("Test Mixer") && !surface->accepts(nektar), "Preferences input override immediately scopes the live profile");
        activate(bank, bank->findData(8));
        pickup->setChecked(false);
        while (project->tracks.size() < 9) project->insertTrack(project->tracks.size());
        project->tracks[8].volume = 0.8;
        input->inject(0xb0, 7, 18, "Test Mixer");
        check(near(project->tracks[8].volume, 18 / 127.0), "Preferences bank and pickup changes immediately affect incoming mixer CCs");
        QSettings saved("Sessions", "Sessions");
        check(settingsSpy.count() == 5 && saved.value("midi/controller").toString() == "auto"
                  && saved.value("midi/controllerInput").toString() == "Test Mixer"
                  && saved.value("midi/controllerBank").toInt() == 8 && !saved.value("midi/controllerPickup").toBool(),
              "all controller preference changes persist and emit the live settings signal");
        PreferencesDialog restored(window.engine(), input, {}, &window);
        check(restored.findChild<QComboBox*>("controllerInput")->currentData().toString() == "Test Mixer"
                  && restored.findChild<QComboBox*>("controllerBank")->currentData().toInt() == 8
                  && !restored.findChild<QCheckBox*>("controllerPickup")->isChecked(), "controller preferences restore even when the selected input is disconnected");
    }
    const QString capture = qEnvironmentVariable("SESSIONS_NEKTAR_CAPTURE");
    if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        auto* sidebar = preferences.findChild<QListWidget*>("prefsSidebar");
        sidebar->setCurrentItem(sidebar->findItems("MIDI", Qt::MatchExactly).first());
        preferences.show(); QApplication::processEvents();
        preferences.grab().save(capture + "/nektar-preferences.png");
    }
    std::printf("control_surface: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
