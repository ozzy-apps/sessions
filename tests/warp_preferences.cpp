// The import algorithm is a global preference, independent of each clip's
// Warp switch, timing map and saved settings. Settings and samples are temporary.
#include "audio/Engine.h"
#include "audio/MidiInput.h"
#include "core/Import.h"
#include "core/Model.h"
#include "core/Sample.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/WarpPreferences.h"
#include "ui/Preferences.h"
#include <QApplication>
#include <QComboBox>
#include <QListWidget>
#include <QSettings>
#include <QTemporaryDir>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
void activate(QComboBox* combo, int mode) {
    const int index = combo->findData(mode);
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
    check(clipsettings::defaultWarpMode() == WarpMode::Beats, "an unset preference preserves the Beats import default");
    for (WarpMode mode : {WarpMode::Beats, WarpMode::Tones, WarpMode::Texture, WarpMode::Repitch, WarpMode::Complex, WarpMode::ComplexPro}) {
        clipsettings::setDefaultWarpMode(mode);
        QSettings stored("Sessions", "Sessions");
        stored.sync();
        check(stored.value(clipsettings::kDefaultWarpMode).toInt() == int(mode) && clipsettings::defaultWarpMode() == mode,
              "every Warp algorithm persists and reads back from global preferences");
    }
    for (const QVariant& invalid : {QVariant(-1), QVariant(6), QVariant(999), QVariant("invalid"), QVariant("2.5"), QVariant(QString())}) {
        QSettings("Sessions", "Sessions").setValue(clipsettings::kDefaultWarpMode, invalid);
        check(clipsettings::defaultWarpMode() == WarpMode::Beats, "an invalid saved algorithm falls back safely to Beats");
    }
    clipsettings::setDefaultWarpMode(WarpMode(-1));
    check(clipsettings::defaultWarpMode() == WarpMode::Beats, "the setter sanitizes invalid enum values");

    const SamplePtr silence = samples::fromFrames(std::vector<float>(1600, 0), {}, 8000);
    Project project;
    project.tempo = 176;
    ClipPtr existing;
    for (WarpMode mode : {WarpMode::Beats, WarpMode::Tones, WarpMode::Texture, WarpMode::Repitch, WarpMode::Complex, WarpMode::ComplexPro}) {
        clipsettings::setDefaultWarpMode(mode);
        const auto clip = project.newAudioClip("/generated/source_178bpm.wav", silence);
        check(clip->warpMode == mode && clip->warp && clip->detectedBpm == 178 && clip->warpMarkers.size() == 2,
              "new audio clips use the selected algorithm without changing explicit source timing or the Warp switch");
        if (!existing) existing = clip;
    }
    check(existing->warpMode == WarpMode::Beats, "changing the global default never edits an existing clip");

    clipsettings::setDefaultWarpMode(WarpMode::Texture);
    const auto unknown = project.newAudioClip("/generated/no_rhythm.wav", silence);
    check(unknown->warpMode == WarpMode::Texture && !unknown->warp && unknown->detectedBpm == 0,
          "unknown-tempo audio retains Warp Off while receiving the preferred algorithm");
    const auto batch = import::audioClips(project, {"/generated/a_178bpm.wav", "/generated/b_178bpm.wav"}, {silence, silence});
    check(batch.size() == 2 && batch[0]->warpMode == WarpMode::Texture && batch[1]->warpMode == WarpMode::Texture,
          "the shared multi-file import path applies the preference to every new clip");

    Clip saved = *existing;
    saved.warpMode = WarpMode::ComplexPro;
    saved.warp = false;
    const auto json = setfile::clipToJson(saved);
    clipsettings::setDefaultWarpMode(WarpMode::Tones);
    const Clip restored = setfile::clipFromJson(json);
    check(restored.warpMode == WarpMode::ComplexPro && !restored.warp,
          "loading a saved audio clip preserves its explicit mode and Warp Off regardless of the import default");
    auto legacy = json;
    auto audioJson = legacy.value("audio").toObject();
    audioJson.remove("warpMode");
    legacy.insert("audio", audioJson);
    check(setfile::clipFromJson(legacy).warpMode == WarpMode::Beats,
          "legacy clips without a saved algorithm keep the historical Beats fallback");
    check(project.newClip("MIDI", 4)->warpMode == WarpMode::Beats,
          "the import preference does not change MIDI clip construction");

    audio::Engine engine(&project);
    audio::MidiInput midi;
    clipsettings::setDefaultWarpMode(WarpMode::Repitch);
    QSettings("Sessions", "Sessions").setValue("prefs/page", 1);
    {
        PreferencesDialog preferences(&engine, &midi, {});
        preferences.resize(720, 480);
        preferences.show();
        QApplication::processEvents();
        auto* combo = preferences.findChild<QComboBox*>("defaultWarpMode");
        check(combo && combo->count() == 6 && combo->isVisible(), "Record / Warp exposes all six default algorithms");
        if (!combo) return 1;
        check(combo->currentData().toInt() == int(WarpMode::Repitch), "opening Preferences shows the saved default");
        const QRect bounds(combo->mapTo(&preferences, QPoint()), combo->size());
        check(preferences.rect().contains(bounds), "the new preference fits the minimum 720 by 480 dialog");
        activate(combo, int(WarpMode::Complex));
        check(clipsettings::defaultWarpMode() == WarpMode::Complex
                  && project.newAudioClip("/generated/ui_178bpm.wav", silence)->warpMode == WarpMode::Complex,
              "selecting the preference immediately updates future audio imports");
        check(existing->warpMode == WarpMode::Beats && !unknown->warp,
              "changing Preferences leaves existing algorithms and Warp Off states intact");
    }
    {
        PreferencesDialog reopened(&engine, &midi, {});
        auto* combo = reopened.findChild<QComboBox*>("defaultWarpMode");
        check(combo && combo->currentData().toInt() == int(WarpMode::Complex),
              "a recreated Preferences dialog restores the last selected Warp mode");
    }
    std::printf("warp_preferences: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
