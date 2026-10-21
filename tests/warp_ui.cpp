#include "audio/Engine.h"
#include "core/Model.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/ClipView.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }
SamplePtr tone() {
    std::vector<float> s(96000);
    for (size_t i = 0; i < s.size(); ++i) s[i] = 0.4f * float(std::sin(2 * M_PI * 220 * i / 48000));
    return samples::fromFrames(std::move(s), {}, 48000);
}
SamplePtr beatLoop(double bpm) {
    std::vector<float> s(size_t(std::lround(16 * 60 / bpm * 48000)), 0);
    for (int beat = 0; beat < 16; ++beat) {
        const size_t at = size_t(std::lround(beat * 60 / bpm * 48000));
        for (int i = 0; i < 4800 && at + i < s.size(); ++i) {
            const double t = i / 48000.0;
            s[at + i] += 0.8f * float(std::sin(2 * M_PI * 70 * t) * std::exp(-t * 35));
        }
    }
    return samples::fromFrames(std::move(s), {}, 48000);
}
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    QSettings("Sessions", "Sessions").setValue("clipView/notes", true);
    Project p;
    p.scenes = {"1"};
    p.addTrack("Audio", QColor("#80c0ff"));
    p.tracks[0].type = TrackType::Audio;
    p.tempo = 120;
    auto c = p.newAudioClip("/tmp/unknown-tone.wav", tone());
    p.tracks[0].clipSlots[0] = c;
    c->start = 1; c->loopStart = 0.5;
    c->envelopes = {{"mixer:volume", {{0, 1}, {2, 0.5}}}};
    c->comp = {{0.5, 2.5, 0}};
    p.tracks[0].arrangement = {{8, 6, 1, c, 0.25, 0.5}};
    p.undoStack()->clear();
    const auto before = p.captureEditState();
    audio::Engine engine(&p);
    Transport transport(&p, &engine);
    ClipView view(&p, &transport);
    view.resize(1300, 450);
    view.setClip(c);
    view.show();
    QApplication::processEvents();
    auto* bpm = view.findChild<DragValue*>("sourceBpm");
    auto* info = view.findChild<QLabel*>("sourceTempoInfo");
    auto* tail = view.findChild<QComboBox*>("warpTail");
    check(bpm && info && tail, "source tempo and Beats tail controls are discoverable");
    if (!bpm || !info || !tail) return 1;
    check(c->detectedBpm == 0 && !c->warp && bpm->isEnabled() && bpm->text() == QString::fromUtf8("—")
              && info->text().contains("unknown"), "unknown tempo stays unwarped with an editable empty source field");
    QTest::mouseDClick(bpm, Qt::LeftButton);
    auto* editor = bpm->findChild<QLineEdit*>();
    check(editor && editor->isVisible(), "source BPM supports precise typed input");
    if (!editor) return 1;
    QTest::keyClicks(editor, "176");
    QTest::keyClick(editor, Qt::Key_Return);
    QApplication::processEvents();
    const double k = 176.0 / 120;
    const auto& placed = p.tracks[0].arrangement[0];
    check(c->warp && near(c->sampleBpm(), 176) && near(c->detectedBpm, 176) && bpm->text() == "176.00",
          "typing source176 enables Warp and maps the source instead of overwriting project120");
    check(near(p.tempo, 120) && near(c->length, 4 * k) && near(c->start, k) && near(c->loopStart, 0.5 * k)
              && near(c->envelopes[0].points[1].beat, 2 * k) && near(c->comp[0].start, 0.5 * k) && near(c->comp[0].end, 2.5 * k),
          "source tempo changes preserve the same audio under start/loop/envelope/comp positions");
    check(near(placed.start, 8) && near(placed.length, 6 * k) && near(placed.offset, k)
              && near(placed.fadeIn, 0.25 * k) && near(placed.fadeOut, 0.5 * k),
          "Arrangement placements retain their audio offsets and fades when source BPM is corrected");
    check(info->text().contains("176") && info->text().contains("120"), "the tempo readout distinguishes source176 from project120");
    const QString capture = qEnvironmentVariable("SESSIONS_WARP_CAPTURE");
    if (!capture.isEmpty()) view.grab().save(capture);
    p.undoStack()->undo();
    QApplication::processEvents();
    check(p.captureEditState() == before && !c->warp && bpm->text() == QString::fromUtf8("—"),
          "one undo restores all source-tempo changes, including unknown status and arrangement positions");
    p.undoStack()->redo();
    p.setTempo(140);
    QApplication::processEvents();
    check(near(c->sampleBpm(), 176) && near(c->length, 4 * k) && info->text().contains("140"),
          "live project tempo changes update the readout without remapping source beats");

    // A previously saved clip can have the old assumed120 marker map and no detected tempo.
    // Sync must analyze the source rather than continuing to trust that project-derived grid.
    auto recovered = p.newAudioClip("/tmp/old-clip.wav", beatLoop(176));
    recovered->detectedBpm = 0; recovered->warp = true;
    recovered->length = recovered->sample->seconds() * 120 / 60;
    recovered->warpMarkers = {{0, 0}, {recovered->length, recovered->sample->seconds()}};
    p.tracks[0].clipSlots[0] = recovered;
    view.setClip(recovered);
    auto* sync = view.findChild<QPushButton*>("syncTempo");
    check(sync, "Sync remains available for old assumed-tempo clips");
    if (sync) QTest::mouseClick(sync, Qt::LeftButton);
    QApplication::processEvents();
    check(recovered->warp && std::abs(recovered->sampleBpm() - 176) < 0.1 && near(recovered->length, 16),
          "Sync repairs an old clip's assumed120 BPM map from its actual176 BPM audio");
    check(tail->isVisible() && tail->isEnabled(), "Beats mode exposes tail preservation");
    const int previousTail = recovered->beatsTail;
    tail->setCurrentIndex(2);
    QMetaObject::invokeMethod(tail, "activated", Qt::DirectConnection, Q_ARG(int, 2));
    check(recovered->beatsTail == 2, "Bounce tail selection reaches the clip model");
    p.undoStack()->undo();
    check(recovered->beatsTail == previousTail && tail->currentIndex() == previousTail, "Beats tail changes undo in one step");
    p.undoStack()->redo();
    const auto restored = setfile::clipFromJson(setfile::clipToJson(*recovered));
    check(restored.beatsTail == 2, "Beats tail selection survives serialization");
    recovered->warpMode = WarpMode::Tones;
    emit p.clipChanged();
    check(!tail->isVisible(), "tail controls appear only for the mode that uses them");
    recovered->warpMode = WarpMode::Beats; recovered->warp = false;
    emit p.clipChanged();
    check(tail->isVisible() && tail->isEnabled(), "Beats tail can be prepared while Warp is off");
    std::printf("warp_ui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
