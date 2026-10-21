#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/ClipView.h"
#include "ui/SampleEditor.h"
#include "ui/TempoActions.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
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
bool sameMap(const ClipPtr& a, const ClipPtr& b) {
    if (a->warpMarkers.size() != b->warpMarkers.size()) return false;
    for (int i = 0; i < a->warpMarkers.size(); ++i)
        if (!near(a->warpMarkers[i].beat, b->warpMarkers[i].beat) || !near(a->warpMarkers[i].seconds, b->warpMarkers[i].seconds)) return false;
    return true;
}

// Open the actual widget menu, select a named action and close the nested loop.
bool triggerMenu(QWidget* control, const char* name) {
    bool fired = false;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        for (QAction* action : menu->actions()) if (action->objectName() == name && action->isEnabled()) {
            action->trigger();
            fired = true;
            break;
        }
        menu->close();
    });
    timer.start(0);
    const QPoint pos = control->rect().center();
    QContextMenuEvent event(QContextMenuEvent::Mouse, pos, control->mapToGlobal(pos));
    QApplication::sendEvent(control, &event);
    return fired;
}
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    Project p;
    p.tempo = 176;
    p.scenes = {"1"};
    p.addTrack("Drums", Qt::red);
    p.addTrack("Bass", Qt::blue);
    for (Track& track : p.tracks) track.type = TrackType::Audio;
    // Unpitched/beatless source is intentional: a repair must not invent a beat.
    auto sample = samples::fromFrames(std::vector<float>(48000 * 32, 0), {}, 48000);
    auto drums = p.newClip("Drums", 32 * 116.0 / 60);
    auto bass = p.newClip("Bass", 32 * 232.0 / 60);
    for (const ClipPtr& c : {drums, bass}) {
        c->sample = sample;
        c->warp = true;
        c->loop = false;
        c->warpMarkers = {{0, 0}, {c->length, 32}};
    }
    drums->samplePath = "/tmp/tempo-repair-song/0 Drums.wav";
    bass->samplePath = "/tmp/tempo-repair-song/1 Bass.wav";
    drums->detectedBpm = 116;
    bass->detectedBpm = 232;
    p.tracks[0].clipSlots[0] = drums;
    p.tracks[1].clipSlots[0] = bass;
    p.undoStack()->clear();
    const auto before = p.captureEditState();
    audio::Engine engine(&p);
    Transport transport(&p, &engine);
    ClipView view(&p, &transport);
    view.resize(1400, 420);
    view.setClip(drums);
    view.show();
    QApplication::processEvents();
    auto* bpm = view.findChild<DragValue*>("sourceBpm");
    auto* sync = view.findChild<QPushButton*>("syncTempo");
    auto* editor = view.findChild<SampleEditor*>();
    check(bpm && sync && editor, "source field, Sync and waveform expose the repair workflow");
    check(bpm && triggerMenu(bpm, "useProjectSourceTempo"), "Source BPM context menu can use a known project tempo for matching stems");
    check(near(drums->sampleBpm(), 176) && near(bass->sampleBpm(), 176), "known tempo repairs both differently warped legacy stems");
    check(sameMap(drums, bass) && near(drums->secondsAt(drums->length), 32), "repair gives stems identical timing and retains their complete source");
    check(p.undoStack()->count() == 1, "repairing the related set is one undo step");
    p.undoStack()->undo();
    check(p.captureEditState() == before, "undo restores both original maps and source metadata");
    p.undoStack()->redo();
    check(near(bass->sampleBpm(), 176), "redo restores the related correction");
    check(sync && triggerMenu(sync, "redetectStemTempo"), "Sync context menu exposes a fresh shared analysis");
    check(!drums->warp && !bass->warp && drums->detectedBpm == 0 && bass->detectedBpm == 0,
          "beatless source remains native after reanalysis instead of reusing a wrong saved positive");
    p.undoStack()->undo();
    check(near(drums->sampleBpm(), 176) && near(bass->sampleBpm(), 176), "reanalysis is reversible for the whole stem set");
    check(editor && triggerMenu(editor, "redetectSourceTempo"), "waveform menu exposes independent reanalysis");
    check(!drums->warp && bass->warp, "individual reanalysis leaves another stem's explicit timing alone");

    auto shortClip = p.newClip("Short", 4);
    shortClip->sample = samples::fromFrames(std::vector<float>(48000 * 2, 0), {}, 48000);
    shortClip->samplePath = "/tmp/tempo-repair-song/2 Synth.wav";
    shortClip->warpMarkers = {{0, 0}, {4, 2}};
    QMenu menu;
    addSourceTempoActions(menu, &p, shortClip);
    QAction* related = menu.findChild<QAction*>("redetectStemTempo");
    QAction* apply = menu.findChild<QAction*>("applySourceTempoToStems");
    check(related && !related->isEnabled() && apply && !apply->isEnabled(), "short unrelated clips do not offer enabled stem-wide corrections");
    drums->detectedBpm = 178;
    drums->warp = true;
    drums->warpMarkers = {{0, 0}, {16 * 174.0 / 60, 16}, {32 * 178.0 / 60, 32}};
    drums->length = drums->warpMarkers.last().beat;
    view.setClip(drums);
    QApplication::processEvents();
    check(sourceTempoRange(*drums) == QString("174.0–182.0"), "variable maps describe the complete measured tempo range");
    auto* info = view.findChild<QLabel*>("sourceTempoInfo");
    check(info && info->text().contains("174.0–182.0") && info->toolTip().contains("Variable source"),
          "clip tempo display identifies changing source tempo instead of implying a fixed average");
    check(bpm && bpm->toolTip().contains("average"), "source BPM field explains that editing a variable map scales its average");
    std::printf("tempo_repair_ui: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
