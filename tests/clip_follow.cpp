// Follow uses the selected clip's own clock and preserves its editor's zoom.
// No audio device is opened; transport ticks and native source positions are explicit.
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Sample.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/ClipView.h"
#include "ui/PianoRoll.h"
#include "ui/SampleEditor.h"
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); }
}
bool near(double a, double b) { return std::abs(a - b) < 1e-7; }
template<typename Editor> bool visible(Editor* editor, double beat) {
    double first = 0, last = 0;
    return editor->viewport(first, last) && first <= beat && beat < last;
}
template<typename Editor> double firstBeat(Editor* editor) {
    double first = 0, last = 0;
    editor->viewport(first, last);
    return first;
}
QColor pixel(QWidget* widget, int x, int y) {
    const QPixmap capture = widget->grab();
    const double scale = capture.devicePixelRatio();
    return capture.toImage().pixelColor(int(std::lround(x * scale)), int(std::lround(y * scale)));
}
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    QSettings("Sessions", "Sessions").setValue("clipView/follow", true);
    Project project;
    project.scenes = {"1"};
    project.addTrack("Audio", QColor("#8ac6d1"));
    project.tracks[0].type = TrackType::Audio;
    auto audioClip = project.newClip("Long source", 64);
    audioClip->samplePath = "/generated/follow.wav";
    audioClip->sample = samples::fromFrames(std::vector<float>(6400, 0), {}, 100);
    audioClip->warpMarkers = {{0, 0}, {64, 64}};
    audioClip->warp = false;
    project.tracks[0].clipSlots[0] = audioClip;
    project.addTrack("MIDI", QColor("#e6b36a"));
    auto midiClip = project.newClip("Long pattern", 64);
    midiClip->notes = {{60, 0, 1, 0.8, 1}, {67, 48, 1, 0.8, 1}};
    project.tracks[1].clipSlots[0] = midiClip;

    audio::Engine engine(&project);
    Transport transport(&project, &engine);
    ClipView view(&project, &transport);
    view.resize(1500, 420);
    view.setClip(audioClip);
    view.show();
    QApplication::processEvents();
    auto* follow = view.findChild<QToolButton*>("clipFollowButton");
    auto* sample = view.sampleEditor();
    auto* roll = view.pianoRoll();
    check(follow && follow->isVisible() && follow->isCheckable(), "clip editor exposes a visible Follow toggle");
    if (!follow) return 1;
    check(view.follow() && follow->isChecked() && sample->follow() && roll->follow(), "one clip Follow control governs both audio and MIDI editors");

    sample->setViewport(0, 4);
    const double audioZoom = sample->pixelsPerBeatForTests();
    transport.play();
    project.tracks[0].playingSlot = 0;
    project.tracks[0].slotStart = 0;
    project.tracks[0].playingClipBeat = 24;
    transport.setBeat(1);
    check(visible(sample, 24) && !visible(sample, 1), "unwarped audio follows native source position rather than the project beat");
    check(near(audioZoom, sample->pixelsPerBeatForTests()), "audio Follow preserves horizontal zoom");
    const double held = firstBeat(sample);
    transport.setBeat(2);
    check(near(held, firstBeat(sample)), "audio viewport stays still while its playhead remains visible");

    QTest::mouseClick(follow, Qt::LeftButton);
    sample->setViewport(0, 4);
    project.tracks[0].playingClipBeat = 40;
    transport.setBeat(40);
    check(!view.follow() && !sample->follow() && !roll->follow(), "turning Follow off reaches both clip editors");
    check(near(firstBeat(sample), 0), "fixed audio view stays put during playback");
    check(!QSettings("Sessions", "Sessions").value("clipView/follow", true).toBool(), "clip Follow state is saved independently in preferences");
    QTest::mouseClick(follow, Qt::LeftButton);
    check(visible(sample, 40), "enabling Follow immediately brings the audio playhead back");
    project.tracks[0].playingClipBeat = 0.5;
    transport.setBeat(41);
    check(visible(sample, 0.5) && near(firstBeat(sample), 0), "audio Follow returns to the beginning when the source loops");

    // A manual ruler gesture must finish before Follow moves the editor.
    sample->setViewport(0, 4);
    QTest::mousePress(sample, Qt::LeftButton, Qt::NoModifier, QPoint(70, 4));
    project.tracks[0].playingClipBeat = 20;
    transport.setBeat(42);
    check(near(firstBeat(sample), 0), "audio Follow does not interrupt a manual ruler gesture");
    QTest::mouseRelease(sample, Qt::LeftButton, Qt::NoModifier, QPoint(70, 4));
    emit transport.tick();
    check(visible(sample, 20), "audio Follow resumes after the manual gesture ends");

    project.tracks[0].playingSlot = -1;
    project.tracks[0].overridden = true;
    sample->setViewport(0, 4);
    transport.setBeat(45);
    check(near(firstBeat(sample), 0), "an audio clip that is not playing never drags the viewport");
    project.tracks[1].playingSlot = 0;
    project.tracks[1].slotStart = 0;
    view.setClip(midiClip);
    roll->setViewport(0, 4);
    const double midiZoom = roll->zoom();
    transport.setBeat(28);
    check(visible(roll, 28), "MIDI Follow pages to a pattern's own playhead");
    check(near(midiZoom, roll->zoom()), "MIDI Follow preserves horizontal zoom");
    const double midiHeld = firstBeat(roll);
    transport.setBeat(28.5);
    check(near(midiHeld, firstBeat(roll)), "MIDI viewport remains fixed while the playhead is visible");
    transport.setBeat(64.5);
    check(visible(roll, 0.5) && near(firstBeat(roll), 0), "MIDI Follow returns to the loop start");
    view.setFollow(false);
    roll->setViewport(8, 12);
    transport.setBeat(32);
    check(near(firstBeat(roll), 8), "fixed MIDI view retains a manually chosen range during playback");
    view.setFollow(true);
    check(visible(roll, 32), "enabling Follow immediately recovers the MIDI playhead");

    roll->setViewport(0, 4);
    QTest::mousePress(roll, Qt::LeftButton, Qt::NoModifier, QPoint(90, 15));
    transport.setBeat(44);
    check(near(firstBeat(roll), 0), "MIDI Follow does not interrupt a manual ruler gesture");
    QTest::mouseRelease(roll, Qt::LeftButton, Qt::NoModifier, QPoint(90, 15));
    emit transport.tick();
    check(visible(roll, 44), "MIDI Follow resumes after the manual gesture ends");

    project.tracks[1].playingSlot = -1;
    project.tracks[1].overridden = false;
    project.tracks[1].arrangement = {{100, 16, 12, midiClip, 0, 0}};
    roll->setViewport(0, 4);
    transport.setBeat(104);
    check(visible(roll, 16) && !visible(roll, 104), "Arrangement clips follow their source offset rather than absolute song time");
    transport.stop();
    roll->setViewport(0, 4);
    transport.setBeat(108);
    check(near(firstBeat(roll), 0), "stopped transport never changes a fixed editor position");

    view.setFollow(false);
    ClipView restored(&project, &transport);
    check(!restored.follow() && !restored.pianoRoll()->follow() && !restored.sampleEditor()->follow(), "a recreated editor restores the saved fixed-view preference");
    QSettings("Sessions", "Sessions").setValue("arrangement/follow", true);
    check(!view.follow() && !restored.follow(), "Arrangement Follow preferences do not change Clip Follow");

    // Loop/start/end geometry is independent of the clip colour, with neutral ink
    // that remains legible on all three base palettes. Warp pins keep their role colour.
    audioClip->color = QColor("#ed3e9b");
    midiClip->color = QColor("#68ce3d");
    for (theme::Kind palette : {theme::Kind::Dark, theme::Kind::Mid, theme::Kind::Light}) {
        theme::apply(palette);
        view.setClip(audioClip);
        sample->setViewport(0, 4);
        check(pixel(sample, 100, SampleEditor::kBraceY + 3) == theme::text
                  && pixel(sample, SampleEditor::kLeft + 3, SampleEditor::kMarkY + 5) == theme::text,
              "audio loop brace and start marker use neutral theme ink rather than the clip colour");
        view.setClip(midiClip);
        roll->setViewport(0, 4);
        check(pixel(roll, 100, 1) == theme::text && pixel(roll, 37, 9) == theme::text,
              "MIDI loop brace and start marker use neutral theme ink rather than the note colour");
    }
    theme::apply(theme::Kind::Dark);
    const QString capture = qEnvironmentVariable("SESSIONS_CLIP_FOLLOW_CAPTURE");
    if (!capture.isEmpty()) view.grab().save(capture);
    std::printf("clip_follow: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
