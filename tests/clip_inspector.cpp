// Native clip inspector workflows: readable controls without stealing the editor,
// type-aware pages, and preparation of Warp settings while Warp is bypassed.
#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/ClipView.h"
#include "ui/EnvelopeEditor.h"
#include "ui/OverviewBar.h"
#include "ui/PianoRoll.h"
#include "ui/SampleEditor.h"
#include "ui/TakesEditor.h"
#include "widgets/DragValue.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <algorithm>
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
void settle() { QApplication::processEvents(); QApplication::processEvents(); }

template<class T> T* titled(QWidget* parent, const QString& title) {
    for (T* child : parent->findChildren<T*>())
        if (child->property("info").toString().section('|', 0, 0) == title) return child;
    return nullptr;
}

bool typeNumber(DragValue* field, const QString& number) {
    if (!field || !field->isVisible() || !field->isEnabled()) return false;
    QTest::mouseDClick(field, Qt::LeftButton, Qt::NoModifier, field->rect().center());
    auto* editor = field->findChild<QLineEdit*>("dragValueEditor");
    if (!editor || !editor->isVisible()) return false;
    QTest::keyClick(editor, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClicks(editor, number);
    QTest::keyClick(editor, Qt::Key_Return);
    settle();
    return true;
}

bool choose(QComboBox* combo, int index) {
    if (!combo || !combo->isVisible() || !combo->isEnabled() || index < 0 || index >= combo->count()) return false;
    combo->setCurrentIndex(index);
    QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection, Q_ARG(int, index));
    settle();
    return true;
}

bool page(QTabBar* tabs, int index) {
    if (!tabs || !tabs->isVisible() || !tabs->isTabEnabled(index)) return false;
    QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(index).center());
    settle();
    return tabs->currentIndex() == index;
}

bool aligned(const ClipView& view, QWidget* editor) {
    auto* strip = view.overviewStrip();
    return strip && editor && editor->isVisible() && strip->isVisible()
        && strip->mapTo(&view, QPoint()).x() == editor->mapTo(&view, QPoint()).x()
        && strip->width() == editor->width()
        && strip->mapTo(&view, QPoint()).y() < editor->mapTo(&view, QPoint()).y();
}

bool controlsFit(QWidget* inspector) {
    if (!inspector) return false;
    const QRect bounds = inspector->rect();
    for (QWidget* child : inspector->findChildren<QWidget*>()) {
        if (!child->isVisible() || child->isWindow()) continue;
        if (!qobject_cast<DragValue*>(child) && !qobject_cast<QAbstractButton*>(child)
            && !qobject_cast<QComboBox*>(child) && !qobject_cast<QLineEdit*>(child)) continue;
        const QRect actual(child->mapTo(inspector, QPoint()), child->size());
        if (!bounds.contains(actual)) {
            std::fprintf(stderr, "clipped control: %s (%s), %d,%d %dx%d in %dx%d\n",
                         qPrintable(child->objectName()), child->metaObject()->className(), actual.x(), actual.y(),
                         actual.width(), actual.height(), bounds.width(), bounds.height());
            return false;
        }
    }
    return true;
}
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    // All old boxes used to be enabled independently; those saved settings must
    // not reopen four columns in the new inspector.
    for (const char* box : {"launch", "notes", "envelopes"})
        QSettings("Sessions", "Sessions").setValue(QString("clipView/") + box, true);

    Project p;
    p.tempo = 176;
    p.scenes = {"1"};
    p.addTrack("Audio", QColor("#40bfe1"));
    p.tracks[0].type = TrackType::Audio;
    p.addTrack("MIDI", QColor("#e6a64c"));
    auto audioClip = p.newClip("Long audio clip name for testing", 16);
    audioClip->samplePath = "/generated/An exceptionally long exported stem filename that must not widen the inspector.wav";
    audioClip->sample = samples::fromFrames(std::vector<float>(48000 * 4, 0), {}, 48000);
    audioClip->warpMarkers = {{0, 0}, {16, 4}};
    audioClip->detectedBpm = 240;
    audioClip->warp = false;
    audioClip->envelopes = {{"mixer:volume", {{0, 0.8}, {16, 0.2}}}};
    auto midi = p.newClip("MIDI pattern", 16);
    midi->notes = {{60, 0, 1, 0.8, 1}, {67, 4, 2, 0.7, 1}};
    p.tracks[0].clipSlots[0] = audioClip;
    p.tracks[1].clipSlots[0] = midi;
    p.undoStack()->clear();
    audio::Engine engine(&p);
    Transport transport(&p, &engine);
    ClipView view(&p, &transport);
    view.resize(900, 280);
    view.setClip(audioClip);
    view.show();
    settle();
    auto* props = view.findChild<QWidget*>("clipProps");
    auto* tabs = view.findChild<QTabBar*>("clipInspectorTabs");
    auto* pages = view.findChild<QStackedWidget*>("clipInspectorPages");
    auto* name = view.findChild<QLineEdit*>("clipName");
    auto* follow = view.findChild<QToolButton*>("clipFollowButton");
    auto* loop = titled<QPushButton>(&view, "Loop");
    auto* warp = view.findChild<QComboBox*>("warpMode");
    auto* warpOn = view.findChild<QPushButton*>("warpOn");
    check(props && tabs && pages && name && follow && loop && warp && warpOn,
          "inspector exposes common clip controls and readable page navigation");
    if (!props || !tabs || !pages || !name || !follow || !loop || !warp || !warpOn) return 1;
    check(tabs->count() == 3 && tabs->tabText(0) == "Sample" && tabs->tabText(1) == "Launch"
              && tabs->tabText(2) == "Envelopes" && tabs->currentIndex() == 0,
          "an audio clip opens Sample with Launch and Envelopes one click away");
    check(props->width() <= 440 && view.width() == 900 && view.sampleEditor()->width() >= 450,
          "at 900 pixels the inspector leaves at least half the view for the waveform");
    check(warp->height() >= 20 && warpOn->height() >= 20 && tabs->height() >= 20,
          "warp controls and page tabs have readable twenty-pixel interaction targets");
    check(controlsFit(props) && aligned(view, view.sampleEditor()),
          "audio controls fit and the overview stays exactly aligned above the waveform");
    const int inspectorWidth = props->width();

    auto* transpose = view.findChild<DragValue*>("clipTranspose");
    auto* detune = view.findChild<DragValue*>("clipDetune");
    auto* gain = view.findChild<DragValue*>("clipGain");
    check(transpose && detune && gain && transpose->isEnabled() && detune->isEnabled(),
          "a bypassed Warp still offers pitch, fine tune and clip gain");
    check(typeNumber(transpose, "7") && audioClip->transpose == 7 && !audioClip->warp,
          "typed pitch changes do not silently turn Warp on");
    p.undoStack()->undo(); settle();
    check(audioClip->transpose == 0 && transpose && near(transpose->value(), 0) && !audioClip->warp,
          "unwarped pitch editing undoes and refreshes its control");
    check(typeNumber(detune, "-17") && near(audioClip->detune, -17) && !audioClip->warp,
          "fine tuning can be typed with Warp off");
    check(typeNumber(gain, "-3.5") && near(audioClip->gain, -3.5),
          "clip gain retains precise typed editing in the compact inspector");

    for (int mode = 0; mode < 6; ++mode) {
        const bool selected = choose(warp, mode);
        check(selected && audioClip->warpMode == WarpMode(mode) && !audioClip->warp
                  && transpose && transpose->isEnabled() && detune && detune->isEnabled(),
              "every algorithm can be prepared while Warp and native pitch editing remain available");
    }
    auto* grain = view.findChild<DragValue*>("warpGrain");
    auto* flux = view.findChild<DragValue*>("warpFlux");
    auto* formants = view.findChild<DragValue*>("warpFormants");
    choose(warp, int(WarpMode::Repitch));
    QTest::mouseClick(warpOn, Qt::LeftButton); settle();
    check(audioClip->warp && !transpose->isEnabled() && !detune->isEnabled(),
          "active Re-Pitch derives pitch from tempo and disables independent tuning");
    QTest::mouseClick(warpOn, Qt::LeftButton); settle();
    check(!audioClip->warp && transpose->isEnabled() && detune->isEnabled(),
          "bypassing Re-Pitch restores native pitch editing");
    choose(warp, int(WarpMode::Complex));
    check(formants && !formants->isVisible() && grain && !grain->isVisible() && flux && !flux->isVisible(),
          "plain Complex hides parameters that its algorithm does not use");
    choose(warp, int(WarpMode::Texture));
    check(typeNumber(grain, "80") && typeNumber(flux, "42")
              && near(audioClip->grainMs, 80) && near(audioClip->flux, 0.42) && !audioClip->warp,
          "Texture grain and flux remain editable while tempo warping is bypassed");
    choose(warp, int(WarpMode::ComplexPro));
    check(typeNumber(formants, "60") && near(audioClip->formants, 0.6) && !audioClip->warp,
          "Complex Pro formants can be prepared without enabling Warp");
    QTest::mouseClick(warpOn, Qt::LeftButton); settle();
    check(audioClip->warp && audioClip->warpMode == WarpMode::ComplexPro && near(audioClip->formants, 0.6),
          "enabling Warp uses the mode and parameters already prepared");

    QSignalSpy commands(&view, &ClipView::commandRequested);
    auto* save = view.findChild<QPushButton*>("saveClip");
    if (save) QTest::mouseClick(save, Qt::LeftButton);
    check(save && commands.size() == 1 && commands.first().first().toString() == "saveClip",
          "Save to Library remains available in the persistent clip controls");

    check(page(tabs, 1) && name->isVisible() && loop->isVisible() && follow->isVisible()
              && !warp->isVisible() && props->width() == inspectorWidth && controlsFit(props),
          "Launch replaces the auxiliary page without hiding clip timing or widening the inspector");
    auto* mode = titled<QComboBox>(&view, "Launch Mode");
    check(choose(mode, int(LaunchMode::Gate)) && audioClip->launchMode == LaunchMode::Gate,
          "the Launch page edits the selected clip's launch mode");
    p.undoStack()->undo(); settle();
    check(audioClip->launchMode == LaunchMode::Trigger && mode && mode->currentIndex() == int(LaunchMode::Trigger),
          "Launch changes undo while preserving the active page");
    auto* actions = titled<QCheckBox>(&view, "Follow Action");
    if (actions) QTest::mouseClick(actions, Qt::LeftButton, Qt::NoModifier, QPoint(8, actions->height() / 2));
    auto* actionA = titled<QComboBox>(&view, "Follow Action A");
    check(actions && audioClip->followEnabled && choose(actionA, int(FollowAction::Stop))
              && audioClip->followA == FollowAction::Stop,
          "Follow actions remain reachable and functional on the Launch page");

    check(page(tabs, 2) && view.envelopeEditor()->isVisible() && name->isVisible()
              && follow->isVisible() && props->width() == inspectorWidth && controlsFit(props),
          "Envelopes opens its editor while retaining common controls and the narrow inspector");
    auto* envParam = view.findChild<QComboBox*>("envParam");
    const int panIndex = envParam ? envParam->findData("mixer:pan") : -1;
    check(choose(envParam, panIndex) && view.envelopeEditor()->target() == "mixer:pan",
          "the Envelopes chooser selects the actual editor parameter");
    check(page(tabs, 0) && view.sampleEditor()->isVisible() && !view.envelopeEditor()->isVisible()
              && aligned(view, view.sampleEditor()),
          "returning to Sample restores the waveform rather than leaving a hidden envelope toggle");

    audioClip->takes = {{audioClip->samplePath, audioClip->sample, "Take 1", 0}};
    emit p.clipChanged(); settle();
    auto* takes = view.findChild<QPushButton*>("takesShow");
    if (takes) QTest::mouseClick(takes, Qt::LeftButton);
    check(takes && takes->isEnabled() && view.takesEditor()->isVisible(),
          "recorded takes remain reachable from the Sample page");
    page(tabs, 2); page(tabs, 0);
    check(view.sampleEditor()->isVisible() && !view.takesEditor()->isVisible(),
          "returning from Envelopes clears the previous Takes editor state");

    view.setClip(midi); settle();
    check(tabs->tabText(0) == "Notes" && view.pianoRoll()->isVisible() && !warp->isVisible()
              && props->width() == inspectorWidth && aligned(view, view.pianoRoll()),
          "a MIDI clip substitutes Notes without changing inspector width or overview alignment");
    auto* octave = titled<QPushButton>(&view, "Octave Up");
    if (octave) QTest::mouseClick(octave, Qt::LeftButton);
    check(octave && midi->notes[0].pitch == 72 && midi->notes[1].pitch == 79,
          "Notes tools act on the selected MIDI clip");
    p.undoStack()->undo(); settle();
    check(midi->notes[0].pitch == 60 && midi->notes[1].pitch == 67,
          "MIDI note edits in the inspector remain undoable");
    auto* scale = titled<QPushButton>(&view, "Scale");
    if (scale) QTest::mouseClick(scale, Qt::LeftButton);
    check(scale && midi->scaleOn && controlsFit(props), "scale tools remain usable in the Notes page");
    page(tabs, 2);
    check(view.envelopeEditor()->isVisible(), "the same Envelopes page works for MIDI clips");
    auto timingVisible = [props](QWidget* field) {
        return field && field->isVisible() && field->width() > 0 && field->height() > 0
            && props->rect().contains(QRect(field->mapTo(props, QPoint()), field->size()));
    };
    check(timingVisible(titled<DragValue>(&view, "Clip Start"))
              && timingVisible(titled<DragValue>(&view, "Loop Position"))
              && timingVisible(titled<DragValue>(&view, "Loop Length")) && timingVisible(loop),
          "Start, loop position, loop length and Loop retain visible geometry on MIDI Envelopes");
    view.setClip(audioClip); settle();
    check(tabs->tabText(0) == "Sample" && props->width() == inspectorWidth && controlsFit(props),
          "switching clip types on Envelopes keeps a single coherent inspector");
    page(tabs, 0);
    check(view.sampleEditor()->isVisible() && !view.pianoRoll()->isVisible(),
          "returning to Sample after a type switch restores the correct editor");

    for (theme::Kind palette : {theme::Kind::Dark, theme::Kind::Mid, theme::Kind::Light}) {
        theme::apply(palette);
        view.resize(900, std::max(280, view.minimumSizeHint().height()));
        settle();
        bool fits = true;
        for (int index = 0; index < 3; ++index) fits = page(tabs, index) && controlsFit(props) && fits;
        check(fits && props->width() <= 440 && view.width() == 900,
              "all inspector pages fit a narrow native view in each base theme");
    }
    page(tabs, 1);
    ClipView recreated(&p, &transport);
    recreated.setClip(audioClip);
    auto* restoredTabs = recreated.findChild<QTabBar*>("clipInspectorTabs");
    check(restoredTabs && restoredTabs->currentIndex() == 1,
          "recreating the view restores the selected inspector page");
    page(tabs, 0);
    view.setClip(nullptr); settle();
    check(!props->isEnabled() && name->text().isEmpty(), "empty clip selection disables editing and clears the name");
    view.setClip(audioClip); settle();
    check(props->isEnabled() && name->text() == audioClip->name && view.sampleEditor()->isVisible(),
          "selecting a clip after an empty selection restores a usable inspector");

    const QString capture = qEnvironmentVariable("SESSIONS_CLIP_INSPECTOR_CAPTURE");
    if (!capture.isEmpty()) view.grab().save(capture);
    std::printf("clip_inspector: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
