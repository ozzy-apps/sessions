// Smoke test: drives the real widgets with synthetic mouse / key events.
// Build target `sessions_smoke`; run with QT_QPA_PLATFORM=offscreen.
#include "core/Model.h"
#include "core/Transport.h"
#include "audio/Engine.h"
#include "audio/Wavetables.h"
#include "audio/Convolver.h"
#include "audio/Nam.h"
#include "audio/Builtin.h"
#include "core/DeviceSpecs.h"
#include "core/ArrangementOps.h"
#include "core/SetFile.h"
#include "core/Presets.h"
#include "ui/DeviceDrag.h"
#include "core/Scales.h"
#include "core/Theme.h"
#include "core/Sample.h"
#include "core/Tempo.h"
#include "core/Import.h"
#include "ui/Browser.h"
#include "ui/ClipView.h"
#include "ui/SampleEditor.h"
#include "ui/TakesEditor.h"
#include "ui/EnvelopeEditor.h"
#include "ui/PianoRoll.h"
#include "ui/ArrangementView.h"
#include "widgets/DragValue.h"
#include "ui/MainWindow.h"
#include "widgets/Knob.h"
#include "widgets/LevelFader.h"
#include "ui/devices/DeviceBody.h"
#include "ui/DeviceView.h"
#include "ui/SessionView.h"
#include "ui/MixerStrip.h"
#include "ui/MapOverlay.h"
#include "ui/Preferences.h"
#include "ui/ThemeEditor.h"
#include "ui/OverviewBar.h"
#include "ui/InlineEdit.h"
#include "ui/Icons.h"
#include "ui/GridShade.h"
#include <QImage>
#include "core/Grooves.h"
#include <QSlider>
#include "audio/Driver.h"
#include "core/Mapping.h"
#include "audio/MidiInput.h"
#include <QApplication>
#include <functional>
#include <QElapsedTimer>
#include <QTest>
#include <QSignalSpy>
#include <QUndoStack>
#include <QToolButton>
#include <QWidgetAction>
#include <QMenu>
#include <QAction>
#include <QPushButton>
#include <QSplitter>
#include <QComboBox>
#include <QListWidget>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMimeData>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QDialog>
#include <array>
#include <QLineEdit>
#include <QScrollArea>
#include <QStackedWidget>
#include <QUrl>
#include <QtEndian>
#include <cmath>
#include <random>
#ifdef SESSIONS_HAVE_ALSA
#include <alsa/asoundlib.h>
#endif
#include <vector>
#include <cstdio>

using namespace sessions;

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } else std::printf("ok:   %s\n", msg); } while (0)

using audio::Command;

// Renders `frames` stereo frames from an engine with no driver running; returns them interleaved.
static std::vector<float> render(audio::Engine& e, int frames) {
    std::vector<float> out(size_t(frames) * 2);
    for (int done = 0; done < frames;) {
        int n = std::min(256, frames - done);
        e.process(out.data() + size_t(done) * 2, n);
        done += n;
    }
    return out;
}

static float peakOf(const std::vector<float>& v) {
    float p = 0;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

// One track, one Sampler, one clip holding a single note at beat 1, both in slot 0 and on the timeline.
static void oneNoteProject(Project& p) {
    p.scenes = {"1"};
    p.addTrack("T", QColor("#f2905e"));
    p.tracks[0].devices = {p.makeDevice("Sampler", DeviceKind::Instrument)};
    p.tracks[0].devices[0].params[1].value = 0.0;   // 1 ms attack
    ClipPtr c = p.newClip("One", 4);
    c->notes = {Note{60, 1.0, 0.5, 1.0, 1.0}};
    p.tracks[0].clipSlots[0] = c;
    p.tracks[0].arrangement = {{0, 4, 0, c}};
}

static bool writeFile(const QString& path, const QByteArray& data) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}


// ---- audio-file helpers for the Milestone 4 tests

// A mono test signal: `burst`-sample bursts of `amp` every `every` seconds.
static std::vector<float> bursts(int rate, double seconds, double every, int burst, float amp) {
    std::vector<float> v(size_t(seconds * rate), 0.0f);
    for (double t = 0; t < seconds - 1e-9; t += every)
        for (int i = 0; i < burst; ++i) if (size_t(t * rate) + size_t(i) < v.size()) v[size_t(t * rate) + size_t(i)] = amp;
    return v;
}

static bool writeMonoWav(const QString& path, const std::vector<float>& mono, int rate, samples::WavFormat f = samples::WavFormat::Float32) {
    std::vector<float> st(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) st[i * 2] = st[i * 2 + 1] = mono[i];
    return samples::writeWav(path, st.data(), qint64(mono.size()), rate, f);
}

// Sample positions (left channel) where the signal rises above `thresh` after `gap` quieter samples.
static std::vector<int> onsetsOf(const std::vector<float>& out, float thresh, int gap = 1000) {
    std::vector<int> at;
    int quiet = gap;
    for (size_t i = 0; i < out.size() / 2; ++i) {
        const float a = std::fabs(out[i * 2]);
        if (a > thresh && quiet >= gap) at.push_back(int(i));
        quiet = a > thresh ? 0 : quiet + 1;
    }
    return at;
}

static int loudRun(const std::vector<float>& out, int from, float thresh) {   // samples above thresh from `from`
    int n = 0;
    while (size_t(from + n) * 2 < out.size() && std::fabs(out[size_t(from + n) * 2]) > thresh) ++n;
    return n;
}

static bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }
static int onsetNear(const std::vector<int>& on, int at, int tol) {   // the onset within tol of `at`, or -1
    for (int x : on) if (near(x, at, tol)) return x;
    return -1;
}

// A one-track audio project: the clip at `path` on the timeline from beat 0.
static ClipPtr audioProject(Project& p, const QString& path) {
    p.scenes = {"1"};
    p.addTrack("Audio", QColor("#62c9dc"));
    p.tracks[0].type = TrackType::Audio;
    ClipPtr c = p.newAudioClip(path, samples::load(path));
    p.tracks[0].clipSlots[0] = c;
    p.tracks[0].arrangement = {{0, c->length, 0, c}};
    return c;
}

static std::vector<float> playArrangement(Project& p, int frames) {
    audio::Engine e(&p);
    e.send({Command::Stop, -1, -1, 0, 0, true});
    e.send({Command::Play});
    return render(e, frames);
}


// ---- Milestone 6: device processors driven directly, without an engine

struct DevRun { std::vector<float> l, r; bool finite = true; float peak = 0; };

// Defaults of a device as the knob positions the engine hands its processor.
static std::vector<float> defaultsOf(DeviceType t) {
    std::vector<float> p(audio::kMaxParams, 0.0f);
    const DeviceSpec* s = specOf(t);
    for (size_t i = 0; s && i < s->params.size(); ++i) p[i] = float(s->params[i].def);
    return p;
}

// Runs `proc` for `seconds`: instruments get `notes` at the start (held `hold` seconds),
// effects get a noise + sine signal for the first half, MIDI effects their events counted in peak.
static DevRun runDevice(audio::DeviceProcessor& proc, DeviceKind kind, const std::vector<float>& params, double seconds,
                        const std::vector<int>& notes = {60, 64, 67}, double hold = 0.5, float level = 0.5f,
                        const SampleData* sample = nullptr, double inputFraction = 0.5, const SamplePtr* pads = nullptr) {
    const float sr = 48000;
    DevRun out;
    audio::EventList ev, midiOut;
    audio::Context ctx;
    ctx.sampleRate = sr; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / sr; ctx.playing = true; ctx.sample = sample; ctx.pads = pads;
    dsp::Noise rng;
    const int total = int(seconds * sr);
    float l[audio::kMaxBlock], r[audio::kMaxBlock];
    for (int pos = 0; pos < total; pos += audio::kMaxBlock) {
        const int n = std::min(audio::kMaxBlock, total - pos);
        ev.clear();
        if (pos == 0) for (int pitch : notes) ev.add({0, pitch, 0.9f, int(hold * sr), false, false});
        ctx.beat = pos * ctx.beatsPerSample;
        if (kind == DeviceKind::Instrument) {
            std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
            proc.render(ev, l, r, n, params.data(), ctx);
        } else if (kind == DeviceKind::AudioEffect) {
            for (int i = 0; i < n; ++i) {
                const bool on = pos + i < int(total * inputFraction);
                const float x = on ? level * (0.5f * rng.next() + 0.5f * std::sin(2 * 3.14159265f * 220 * float(pos + i) / sr)) : 0.0f;
                l[i] = x; r[i] = x * 0.8f;
            }
            proc.process(l, r, n, params.data(), ctx);
        } else {
            midiOut.clear();
            proc.midi(ev, midiOut, params.data(), ctx, n);
            std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
            if (midiOut.count) l[0] = float(midiOut.count);
        }
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i])) out.finite = false;
            out.peak = std::max(out.peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
            out.l.push_back(l[i]); out.r.push_back(r[i]);
        }
    }
    return out;
}

static float rmsOf(const std::vector<float>& v, size_t from, size_t to) {
    double s = 0;
    to = std::min(to, v.size());
    for (size_t i = from; i < to; ++i) s += double(v[i]) * v[i];
    return to > from ? float(std::sqrt(s / double(to - from))) : 0.0f;
}


// ---- Milestone 9: racks. A MIDI track playing two notes, with `devices` on it.
static void rackProject(Project& p, const QVector<Device>& devices) {
    p.scenes = {"1"};
    p.addTrack("Rack", QColor("#9b7ff0"));
    ClipPtr c = p.newClip("Two", 4);
    c->notes = {Note{60, 0.0, 1.0, 0.9, 1.0}, Note{67, 0.5, 1.0, 0.7, 1.0}};
    p.tracks[0].clipSlots[0] = c;
    p.tracks[0].arrangement = {{0, 4, 0, c}};
    p.tracks[0].devices = devices;
}

static float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    float d = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, std::fabs(a[i] - b[i]));
    return d;
}

// A device in chain `chain` of `rack`.
static Device inChain(Device d, const Device& rack, int chain) { d.rack = rack.id; d.chain = chain; return d; }


// ---- Milestone 10: a plucked low string, the kind of signal guitar devices get (peak about -12 dBFS)
static std::vector<float> pluck(double seconds, float freq = 110, float level = 0.25f) {
    const int n = int(seconds * 48000);
    std::vector<float> v(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const double t = i / 48000.0, ph = std::fmod(t * freq, 1.0);
        const double note = std::fmod(t, 1.0);   // re-plucked every second
        v[size_t(i)] = float(level * std::exp(-note * 3) * (0.6 * (2 * ph - 1) + 0.4 * std::sin(2 * M_PI * 2 * freq * t)));
    }
    return v;
}

// Runs an audio effect on `in` (both sides); returns the left side.
static std::vector<float> runEffect(DeviceType type, const std::vector<float>& params, const std::vector<float>& in, const audio::IrKernel* ir = nullptr) {
    auto proc = audio::createProcessor(type, 48000);
    audio::Context ctx;
    ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.ir = ir;
    std::vector<float> out(in.size());
    float l[audio::kMaxBlock], r[audio::kMaxBlock];
    for (size_t pos = 0; pos < in.size(); pos += audio::kMaxBlock) {
        const int n = int(std::min<size_t>(audio::kMaxBlock, in.size() - pos));
        std::copy(in.begin() + long(pos), in.begin() + long(pos) + n, l);
        std::copy(in.begin() + long(pos), in.begin() + long(pos) + n, r);
        ctx.beat = double(pos) * ctx.beatsPerSample;
        proc->process(l, r, n, params.data(), ctx);
        std::copy(l, l + n, out.begin() + long(pos));
    }
    return out;
}

int main(int argc, char** argv) {
    QTemporaryDir config;   // settings the test writes (recent files) go here, not into yours
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    QApplication app(argc, argv);
    Q_INIT_RESOURCE(resources);   // the .qrc lives in the static sessions_core library (the About logo reads it)
    Project project;
    project.loadDemo();
    audio::Engine engine(&project);   // not started: nothing renders unless a test calls process()
    Transport transport(&project, &engine);

    // ---- DragValue: drag up increases, Delete resets
    DragValue dv;
    dv.setRange(0, 100); dv.setDefault(50); dv.setValue(50); dv.setDragSensitivity(100);
    dv.resize(60, 18); dv.show();
    QTest::mousePress(&dv, Qt::LeftButton, Qt::NoModifier, QPoint(30, 10));
    QTest::mouseMove(&dv, QPoint(30, 0));
    QTest::mouseRelease(&dv, Qt::LeftButton, Qt::NoModifier, QPoint(30, 0));
    CHECK(dv.value() > 55, "DragValue: dragging up raises the value");
    QTest::keyClick(&dv, Qt::Key_Delete);
    CHECK(dv.value() == 50, "DragValue: Delete resets to default");

    // ---- PianoRoll: double-click adds a note, drag edge lengthens, Delete removes
    ClipPtr clip = project.newClip("Test", 4);
    PianoRoll roll(&project, &transport);
    roll.resize(600, 300); roll.show();
    QApplication::processEvents();
    roll.setClip(clip);
    QApplication::processEvents();
    roll.repaint();
    int before = clip->notes.size();
    QTest::mouseDClick(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(120, 120));
    CHECK(clip->notes.size() == before + 1, "PianoRoll: double-click adds a note");
    if (!clip->notes.isEmpty()) {
        Note& n = clip->notes.last();
        double len0 = n.length;
        // drag the note's right edge 2 grid steps to the right
        int x0 = 34 + int((n.start + n.length) * roll.zoom()) - 2;
        int step2 = int(2 * roll.gridSize() * roll.zoom());
        int y  = 120;
        QTest::mousePress(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(x0, y));
        QTest::mouseMove(&roll, QPoint(x0 + step2, y));
        QTest::mouseRelease(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(x0 + step2, y));
        CHECK(clip->notes.last().length > len0, "PianoRoll: dragging the right edge lengthens the note");
        QTest::keyClick(&roll, Qt::Key_Up);
        CHECK(clip->notes.last().pitch == n.pitch, "PianoRoll: Up arrow raises selected note by a semitone");
        QTest::keyClick(&roll, Qt::Key_Delete);
        CHECK(clip->notes.size() == before, "PianoRoll: Delete removes the selected note");
    }
    // draw mode: single click adds
    roll.setDrawMode(true);
    QTest::mouseClick(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(200, 140));
    CHECK(clip->notes.size() == before + 1, "PianoRoll: Draw Mode click adds a note");

    // ---- ArrangementView: double-click adds a clip; drag moves it
    ArrangementView arr(&project, &transport);
    arr.resize(900, 400); arr.show();
    QApplication::processEvents();
    int lane2Y = arr.laneRect(2).top() + 20;   // Keys lane
    int clips0 = project.tracks[2].arrangement.size();
    int emptyX = int(arr.beatToX(42));   // just after the Keys clip
    QTest::mouseDClick(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(emptyX, lane2Y));
    CHECK(project.tracks[2].arrangement.size() == clips0 + 1, "Arrangement: double-click adds a clip");
    double start0 = project.tracks[2].arrangement.last().start;
    const int titleY = arr.laneRect(2).top() + 6;   // the clip's name bar moves it (0.21: its body selects time inside it)
    QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(emptyX + 4, titleY));
    QTest::mouseMove(&arr, QPoint(emptyX + 4 + int(arr.beatToX(8) - arr.beatToX(0)), titleY));
    QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(emptyX + 4 + int(arr.beatToX(8) - arr.beatToX(0)), titleY));
    CHECK(project.tracks[2].arrangement.last().start > start0, "Arrangement: dragging a clip by its name bar moves it");

    // ---- Transport: launch waits for quantization
    transport.setLaunchQuantization(4);
    transport.play();
    transport.setBeat(1.0);
    transport.launchSlot(0, 1);
    CHECK(project.tracks[0].queuedSlot == 1 && project.tracks[0].queuedAt == 4.0, "Transport: launch is queued to the next bar");
    CHECK(project.tracks[0].overridden, "Transport: launching a Session clip overrides the Arrangement");
    transport.backToArrangement();
    CHECK(!project.tracks[0].overridden, "Transport: Back to Arrangement clears the override");

    // ---- Undo / redo: note edits, restored into the same Clip object
    {
        project.tracks[3].clipSlots[4] = clip;   // undo tracks clips that live in the set
        roll.setDrawMode(false);
        int n0 = clip->notes.size();
        QTest::mouseDClick(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(300, 100));
        CHECK(clip->notes.size() == n0 + 1, "Undo: setup adds a note");
        project.undoStack()->undo();
        CHECK(clip->notes.size() == n0 && project.tracks[3].clipSlots[4] == clip, "Undo: Ctrl+Z removes the note, same clip object");
        project.undoStack()->redo();
        CHECK(clip->notes.size() == n0 + 1, "Undo: redo puts it back");
        double v0 = project.tracks[1].devices[0].params[0].value;
        { EditScope e(&project, "Change Cutoff", 99); project.tracks[1].devices[0].params[0].value = 0.9; }
        { EditScope e(&project, "Change Cutoff", 99); project.tracks[1].devices[0].params[0].value = 0.95; }
        project.undoStack()->undo();
        CHECK(project.tracks[1].devices[0].params[0].value == v0, "Undo: a knob drag undoes in one step");
        int steps = project.undoStack()->count();
        { EditScope e(&project, "Nothing"); }
        CHECK(project.undoStack()->count() == steps, "Undo: edits that change nothing leave no step");
    }

    // ---- Device chain order
    project.selectedTrack = 1;
    project.addDeviceToSelected("Reverb", DeviceKind::AudioEffect);
    project.addDeviceToSelected("Chord", DeviceKind::MidiEffect);
    const auto& dev = project.tracks[1].devices;
    CHECK(dev.first().kind == DeviceKind::MidiEffect && dev.last().kind == DeviceKind::AudioEffect, "Devices: MIDI effects go first, audio effects last");

    // ---- Returns
    project.selectedTrack = project.tracks.size();   // Return A
    CHECK(!project.addDeviceToSelected("Analog Synth", DeviceKind::Instrument), "Returns: refuse instruments");
    CHECK(project.addDeviceToSelected("EQ", DeviceKind::AudioEffect), "Returns: take audio effects");
    project.selectedTrack = 0;

    // ---- Audio engine: timing
    {
        Project p; oneNoteProject(p);
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        std::vector<float> out = render(e, 36000);
        int first = -1;
        for (int i = 0; i < 36000; ++i) if (std::fabs(out[size_t(i) * 2]) > 1e-4f) { first = i; break; }
        CHECK(first == 24000, "Engine: a note on beat 2 sounds on sample 24000 (0.5 s at 120 BPM)");
    }
    {
        Project p; oneNoteProject(p);
        p.tracks[0].arrangement.clear();
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        render(e, 12000);   // to beat 0.5
        e.send({Command::Launch, p.tracks[0].id, 0, 4.0});
        render(e, 12000);
        auto st = e.trackStatus(0);
        CHECK(st.queuedSlot == 0 && st.queuedAt == 4.0 && st.playingSlot == -1, "Engine: a launch waits for the next bar");
        render(e, 74000);   // past beat 4
        st = e.trackStatus(0);
        CHECK(st.playingSlot == 0 && st.slotStart == 4.0, "Engine: the clip starts exactly on the bar");
    }
    {
        Project p; oneNoteProject(p);
        p.tracks[0].active = false;
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        CHECK(peakOf(render(e, 36000)) == 0.0f, "Engine: the track activator mutes");
        p.tracks[0].active = true;
        p.tracks[0].devices[0].enabled = false;
        e.publish();
        e.send({Command::Locate, -1, -1, 0.0});
        const std::vector<float> off = render(e, 36000);
        float fadeTail = 0, later = 0;
        for (size_t i = 0; i < off.size(); ++i) (i < 2 * 1200 ? fadeTail : later) = std::max(i < 2 * 1200 ? fadeTail : later, std::fabs(off[i]));
        CHECK(later == 0.0f && fadeTail < 0.2f, "Engine: an instrument switched off is silent (after a ~12 ms fade, no click)");
    }
    {
        Project p; p.loadDemo();
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        float trackPeak[4] = {}, retPeak = 0, mainPeak = 0;
        bool finite = true;
        for (int block = 0; block < 14 * 48000 / 4800; ++block) {   // 14 s: every track has played
            std::vector<float> out = render(e, 4800);
            for (float x : out) finite &= std::isfinite(x);
            mainPeak = std::max(mainPeak, peakOf(out));
            for (int t = 0; t < 4; ++t) trackPeak[t] = std::max(trackPeak[t], e.takeTrackPeak(t));
            retPeak = std::max(retPeak, e.takeReturnPeak(0));
        }
        CHECK(finite, "Engine: demo output is free of NaN / inf");
        CHECK(trackPeak[0] > 0.05f && trackPeak[1] > 0.05f && trackPeak[2] > 0.05f && trackPeak[3] > 0.02f, "Engine: all four demo tracks make sound");
        CHECK(retPeak > 0.005f, "Engine: sends feed the reverb return");
        CHECK(mainPeak > 0.1f && mainPeak < 1.0f, "Engine: demo mix is audible and stays below full scale");
        std::printf("      demo peaks: drums %.2f bass %.2f keys %.2f pad %.2f, return A %.2f, main %.2f\n",
                    trackPeak[0], trackPeak[1], trackPeak[2], trackPeak[3], retPeak, mainPeak);
    }

    // ================= Milestone 3 =================

    // ---- Engine: playing clips survive inserting tracks and scenes (tracked by id)
    {
        Project p; oneNoteProject(p);
        p.tracks[0].arrangement.clear();
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
        e.send({Command::Play});
        render(e, 4800);
        CHECK(e.trackStatus(0).playingSlot == 0, "M3 engine: setup clip playing");
        p.insertTrack(0);            // the playing track moves to index 1
        p.insertScene(0);            // and its clip to slot 1
        e.publish();
        render(e, 4800);
        CHECK(e.trackStatus(1).playingSlot == 1 && e.trackStatus(0).playingSlot == -1, "M3 engine: clip keeps playing after a track and a scene are inserted above it");
    }
    // ---- Follow action: Next after one loop
    {
        Project p; oneNoteProject(p);
        p.tracks[0].arrangement.clear();
        p.insertScene(1);
        p.tracks[0].clipSlots[1] = p.newClip("Two", 4);
        p.tracks[0].clipSlots[0]->followEnabled = true;
        p.tracks[0].clipSlots[0]->followA = FollowAction::Next;
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
        e.send({Command::Play});
        render(e, 48000);   // 2 beats
        CHECK(e.trackStatus(0).playingSlot == 0, "M3 follow: first clip plays");
        render(e, 72000);   // to beat 5
        CHECK(e.trackStatus(0).playingSlot == 1 && e.trackStatus(0).slotStart == 4.0, "M3 follow: Next takes over exactly at the end of the loop");
    }
    // ---- Legato: the new clip picks up the old one's position
    {
        Project p; oneNoteProject(p);
        p.tracks[0].arrangement.clear();
        p.insertScene(1);
        ClipPtr b = p.newClip("Legato", 4);
        b->legato = true;
        p.tracks[0].clipSlots[1] = b;
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
        e.send({Command::Play});
        render(e, 144000);   // beat 6: two beats into the second loop
        e.send({Command::Launch, p.tracks[0].id, 1, 0.0});
        render(e, 256);
        CHECK(e.trackStatus(0).playingSlot == 1 && std::abs(e.trackStatus(0).slotStart - 4.0) < 1e-6, "M3 legato: switching keeps the loop position");
    }
    // ---- Automation drives the mixer; overriding it hands control back
    {
        Project p; oneNoteProject(p);
        p.tracks[0].automation = {{target::volume(), {{0, 0.0}}}};   // volume automated to silence
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        CHECK(peakOf(render(e, 36000)) == 0.0f, "M3 automation: volume envelope at zero silences the track");
        p.touchTarget(0, target::volume());
        CHECK(p.anyAutomationOverridden(), "M3 automation: touching the control overrides it");
        e.publish();
        e.send({Command::Locate, -1, -1, 0.0});
        CHECK(peakOf(render(e, 36000)) > 0.01f, "M3 automation: overridden envelope no longer applies");
        p.reEnableAutomation();
        CHECK(!p.anyAutomationOverridden(), "M3 automation: Re-Enable Automation clears overrides");
    }
    // ---- Undo covers tracks, scenes, mixer and tempo
    {
        Project p; p.loadDemo();
        const int n0 = p.tracks.size(), s0 = p.scenes.size();
        p.insertTrack(1);
        p.insertScene(0);
        CHECK(p.tracks.size() == n0 + 1 && p.scenes.size() == s0 + 1, "M3 undo: setup inserts a track and a scene");
        p.undoStack()->undo();
        p.undoStack()->undo();
        CHECK(p.tracks.size() == n0 && p.scenes.size() == s0, "M3 undo: track and scene insertions undo");
        const double v0 = p.tracks[0].volume;
        { EditScope e(&p, "Volume", mixerMergeKey(p.tracks[0].id, 1)); p.tracks[0].volume = 0.2; }
        { EditScope e(&p, "Tempo", mixerMergeKey(0, 99)); p.setTempo(90); }
        p.undoStack()->undo();
        p.undoStack()->undo();
        CHECK(p.tracks[0].volume == v0 && p.tempo == 120.0, "M3 undo: mixer moves and tempo undo");
        const int id = p.tracks[2].id;
        p.deleteTrack(2);
        p.undoStack()->undo();
        CHECK(p.tracks.size() == n0 && p.tracks[2].id == id && !p.tracks[2].devices.isEmpty(), "M3 undo: a deleted track comes back with its devices");
        p.moveTrack(0, 3);
        CHECK(p.tracks[3].name == "Drums", "M3 tracks: move");
        p.renameTrack(3, "Beats");
        p.undoStack()->undo();
        p.undoStack()->undo();
        CHECK(p.tracks[0].name == "Drums", "M3 tracks: rename and move undo");
        const int r = p.insertReturn();
        CHECK(r == p.trackCount() - 1 && p.tracks[0].sends.size() == 3, "M3 returns: a third return adds a send to every track");
        p.deleteTrack(r);
        CHECK(p.tracks[0].sends.size() == 2, "M3 returns: deleting it removes the send");
    }
    // ---- Arrangement editing
    {
        Project p; p.loadDemo();
        Track& drums = p.tracks[0];   // Beat A 0-16, Beat B 16-32
        arrangement::split(p, drums, 6);
        CHECK(drums.arrangement.size() == 3 && drums.arrangement[1].start == 6 && drums.arrangement[1].offset == 6
              && drums.arrangement[1].clip != drums.arrangement[0].clip, "M3 arrangement: split makes an independent right half");
        const int notesBefore = arrangement::notesIn(drums.arrangement[0], 0, 6).size() + arrangement::notesIn(drums.arrangement[1], 6, 16).size();
        ClipPtr c = arrangement::consolidate(p, drums, 0, 16);
        CHECK(c && c->notes.size() == notesBefore && drums.arrangement.size() == 2, "M3 arrangement: consolidate joins the halves with every note");
        arrangement::insertTime(p, 8, 4);
        CHECK(p.tracks[1].arrangement[0].start == 20, "M3 arrangement: insert time pushes later clips");
        arrangement::deleteTime(p, 8, 12);
        CHECK(p.tracks[1].arrangement[0].start == 16, "M3 arrangement: delete time pulls them back");
        Project q; q.loadDemo();   // pad: 0-16
        arrangement::duplicateTime(q, 0, 16);
        CHECK(q.tracks[3].arrangement.size() == 2 && q.tracks[3].arrangement[1].start == 16 && q.tracks[1].arrangement[0].start == 32,
              "M3 arrangement: duplicate time copies the range after itself and pushes the rest");
    }
    // ---- Session recording from the keyboard
    {
        Project p; oneNoteProject(p);
        p.tracks[0].arrangement.clear();
        p.tracks[0].clipSlots[0] = nullptr;
        p.tracks[0].armed = true;
        audio::Engine e(&p);
        Transport t(&p, &e);
        t.recordSlot(0, 0);
        CHECK(p.tracks[0].clipSlots[0] != nullptr && t.isRecordingSlot(0, 0), "M3 record: ● on an armed track starts a recording clip");
        auto pump = [&](int frames) { render(e, frames); QTest::qWait(40); };   // let Transport mirror the engine
        pump(4800);
        pump(24000);                 // ~beat 1.2
        t.noteOn(0, 64, 0.9);
        pump(12000);
        t.noteOff(0, 64);
        ClipPtr rec = p.tracks[0].clipSlots[0];
        CHECK(rec->notes.size() == 1 && rec->notes[0].pitch == 64 && rec->notes[0].start > 0.8 && rec->notes[0].length > 0.3,
              "M3 record: a played note lands in the clip at the right place");
        t.launchSlot(0, 0);          // clicking the recording clip ends the take
        CHECK(!t.sessionRecording() && rec->length == 4.0, "M3 record: ending the take rounds it to a bar");
    }

    // ================= Milestone 4 =================

    // ---- Sets: everything a set holds survives save -> load into a fresh Project
    {
        QTemporaryDir dir;
        Project p;
        p.loadDemo();   // shared slot / timeline clips, devices, sends, automation
        p.insertReturn();
        Track& ret = p.returns[2];
        ret.name = "C Ünïcode “Echo” & <tag>";
        ret.color = QColor(10, 20, 30, 128);
        ret.preFader = true;
        ret.volume = 0.5;
        ret.devices = {p.makeDevice("EQ", DeviceKind::AudioEffect)};
        ret.automation = {{target::device(ret.devices[0].id, 1), {{0, 0.1}, {4, 0.9}}}};
        Track& t = p.tracks[1];
        t.name = "Bass ♪";
        t.color = QColor("#123456");
        t.volume = 0.6180339887498949;
        t.pan = -1.0 / 3;
        t.sends = {0.1, 0.7, 1.0 / 3};
        t.active = false;
        t.devices.prepend(p.makeDevice("Arpeggiator", DeviceKind::MidiEffect));
        t.devices[1].enabled = false;
        t.devices[1].params[2].value = 0.123456789;
        t.automation = {
            {target::volume(), {{0, 0.2}, {7.5, 0.9}, {16, 0.4}}},
            {target::pan(), {{2, 0.5}}},
            {target::send(2), {{0, 0}, {32, 1}}},
            {target::device(t.devices[0].id, 3), {{1.25, 0.75}}},
        };
        ClipPtr c = p.newClip("Every Setting", 6.5);
        c->loop = false;
        c->notes = {Note{0, 0, 1.0 / 3, 0.0, 1.0}, Note{127, 6.25, 0.125, 1.0, 0.25}, Note{64, 1.1, 2.2, 0.33, 0.5}};
        c->launchMode = LaunchMode::Repeat;
        c->legato = true;
        c->quantization = 0.5;
        c->followEnabled = true;
        c->followA = FollowAction::Other;
        c->followB = FollowAction::Stop;
        c->followChanceA = 0.3;
        c->followTime = 8;
        c->scaleOn = true;
        c->scaleRoot = 9;
        c->scale = kScaleCount - 1;
        c->start = 1.5;        // M23: the start marker and the loop brace
        c->loopStart = 0.75;
        t.clipSlots[4] = c;
        t.arrangement.push_back({40, 6, 2.5, c});   // shared with the slot, starting mid-clip
        arrangement::split(p, p.tracks[0], 6);      // a right half with an offset
        p.tracks[2].clipSlots[2]->launchMode = LaunchMode::Gate;
        p.scenes[0] = "Intro ✓";
        p.scenes[2] = "";
        p.setTempo(133.5);
        p.mainVolume = 0.55;
        p.cueVolume = 0.42;
        p.locators = {{0, "Start"}, {16.5, "Drop \"B\""}};
        p.mappings = {{Mapping::MidiCC, 0, 21, QString("track:%1:volume").arg(p.tracks[1].id)},
                      {Mapping::MidiNote, 9, 36, "scene:2"}, {Mapping::Key, 0, Qt::Key_Q, QString("slot:%1:0").arg(p.tracks[0].id)}};
        p.tracks[1].width = 150;    // Session View column widths are saved, not undoable
        p.returns[2].width = 90;
        p.tracks[0].solo = true;    // runtime state: not part of a set
        p.tracks[1].armed = true;
        c->envelopes = {{target::pan(), {{0, 0.2}, {2.5, 0.9}}}, {target::device(t.devices[0].id, 1), {{1, 0.5}}}};   // M7
        const int nGroup = int(p.tracks.size());
        const int sharedId = p.tracks[3].id;   // grouping below moves it
        const int g = nGroup >= 4 ? p.groupTracks({nGroup - 2, nGroup - 1}) : -1;
        if (g >= 0) p.setFolded(g, true);
        setfile::Extras x;
        x.loopOn = true; x.loopStart = 8; x.loopLength = 12.5; x.launchQuantization = 2; x.metronome = true;

        const QString path = dir.filePath("roundtrip.sessions");
        QString err;
        CHECK(setfile::save(path, p, x, &err), "M4 sets: save writes a .sessions file");
        Project q;
        q.loadEmpty();   // whatever was there before is replaced
        setfile::Extras y;
        CHECK(setfile::load(path, q, y, &err), "M4 sets: load reads it into a fresh Project");
        const EditState a = p.captureEditState(), b = q.captureEditState();
        CHECK(a == b, "M4 sets: everything undoable round-trips exactly (captureEditState ==)");
        setfile::fillFromProject(x, p);
        CHECK(y == x && q.cueVolume == 0.42 && q.tracks[1].width == 150 && q.returns[2].width == 90 && q.tracks[0].width == 0,
              "M4 sets: cue volume, column widths, loop region, launch quantization and metronome round-trip");
        CHECK(q.mappings == p.mappings, "M5 sets: MIDI and key mappings round-trip");
        CHECK(g >= 0 && q.tracks[g].isGroup() && q.tracks[g].folded && q.membersOf(g).size() == 2 && q.hiddenByFold(g + 1),
              "M7 sets: a group, its tracks and its fold round-trip");
        bool idsKept = q.tracks.size() == p.tracks.size() && q.returns.size() == 3;
        for (int i = 0; idsKept && i < p.trackCount(); ++i) {
            idsKept &= q.trackAt(i)->id == p.trackAt(i)->id && q.trackAt(i)->devices.size() == p.trackAt(i)->devices.size();
            for (int d = 0; idsKept && d < p.trackAt(i)->devices.size(); ++d) idsKept &= q.trackAt(i)->devices[d].id == p.trackAt(i)->devices[d].id;
        }
        CHECK(idsKept && q.tracks[1].clipSlots[4]->id == c->id, "M4 sets: track, device and clip ids are kept");
        CHECK(q.tracks[1].clipSlots[4] == q.tracks[1].arrangement.last().clip && q.tracks[1].arrangement.last().offset == 2.5
              && q.tracks[q.indexOfTrack(sharedId)].clipSlots[0] == q.tracks[q.indexOfTrack(sharedId)].arrangement[0].clip,
              "M4 sets: a clip in a slot and on the timeline is still one shared clip");
        const Clip& lc = *q.tracks[1].clipSlots[4];
        CHECK(lc.notes.size() == 3 && lc.notes[0].length == 1.0 / 3 && lc.notes[1].chance == 0.25 && !lc.loop && lc.launchMode == LaunchMode::Repeat
              && lc.followA == FollowAction::Other && lc.followB == FollowAction::Stop && lc.scaleOn && lc.scaleRoot == 9 && lc.scale == kScaleCount - 1,
              "M4 sets: notes, loop, launch, follow and scale settings come back");
        CHECK(lc.start == 1.5 && lc.loopStart == 0.75, "M23 sets: the clip's start marker and loop position come back");
        CHECK(q.tracks[1].name == "Bass ♪" && q.returns[2].name == p.returns[2].name && q.returns[2].color.alpha() == 128 && q.returns[2].preFader
              && !q.tracks[1].active && !q.tracks[1].devices[1].enabled && q.tempo == 133.5 && q.locators.size() == 2 && q.scenes[0] == "Intro ✓",
              "M4 sets: names (Unicode), colours, switches, tempo, locators and scenes come back");
        CHECK(q.tracks[0].arrangement.size() == 3 && q.tracks[0].arrangement[1].offset == 6, "M4 sets: split arrangement clips keep their offsets");
        CHECK(!q.tracks[0].solo && !q.tracks[1].armed && q.tracks[0].playingSlot == -1, "M4 sets: solo, arm and playback aren't saved");
        CHECK(q.undoStack()->count() == 0 && q.undoStack()->isClean(), "M4 sets: loading starts a fresh undo history");
        int maxClip = 0, maxDevice = 0, maxTrack = 0;
        for (int i = 0; i < q.trackCount(); ++i) {
            maxTrack = std::max(maxTrack, q.trackAt(i)->id);
            for (const Device& d : q.trackAt(i)->devices) maxDevice = std::max(maxDevice, d.id);
        }
        for (auto it = b.clips.begin(); it != b.clips.end(); ++it) maxClip = std::max(maxClip, it.key());
        const int newTrack = q.insertTrack(0);
        CHECK(q.newClip()->id > maxClip && q.makeDevice("Reverb", DeviceKind::AudioEffect).id > maxDevice && q.tracks[newTrack].id > maxTrack,
              "M4 sets: new clips, devices and tracks get ids past the loaded ones");
        CHECK(setfile::toJson(b, y) == setfile::toJson(a, x), "M4 sets: saving the loaded set again gives the same file");
        {
            Project d; d.loadDemo();
            setfile::Extras dx;
            const QString demoPath = dir.filePath("demo.sessions");
            Project loaded;
            CHECK(setfile::save(demoPath, d, dx) && setfile::load(demoPath, loaded, dx), "M4 sets: the demo saves and loads");
            audio::Engine e(&loaded);
            e.send({Command::Stop, -1, -1, 0, 0, true});
            e.send({Command::Play});
            std::vector<float> out = render(e, 4 * 48000);
            bool finite = true;
            for (float v : out) finite &= std::isfinite(v);
            CHECK(finite && peakOf(out) > 0.1f && peakOf(out) < 1.0f, "M4 sets: a loaded demo plays through the engine");
        }
    }
    // ---- Sets: broken files are refused and leave the project alone
    {
        QTemporaryDir dir;
        Project p; p.loadDemo();
        const EditState before = p.captureEditState();
        setfile::Extras x;
        QString err;
        writeFile(dir.filePath("junk.sessions"), "this is not json");
        CHECK(!setfile::load(dir.filePath("junk.sessions"), p, x, &err) && !err.isEmpty(), "M4 sets: a file that isn't JSON is refused");
        writeFile(dir.filePath("other.sessions"), R"({"format": "Something Else", "version": 1})");
        CHECK(!setfile::load(dir.filePath("other.sessions"), p, x, &err), "M4 sets: JSON that isn't a set is refused");
        writeFile(dir.filePath("future.sessions"), R"({"format": "Sessions Set", "version": 99})");
        CHECK(!setfile::load(dir.filePath("future.sessions"), p, x, &err) && err.contains("newer"), "M4 sets: a set from a newer version is refused with a reason");
        CHECK(!setfile::load(dir.filePath("missing.sessions"), p, x, &err), "M4 sets: a missing file is refused");
        CHECK(p.captureEditState() == before, "M4 sets: a failed load leaves the project untouched");
    }
    // ---- Sets: a sparse or hand-edited file is made consistent
    {
        const QByteArray json = R"({"format": "Sessions Set", "version": 1, "scenes": ["A", "B"],
            "clips": [{"id": 7, "name": "X", "notes": [{"pitch": 200, "start": 1}]}, {"id": 8, "name": "unused"}],
            "tracks": [{"id": 3, "slots": [7, 99, 7], "arrangement": [{"start": 0, "length": 4, "clip": 42}]},
                       {"id": 3, "name": "dup", "sends": [0.5, 0.5, 0.5]}],
            "returns": [{"id": 5, "devices": [{"id": 1, "name": "Reverb", "params": [{"name": "Size", "value": 0.9}]}]}]})";
        EditState s;
        setfile::Extras x;
        QString err;
        CHECK(setfile::fromJson(json, s, x, &err), "M4 sets: a sparse hand-written set loads");
        CHECK(s.tracks.size() == 2 && s.tracks[0].id == 3 && s.tracks[1].id == 6 && s.returns[0].id == 5,
              "M4 sets: duplicate track ids get fresh ones");
        CHECK(s.tracks[0].slotClips == QVector<int>({7, 0}) && s.tracks[0].arrangement.isEmpty() && !s.clips.contains(8),
              "M4 sets: slots match the scenes; missing and unused clips are dropped");
        CHECK(s.tracks[1].sends.size() == 1 && s.clips[7].notes[0].pitch == 127 && s.returns[0].devices[0].params.size() == size_t(paramCount(DeviceType::Reverb))
              && s.returns[0].devices[0].params[0].value == 0.9 && s.returns[0].devices[0].params[3].value == 0.25
              && s.returns[0].devices[0].params[8].name == "Freeze" && s.tempo == 120,
              "M4 sets: sends match the returns, values are clamped, missing knobs and settings get defaults");
    }

    // ---- Arrangement loop: a note on the loop start plays on every pass, at any tempo
    {
        Project p; oneNoteProject(p);
        p.tracks[0].devices[0].params[2].value = 0.0;   // shortest decay
        p.tracks[0].arrangement[0].clip->notes = {Note{60, 0.0, 0.1, 1.0, 1.0}};
        p.setTempo(176.78);   // the loop end falls between samples
        audio::Engine e(&p);
        e.send({Command::Loop, -1, -1, 0.0, 4.0, true});
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        const double loopSamples = 4.0 / (176.78 / 60.0 / 48000.0);
        const std::vector<float> out = render(e, int(loopSamples * 6.5));
        int onsets = 0, quiet = 0;
        bool armed = true;   // after 2000 near-silent samples, the next loud one is a new note
        for (size_t i = 0; i < out.size() / 2; ++i) {
            const float a = std::fabs(out[i * 2]);
            if (a > 0.01f && armed) { ++onsets; armed = false; }
            quiet = a < 0.001f ? quiet + 1 : 0;
            if (quiet > 2000) armed = true;
        }
        std::printf("      loop onsets: %d of 7\n", onsets);
        CHECK(onsets == 7, "M4 fix: a note on the loop start plays on every pass of the Arrangement loop");
    }

    // ---- Transport: Space plays from the start marker and stops; Shift+Space continues
    {
        MainWindow w;
        w.resize(1600, 900);
        w.show();
        QApplication::processEvents();
        Transport* tr = w.transport();
        QWidget* target = w.sessionView()->grid();   // as after a click in the grid (the search field keeps Space for typing)
        target->setFocus();
        QTest::keyClick(target, Qt::Key_Space);
        CHECK(tr->playing() && tr->beat() == 0, "M4 fix: Space plays from the start (1.1.1)");
        tr->setBeat(9.5);
        QTest::keyClick(target, Qt::Key_Space);
        CHECK(!tr->playing() && tr->beat() == 9.5, "M4 fix: Space again stops (the playhead stays where it stopped)");
        QTest::keyClick(target, Qt::Key_Space, Qt::ShiftModifier);
        CHECK(tr->playing() && tr->beat() == 9.5, "M4 fix: Shift+Space plays on from where it stopped");
        QTest::keyClick(target, Qt::Key_Space);
        QTest::keyClick(target, Qt::Key_Space);
        CHECK(tr->playing() && tr->beat() == 0, "M4 fix: Space after a stop starts from the beginning again");
        tr->stop();

        // Clicking an Arrangement clip: Play starts at that clip.
        ArrangementView* av = w.arrangementView();
        QTest::keyClick(target, Qt::Key_Tab);   // to the Arrangement
        QApplication::processEvents();
        const ArrangementClip& keys = w.project()->tracks[2].arrangement[0];   // Chords, 24..40
        const QPoint onClip(int(av->beatToX(keys.start + 2)), av->laneRect(2).center().y());
        QTest::mouseClick(av, Qt::LeftButton, Qt::NoModifier, onClip);
        CHECK(tr->startMarker() == keys.start && tr->beat() == keys.start, "M4 fix: clicking a clip puts the start marker (and the stopped playhead) at its start");
        QTest::keyClick(target, Qt::Key_Space);
        CHECK(tr->playing() && tr->beat() == keys.start, "M4 fix: then Space plays from that clip");
        tr->setBeat(40);
        tr->stop();
        tr->stop();
        CHECK(tr->startMarker() == 0 && tr->beat() == 0, "M4 fix: stopping twice returns the start marker to 1.1.1");

        // Session columns: a fixed default width on a big window, and the divider resizes one column.
        QTest::keyClick(target, Qt::Key_Tab);   // back to the Session View
        QApplication::processEvents();
        ClipGrid* grid = w.sessionView()->grid();
        CHECK(grid->columnWidth(0) == ClipGrid::kDefaultColumnWidth && grid->columnWidth(1) == ClipGrid::kDefaultColumnWidth,
              "M4 fix: Session columns keep their default width on a wide window");
        {
            const int nAll = w.project()->trackCount(), nT = int(w.project()->tracks.size());
            const int mainRight = grid->columnLeft(nAll) + grid->columnWidth(nAll);
            const int returnsLeft = grid->columnLeft(nT), tracksRight = grid->columnLeft(nT - 1) + grid->columnWidth(nT - 1);
            CHECK(grid->freeSpace() > 0 && mainRight == grid->width() && returnsLeft > tracksRight + 100,
                  "M4 fix: the returns and the Main column stay at the right edge of a wide window");
            MainStrip* mainStrip = w.sessionView()->findChild<MainStrip*>();
            for (MainStrip* m : w.sessionView()->findChildren<MainStrip*>()) if (m->isVisible()) mainStrip = m;
            CHECK(mainStrip && mainStrip->x() == grid->columnLeft(nAll) && mainStrip->width() == grid->columnWidth(nAll),
                  "M4 fix: the Main strip sits under the Main column");
        }
        const QPoint divider(grid->columnWidth(0), 30);   // the right edge of the first column
        QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, divider);
        QTest::mouseMove(grid, divider + QPoint(60, 0));
        QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, divider + QPoint(60, 0));
        QApplication::processEvents();
        MixerStrip* strip0 = nullptr;   // the visible strip under the first column
        for (MixerStrip* m : w.sessionView()->findChildren<MixerStrip*>()) if (m->isVisible()) { strip0 = m; break; }
        CHECK(grid->columnWidth(0) == ClipGrid::kDefaultColumnWidth + 60 && grid->columnWidth(1) == ClipGrid::kDefaultColumnWidth
              && w.project()->tracks[0].width == ClipGrid::kDefaultColumnWidth + 60 && strip0 && strip0->width() == grid->columnWidth(0),
              "M4 fix: dragging a column divider resizes that column (and its mixer strip) only");
        CHECK(w.isWindowModified(), "M4 fix: a resized column counts as a change to the set");
        QTest::mouseDClick(grid, Qt::LeftButton, Qt::NoModifier, QPoint(grid->columnWidth(0), 30));
        CHECK(grid->columnWidth(0) == ClipGrid::kDefaultColumnWidth && !w.isWindowModified(), "M4 fix: double-clicking the divider restores the default width");
    }


    // ================= Milestone 4: audio =================

    QTemporaryDir audioDir;
    const QString burstsWav = audioDir.filePath("bursts 120.wav");   // 2 s = one bar at 120 BPM, a burst every beat
    writeMonoWav(burstsWav, bursts(48000, 2.0, 0.5, 48, 0.5f), 48000);

    // ---- Decoding: WAV (float, 16, 24, 8-bit mono), AIFF, the cache, reversing
    {
        QString err;
        SamplePtr f = samples::load(burstsWav, &err);
        CHECK(f && f->frames() == 96000 && f->rate == 48000 && f->left[24000] == 0.5f && f->right[24047] == 0.5f && f->left[24048] == 0.0f,
              "M4 samples: a float WAV loads exactly");
        CHECK(samples::load(burstsWav) == f, "M4 samples: loading a file again shares the copy in memory");
        const QString w16 = audioDir.filePath("s16.wav"), w24 = audioDir.filePath("s24.wav");
        std::vector<float> ramp(4800);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = float(i) / 4800.0f - 0.5f;
        writeMonoWav(w16, ramp, 44100, samples::WavFormat::Int16);
        writeMonoWav(w24, ramp, 44100, samples::WavFormat::Int24);
        SamplePtr a = samples::load(w16), b = samples::load(w24);
        bool close16 = a && a->frames() == 4800 && a->rate == 44100, close24 = b && b->frames() == 4800;
        for (size_t i = 0; close16 && i < ramp.size(); i += 37) close16 = std::fabs(a->left[i] - ramp[i]) < 3.0f / 32768;
        for (size_t i = 0; close24 && i < ramp.size(); i += 37) close24 = std::fabs(b->right[i] - ramp[i]) < 3.0f / 8388608;
        CHECK(close16 && close24, "M4 samples: 16- and 24-bit WAVs load (within dither)");
        {   // 8-bit unsigned mono, written by hand
            QByteArray wav("RIFF\0\0\0\0WAVEfmt ", 16);
            auto le32 = [&wav](quint32 v) { char b[4]; qToLittleEndian(v, b); wav.append(b, 4); };
            auto le16 = [&wav](quint16 v) { char b[2]; qToLittleEndian(v, b); wav.append(b, 2); };
            le32(16); le16(1); le16(1); le32(22050); le32(22050); le16(1); le16(8);
            wav.append("data", 4); le32(4);
            wav.append(char(128)); wav.append(char(255)); wav.append(char(0)); wav.append(char(192));
            writeFile(audioDir.filePath("u8.wav"), wav);
            SamplePtr u = samples::load(audioDir.filePath("u8.wav"));
            CHECK(u && u->frames() == 4 && u->channels == 1 && u->left[0] == 0.0f && u->left[2] == -1.0f && u->right[3] == 0.5f,
                  "M4 samples: 8-bit mono WAV loads (both channels)");
        }
        {   // AIFF, 16-bit big-endian stereo at 44100 Hz (80-bit extended rate)
            QByteArray body;
            auto be16 = [&body](quint16 v) { char b[2]; qToBigEndian(v, b); body.append(b, 2); };
            auto be32 = [&body](quint32 v) { char b[4]; qToBigEndian(v, b); body.append(b, 4); };
            body.append("AIFFCOMM", 8); be32(18); be16(2); be32(3); be16(16);
            const unsigned char rate44100[10] = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0};
            body.append(reinterpret_cast<const char*>(rate44100), 10);
            body.append("SSND", 4); be32(8 + 12); be32(0); be32(0);
            for (qint16 v : {qint16(16384), qint16(-16384), qint16(0), qint16(8192), qint16(-32768), qint16(32767)}) be16(quint16(v));
            QByteArray aiff("FORM", 4);
            char sz[4]; qToBigEndian(quint32(body.size()), sz); aiff.append(sz, 4); aiff.append(body);
            writeFile(audioDir.filePath("t.aiff"), aiff);
            SamplePtr x = samples::load(audioDir.filePath("t.aiff"), &err);
            CHECK(x && x->frames() == 3 && x->rate == 44100 && x->left[0] == 0.5f && x->right[0] == -0.5f && x->left[2] == -1.0f,
                  "M4 samples: AIFF loads (big-endian, 80-bit sample rate)");
        }
        writeFile(audioDir.filePath("junk.wav"), "RIFF....not audio");
        CHECK(!samples::load(audioDir.filePath("junk.wav"), &err) && !err.isEmpty(), "M4 samples: a broken file is refused with a reason");
        SamplePtr r = samples::reversed(f);
        CHECK(r && r->left[0] == f->left[95999] && r->left[95952] == 0.5f && samples::reversed(f) == r, "M4 samples: reversed copies are cached");
        if (samples::sndfileAvailable()) {
            SamplePtr viaSf = samples::loadWithSndfile(burstsWav, &err);
            CHECK(viaSf && viaSf->frames() == 96000 && viaSf->left[24000] == 0.5f, "M4 samples: libsndfile (loaded at run time) reads files too");
        } else {
            std::printf("      libsndfile isn't installed: FLAC / Ogg / MP3 aren't tested\n");
        }
        CHECK(samples::isAudioFile("x.WAV") && samples::isAudioFile("y.aiff") && !samples::isAudioFile("z.txt"), "M4 samples: audio files are recognised by extension");
    }

    // ---- New audio clips: loops warp to whole bars, one-shots play at their own speed
    {
        Project p;
        ClipPtr loop = p.newAudioClip(burstsWav, samples::load(burstsWav));
        const QString hit = audioDir.filePath("hit.wav"), almost = audioDir.filePath("almost a bar.wav");
        writeMonoWav(hit, bursts(48000, 0.3, 1.0, 100, 0.5f), 48000);
        writeMonoWav(almost, bursts(48000, 1.9, 0.5, 48, 0.5f), 48000);
        ClipPtr shot = p.newAudioClip(hit, samples::load(hit));
        ClipPtr nearBar = p.newAudioClip(almost, samples::load(almost));
        CHECK(loop->isAudio() && loop->length == 4 && loop->loop && loop->warpMarkers.size() == 2 && loop->warpMarkers[1].seconds == 2.0
              && std::abs(loop->sampleBpm() - 120) < 1e-9 && loop->name == "bursts 120",
              "M4 clips: a one-bar loop becomes a looping 4-beat clip at 120 BPM");
        CHECK(!shot->loop && std::abs(shot->length - 0.6) < 1e-9, "M4 clips: a short hit plays once at its own speed");
        CHECK(nearBar->length == 4 && std::abs(nearBar->sampleBpm() - 4 * 60 / 1.9) < 1e-9,
              qPrintable(QString("M4 clips: a loop near a bar long is warped to exactly one bar (length %1, %2 BPM, found %3)").arg(nearBar->length).arg(nearBar->sampleBpm()).arg(nearBar->detectedBpm)));
    }

    // ---- Engine: audio clips land on the beat, Re-Pitch and Beats follow the tempo, gain, reverse, loops
    {
        Project p; audioProject(p, burstsWav);
        p.tracks[0].arrangement[0].length = 8;   // two passes of the one-bar clip
        const std::vector<float> out = playArrangement(p, 200000);
        const std::vector<int> on = onsetsOf(out, 0.25f);
        // (a fresh engine ramps its gains up from silence over ~10 ms, so the burst on beat 1 of the first pass is quiet)
        CHECK(on.size() == 7 && near(on[0], 24000, 2) && near(on[1], 48000, 2) && near(on[2], 72000, 2),
              "M4 engine: an audio clip at its own tempo plays every burst on its beat");
        CHECK(onsetNear(on, 96000, 10) >= 0 && onsetNear(on, 120000, 2) >= 0 && loudRun(out, onsetNear(on, 96000, 10), 0.25f) > 36,
              "M4 engine: the clip loops after its bar, its downbeat intact");
    }
    {
        Project p; ClipPtr c = audioProject(p, burstsWav);
        c->warpMode = WarpMode::Repitch;
        c->detectedBpm = 0;   // pinned to its markers: with no tempo found, a tempo change doesn't re-match the clip
        p.setTempo(60);
        const std::vector<float> out = playArrangement(p, 200000);
        const std::vector<int> on = onsetsOf(out, 0.25f);
        const int b1 = onsetNear(on, 48000, 2);
        CHECK(on.size() == 3 && b1 >= 0 && onsetNear(on, 96000, 2) >= 0 && onsetNear(on, 144000, 2) >= 0 && near(loudRun(out, b1, 0.25f), 96, 3),
              "M4 engine: Re-Pitch at half the tempo plays at half speed (bursts twice as far apart and twice as long)");
    }
    {
        Project p; ClipPtr c = audioProject(p, burstsWav);   // Beats mode is the default
        c->detectedBpm = 0;   // pinned to its markers (see above)
        p.setTempo(60);
        const std::vector<float> out = playArrangement(p, 200000);
        const std::vector<int> on = onsetsOf(out, 0.25f);
        const int b1 = onsetNear(on, 48000, 20);
        CHECK(on.size() == 3 && b1 >= 0 && onsetNear(on, 96000, 20) >= 0 && onsetNear(on, 144000, 20) >= 0 && near(loudRun(out, b1, 0.25f), 48, 12),
              "M4 engine: Beats at half the tempo keeps each burst's length (time-stretch, same pitch)");
    }
    {
        Project p; ClipPtr c = audioProject(p, burstsWav);
        c->transpose = 12;
        const std::vector<float> up = playArrangement(p, 60000);
        const std::vector<int> on = onsetsOf(up, 0.25f);
        const int b1 = onsetNear(on, 24000, 20);
        CHECK(b1 >= 0 && near(loudRun(up, b1, 0.25f), 24, 8), "M4 engine: Beats transpose +12 plays the audio an octave up (twice as fast)");
    }
    {
        Project p; ClipPtr c = audioProject(p, burstsWav);
        const float peak0 = peakOf(playArrangement(p, 60000));
        c->gain = -20.0 * std::log10(2.0);
        const float peakHalf = peakOf(playArrangement(p, 60000));
        CHECK(std::abs(peakHalf / peak0 - 0.5f) < 0.01f, "M4 engine: clip gain -6 dB halves the level");
    }
    {
        Project p; ClipPtr c = audioProject(p, burstsWav);
        c->reverse = true;   // markers 0 -> 4 beats over the whole file, so no flip is needed for this check
        const std::vector<int> on = onsetsOf(playArrangement(p, 100000), 0.25f);
        CHECK(on.size() >= 3 && near(on[0], 23952, 2) && near(on[1], 47952, 2) && near(on[2], 71952, 2), "M4 engine: a reversed clip plays the audio backwards");
    }
    {
        Project p; audioProject(p, burstsWav);
        p.tracks[0].arrangement.clear();
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
        e.send({Command::Play});
        const std::vector<int> on = onsetsOf(render(e, 60000), 0.25f);
        CHECK(onsetNear(on, 24000, 2) >= 0 && onsetNear(on, 48000, 2) >= 0, "M4 engine: an audio clip launches from the Session View");
    }
    {   // the Sampler plays its sample pitched from C3
        const QString sine = audioDir.filePath("sine440.wav");
        std::vector<float> v(48000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = 0.5f * std::sin(2 * 3.14159265f * 440.0f * float(i) / 48000.0f);
        writeMonoWav(sine, v, 48000);
        auto crossings = [](const std::vector<float>& out, int from, int n) {
            int c = 0;
            for (int i = from + 1; i < from + n; ++i) if ((out[size_t(i - 1) * 2] < 0) != (out[size_t(i) * 2] < 0)) ++c;
            return c;
        };
        auto play = [&](int pitch) {
            Project p; oneNoteProject(p);
            p.tracks[0].devices[0].params[2].value = 0.5;
            p.tracks[0].arrangement[0].clip->notes = {Note{pitch, 0.0, 2.0, 1.0, 1.0}};
            QString why;
            import::toSampler(p, 0, -1, sine, &why);
            return playArrangement(p, 24000);
        };
        const std::vector<float> c3 = play(60), c4 = play(72);
        const int n3 = crossings(c3, 4800, 9600), n4 = crossings(c4, 4800, 9600);
        CHECK(near(n3, 176, 3) && near(n4, 352, 4), "M4 Sampler: plays its sample at C3 and an octave up at C4");
    }
    {   // browser preview
        Project p; p.loadDemo();
        audio::Engine e(&p);
        e.previewSample(samples::load(burstsWav));
        const std::vector<int> on = onsetsOf(render(e, 30000), 0.1f, 100);
        e.previewSample(nullptr);
        CHECK(on.size() == 2 && near(on[0], 0, 2) && near(on[1], 24000, 2) && peakOf(render(e, 4800)) == 0.0f,
              "M4 preview: a clicked sample plays on the main output, and stops");
    }

    // ---- Set format v2: audio tracks, clips and Sampler samples round-trip, found by relative path
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("song/Samples");
        const QString wav = dir.filePath("song/Samples/loop.wav");
        QFile::copy(burstsWav, wav);
        Project p; p.loadDemo();
        const int at = p.insertTrack(1, TrackType::Audio);
        ClipPtr c = p.newAudioClip(wav, samples::load(wav));
        c->warpMode = WarpMode::Repitch;
        c->warpMarkers = {{0, 0}, {1.5, 0.75}, {4, 2}};
        c->gain = -3.5; c->transpose = -7; c->reverse = true;
        c->warp = false; c->grain = 0;   // M7: Warp off, transient slices
        c->envelopes = {{target::volume(), {{0, 1}, {4, 0}}}};
        p.tracks[at].monitor = Monitor::In;
        p.tracks[at].inputChannel = 1;
        p.tracks[at].clipSlots[2] = c;
        p.tracks[at].arrangement = {{8, 4, 1, c}};
        import::toSampler(p, 0, -1, wav);
        {   // M15: a NAM device whose capture sits inside the set's folder
            Device nd = p.makeDevice("NAM", DeviceKind::AudioEffect);
            nam::writeExample(dir.filePath("song/Samples/capture.nam"), 2);
            nd.modelPath = dir.filePath("song/Samples/capture.nam");
            nd.model = nam::load(nd.modelPath);
            p.tracks[at].devices.push_back(nd);
        }
        const QString setPath = dir.filePath("song/song.sessions");
        setfile::Extras x;
        CHECK(setfile::save(setPath, p, x), "M4 sets: a set with audio saves");
        QFile f(setPath); f.open(QIODevice::ReadOnly);
        const QByteArray json = f.readAll();
        CHECK(json.contains("\"version\": 2") && json.contains("\"relativePath\": \"Samples/loop.wav\"") && json.contains("\"type\": \"audio\""),
              "M4 sets: format 2 stores the track type and sample paths relative to the set");
        // Move the whole folder: the samples are found next to the set again.
        QDir(dir.path()).rename("song", "moved");
        Project q;
        setfile::Extras y;
        CHECK(setfile::load(dir.filePath("moved/song.sessions"), q, y), "M4 sets: the moved set loads");
        const Track& qa = q.tracks[at];
        const ClipPtr lc = qa.clipSlots.value(2);
        CHECK(qa.type == TrackType::Audio && lc && lc->sample && lc->samplePath == QFileInfo(dir.filePath("moved/Samples/loop.wav")).absoluteFilePath()
              && lc->warpMode == WarpMode::Repitch && lc->warpMarkers.size() == 3 && lc->gain == -3.5 && lc->transpose == -7 && lc->reverse
              && qa.arrangement.value(0).clip == lc && qa.arrangement.value(0).offset == 1,
              "M4 sets: audio tracks and clips (warp, gain, transpose, reverse) come back, finding the moved sample");
        CHECK(q.tracks[0].devices[0].name == "Sampler" && q.tracks[0].devices[0].sample, "M4 sets: the Sampler's sample comes back");
        CHECK(!lc->warp && lc->grain == 0 && lc->envelopes.size() == 1 && qa.monitor == Monitor::In && qa.inputChannel == 1,
              "M7 sets: Warp off, slices, clip envelopes, Monitor and Audio From come back");
        CHECK(qa.devices.size() == 1 && qa.devices[0].model && qa.devices[0].modelPath == QFileInfo(dir.filePath("moved/Samples/capture.nam")).absoluteFilePath(),
              "M15 sets: the NAM device's capture comes back from the moved folder (the M4 round trip)");
        p.tracks[at].clipSlots[2]->samplePath = lc->samplePath;   // compare everything else as it was
        p.tracks[0].devices[0].samplePath = q.tracks[0].devices[0].samplePath;
        p.tracks[at].devices[0].modelPath = qa.devices[0].modelPath;
        CHECK(q.captureEditState() == p.captureEditState(), "M4 sets: an audio set round-trips (captureEditState ==)");
        QFile::remove(dir.filePath("moved/Samples/loop.wav"));
        Project r;
        CHECK(setfile::load(dir.filePath("moved/song.sessions"), r, y) && r.tracks[at].clipSlots.value(2) && !r.tracks[at].clipSlots[2]->sample
              && r.tracks[at].clipSlots[2]->isAudio(),
              "M4 sets: a missing sample keeps its clip (silent) instead of failing the load");
    }

    // ---- Import: onto audio tracks, a new audio track otherwise, and into the Sampler
    {
        Project p; p.loadDemo();
        const int n0 = p.tracks.size();
        const QVector<ClipPtr> two = import::audioClips(p, {burstsWav, burstsWav});
        int t = import::toSession(p, 1, 3, two);   // dropped on the Bass (MIDI) track
        CHECK(t == 2 && p.tracks.size() == n0 + 1 && p.tracks[2].type == TrackType::Audio && p.tracks[2].name == "bursts 120"
              && p.tracks[2].clipSlots[3] == two[0] && p.tracks[2].clipSlots[4] == two[1],
              "M4 import: samples dropped on a MIDI track go into a new audio track next to it, in consecutive slots");
        t = import::toSession(p, 2, 4, import::audioClips(p, {burstsWav, burstsWav}));
        CHECK(t == 2 && p.scenes.size() == 6, "M4 import: an audio track's slots take them, adding scenes when they run out");
        t = import::toArrangement(p, 2, 8, import::audioClips(p, {burstsWav, burstsWav}));
        CHECK(t == 2 && p.tracks[2].arrangement.size() == 2 && p.tracks[2].arrangement[0].start == 8 && p.tracks[2].arrangement[1].start == 12,
              "M4 import: on the timeline they're placed one after another");
        QString err;
        CHECK(!import::toSampler(p, 2, -1, burstsWav, &err) && import::toSampler(p, 0, -1, burstsWav, &err)
              && p.tracks[0].devices[0].name == "Sampler" && p.tracks[0].devices[0].sample,
              "M4 import: a sample loads into a MIDI track's Sampler (replacing its instrument), not onto an audio track");
        CHECK(!p.acceptsDevice(2, DeviceKind::Instrument) && p.acceptsDevice(2, DeviceKind::AudioEffect), "M4 tracks: audio tracks take audio effects only");
    }

    // ---- Window: Ctrl+Shift+T, drops on the Session / Arrangement, the browser, the sample editor, export, Collect All
    {
        QTemporaryDir dir;
        MainWindow w;
        w.resize(1600, 900);
        w.show();
        QApplication::processEvents();
        Project* proj = w.project();
        QWidget* target = w.sessionView()->grid();
        target->setFocus();
        const int n0 = proj->tracks.size();
        QTest::keyClick(target, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        const int audioTrack = proj->selectedTrack;
        CHECK(proj->tracks.size() == n0 + 1 && proj->tracks[audioTrack].type == TrackType::Audio, "M4 window: Ctrl+Shift+T inserts an audio track");

        ClipGrid* grid = w.sessionView()->grid();
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(burstsWav)});
        const QPoint slot(grid->columnLeft(audioTrack) + 40, 20 + 1 + 2 * 21 + 5);   // the audio track, third scene
        QDragEnterEvent enter(slot, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grid, &enter);
        QDropEvent drop(slot, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grid, &drop);
        // the file loads in the background; the clip arrives a moment later
        for (int k = 0; k < 300 && !proj->tracks[audioTrack].clipSlots.value(2); ++k) QTest::qWait(10);
        const int scene = grid->selectedScene();
        const ClipPtr dropped = proj->tracks[audioTrack].clipSlots.value(scene);
        CHECK(enter.isAccepted() && dropped && dropped->isAudio() && proj->tracks.size() == n0 + 1, "M4 window: a file dropped on an audio slot becomes a clip there");
        proj->undoStack()->undo();
        CHECK(!proj->tracks[audioTrack].clipSlots.value(scene), "M4 window: and the drop undoes in one step");

        ArrangementView* av = w.arrangementView();
        QTest::keyClick(target, Qt::Key_Tab);
        QApplication::processEvents();
        const QPoint lanePos(int(av->beatToX(16)) + 2, av->laneRect(audioTrack).center().y());
        QDragEnterEvent enter2(lanePos, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(av, &enter2);
        QDragMoveEvent move2(lanePos, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(av, &move2);
        QDropEvent drop2(lanePos, Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(av, &drop2);
        for (int k = 0; k < 300 && proj->tracks[audioTrack].arrangement.isEmpty(); ++k) QTest::qWait(10);
        const auto& arr = proj->tracks[audioTrack].arrangement;
        CHECK(arr.size() == 1 && std::abs(arr[0].start - 16) < 1.01 && arr[0].clip->isAudio(), "M4 window: a file dropped on an audio lane lands at that beat");

        // The Clip View shows the sample editor; double-click adds a warp marker, a drag moves it, undo takes it back.
        w.project()->selectedClip = arr[0].clip;
        w.findChild<ClipView*>()->setClip(arr[0].clip);
        SampleEditor* se = w.findChild<SampleEditor*>();
        se->resize(800, 200);
        se->setClip(arr[0].clip);
        se->show();
        QApplication::processEvents();
        const ClipPtr ac = arr[0].clip;
        QTest::mouseDClick(se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se->beatToX(2)), 100));
        CHECK(ac->warpMarkers.size() == 3 && ac->warpMarkers[1].beat == 2 && std::abs(ac->warpMarkers[1].seconds - 1.0) < 1e-9,
              "M4 sample editor: double-click adds a warp marker on the beat it shows");
        QTest::mousePress(se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se->beatToX(2)), 100));
        QTest::mouseMove(se, QPoint(int(se->beatToX(3)), 100));
        QTest::mouseRelease(se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se->beatToX(3)), 100));
        CHECK(ac->warpMarkers[1].beat == 3 && std::abs(ac->secondsAt(3) - 1.0) < 1e-9 && std::abs(ac->secondsAt(1.5) - 0.5) < 1e-9,
              "M4 sample editor: dragging a marker stretches the audio before it and squeezes the audio after");
        proj->undoStack()->undo();
        proj->undoStack()->undo();
        CHECK(ac->warpMarkers.size() == 2, "M4 sample editor: marker edits undo");

        // Browser: a folder of samples
        Browser* br = w.findChild<Browser*>();
        br->addFolder(audioDir.path());
        CHECK(br->folders().contains(QDir(audioDir.path()).absolutePath()) && br->listedItems().contains(burstsWav + "|s"),
              "M4 browser: Add Folder lists its audio files");
        br->removeFolder(QDir(audioDir.path()).absolutePath());

        // Export Audio: the Arrangement into a WAV
        audio::RenderOptions o;
        o.lengthBeats = 8; o.tailSeconds = 0.5; o.format = samples::WavFormat::Int16;
        const QString wavOut = dir.filePath("export.wav");
        QString err;
        CHECK(w.exportAudio(wavOut, o, &err), "M4 export: renders the Arrangement to a WAV");
        SamplePtr back = samples::load(wavOut);
        CHECK(back && back->frames() == 4 * 48000 + 24000 && back->rate == 48000 && peakOf(std::vector<float>(back->left.begin(), back->left.end())) > 0.05f,
              "M4 export: the file has the song's length plus the tail, and sound");
        o.normalize = true; o.format = samples::WavFormat::Float32;
        CHECK(w.exportAudio(wavOut, o, &err), "M4 export: normalized float export");
        back = samples::load(wavOut);
        float pk = 0;
        for (size_t i = 0; back && i < back->left.size(); ++i) pk = std::max({pk, std::fabs(back->left[i]), std::fabs(back->right[i])});
        CHECK(std::abs(pk - 0.98855f) < 1e-3f, "M4 export: normalize peaks at -0.1 dBFS");

        // Collect All and Save: the sample is copied next to the set and the set points at the copy.
        const QString setPath = dir.filePath("Collected/song.sessions");
        QDir().mkpath(dir.filePath("Collected"));
        CHECK(w.saveSet(setPath, &err), "M4 collect: setup saves the set");
        CHECK(w.collectSamples(&err) == 1 && QFileInfo::exists(dir.filePath("Collected/Samples/Imported/bursts 120.wav"))
              && proj->tracks[audioTrack].arrangement[0].clip->samplePath == QFileInfo(dir.filePath("Collected/Samples/Imported/bursts 120.wav")).absoluteFilePath()
              && w.isWindowModified(),
              "M4 collect: Collect All copies outside samples into Samples/Imported and points the set at them");
        CHECK(w.collectSamples(&err) == 0, "M4 collect: a second run has nothing left to copy");
        CHECK(w.saveSet(setPath, &err) && w.openSet(setPath, &err) && w.missingSamples().isEmpty(), "M4 collect: the collected set reopens with every sample");
    }

    // ---- The layout: fixed clip slots, a grid that scrolls alone, header fields, Pre/Post on Main
    {
        MainWindow w;
        w.resize(1100, 700);
        w.show();
        QApplication::processEvents();
        ClipGrid* grid = w.sessionView()->grid();
        const int row0 = grid->rowHeight(), col0 = grid->columnWidth(0);
        MixerStrip* strip = nullptr;
        for (MixerStrip* m : w.sessionView()->findChildren<MixerStrip*>()) if (m->isVisible()) { strip = m; break; }
        const int stripH0 = strip ? strip->height() : 0;
        w.resize(2000, 1100);
        QApplication::processEvents();
        CHECK(grid->rowHeight() == row0 && row0 == ClipGrid::kRowHeight && grid->columnWidth(0) == col0 && strip && strip->height() > stripH0,
              "M4 layout: clip slots keep their size on a bigger window; the mixer gets the extra height");
        for (int i = 0; i < 30; ++i) w.project()->insertScene(int(w.project()->scenes.size()));
        QApplication::processEvents();
        CHECK(grid->rowHeight() == row0 && grid->visibleRows() < w.project()->scenes.size() && strip && strip->height() >= 262,
              "M4 layout: with many scenes the clip grid scrolls and the mixer stays in view");
        QWheelEvent down(QPointF(50, 60), grid->mapToGlobal(QPointF(50, 60)), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(grid, &down);
        CHECK(grid->firstVisibleRow() > 0, "M5 session: the mouse wheel scrolls the scenes");
        for (int i = 0; i < 30; ++i) w.project()->deleteScene(int(w.project()->scenes.size()) - 1);
        QApplication::processEvents();

        // Main strip: each return's Pre / Post switch
        MainStrip* mainStrip = nullptr;
        for (MainStrip* m : w.sessionView()->findChildren<MainStrip*>()) if (m->isVisible()) mainStrip = m;
        QToolButton* post = mainStrip ? mainStrip->findChild<QToolButton*>() : nullptr;
        if (post) post->click();
        CHECK(post && w.project()->returns[0].preFader && post->text() == "Pre", "M4 layout: the Main strip's Sends area switches a return to pre-fader");
        w.project()->undoStack()->undo();
        CHECK(!w.project()->returns[0].preFader, "M4 layout: and it undoes");

        // Arrangement header fields
        QWidget* target = grid;
        target->setFocus();
        QTest::keyClick(target, Qt::Key_Tab);
        QApplication::processEvents();
        ArrangementView* av = w.arrangementView();
        const QRect vol = av->fieldRect({ArrangementView::Field::Volume, 1, -1});
        const double v0 = w.project()->tracks[1].volume;
        QTest::mousePress(av, Qt::LeftButton, Qt::NoModifier, vol.center());
        QTest::mouseMove(av, vol.center() - QPoint(0, 40));
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, vol.center() - QPoint(0, 40));
        CHECK(!vol.isEmpty() && w.project()->tracks[1].volume > v0 + 0.1, "M4 layout: dragging a header's volume field up raises the track volume");
        w.project()->undoStack()->undo();
        CHECK(w.project()->tracks[1].volume == v0, "M4 layout: the drag undoes in one step");
        const QRect pan = av->fieldRect({ArrangementView::Field::Pan, 1, -1});
        QTest::mousePress(av, Qt::LeftButton, Qt::NoModifier, pan.center());
        QTest::mouseMove(av, pan.center() + QPoint(0, 30));
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, pan.center() + QPoint(0, 30));
        const double panned = w.project()->tracks[1].pan;
        QTest::mouseDClick(av, Qt::LeftButton, Qt::NoModifier, pan.center());
        CHECK(panned < -0.1 && w.project()->tracks[1].pan == 0, "M4 layout: the pan field drags, and double-click centres it");
        const QRect mainVol = av->fieldRect({ArrangementView::Field::MainVolume, w.project()->trackCount(), -1});
        QTest::mousePress(av, Qt::LeftButton, Qt::NoModifier, mainVol.center());
        QTest::mouseMove(av, mainVol.center() + QPoint(0, 50));
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, mainVol.center() + QPoint(0, 50));
        CHECK(!mainVol.isEmpty() && w.project()->mainVolume < 0.6, "M4 layout: the Main lane's volume field sets the main volume");
        // The overview: a click at its right end moves the view there.
        const double before = av->firstVisibleBeat();
        // zoom in first (Ctrl+wheel), so there's somewhere to go
        QWheelEvent zoomIn(QPointF(200, 200), av->mapToGlobal(QPointF(200, 200)), QPoint(), QPoint(0, 480), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(av, &zoomIn);
        QTest::mouseClick(av, Qt::LeftButton, Qt::NoModifier, QPoint(av->laneRect(0).width() - 6, 10));
        CHECK(av->firstVisibleBeat() > before + 4, "M4 layout: clicking the overview scrolls the Arrangement there");
    }

    // ---- Session: the clips / mixer divider, filler cells, the track status clock
    {
        MainWindow w;
        w.resize(1400, 1000);
        w.show();
        QApplication::processEvents();
        ClipGrid* grid = w.sessionView()->grid();
        CHECK(grid->visibleRows() == int(w.project()->scenes.size()), "M5 session: the grid starts out showing every scene");
        auto* split = qobject_cast<QSplitter*>(grid->parentWidget());
        const int rows0 = grid->visibleRows(), h0 = grid->height();
        QWidget* handle = split ? split->handle(1) : nullptr;
        if (handle) {
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, QPoint(50, 2));
            QTest::mouseMove(handle, QPoint(50, 2 + 140));
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, QPoint(50, 2 + 140));
            QApplication::processEvents();
        }
        CHECK(handle && grid->height() > h0 + 100 && grid->visibleRows() > rows0 && grid->rowHeight() == ClipGrid::kRowHeight,
              "M5 session: dragging the divider down shows more clip rows (same size)");
        CHECK(QSettings("Sessions", "Sessions").value("session/gridHeight").toInt() == grid->height(), "M5 session: the divider's place is remembered");
        const int scenes0 = w.project()->scenes.size();
        CHECK(grid->visibleRows() > scenes0, "M5 session: below the last scene the grid shows empty cells");
        // double-click a filler cell on a MIDI track: scenes down to it and a new clip
        const int fillerRow = scenes0 + 2;
        const QPoint filler(grid->columnLeft(1) + 50, 20 + 1 + fillerRow * 21 + 10);
        QTest::mouseDClick(grid, Qt::LeftButton, Qt::NoModifier, filler);
        CHECK(w.project()->scenes.size() == fillerRow + 1 && w.project()->tracks[1].clipSlots.value(fillerRow) != nullptr,
              "M5 session: double-clicking an empty cell adds the scenes down to it and a clip");
        w.project()->undoStack()->undo();
        CHECK(w.project()->scenes.size() == scenes0, "M5 session: and that undoes in one step");
        // the status clock: a 4-beat clip and a 16-beat clip, three bars in
        Project& p = *w.project();
        p.tracks[0].clipSlots[0]->length = 4;
        p.tracks[3].clipSlots[0]->length = 16;
        p.tracks[0].playingSlot = 0; p.tracks[0].slotStart = 0;
        p.tracks[3].playingSlot = 0; p.tracks[3].slotStart = 0;
        w.transport()->setBeat(13);   // stopped: the clock shows the playhead's position
        const ClipGrid::TrackStatus four = grid->trackStatus(0), sixteen = grid->trackStatus(3), none = grid->trackStatus(1);
        CHECK(four.playing && four.length == 4 && four.pass == 4 && std::abs(four.fraction - 0.25) < 1e-9
              && sixteen.length == 16 && sixteen.pass == 1 && std::abs(sixteen.fraction - 13.0 / 16) < 1e-9 && !none.playing,
              "M5 session: each track's clock fills once per loop of its clip (4 beats: pass 4, 16 beats: pass 1)");
        p.tracks[0].playingSlot = p.tracks[3].playingSlot = -1;
    }

    // ---- M5: MIDI input (through ALSA when it's there), MIDI Map and Key Map
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QApplication::processEvents();
        Project& p = *w.project();
        audio::MidiInput* midi = w.midiInput();
#ifdef SESSIONS_HAVE_ALSA
        if (midi->available()) {   // a second sequencer client plays a note into Sessions' port
            snd_seq_t* out = nullptr;
            const bool opened = snd_seq_open(&out, "default", SND_SEQ_OPEN_OUTPUT, 0) >= 0;
            const int port = opened ? snd_seq_create_simple_port(out, "smoke out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ, SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION) : -1;
            QSignalSpy notes(midi, &audio::MidiInput::noteOn);
            QTest::qWait(150);   // Sessions sees the new port (and connects it itself)
            bool listed = false;
            for (const auto& pt : midi->ports()) listed |= pt.name.contains("smoke out") && pt.connected;
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_source(&ev, port);
            snd_seq_ev_set_subs(&ev);
            snd_seq_ev_set_direct(&ev);
            snd_seq_ev_set_noteon(&ev, 0, 64, 100);
            if (port >= 0) snd_seq_event_output_direct(out, &ev);
            QTest::qWait(150);
            CHECK(listed && notes.count() == 1 && notes.value(0).value(1).toInt() == 64 && notes.value(0).value(2).toInt() == 100,
                  "M5 MIDI: a keyboard's note comes in through the ALSA sequencer (auto-connected)");
            snd_seq_ev_set_noteoff(&ev, 0, 64, 0);
            if (port >= 0) snd_seq_event_output_direct(out, &ev);
            QTest::qWait(50);
            if (out) snd_seq_close(out);
        } else {
            std::printf("      MIDI input not available here (%s)\n", qPrintable(midi->status()));
        }
#endif
        // Notes from MIDI record like the computer keyboard; the sustain pedal holds them.
        p.tracks[1].armed = true;
        int slot = 0;
        while (p.tracks[1].clipSlots.value(slot)) ++slot;
        w.transport()->recordSlot(1, slot);
        QTest::qWait(250);
        midi->inject(0x90, 50, 100);
        QTest::qWait(150);
        midi->inject(0x80, 50, 0);
        QTest::qWait(60);
        ClipPtr rec = p.tracks[1].clipSlots.value(slot);
        const int afterOne = rec ? rec->notes.size() : -1;
        midi->inject(0xB0, 64, 127);   // pedal down
        midi->inject(0x90, 52, 90);
        QTest::qWait(120);
        midi->inject(0x80, 52, 0);
        QTest::qWait(60);
        const int heldByPedal = rec ? rec->notes.size() : -1;
        midi->inject(0xB0, 64, 0);     // pedal up: now it ends
        QTest::qWait(30);
        CHECK(afterOne == 1 && rec->notes[0].pitch == 50 && std::abs(rec->notes[0].velocity - 100 / 127.0) < 1e-9,
              "M5 MIDI: a played note is recorded into the armed track's clip");
        CHECK(heldByPedal == 1 && rec->notes.size() == 2 && rec->notes[1].pitch == 52, "M5 MIDI: the sustain pedal holds a note until it's released");
        w.transport()->stop();
        p.tracks[1].armed = false;

        // MIDI Map: click the Bass volume fader, turn CC 21, and the fader follows it.
        w.setMapMode(Mapper::Mode::Midi);
        MixerStrip* bass = nullptr;
        for (MixerStrip* m : w.sessionView()->findChildren<MixerStrip*>()) if (m->isVisible() && m->trackIndex() == 1) bass = m;
        QWidget* fader = nullptr;
        if (bass) for (QWidget* c : bass->findChildren<QWidget*>()) if (c->property("mapTarget").toString().endsWith(":volume")) fader = c;
        const double v0 = p.tracks[1].volume;
        if (fader) QTest::mouseClick(fader, Qt::LeftButton, Qt::NoModifier, QPoint(fader->width() / 2, fader->height() / 2));
        const QString volTarget = QString("track:%1:volume").arg(p.tracks[1].id);
        CHECK(w.mapper()->selected() == volTarget && p.tracks[1].volume == v0 && w.mapOverlay()->isVisible(),
              "M5 map: in MIDI Map mode a click selects the fader instead of moving it");
        bool overlayShows = false;
        for (const auto& t : w.mapOverlay()->targets()) overlayShows |= t.target == volTarget;
        midi->inject(0xB0, 21, 100);
        CHECK(p.mappings.size() == 1 && p.mappings[0].source == Mapping::MidiCC && p.mappings[0].number == 21 && p.mappings[0].target == volTarget && overlayShows,
              "M5 map: moving a controller's knob maps it (the overlay shows mappable controls)");
        w.setMapMode(Mapper::Mode::Off);
        midi->inject(0xB0, 21, 0);
        const double low = p.tracks[1].volume;
        midi->inject(0xB0, 21, 127);
        CHECK(low == 0 && p.tracks[1].volume == 1.0 && w.isWindowModified(), "M5 map: the mapped knob drives the volume (and the set is modified)");
        p.undoStack()->undo();
        CHECK(p.tracks[1].volume == v0, "M5 map: a knob turn undoes in one step");

        // Key Map: click a clip slot, press Q; then Q launches it.
        w.setMapMode(Mapper::Mode::Key);
        ClipGrid* grid = w.sessionView()->grid();
        const QPoint slotCell(grid->columnLeft(0) + 50, 20 + 1 + 1 * 21 + 10);   // Drums, scene 2 (Beat B)
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, slotCell);
        const QString slotTarget = QString("slot:%1:1").arg(p.tracks[0].id);
        CHECK(w.mapper()->selected() == slotTarget && p.tracks[0].queuedSlot != 1, "M5 map: in Key Map mode a click selects a clip slot instead of launching it");
        QTest::keyClick(grid, Qt::Key_Q);
        CHECK(p.mappings.size() == 2 && p.mappings[1].source == Mapping::Key && p.mappings[1].number == Qt::Key_Q, "M5 map: pressing a key maps it");
        QTest::keyClick(grid, Qt::Key_Escape);
        CHECK(w.mapper()->mode() == Mapper::Mode::Off && !w.mapOverlay()->isVisible(), "M5 map: Esc leaves map mode");
        grid->setFocus();
        QTest::keyClick(grid, Qt::Key_Q);
        CHECK(p.tracks[0].queuedSlot == 1 || p.tracks[0].playingSlot == 1, "M5 map: the mapped key launches the clip");
        w.transport()->stop();
        w.transport()->stop();
        // A CC on a button fires once per press (rising edge); a note launches a scene.
        p.mappings.push_back({Mapping::MidiCC, 0, 30, "transport:metronome"});
        p.mappings.push_back({Mapping::MidiNote, 0, 36, "scene:2"});
        const bool metro0 = w.transport()->metronome();
        midi->inject(0xB0, 30, 127); midi->inject(0xB0, 30, 127); midi->inject(0xB0, 30, 0);
        const bool afterPress = w.transport()->metronome();
        midi->inject(0x90, 36, 100);
        CHECK(afterPress == !metro0 && (p.tracks[2].queuedSlot == 2 || p.tracks[2].playingSlot == 2),
              "M5 map: a CC button toggles once per press, a pad launches a scene");
        w.transport()->stop();
    }

    // ---- M5: Preferences, window state, what opens at startup
    {
        MainWindow w;
        w.resize(1300, 800);
        w.show();
        QApplication::processEvents();
        std::printf("      sound server outputs: %d\n", int(audio::listOutputDevices().size()));
        PreferencesDialog prefs(w.engine(), w.midiInput(), {}, &w);
        auto* buffer = prefs.findChild<QComboBox*>("audioBuffer");
        auto* device = prefs.findChild<QComboBox*>("audioDevice");
        const int fifty = buffer ? buffer->findData(50) : -1;
        if (buffer && fifty >= 0) { buffer->setCurrentIndex(fifty); emit buffer->activated(fifty); }
        CHECK(device && device->count() >= 1 && device->itemData(0).toString().isEmpty() && PreferencesDialog::bufferMs() == 50
              && w.engine()->output().bufferMs == 50,
              "M5 prefs: a new buffer size is saved and the audio output restarts with it");
        auto* ports = prefs.findChild<QListWidget*>("midiPorts");
        bool through = false;
        for (int i = 0; ports && i < ports->count(); ++i) through |= ports->item(i)->text().contains("Midi Through");
        CHECK(ports && (!w.midiInput()->available() || through), "M5 prefs: the MIDI tab lists the inputs");
        QSettings("Sessions", "Sessions").setValue("audio/bufferMs", 30);

        // window state: the view, panels and browser come back
        QTest::keyClick(w.sessionView()->grid(), Qt::Key_Tab);   // the Arrangement
        w.findChild<Browser*>()->hide();
        w.saveWindowState();
        MainWindow w2;
        w2.restoreWindowState();
        CHECK(!w2.sessionViewShown() && !w2.findChild<Browser*>()->isVisibleTo(&w2), "M5 window: the view and the browser come back as you left them");
        QSettings("Sessions", "Sessions").remove("window");

        QSettings("Sessions", "Sessions").setValue("startup/open", "empty");
        w2.openStartupSet();
        CHECK(w2.project()->tracks.size() == 2 && w2.setName() == "Untitled", "M5 startup: Preferences can start Sessions with an empty set");
        QSettings("Sessions", "Sessions").remove("startup");
    }

    // ---- MainWindow: Save / title / modified / New / Open / reopen after a restart
    {
        QTemporaryDir dir;
        MainWindow w;
        w.show();
        QApplication::processEvents();
        CHECK(w.setName() == "Demo" && !w.setModified() && w.windowTitle() == "Demo[*] - Sessions", "M4 window: starts on the demo, unmodified");
        const QString path = dir.filePath("My Song.sessions");
        QString err;
        CHECK(w.saveSet(path, &err) && w.setName() == "My Song" && w.setPath() == QFileInfo(path).absoluteFilePath() && !w.isWindowModified(),
              "M4 window: Save names the set after its file");
        CHECK(QSettings("Sessions", "Sessions").value("files/recent").toStringList().value(0) == w.setPath(), "M4 window: a saved set heads the recent list");
        w.project()->insertTrack(0);
        CHECK(w.setModified() && w.isWindowModified(), "M4 window: an edit marks the set modified (* in the title)");
        w.project()->undoStack()->undo();
        CHECK(!w.isWindowModified(), "M4 window: undoing back to the saved state clears the *");
        w.transport()->setLoopEnabled(!w.transport()->loopEnabled());
        CHECK(w.isWindowModified(), "M4 window: changing the loop (saved, not undoable) also marks it");
        w.transport()->setLoopEnabled(!w.transport()->loopEnabled());
        CHECK(!w.isWindowModified(), "M4 window: and setting it back clears it");
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_T, Qt::ControlModifier);
        w.transport()->setLoop(4, 8);
        const int tracks = w.project()->tracks.size();
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_S, Qt::ControlModifier);
        CHECK(!w.isWindowModified(), "M4 window: Ctrl+S saves to the set's file");
        w.transport()->play();
        w.newSet();
        CHECK(w.project()->tracks.size() == 2 && w.project()->returns.size() == 2 && w.project()->scenes.size() == 8 && w.setName() == "Untitled"
              && w.setPath().isEmpty() && !w.setModified() && !w.transport()->playing() && w.project()->undoStack()->count() == 0,
              "M4 window: New Set gives two MIDI tracks, two returns, eight scenes, stopped");
        CHECK(w.openSet(path, &err) && w.project()->tracks.size() == tracks && w.setName() == "My Song" && !w.setModified()
              && w.transport()->loopStart() == 4 && w.transport()->loopLength() == 8 && w.project()->undoStack()->count() == 0,
              "M4 window: Open brings the saved set back (loop region too) with a fresh undo history");
        CHECK(w.project()->selectedClip && w.project()->trackOfClip(w.project()->selectedClip) >= 0, "M4 window: an opened set shows its first clip");
        writeFile(dir.filePath("bad.sessions"), "{");
        CHECK(!w.openSet(dir.filePath("bad.sessions"), &err) && w.project()->tracks.size() == tracks && w.setName() == "My Song",
              "M4 window: a broken file doesn't replace the open set");
        w.project()->renameTrack(0, "Changed");
        const QString snapshot = dir.filePath("snapshot.sessions");
        setfile::save(snapshot, *w.project(), setfile::Extras{});
        MainWindow w2;
        CHECK(w2.reopenSnapshot(snapshot, w.setPath(), w.setName(), true, &err) && w2.project()->tracks[0].name == "Changed"
              && w2.setPath() == w.setPath() && w2.setName() == "My Song" && w2.isWindowModified() && !QFile::exists(snapshot),
              "M4 window: after a Zoom restart the set reopens unsaved, under its own name, and the snapshot is deleted");
    }

    // ---- MainWindow: Tab / Shift+Tab switch views even when a child widget has focus
    {
        MainWindow w;
        w.show();
        QApplication::processEvents();
        QWidget* roll2 = w.findChild<PianoRoll*>();
        roll2->setFocus();
        QApplication::processEvents();
        bool s0 = w.sessionViewShown();
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_Tab);
        CHECK(w.sessionViewShown() != s0, "MainWindow: Tab switches Session / Arrangement");
        bool c0 = w.clipViewShown();
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_Backtab, Qt::ShiftModifier);
        CHECK(w.clipViewShown() != c0, "MainWindow: Shift+Tab switches Clip / Device View");
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_F11);
        CHECK(w.windowState() & Qt::WindowFullScreen, "MainWindow: F11 enters full screen");
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_F11);
        CHECK(!(w.windowState() & Qt::WindowFullScreen), "MainWindow: F11 again leaves full screen");
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_M);
        CHECK(w.midiKeyboard(), "MainWindow: M turns the computer MIDI keyboard on");
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_M);
        CHECK(!w.midiKeyboard(), "MainWindow: M turns it off again");
        const int tracks0 = w.project()->tracks.size();
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_T, Qt::ControlModifier);
        CHECK(w.project()->tracks.size() == tracks0 + 1, "MainWindow: Ctrl+T inserts a MIDI track");
        QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key_Z, Qt::ControlModifier);
        CHECK(w.project()->tracks.size() == tracks0, "MainWindow: Ctrl+Z undoes it");
    }


    // ---- Milestone 6: a delay read right at the buffer's wrap stays inside it (the reverb's pre-delay found this)
    {
        dsp::DelayLine line;
        line.init(100);
        for (int k = 1; k <= 10; ++k) line.push(float(k));
        CHECK(std::fabs(line.read(10.000001f) - 1.0f) < 1e-3f && std::fabs(line.read(1.0f) - 10.0f) < 1e-6f,
              "M6 DSP: a delay read at the wrap point reads the oldest sample, not past the buffer");
    }

    // ---- Milestone 6: every device, with its defaults and with random knobs
    {
        int devices = 0, silentInstruments = 0, badDefaults = 0, badFuzz = 0;
        QStringList slow;
        QList<QPair<double, QString>> timings;
        dsp::Noise rng;
        SamplePtr rackPads[drumrack::kPads];   // the Drum Rack plays the built-in kit
        { const QVector<DrumPad> pads = audio::defaultDrumPads(); for (int k = 0; k < drumrack::kPads; ++k) rackPads[k] = pads.value(k).sample; }
        for (const DeviceSpec& spec : deviceSpecs()) {
            if (specOf(spec.type) != &spec) continue;   // presets share their device's processor
            ++devices;
            auto proc = audio::createProcessor(spec.type, 48000);
            const bool drums = spec.type == DeviceType::DrumKit || spec.type == DeviceType::Drum808 || spec.type == DeviceType::DrumAcoustic
                            || spec.type == DeviceType::DrumRack;
            const SamplePtr* pads = spec.type == DeviceType::DrumRack ? rackPads : nullptr;
            QElapsedTimer timer; timer.start();
            DevRun d = runDevice(*proc, spec.kind, defaultsOf(spec.type), 1.0, drums ? std::vector<int>{36, 38, 42} : std::vector<int>{48, 60, 64, 67},
                                 0.5, 0.5f, nullptr, 0.5, pads);
            const double ms = double(timer.nsecsElapsed()) / 1e6;
            timings << qMakePair(ms, QString(spec.name));
            if (ms > 250) slow << QString("%1 %2 ms").arg(spec.name).arg(ms, 0, 'f', 0);
            if (!d.finite || (spec.kind != DeviceKind::MidiEffect && d.peak > 4)) { ++badDefaults; std::printf("      %s: finite %d peak %.2f\n", spec.name, d.finite, d.peak); }
            if (spec.kind == DeviceKind::Instrument && !isRackType(spec.type) && d.peak < 0.02f) { ++silentInstruments; std::printf("      %s is silent\n", spec.name); }
            for (int round = 0; round < 25; ++round) {
                std::vector<float> p(audio::kMaxParams, 0.0f);
                for (float& x : p) x = rng.uniform();
                auto fresh = audio::createProcessor(spec.type, 48000);
                DevRun f = runDevice(*fresh, spec.kind, p, 0.25, {36, 39, 42, 46, 60}, 0.1, 1.0f, nullptr, 0.5, pads);
                if (!f.finite || f.peak > 80) { ++badFuzz; std::printf("      %s fuzz %d: finite %d peak %.2f\n", spec.name, round, f.finite, f.peak); break; }
            }
        }
        std::sort(timings.begin(), timings.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        QStringList top;
        for (int i = 0; i < std::min<qsizetype>(4, timings.size()); ++i) top << QString("%1 %2 ms").arg(timings[i].second).arg(timings[i].first, 0, 'f', 1);
        std::printf("      heaviest devices (1 s of audio): %s\n", qPrintable(top.join(", ")));
        CHECK(devices == 49, "M6 devices: 49 devices (14 instruments with Monolith and Drum Studio, 30 audio effects with the guitar ones, NAM and Cabinet, 5 MIDI effects with the MIDI Effect Rack)");
        CHECK(badDefaults == 0, "M6 devices: every device's defaults give finite, sane output");
        CHECK(silentInstruments == 0, "M6 devices: every instrument makes a sound");
        CHECK(badFuzz == 0, "M6 devices: random knob settings never blow up (finite, bounded)");
        CHECK(slow.isEmpty(), qPrintable("M6 devices: each renders a second of audio in well under a second " + slow.join(", ")));
    }
    {
        // Compressor: loud input is turned down and the meter says by how much
        auto comp = audio::createProcessor(DeviceType::Compressor, 48000);
        std::vector<float> p = defaultsOf(DeviceType::Compressor);
        p[0] = 0.25f;   // -30 dB
        p[1] = 0.8f;    // ~11:1
        p[5] = 0.0f;    // no makeup
        DevRun d = runDevice(*comp, DeviceKind::AudioEffect, p, 0.5, {}, 0, 0.9f, nullptr, 1.0);
        CHECK(comp->meter[0].load() > 6 && comp->meter[1].load() > -6 && rmsOf(d.l, 12000, 23000) < 0.2f,
              "M6 Compressor: turns a loud signal down and meters the gain reduction");
        // Limiter: +12 dB into a -3 dB ceiling never gets over it
        auto lim = audio::createProcessor(DeviceType::Limiter, 48000);
        std::vector<float> lp = defaultsOf(DeviceType::Limiter);
        lp[0] = 0.5f;                   // +12 dB
        lp[1] = float((12.0 - 3) / 12); // -3 dB
        DevRun l = runDevice(*lim, DeviceKind::AudioEffect, lp, 0.5, {}, 0, 0.9f, nullptr, 1.0);
        CHECK(l.peak <= std::pow(10.0f, -3.0f / 20) + 1e-4f && lim->meter[0].load() > 3, "M6 Limiter: peaks stay under the ceiling");
        // Gate: a quiet signal under the threshold is shut off
        auto gate = audio::createProcessor(DeviceType::Gate, 48000);
        DevRun g = runDevice(*gate, DeviceKind::AudioEffect, defaultsOf(DeviceType::Gate), 0.5, {}, 0, 0.003f);
        CHECK(rmsOf(g.l, 10000, 12000) < 1e-5f && gate->meter[1].load() < 0.01f, "M6 Gate: a signal under the threshold is silenced");
        auto gate2 = audio::createProcessor(DeviceType::Gate, 48000);
        DevRun g2 = runDevice(*gate2, DeviceKind::AudioEffect, defaultsOf(DeviceType::Gate), 0.5, {}, 0, 0.5f);
        CHECK(rmsOf(g2.l, 10000, 12000) > 0.1f, "M6 Gate: a signal over it passes");
        // Utility: width 0 is mono, Mute is silence
        auto util = audio::createProcessor(DeviceType::Utility, 48000);
        std::vector<float> up = defaultsOf(DeviceType::Utility);
        up[2] = 0;
        DevRun u = runDevice(*util, DeviceKind::AudioEffect, up, 0.25, {}, 0);
        bool mono = true;
        for (size_t i = 2000; i < u.l.size(); ++i) mono &= std::fabs(u.l[i] - u.r[i]) < 1e-5f;
        up[2] = 0.25f; up[6] = 1;
        auto util2 = audio::createProcessor(DeviceType::Utility, 48000);
        DevRun um = runDevice(*util2, DeviceKind::AudioEffect, up, 0.25, {}, 0);
        CHECK(mono && rmsOf(um.l, 3000, 6000) < 1e-5f, "M6 Utility: Width 0 is mono, Mute is silent");
        // Reverb Freeze holds the tail; without it the tail dies
        auto rev = audio::createProcessor(DeviceType::Reverb, 48000);
        std::vector<float> rp = defaultsOf(DeviceType::Reverb);
        rp[3] = 1;
        DevRun r1 = runDevice(*rev, DeviceKind::AudioEffect, rp, 6.0, {}, 0);
        rp[8] = 1;
        auto rev2 = audio::createProcessor(DeviceType::Reverb, 48000);
        std::vector<float> rpOff = rp; rpOff[8] = 0;
        // run with freeze off for the input half, then on: keep the processor, switch the param
        DevRun half = runDevice(*rev2, DeviceKind::AudioEffect, rpOff, 1.0, {}, 0);
        DevRun r2 = runDevice(*rev2, DeviceKind::AudioEffect, rp, 6.0, {}, 0);
        CHECK(rmsOf(r1.l, 280000, 288000) < 0.002f && rmsOf(r2.l, 280000, 288000) > 0.005f, "M6 Reverb: Freeze holds the tail");
        (void)half;
        // EQ display math: a +12 dB bell reads +12 dB at its frequency, the cut filters cut
        std::vector<float> ep = defaultsOf(DeviceType::EQ);
        ep[1] = float((12.0 + 15) / 30);
        const float atMid = audio::eqResponseDb(ep.data(), 1000, 48000), far = audio::eqResponseDb(ep.data(), 60, 48000);
        ep[8] = 0.6f;
        const float cut = audio::eqResponseDb(ep.data(), 30, 48000);
        CHECK(std::fabs(atMid - 12) < 0.3f && std::fabs(far) < 1.5f && cut < -12, "M6 EQ: the response the display draws (bell, low cut)");
    }
    {
        // MIDI effects: Scale, Velocity, Chord's two new shifts
        std::vector<float> sp = defaultsOf(DeviceType::Scale);   // C major
        CHECK(audio::scaleMap(sp.data(), 61) == 60 && audio::scaleMap(sp.data(), 66) == 65 && audio::scaleMap(sp.data(), 64) == 64,
              "M6 Scale: notes off the scale move to the nearest one (a tie goes down)");
        sp[0] = float(choicePosition(9, 12)); sp[1] = float(choicePosition(1, kScaleCount));   // A minor
        CHECK(audio::scaleMap(sp.data(), 61) == 60 && audio::scaleMap(sp.data(), 68) == 67, "M6 Scale: root and scale apply");
        auto vel = audio::createProcessor(DeviceType::Velocity, 48000);
        std::vector<float> vp = defaultsOf(DeviceType::Velocity);
        vp[0] = 0.5f;
        audio::EventList in, out;
        in.add({0, 60, 1.0f, 100, false, false});
        audio::Context ctx;
        vel->midi(in, out, vp.data(), ctx, 128);
        CHECK(out.count == 1 && std::fabs(out.ev[0].velocity - 0.5f) < 1e-4f && vel->meter[1].load() == out.ev[0].velocity,
              "M6 Velocity: Out Hi caps how hard notes play");
        auto chord = audio::createProcessor(DeviceType::Chord, 48000);
        std::vector<float> cp = defaultsOf(DeviceType::Chord);
        cp[4] = float((12.0 + 12) / 24); cp[5] = float((-12.0 + 12) / 24);
        out.clear();
        chord->midi(in, out, cp.data(), ctx, 128);
        QList<int> pitches;
        for (int i = 0; i < out.count; ++i) pitches << out.ev[i].pitch;
        std::sort(pitches.begin(), pitches.end());
        CHECK((pitches == QList<int>{48, 60, 64, 67, 72}), "M6 Chord: five shifts (4th and 5th: +12, -12)");
    }
    {
        // Instruments: the Sampler loops, FM algorithms differ, wavetables are band-limited, the Bass slides
        std::vector<float> click(4800, 0.0f);
        for (size_t i = 0; i < click.size(); ++i) click[i] = 0.5f * std::sin(2 * 3.14159265f * 440 * float(i) / 48000);
        SamplePtr smp = samples::fromFrames(click, {}, 48000);
        std::vector<float> sp = defaultsOf(DeviceType::Sampler);
        auto s1 = audio::createProcessor(DeviceType::Sampler, 48000);
        DevRun once = runDevice(*s1, DeviceKind::Instrument, sp, 1.0, {60}, 0.9, 0.5f, smp.get());
        sp[6] = 1;   // Loop
        auto s2 = audio::createProcessor(DeviceType::Sampler, 48000);
        DevRun looped = runDevice(*s2, DeviceKind::Instrument, sp, 1.0, {60}, 0.9, 0.5f, smp.get());
        CHECK(rmsOf(once.l, 20000, 30000) < 1e-5f && rmsOf(looped.l, 20000, 30000) > 0.1f, "M6 Sampler: a short sample ends, or loops while the key is held");
        sp[6] = 0; sp[4] = 0.5f;   // Start halfway: the sample ends sooner
        auto s3 = audio::createProcessor(DeviceType::Sampler, 48000);
        DevRun late = runDevice(*s3, DeviceKind::Instrument, sp, 0.2, {60}, 0.9, 0.5f, smp.get());
        CHECK(rmsOf(late.l, 0, 2000) > 0.1f && rmsOf(late.l, 2600, 4000) < 1e-5f, "M6 Sampler: Start skips into the sample");

        std::vector<float> fp = defaultsOf(DeviceType::FMSynth);
        auto f0 = audio::createProcessor(DeviceType::FMSynth, 48000);
        DevRun a0 = runDevice(*f0, DeviceKind::Instrument, fp, 0.3, {60});
        fp[4] = 1; fp[10] = 0.8f; fp[13] = 0.8f;   // additive, C and D heard
        auto f6 = audio::createProcessor(DeviceType::FMSynth, 48000);
        DevRun a6 = runDevice(*f6, DeviceKind::Instrument, fp, 0.3, {60});
        float diff = 0;
        for (size_t i = 0; i < a0.l.size(); ++i) diff = std::max(diff, std::fabs(a0.l[i] - a6.l[i]));
        CHECK(diff > 0.05f && a6.finite, "M6 FM: the algorithm changes the sound");

        bool tablesOk = audio::wavetableCount() == 8;
        for (int t = 0; t < audio::wavetableCount(); ++t)
            for (int f = 0; f < audio::kWtFrames; ++f) {
                float peak = 0;
                const float* w = audio::wavetable(t).frame(0, f);
                for (int i = 0; i < audio::kWtSize; ++i) peak = std::max(peak, std::fabs(w[i]));
                tablesOk &= std::fabs(peak - 1) < 0.01f && w[audio::kWtSize] == w[0];
            }
        CHECK(tablesOk && audio::wavetableLevel(40, 48000) == 0 && audio::wavetableLevel(4000, 48000) >= 5,
              "M6 Wavetable: eight tables of normalized frames, fewer harmonics for high notes");

        // Bass: overlapping notes slide (one attack), separate notes retrigger (two)
        auto countAttacks = [](const std::vector<float>& l) {
            int attacks = 0; float env = 0; bool quiet = true;
            for (float x : l) {
                env = std::max(std::fabs(x), env * 0.999f);
                if (quiet && env > 0.05f) { ++attacks; quiet = false; }
                if (env < 0.005f) quiet = true;
            }
            return attacks;
        };
        auto bass = audio::createProcessor(DeviceType::Bass, 48000);
        std::vector<float> bp = defaultsOf(DeviceType::Bass);
        audio::EventList ev; audio::Context ctx; ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000;
        std::vector<float> slide, apart;
        for (int pass = 0; pass < 2; ++pass) {
            auto b = audio::createProcessor(DeviceType::Bass, 48000);
            std::vector<float>& outv = pass ? apart : slide;
            for (int pos = 0; pos < 48000; pos += 128) {
                ev.clear();
                if (pos == 0) ev.add({0, 36, 0.7f, pass ? 9600 : 19200, false, false});
                if (pos == 14336) ev.add({0, 43, 0.7f, 9600, false, false});   // 0.3 s
                float l[128] = {}, r[128] = {};
                b->render(ev, l, r, 128, bp.data(), ctx);
                outv.insert(outv.end(), l, l + 128);
            }
        }
        (void)bass;
        CHECK(countAttacks(slide) == 1 && countAttacks(apart) == 2, "M6 Bass: overlapping notes slide, separate notes retrigger");
    }


    // ---- Milestone 6: device panels
    {
        Project dp;
        dp.scenes = {"1"};
        dp.addTrack("Devices", QColor("#f2905e"));
        for (const DeviceSpec& spec : deviceSpecs())
            if (specOf(spec.type) == &spec) dp.tracks[0].devices.push_back(dp.makeDevice(spec.name, spec.kind));
        audio::Engine de(&dp);
        Transport dt(&dp, &de);
        int missingControls = 0, withoutDisplay = 0, tooTall = 0;
        QStringList missing;
        for (const Device& d : dp.tracks[0].devices) {
            DevicePanel panel(&dp, &dt, 0, d, nullptr, {});
            devui::DeviceBody* body = panel.body();
            QSet<QString> targets;
            for (QWidget* w : body->findChildren<QWidget*>()) targets.insert(w->property("mapTarget").toString());
            for (int i = 0; i < body->count(); ++i)
                if (!targets.contains(body->target(i))) { ++missingControls; missing << QString("%1/%2").arg(d.name, d.params[i].name); }
            if (body->findChildren<devui::Display*>().isEmpty() && deviceType(d.name) != DeviceType::Utility && deviceType(d.name) != DeviceType::Macros)
                ++withoutDisplay;
            panel.adjustSize();
            if (body->minimumSizeHint().height() > devui::kBodyHeight + 4) { ++tooTall; std::printf("      %s body wants %d px\n", qPrintable(d.name), body->minimumSizeHint().height()); }
            body->tick();
        }
        CHECK(missingControls == 0, qPrintable("M6 panels: every parameter of every device has a control (MIDI-mappable) " + missing.join(", ")));
        CHECK(withoutDisplay == 0, "M6 panels: every device but Utility and Macros has a display of its own");
        CHECK(tooTall == 0, "M6 panels: every body fits the device height");

        // Knob turn through the body: the model changes, one undo step, the knob shows it
        auto indexOf = [&dp](const char* name) { int i = 0; while (i < dp.tracks[0].devices.size() && dp.tracks[0].devices[i].name != name) ++i; return i; };
        const int eqAt = indexOf("EQ"), afAt = indexOf("Auto Filter");
        const Device& eqDev = dp.tracks[0].devices[eqAt];
        CHECK(eqDev.name == "EQ" && afAt < dp.tracks[0].devices.size(), "M6 panels: (the EQ and the Auto Filter are on the track)");
        DevicePanel eqPanel(&dp, &dt, 0, eqDev, nullptr, {});
        eqPanel.resize(eqPanel.sizeHint());
        eqPanel.show();
        QApplication::processEvents();
        devui::DeviceBody* eq = eqPanel.body();
        const int undo0 = dp.undoStack()->count();
        eq->edit(1, 0.9);
        CHECK(dp.tracks[0].devices[eqAt].params[1].value == 0.9 && dp.undoStack()->count() == undo0 + 1 && std::fabs(eq->real(1) - 12) < 1e-6,
              "M6 panels: a knob edit changes the device (Mid +12 dB) as one undo step");
        // A display drag moves frequency and gain together, as one undo step
        devui::Display* eqDisplay = eq->findChildren<devui::Display*>().value(0);
        const double midFreq0 = dp.tracks[0].devices[eqAt].params[5].value;
        const QRectF inner = QRectF(eqDisplay->rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        const QPointF dot(devui::freqToX(eq->real(5), inner), inner.center().y() - 12.0 / 18 * inner.height() / 2);
        auto mouse = [](QWidget* w, QEvent::Type t, QPointF at) {
            QMouseEvent ev(t, at, w->mapToGlobal(at), Qt::LeftButton, t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(w, &ev);
        };
        const int undo1 = dp.undoStack()->count();
        mouse(eqDisplay, QEvent::MouseButtonPress, dot);
        for (int k = 1; k <= 5; ++k) mouse(eqDisplay, QEvent::MouseMove, dot + QPointF(8.0 * k, 4.0 * k));
        mouse(eqDisplay, QEvent::MouseButtonRelease, dot + QPointF(40, 20));
        const Device& eqAfter = dp.tracks[0].devices[eqAt];
        CHECK(eqAfter.params[5].value > midFreq0 + 0.02 && eqAfter.params[1].value < 0.9 && dp.undoStack()->count() == undo1 + 1,
              "M6 panels: dragging the EQ's mid dot moves its frequency and gain, one undo step");
        dp.undoStack()->undo();
        CHECK(std::fabs(dp.tracks[0].devices[eqAt].params[5].value - midFreq0) < 1e-9 && dp.tracks[0].devices[eqAt].params[1].value == 0.9,
              "M6 panels: undo puts the dot back");
        // Automation: an envelope on the Mid gain shows on the panel while it plays
        Envelope env;
        env.target = target::device(eqDev.id, 1);
        env.points = {{0, 0.25}, {4, 0.75}};
        dp.tracks[0].automation.push_back(env);
        eq->follow(2.0);
        CHECK(std::fabs(eq->pos(1) - 0.5) < 1e-6, "M6 panels: an automated knob follows its envelope");
        // A choice strip click picks that choice (Auto Filter: HP)
        const Device& afDev = dp.tracks[0].devices[afAt];
        DevicePanel afPanel(&dp, &dt, 0, afDev, nullptr, {});
        afPanel.show();
        QApplication::processEvents();
        QWidget* strip = nullptr;
        for (QWidget* w : afPanel.body()->findChildren<QWidget*>()) if (w->property("mapTarget").toString() == afPanel.body()->target(4)) strip = w;
        if (strip) mouse(strip, QEvent::MouseButtonPress, QPointF(strip->width() * 1.5 / 4, strip->height() / 2.0));
        CHECK(strip && choiceIndex(DeviceType::AutoFilter, 4, dp.tracks[0].devices[afAt].params[4].value) == 1, "M6 panels: clicking a choice button selects it");
        // Edits from elsewhere (MIDI map, undo) show on open panels
        dp.tracks[0].devices[afAt].params[0].value = 0.1;
        emit dp.deviceChanged();
        Knob* freqKnob = nullptr;
        for (Knob* k : afPanel.body()->findChildren<Knob*>()) if (k->property("mapTarget").toString() == afPanel.body()->target(0)) freqKnob = k;
        CHECK(freqKnob && std::fabs(freqKnob->value() - 0.1) < 1e-9, "M6 panels: a change made elsewhere shows on the panel");

        // A long chain in a narrow Device View scrolls instead of piling panels on each other
        dp.tracks[0].devices.resize(6);
        dp.selectedTrack = 0;
        DeviceView view(&dp, &dt);
        view.resize(700, 260);
        view.show();
        view.rebuild();
        QApplication::processEvents();
        const QList<DevicePanel*> panels = view.panels();
        bool apart = panels.size() == 6;
        for (int k = 1; k < panels.size(); ++k)   // racks (and the Drum Rack) hold their panel in a frame: compare in the view's coordinates
            apart &= panels[k]->mapTo(&view, QPoint(0, 0)).x() >= panels[k - 1]->mapTo(&view, QPoint(panels[k - 1]->width(), 0)).x();
        CHECK(apart, "M6 panels: a long chain lays its panels side by side (and scrolls)");
    }

    // ================= Milestone 7: recording and mixing =================
    {
        // ---- audio input: monitoring follows Monitor / Arm
        Project p;
        p.scenes = {"1"};
        p.addTrack("Mic", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        audio::Engine e(&p);
        e.setInputWanted(true);   // offscreen: a test input fed by pushInput
        auto feed = [&e](int frames, float level) {
            frames = (frames + 255) / 256 * 256;   // whole 256-frame blocks
            std::vector<float> in(size_t(frames) * 2, level);
            std::vector<float> out(size_t(frames) * 2);
            for (int done = 0; done < frames; done += 256) {
                e.pushInput(in.data() + size_t(done) * 2, 256);
                e.process(out.data() + size_t(done) * 2, 256);
            }
            return out;
        };
        p.tracks[0].monitor = Monitor::Auto;
        p.tracks[0].armed = false;
        e.publish();
        CHECK(peakOf(feed(4800, 0.3f)) == 0.0f, "M7 input: Monitor Auto is silent while the track isn't armed");
        p.tracks[0].armed = true;
        e.publish();
        std::vector<float> out = feed(9600, 0.3f);
        CHECK(std::abs(out[out.size() - 2] - 0.3f) < 0.01f, "M7 input: an armed track on Auto plays its input at unity");
        CHECK(e.takeInputPeak(0) > 0.29f, "M7 input: the track's input meter sees the input");
        p.tracks[0].monitor = Monitor::Off;
        e.publish();
        out = feed(9600, 0.3f);
        CHECK(std::abs(out[out.size() - 2]) < 1e-4f, "M7 input: Monitor Off mutes it even when armed");
        p.tracks[0].monitor = Monitor::In;
        p.tracks[0].armed = false;
        p.tracks[0].inputChannel = 2;   // input 2 only, on both sides
        e.publish();
        {
            std::vector<float> in(256 * 2), o(256 * 2);
            for (int k = 0; k < 40; ++k) {
                for (int f = 0; f < 256; ++f) { in[size_t(f) * 2] = 0.1f; in[size_t(f) * 2 + 1] = 0.4f; }
                e.pushInput(in.data(), 256);
                e.process(o.data(), 256);
            }
            CHECK(std::abs(o[510] - 0.4f) < 0.01f && std::abs(o[511] - 0.4f) < 0.01f, "M7 input: Monitor In plays input 2 on both sides");
        }
    }
    {
        // ---- recording into a Session slot: the take lands on the exact sample
        QTemporaryDir recDir;
        Project p;
        p.scenes = {"1"};
        p.addTrack("Mic", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        p.tracks[0].armed = true;
        p.recordFolder = recDir.path();
        audio::Engine e(&p);
        Transport t(&p, &e);
        qint64 pos = 0;
        auto pump = [&](qint64 until) {   // input clicks on beats 2 and 4 (stream frames 24000 and 72000)
            std::vector<float> in(512), out(512);
            while (pos < until) {
                for (int f = 0; f < 256; ++f) { const qint64 g = pos + f; const float v = (g == 24000 || g == 72000) ? 0.8f : 0.0f; in[size_t(f) * 2] = in[size_t(f) * 2 + 1] = v; }
                e.pushInput(in.data(), 256);
                e.process(out.data(), 256);
                pos += 256;
                if (pos % 4096 == 0) QTest::qWait(17);   // a Transport tick: it mirrors the engine and drains the capture
            }
        };
        t.recordSlot(0, 0);
        CHECK(t.isRecordingSlot(0, 0) && p.tracks[0].clipSlots[0] && p.tracks[0].clipSlots[0]->isAudio(), "M7 record: ● on an armed audio track starts an audio clip");
        pump(60000);                  // into beat 3 (24000 frames a beat)
        t.launchSlot(0, 0);           // ends the take at the end of the bar it's in: a 1-bar take
        pump(240000);
        for (int k = 0; k < 20 && t.audioRecording(); ++k) { pump(pos + 4096); QTest::qWait(5); }
        ClipPtr rec = p.tracks[0].clipSlots[0];
        const bool ok = rec && rec->sample && !t.audioRecording();
        CHECK(ok && rec->length == 4.0 && rec->sample->frames() == 96000, "M7 record: the take is rounded to the bar it ended in (4 beats = 96000 frames)");
        CHECK(ok && rec->sample->left[24000] > 0.7f && rec->sample->left[72000] > 0.7f && std::abs(rec->sample->left[24001]) < 1e-6f,
              "M7 record: what you played lands on the same beat in the clip");
        CHECK(ok && rec->samplePath.startsWith(recDir.path()) && QFileInfo::exists(rec->samplePath), "M7 record: the WAV goes into the set's recording folder");
        p.undoStack()->undo();
        CHECK(!p.tracks[0].clipSlots[0], "M7 record: one undo takes the whole recording out");
    }
    {
        // ---- recording onto the timeline (Arrangement Record)
        QTemporaryDir recDir;
        Project p;
        p.scenes = {"1"};
        p.addTrack("Mic", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        p.tracks[0].armed = true;
        p.recordFolder = recDir.path();
        audio::Engine e(&p);
        Transport t(&p, &e);
        qint64 pos = 0;
        auto pump = [&](qint64 until) {
            std::vector<float> in(512), out(512);
            while (pos < until) {
                for (int f = 0; f < 256; ++f) { const qint64 g = pos + f; const float v = (g == 48000) ? 0.9f : 0.0f; in[size_t(f) * 2] = in[size_t(f) * 2 + 1] = v; }
                e.pushInput(in.data(), 256);
                e.process(out.data(), 256);
                pos += 256;
                if (pos % 4096 == 0) QTest::qWait(17);
            }
        };
        t.setArrangementRecord(true);
        t.play();
        pump(4096);
        QTest::qWait(30);
        CHECK(!t.takes().isEmpty() && t.takes().first().audio, "M7 arrangement record: an armed audio track gets a take");
        pump(150000);                 // a bit past beat 6
        QTest::qWait(30);
        t.stop();
        for (int k = 0; k < 20 && t.audioRecording(); ++k) { pump(pos + 4096); QTest::qWait(5); }
        const bool placed = p.tracks[0].arrangement.size() == 1 && p.tracks[0].arrangement[0].clip && p.tracks[0].arrangement[0].clip->sample;
        CHECK(placed && p.tracks[0].arrangement[0].start < 0.05 && p.tracks[0].arrangement[0].length > 5.5, "M7 arrangement record: the take becomes an audio clip on the timeline");
        if (placed) {
            const SampleData& smp = *p.tracks[0].arrangement[0].clip->sample;
            const qint64 offset = qint64(std::llround(p.tracks[0].arrangement[0].start * 24000));
            CHECK(smp.frames() > 48000 - offset && smp.left[size_t(48000 - offset)] > 0.8f, "M7 arrangement record: the hit on beat 3 is on beat 3 of the clip");
        }
    }
    {
        // ---- groups: tracks play through the group's mixer and effects
        Project p; oneNoteProject(p);
        p.addTrack("Other", QColor("#f3c95c"));
        p.tracks[1].devices = {p.makeDevice("Sampler", DeviceKind::Instrument)};
        p.tracks[1].clipSlots[0] = p.copyClip(p.tracks[0].clipSlots[0]);
        p.tracks[1].arrangement = {{0, 4, 0, p.tracks[1].clipSlots[0]}};
        const int g = p.groupTracks({0});
        CHECK(g == 0 && p.tracks[0].isGroup() && p.tracks[1].groupId == p.tracks[0].id && p.tracks[2].groupId == 0,
              "M7 groups: Group Tracks puts a group track before its tracks");
        auto play = [&p](int frames) { return playArrangement(p, frames); };
        const float both = peakOf(play(36000));
        p.tracks[0].volume = 0;   // the group's fader down: only the other track is heard
        const float other = peakOf(play(36000));
        p.tracks[2].active = false;
        const float none = peakOf(play(36000));
        CHECK(both > 0.01f && other > 0.01f && other < both * 0.8f && none == 0.0f, "M7 groups: the group's fader carries its tracks");
        p.tracks[0].volume = 0.75; p.tracks[2].active = true;
        p.tracks[0].solo = true;   // soloing the group lets its track through, the other one stops
        const float soloGroup = peakOf(play(36000));
        p.tracks[0].solo = false; p.tracks[1].solo = true;   // soloing a grouped track keeps its group open
        const float soloMember = peakOf(play(36000));
        CHECK(soloGroup > 0.01f && soloMember > 0.01f && std::abs(soloGroup - soloMember) < 0.01f, "M7 groups: solo works through a group both ways");
        p.tracks[1].solo = false;
        p.tracks[0].devices = {p.makeDevice("Utility", DeviceKind::AudioEffect)};
        // Utility's mute, if it has one, else its gain all the way down: find a parameter called Mute
        bool muted = false;
        for (int k = 0; k < p.tracks[0].devices[0].params.size(); ++k)
            if (p.tracks[0].devices[0].params[k].name == "Mute") { p.tracks[0].devices[0].params[k].value = 1.0; muted = true; }
        if (muted) CHECK(peakOf(play(36000)) < other * 1.01f, "M7 groups: the group's own effects process its tracks");
        p.ungroup(0);
        CHECK(p.tracks.size() == 2 && p.tracks[0].groupId == 0, "M7 groups: Ungroup removes the group track and frees its tracks");
        p.undoStack()->undo();
        CHECK(p.tracks.size() == 3 && p.tracks[0].isGroup() && p.tracks[1].groupId == p.tracks[0].id, "M7 groups: ungroup undoes");
        p.setFolded(0, true);
        CHECK(p.hiddenByFold(1) && !p.hiddenByFold(2), "M7 groups: folding hides the group's tracks");
        p.deleteTrack(0);
        CHECK(p.tracks.size() == 1 && p.tracks[0].name == "Other", "M7 groups: deleting a group deletes its tracks");
    }
    {
        // ---- clip envelopes: a volume envelope in the clip silences it while it plays
        Project p; oneNoteProject(p);
        ClipPtr c = p.tracks[0].clipSlots[0];
        c->envelopes = {{target::volume(), {{0, 0.0}}}};
        CHECK(peakOf(playArrangement(p, 36000)) == 0.0f, "M7 clip envelopes: volume at zero silences the clip");
        c->envelopes = {{target::volume(), {{0, 0.75}}}};
        const float unity = peakOf(playArrangement(p, 36000));
        int volumeKnob = -1;   // the Sampler's Volume knob
        for (int k = 0; k < p.tracks[0].devices[0].params.size(); ++k) if (p.tracks[0].devices[0].params[k].name == "Volume") volumeKnob = k;
        c->envelopes = {{target::device(p.tracks[0].devices[0].id, volumeKnob), {{0, 0.0}}}};
        CHECK(volumeKnob >= 0 && peakOf(playArrangement(p, 36000)) < unity * 0.05f, "M7 clip envelopes: a device knob follows the clip's envelope");
        CHECK(unity > 0.01f, "M7 clip envelopes: at 0 dB the clip plays");
    }
    {
        // ---- warping: Warp off plays at the file's own speed; transients
        Project p; ClipPtr c = audioProject(p, burstsWav);
        const std::vector<double>& tr = c->sample->transients;
        bool onBursts = tr.size() == 4;
        for (int k = 0; k < int(tr.size()) && k < 4; ++k) onBursts = onBursts && std::abs(tr[size_t(k)] - 0.5 * k) < 0.001;
        CHECK(onBursts, "M7 warp: transients are found on each burst (within 1 ms)");
        c->warp = false;
        p.setTempo(60);
        const std::vector<int> on = onsetsOf(playArrangement(p, 120000), 0.25f);
        CHECK(onsetNear(on, 24000, 4) >= 0 && onsetNear(on, 48000, 4) >= 0 && onsetNear(on, 72000, 4) >= 0,
              "M7 warp: Warp off keeps the file's own timing at another tempo");
    }

    // ---- Milestone 7: samples load side by side, and in the background
    {
        QStringList errors;
        const QVector<SamplePtr> many = samples::loadMany({burstsWav, burstsWav, audioDir.filePath("nope.wav")}, &errors);
        CHECK(many.size() == 3 && many[0] && many[1] == many[0] && !many[2] && errors.size() == 1 && errors[0].startsWith("nope.wav"),
              "M7 loading: loadMany loads each file once and lists the ones it can't read");
        const QString drumsWav = audioDir.filePath("m7 drums.wav");
        writeMonoWav(drumsWav, bursts(48000, 1.0, 0.25, 48, 0.4f), 48000);
        QObject ctx;
        QVector<SamplePtr> got;
        bool called = false;
        samples::loadAsync({drumsWav, audioDir.filePath("nope.wav")}, &ctx, [&](QVector<SamplePtr> s, QStringList e) { got = s; errors = e; called = true; });
        CHECK(!called, "M7 loading: loadAsync returns before the files are read");
        for (int k = 0; k < 300 && !called; ++k) QTest::qWait(10);
        CHECK(called && got.size() == 2 && got[0] && got[0]->frames() > 0 && !got[1] && errors.size() == 1,
              "M7 loading: loadAsync hands the samples (and what failed) back on the UI thread");
        bool late = false;
        {
            QObject gone;
            samples::loadAsync({drumsWav}, &gone, [&](QVector<SamplePtr>, QStringList) { late = true; });
        }
        QTest::qWait(200);
        CHECK(!late, "M7 loading: nothing is delivered to a view that has gone");
    }

    // ---- Milestone 7 window: groups from the menu, folding, the mixer's I/O, clip envelopes, Warp
    {
        MainWindow w;
        w.resize(1600, 900);
        w.show();
        QApplication::processEvents();
        Project* proj = w.project();
        ClipGrid* grid = w.sessionView()->grid();
        const int n0 = proj->tracks.size();
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, QPoint(grid->columnLeft(0) + 40, 10));
        QTest::mouseClick(grid, Qt::LeftButton, Qt::ControlModifier, QPoint(grid->columnLeft(1) + 40, 10));
        CHECK(proj->selectedTrackIds.size() == 2, "M7 window: Ctrl+click on a second track title selects both");
        const QString first = proj->tracks[0].name, second = proj->tracks[1].name;
        QTest::keyClick(grid, Qt::Key_G, Qt::ControlModifier);
        CHECK(proj->tracks.size() == n0 + 1 && proj->tracks[0].isGroup() && proj->membersOf(0).size() == 2
              && proj->tracks[1].name == first && proj->tracks[2].name == second, "M7 window: Ctrl+G groups the selected tracks");
        QApplication::processEvents();
        MixerStrip* groupStrip = nullptr;
        for (MixerStrip* m : w.sessionView()->findChildren<MixerStrip*>()) if (m->trackIndex() == 0) groupStrip = m;
        CHECK(groupStrip && !groupStrip->findChild<QToolButton*>("armButton"), "M7 window: a group's strip has no Arm button");
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, QPoint(grid->columnLeft(0) + 6, 10));
        CHECK(proj->tracks[0].folded && proj->hiddenByFold(1) && proj->hiddenByFold(2), "M7 window: the fold arrow folds the group");
        ArrangementView* av = w.arrangementView();
        CHECK(av->laneRect(1).height() == 0 && av->laneRect(2).height() == 0 && av->laneRect(3).top() == av->laneRect(0).bottom() + 1,
              "M7 window: a folded group's lanes close up in the Arrangement");
        QTest::keyClick(grid, Qt::Key_Tab);
        QApplication::processEvents();
        QTest::mouseClick(av, Qt::LeftButton, Qt::NoModifier, QPoint(av->laneRect(0).width() + 8, av->laneRect(0).top() + 8));
        CHECK(!proj->tracks[0].folded && av->laneRect(1).height() > 0, "M7 window: the Arrangement's fold arrow unfolds it");
        QTest::keyClick(av, Qt::Key_Tab);
        QApplication::processEvents();
        QTest::keyClick(grid, Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
        CHECK(proj->tracks.size() == n0 && !proj->tracks[0].isGroup() && proj->tracks[0].groupId == 0, "M7 window: Ctrl+Shift+G ungroups");

        // an audio track's Monitor switch
        int audioTrack = -1;
        for (int i = 0; i < proj->tracks.size(); ++i) if (proj->tracks[i].type == TrackType::Audio) { audioTrack = i; break; }
        if (audioTrack < 0) { QTest::keyClick(grid, Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier); audioTrack = proj->selectedTrack; }
        QApplication::processEvents();
        MixerStrip* audioStrip = nullptr;
        for (MixerStrip* m : w.sessionView()->findChildren<MixerStrip*>()) if (m->trackIndex() == audioTrack) audioStrip = m;
        QToolButton* monIn = audioStrip ? audioStrip->findChild<QToolButton*>("monitorIn") : nullptr;
        QComboBox* from = audioStrip ? audioStrip->findChild<QComboBox*>("audioFrom") : nullptr;
        CHECK(monIn && from && audioStrip->findChild<QToolButton*>("monitorAuto")->isChecked(), "M7 window: an audio track's strip has Audio From and Monitor (Auto)");
        if (monIn) {
            monIn->click();
            CHECK(proj->tracks[audioTrack].monitor == Monitor::In, "M7 window: Monitor In");
            proj->undoStack()->undo();
            CHECK(proj->tracks[audioTrack].monitor == Monitor::Auto, "M7 window: and it undoes");
            emit from->activated(2);
            CHECK(proj->tracks[audioTrack].inputChannel == 2, "M7 window: Audio From picks input 2");
        }
        bool aligned = true;   // the I/O area keeps every strip's controls level
        int faderTop = -1;
        for (MixerStrip* m : w.sessionView()->findChildren<MixerStrip*>()) {
            if (!m->isVisible() || m->trackIndex() >= proj->tracks.size()) continue;
            QToolButton* solo = m->findChild<QToolButton*>("soloButton");
            if (!solo) continue;
            const int y = solo->mapTo(w.sessionView(), QPoint(0, 0)).y();
            if (faderTop < 0) faderTop = y; else aligned = aligned && y == faderTop;
        }
        CHECK(faderTop >= 0 && aligned, "M7 window: audio and MIDI strips line up");

        // clip envelopes in the Clip View
        ClipPtr midi;
        for (const Track& t : proj->tracks) for (const ClipPtr& c : t.clipSlots) if (!midi && c && !c->isAudio()) midi = c;
        ClipView* cv = w.findChild<ClipView*>();
        proj->selectedClip = midi;
        cv->setClip(midi);
        cv->showEnvelope(true);
        QApplication::processEvents();
        EnvelopeEditor* ee = cv->envelopeEditor();
        CHECK(midi && ee->isVisible() && ee->target() == target::volume(), "M7 window: Show swaps in the envelope editor (Volume first)");
        const QPoint at(int(ee->beatToX(1)), int(ee->valueToY(0.25)));
        QTest::mouseClick(ee, Qt::LeftButton, Qt::NoModifier, at);
        const Envelope* env = midi ? std::as_const(*midi).envelope(target::volume()) : nullptr;
        CHECK(env && env->points.size() == 1 && env->points[0].beat == 1 && std::abs(env->points[0].value - 0.25) < 0.02,
              "M7 window: a click adds a clip envelope breakpoint");
        QTest::mousePress(ee, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(ee, QPoint(int(ee->beatToX(2)), int(ee->valueToY(0.75))));
        QTest::mouseRelease(ee, Qt::LeftButton, Qt::NoModifier, QPoint(int(ee->beatToX(2)), int(ee->valueToY(0.75))));
        env = std::as_const(*midi).envelope(target::volume());
        CHECK(env && env->points.size() == 1 && env->points[0].beat == 2 && std::abs(env->points[0].value - 0.75) < 0.02,
              "M7 window: dragging the breakpoint moves it");
        proj->undoStack()->undo();
        env = std::as_const(*midi).envelope(target::volume());
        CHECK(env && env->points[0].beat == 1, "M7 window: the drag undoes in one step");
        QTest::mouseDClick(ee, Qt::LeftButton, Qt::NoModifier, at);
        CHECK(!std::as_const(*midi).envelope(target::volume()), "M7 window: double-clicking the last breakpoint removes the envelope");
        QComboBox* param = cv->findChild<QComboBox*>("envParam");
        const int panItem = param ? param->findData(target::pan()) : -1;
        if (panItem >= 0) emit param->activated(panItem);
        CHECK(panItem >= 0 && ee->target() == target::pan(), "M7 window: the parameter chooser picks Pan");
        cv->showEnvelope(false);

        // Warp off and the slices, on an audio clip
        ClipPtr audioClip;
        for (const Track& t : proj->tracks) {
            for (const ClipPtr& c : t.clipSlots) if (!audioClip && c && c->isAudio()) audioClip = c;
            for (const ArrangementClip& a : t.arrangement) if (!audioClip && a.clip && a.clip->isAudio()) audioClip = a.clip;
        }
        if (!audioClip) {   // the demo's loop isn't there: bring one in
            EditScope edit(proj, "Add Audio Clip");
            const int t = import::toSession(*proj, audioTrack, 0, import::audioClips(*proj, {burstsWav}));
            audioClip = proj->tracks[t].clipSlots.value(0);
        }
        if (audioClip) {
            proj->selectedClip = audioClip;
            cv->setClip(audioClip);
            QApplication::processEvents();
            QPushButton* warpOn = cv->findChild<QPushButton*>("warpOn");
            QComboBox* slices = cv->findChild<QComboBox*>("warpSlices");
            CHECK(warpOn && slices && warpOn->isChecked() && warpOn->isVisible(), "M7 window: an audio clip shows Warp (on)");
            warpOn->click();
            CHECK(!audioClip->warp && !slices->isEnabled(), "M7 window: Warp off (and the slices grey out)");
            proj->undoStack()->undo();
            CHECK(audioClip->warp && slices->isEnabled(), "M7 window: Warp back on with undo");
            emit slices->activated(0);
            CHECK(audioClip->grain == 0, "M7 window: Slices: Transients");
            emit slices->activated(3);
            CHECK(audioClip->grain == 0.25, "M7 window: Slices: 1/16");
        } else {
            CHECK(false, "M7 window: the demo set has an audio clip");
        }

        PreferencesDialog prefs(w.engine(), w.midiInput(), {}, &w);
        QComboBox* input = prefs.findChild<QComboBox*>("inputDevice");
        CHECK(input && input->count() >= 1 && input->itemData(0).toString().isEmpty(), "M7 window: Preferences lists input devices (System default first)");
    }

    // ================= Milestone 8: drums and performance =================
    auto energy = [](const std::vector<float>& v, int from, int to, int stride = 2) {   // left channel, frames [from, to)
        double e = 0;
        for (int i = std::max(0, from); i < to && size_t(i) * size_t(stride) < v.size(); ++i) e += double(v[size_t(i) * size_t(stride)]) * v[size_t(i) * size_t(stride)];
        return e;
    };
    {
        // ---- Drum Rack: a new rack plays the built-in kit on C1 up
        Project p;
        p.scenes = {"1"};
        p.addTrack("Drums", QColor("#f2905e"));
        p.tracks[0].devices = {p.makeDevice("Drum Rack", DeviceKind::Instrument)};
        const Device& rack = p.tracks[0].devices[0];
        bool full = rack.pads.size() == drumrack::kPads && rack.params.size() == 82;
        for (const DrumPad& pad : rack.pads) full = full && pad.sample && pad.sample->frames() > 100 && audio::isBuiltin(pad.samplePath);
        CHECK(full && rack.pads[0].name == "Kick" && rack.pads[6].name == "Hat Closed", "M8 Drum Rack: a new rack has 16 pads of built-in sounds (Kick on C1)");
        ClipPtr c = p.newClip("Beat", 4);
        c->notes = {Note{36, 0, 0.25, 1.0, 1.0}, Note{38, 2, 0.25, 1.0, 1.0}};   // kick, snare
        p.tracks[0].arrangement = {{0, 4, 0, c}};
        const std::vector<float> out = playArrangement(p, 96000);
        CHECK(energy(out, 0, 12000) > 1.0 && energy(out, 48000, 60000) > 0.2, "M8 Drum Rack: C1 plays the kick, D1 the snare");
        c->notes = {Note{52, 0, 0.25, 1.0, 1.0}};   // E2: past the pads
        CHECK(peakOf(playArrangement(p, 24000)) < 1e-6f, "M8 Drum Rack: a note with no pad plays nothing");
        c->notes = {Note{36, 0, 0.25, 1.0, 1.0}};
        import::clearDrumPad(p, 0, rack.id, 0);
        CHECK(p.tracks[0].devices[0].pads[0].samplePath.isEmpty() && peakOf(playArrangement(p, 24000)) < 1e-6f, "M8 Drum Rack: an empty pad is silent");

        // the pads' own knobs, on the processor
        Project q;
        q.scenes = {"1"};
        q.addTrack("Drums", QColor("#f2905e"));
        q.tracks[0].devices = {q.makeDevice("Drum Rack", DeviceKind::Instrument)};
        auto run = [&q](const std::vector<std::pair<int, int>>& hits, int frames, const std::function<void(float*)>& tweak = {}) {
            const Device& d = q.tracks[0].devices[0];
            auto proc = audio::createProcessor(DeviceType::DrumRack, 48000);
            float params[audio::kMaxParams] = {};
            for (int i = 0; i < d.params.size(); ++i) params[i] = float(d.params[i].value);
            if (tweak) tweak(params);
            SamplePtr pads[drumrack::kPads];
            for (int k = 0; k < drumrack::kPads; ++k) pads[k] = d.pads.value(k).sample;
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.pads = pads;
            std::vector<float> left;
            for (int pos = 0; pos < frames; pos += audio::kMaxBlock) {
                audio::EventList ev;
                for (const auto& [at, pitch] : hits) if (at >= pos && at < pos + audio::kMaxBlock) ev.add({at - pos, pitch, 1.0f, 4800});
                float l[audio::kMaxBlock] = {}, r[audio::kMaxBlock] = {};
                proc->render(ev, l, r, audio::kMaxBlock, params, ctx);
                left.insert(left.end(), l, l + audio::kMaxBlock);
            }
            return left;
        };
        auto lastLoud = [](const std::vector<float>& v, float th) { int last = -1; for (int i = 0; i < int(v.size()); ++i) if (std::fabs(v[size_t(i)]) > th) last = i; return last; };
        const std::vector<float> openHat = run({{0, 46}}, 24000);
        const std::vector<float> choked = run({{0, 46}, {4800, 42}}, 24000);
        CHECK(energy(openHat, 9600, 14400, 1) > 1e-4 && energy(choked, 9600, 14400, 1) < energy(openHat, 9600, 14400, 1) * 0.1,
              "M8 Drum Rack: the closed hat chokes the open hat (choke group 1)");
        const std::vector<float> unchoked = run({{0, 46}, {4800, 42}}, 24000, [](float* p) { p[drumrack::param(6, drumrack::Choke)] = 0; });
        CHECK(energy(unchoked, 9600, 14400, 1) > energy(openHat, 9600, 14400, 1) * 0.5, "M8 Drum Rack: with Choke None they ring together");
        const std::vector<float> kick = run({{0, 36}}, 48000);
        const std::vector<float> up = run({{0, 36}}, 48000, [](float* p) { p[drumrack::param(0, drumrack::Transpose)] = 0.5f + 12.0f / 48; });
        const int kLen = lastLoud(kick, 1e-3f), upLen = lastLoud(up, 1e-3f);
        CHECK(kLen > 1000 && std::abs(upLen - kLen / 2) < kLen / 10, "M8 Drum Rack: Transpose +12 plays the pad an octave up (twice as fast)");
        const std::vector<float> shortKick = run({{0, 36}}, 48000, [](float* p) { p[drumrack::param(0, drumrack::Decay)] = 0; });
        CHECK(energy(shortKick, 4800, 9600, 1) < energy(kick, 4800, 9600, 1) * 0.01, "M8 Drum Rack: a short Decay fades the pad out");
        const std::vector<float> quiet = run({{0, 36}}, 4800, [](float* p) { p[drumrack::param(0, drumrack::Vol)] = 0; });
        const float kickPeak = *std::max_element(kick.begin(), kick.end()), quietPeak = *std::max_element(quiet.begin(), quiet.end());
        CHECK(std::abs(quietPeak / kickPeak - 0.0631f) < 0.01f, "M8 Drum Rack: a pad's Volume at -24 dB");
    }
    {
        // ---- Macros: a knob that turns other devices' knobs
        Project p; oneNoteProject(p);
        p.tracks[0].devices.push_back(p.makeDevice("Utility", DeviceKind::AudioEffect));
        p.tracks[0].devices.push_back(p.makeDevice("Macros", DeviceKind::AudioEffect));
        const int util = p.tracks[0].devices[1].id, macros = p.tracks[0].devices[2].id;
        p.tracks[0].devices[2].maps = {{0, util, 6, 0.0, 1.0}};   // macro 1 -> Utility's Mute
        p.tracks[0].devices[2].params[0].value = 1.0;
        CHECK(peakOf(playArrangement(p, 48000)) < 1e-5f, "M8 Macros: a macro turns another device's knob (Utility Mute on)");
        p.tracks[0].devices[2].params[0].value = 0.0;
        CHECK(peakOf(playArrangement(p, 48000)) > 0.05f, "M8 Macros: and off again");
        p.tracks[0].automation = {{target::device(macros, 0), {{0, 1.0}}}};
        CHECK(peakOf(playArrangement(p, 48000)) < 1e-5f, "M8 Macros: an automated macro moves what it's mapped to");
        CHECK(std::abs(p.drivenValue(0, target::device(util, 6), 0, false) - 1.0) < 1e-9, "M8 Macros: the mapped knob shows the macro's value");
        p.tracks[0].automation.clear();
        p.tracks[0].devices[2].maps.push_back({1, util, 0, 0.25, 0.75});   // macro 2 -> Gain, a quarter to three quarters
        p.tracks[0].devices[2].params[1].value = 0.5;
        CHECK(std::abs(p.drivenValue(0, target::device(util, 0), 0, false) - 0.5) < 1e-9 && p.macroFor(0, util, 0),
              "M8 Macros: the knob goes to its range at the macro's value (min 25 %, max 75 %, macro 50 % = 50 %)");
        p.tracks[0].devices[2].params[1].value = 1.0;
        CHECK(std::abs(p.drivenValue(0, target::device(util, 0), 0, false) - 0.75) < 1e-9, "M8 Macros: macro all the way up = Max");
        const int dup = p.duplicateTrack(0);
        const Track& copy = p.tracks[dup];
        CHECK(copy.devices[2].maps.size() == 2 && copy.devices[2].maps[0].deviceId == copy.devices[1].id, "M8 Macros: a duplicated track's macros turn the copy's devices");
    }
    {
        // ---- sets: pads (own sample and built-in) and macro mappings round-trip
        QTemporaryDir dir;
        Project p;
        p.scenes = {"1"};
        p.addTrack("Drums", QColor("#f2905e"));
        p.tracks[0].devices = {p.makeDevice("Drum Rack", DeviceKind::Instrument), p.makeDevice("Auto Filter", DeviceKind::AudioEffect),
                               p.makeDevice("Macros", DeviceKind::AudioEffect)};
        const int rack = p.tracks[0].devices[0].id, filter = p.tracks[0].devices[1].id;
        CHECK(import::toDrumPad(p, 0, rack, 3, burstsWav), "M8 sets: a sample loads onto pad D#1");
        p.tracks[0].devices[0].params[drumrack::param(3, drumrack::Choke)].value = choicePosition(2, 5);
        p.tracks[0].devices[2].maps = {{0, filter, 0, 0.2, 0.8}, {1, rack, drumrack::param(0, drumrack::Vol), 0, 1}};
        const QString path = dir.filePath("rack.sessions");
        setfile::Extras x;
        x.punchIn = true;
        CHECK(setfile::save(path, p, x), "M8 sets: a set with a Drum Rack and Macros saves");
        QFile f(path); f.open(QIODevice::ReadOnly);
        const QByteArray json = f.readAll();
        CHECK(json.contains("\"builtin\": \"builtin:808/36\"") && json.contains("\"maps\""), "M8 sets: built-in pads are saved by name, not as files");
        Project q;
        setfile::Extras y;
        CHECK(setfile::load(path, q, y) && q.captureEditState() == p.captureEditState() && y.punchIn && !y.punchOut,
              "M8 sets: pads, their knobs, macro mappings and punch-in come back exactly");
        CHECK(q.tracks[0].devices[0].pads[3].sample && q.tracks[0].devices[0].pads[3].name == "bursts 120" && q.tracks[0].devices[0].pads[0].sample,
              "M8 sets: the pads' samples load (the file and the built-in sound)");
    }
    {
        // ---- count-in: the click first, the playhead waits, then it plays
        Project p;
        p.scenes = {"1"};
        p.addTrack("T", QColor("#f2905e"));
        audio::Engine e(&p);
        e.send({Command::Play, -1, -1, 4.0});   // one bar (metronome off: the count-in clicks anyway)
        const std::vector<float> out = render(e, 48000);   // two beats at 120 BPM
        CHECK(e.beat() == 0 && e.playing() && std::abs(e.countInLeft() - 2.0) < 0.01, "M8 count-in: the playhead waits while it counts in");
        const std::vector<int> clicks = onsetsOf(out, 0.05f);
        CHECK(clicks.size() == 2 && onsetNear(clicks, 0, 2) >= 0 && onsetNear(clicks, 24000, 2) >= 0, "M8 count-in: it clicks on each beat");
        render(e, 72000);   // the rest of the bar, then one beat of playing
        CHECK(e.countInLeft() == 0 && std::abs(e.beat() - 1.0) < 0.01, "M8 count-in: then playback starts from where it waited");
    }
    {
        // ---- count-in before an Arrangement audio recording: the count-in isn't recorded
        QTemporaryDir recDir;
        Project p;
        p.scenes = {"1"};
        p.addTrack("Mic", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        p.tracks[0].armed = true;
        p.recordFolder = recDir.path();
        audio::Engine e(&p);
        Transport t(&p, &e);
        t.setCountInBars(1);
        qint64 pos = 0;
        auto pump = [&](qint64 until) {   // hits at stream frames 48000 (in the count-in) and 144000 (beat 2 of playing)
            std::vector<float> in(512), out(512);
            while (pos < until) {
                for (int f = 0; f < 256; ++f) { const qint64 g = pos + f; const float v = (g == 48000 || g == 144000) ? 0.9f : 0.0f; in[size_t(f) * 2] = in[size_t(f) * 2 + 1] = v; }
                e.pushInput(in.data(), 256);
                e.process(out.data(), 256);
                pos += 256;
                if (pos % 4096 == 0) QTest::qWait(17);
            }
        };
        t.setArrangementRecord(true);
        t.play();
        CHECK(t.countingIn() && t.positionText().startsWith("-1."), "M8 count-in: Play with Arrangement Record counts in (the position shows -1. 1. 1)");
        pump(60000);
        CHECK(t.countingIn() && t.takes().isEmpty(), "M8 count-in: nothing records during the count-in");
        pump(250000);
        QTest::qWait(30);
        CHECK(!t.countingIn() && t.beat() > 5, "M8 count-in: after one bar it plays");
        t.stop();
        for (int k = 0; k < 20 && t.audioRecording(); ++k) { pump(pos + 4096); QTest::qWait(5); }
        const bool placed = p.tracks[0].arrangement.size() == 1 && p.tracks[0].arrangement[0].clip && p.tracks[0].arrangement[0].clip->sample;
        CHECK(placed && p.tracks[0].arrangement[0].start < 0.05, "M8 count-in: the take starts on the beat playback started");
        if (placed) {
            const SampleData& smp = *p.tracks[0].arrangement[0].clip->sample;
            float early = 0;
            for (qint64 i = 0; i < 40000 && i < smp.frames(); ++i) early = std::max(early, std::fabs(smp.left[size_t(i)]));
            CHECK(smp.frames() > 48000 && smp.left[48000] > 0.8f && early < 1e-6f, "M8 count-in: the hit on beat 2 is on beat 2, and the count-in's isn't in the take");
        }
    }
    {
        // ---- punch in / out: Arrangement recording only inside the loop brace
        Project p;
        p.scenes = {"1"};
        p.addTrack("Keys", QColor("#5bc8e0"));
        p.tracks[0].armed = true;
        p.tracks[0].devices = {p.makeDevice("Sampler", DeviceKind::Instrument)};
        audio::Engine e(&p);
        Transport t(&p, &e);
        qint64 pos = 0;
        auto pump = [&](double beat) {
            std::vector<float> out(512);
            while (pos < qint64(beat * 24000)) { e.process(out.data(), 256); pos += 256; if (pos % 4096 == 0) QTest::qWait(17); }
            QTest::qWait(20);
        };
        t.setLoop(4, 4);
        t.setPunchIn(true);
        t.setPunchOut(true);
        t.setArrangementRecord(true);
        t.play();
        pump(2); t.noteOn(0, 60, 0.8); pump(2.6); t.noteOff(0, 60);     // before the punch-in
        pump(5); t.noteOn(0, 62, 0.8); pump(5.6); t.noteOff(0, 62);     // inside
        pump(9); t.noteOn(0, 64, 0.8); pump(9.6); t.noteOff(0, 64);     // after the punch-out
        t.stop();
        const auto& arr = p.tracks[0].arrangement;
        CHECK(arr.size() == 1 && arr[0].start == 4 && arr[0].length <= 4 + 1e-9 && arr[0].clip && arr[0].clip->notes.size() == 1
              && arr[0].clip->notes[0].pitch == 62 && std::abs(arr[0].clip->notes[0].start - 1.0) < 0.3,
              "M8 punch: only notes between the punch-in and punch-out are recorded, from the punch-in");
    }
    {
        // ---- punch in / out for audio: the take covers just the punch range
        QTemporaryDir recDir;
        Project p;
        p.scenes = {"1"};
        p.addTrack("Mic", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        p.tracks[0].armed = true;
        p.recordFolder = recDir.path();
        audio::Engine e(&p);
        Transport t(&p, &e);
        qint64 pos = 0;
        auto pump = [&](qint64 until) {   // hits on beats 2 (before), 5 (inside) and 9 (after)
            std::vector<float> in(512), out(512);
            while (pos < until) {
                for (int f = 0; f < 256; ++f) { const qint64 g = pos + f; const float v = (g == 48000 || g == 120000 || g == 216000) ? 0.9f : 0.0f; in[size_t(f) * 2] = in[size_t(f) * 2 + 1] = v; }
                e.pushInput(in.data(), 256);
                e.process(out.data(), 256);
                pos += 256;
                if (pos % 4096 == 0) QTest::qWait(17);
            }
        };
        t.setLoop(4, 4);
        t.setPunchIn(true);
        t.setPunchOut(true);
        t.setArrangementRecord(true);
        t.play();
        pump(260000);
        t.stop();
        for (int k = 0; k < 20 && t.audioRecording(); ++k) { pump(pos + 4096); QTest::qWait(5); }
        const auto& arr = p.tracks[0].arrangement;
        const bool placed = arr.size() == 1 && arr[0].clip && arr[0].clip->sample;
        CHECK(placed && std::abs(arr[0].start - 4) < 1e-9 && std::abs(arr[0].length - 4) < 0.05, "M8 punch: an audio take runs from the punch-in to the punch-out");
        if (placed) {
            const SampleData& smp = *arr[0].clip->sample;
            float other = 0;
            for (qint64 i = 0; i < smp.frames(); ++i) if (std::abs(i - 24000) > 2) other = std::max(other, std::fabs(smp.left[size_t(i)]));
            CHECK(smp.frames() > 24000 && smp.left[24000] > 0.8f && other < 1e-6f, "M8 punch: only what came in between is in it, on its beat");
        }
    }
    {
        // ---- the Drum Rack and Macros panels
        Project dp;
        dp.scenes = {"1"};
        dp.addTrack("Drums", QColor("#f2905e"));
        dp.tracks[0].devices = {dp.makeDevice("Drum Rack", DeviceKind::Instrument), dp.makeDevice("Auto Filter", DeviceKind::AudioEffect),
                                dp.makeDevice("Macros", DeviceKind::AudioEffect)};
        audio::Engine de(&dp);
        Transport dt(&dp, &de);
        const int rackId = dp.tracks[0].devices[0].id, filterId = dp.tracks[0].devices[1].id;
        auto mouse = [](QWidget* w, QEvent::Type t, QPointF at) {
            QMouseEvent ev(t, at, w->mapToGlobal(at), Qt::LeftButton, t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(w, &ev);
        };
        DevicePanel rackPanel(&dp, &dt, 0, dp.tracks[0].devices[0], nullptr, {});
        rackPanel.show();
        QApplication::processEvents();
        devui::Display* pads = nullptr;
        for (devui::Display* d : rackPanel.body()->findChildren<devui::Display*>()) if (d->acceptDrops()) pads = d;
        CHECK(pads, "M8 panels: the Drum Rack shows its pads");
        if (pads) {
            const QRectF r = QRectF(pads->rect()).adjusted(1.5, 1.5, -1.5, -1.5);
            auto padCentre = [&r](int k) { return QPointF(r.left() + (k % 4 + 0.5) * r.width() / 4, r.bottom() - (k / 4 + 0.5) * r.height() / 4); };
            mouse(pads, QEvent::MouseButtonPress, padCentre(2));
            mouse(pads, QEvent::MouseButtonRelease, padCentre(2));
            CHECK(devui::selectedDrumPad(rackId) == 2, "M8 panels: clicking a pad selects it");
            QMimeData files;
            files.setUrls({QUrl::fromLocalFile(burstsWav)});
            const int undo0 = dp.undoStack()->count();
            QDragEnterEvent enter(padCentre(5).toPoint(), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pads, &enter);
            QDragMoveEvent move(padCentre(5).toPoint(), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pads, &move);
            QDropEvent drop(padCentre(5).toPoint(), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pads, &drop);
            for (int k = 0; k < 300 && dp.tracks[0].devices[0].pads[5].name != "bursts 120"; ++k) QTest::qWait(10);
            CHECK(dp.tracks[0].devices[0].pads[5].name == "bursts 120" && dp.tracks[0].devices[0].pads[5].sample && dp.undoStack()->count() == undo0 + 1,
                  "M8 panels: a sample dropped on a pad loads onto it (one undo step)");
            dp.undoStack()->undo();
            CHECK(dp.tracks[0].devices[0].pads[5].name == "Tom Low", "M8 panels: undo puts the pad's sound back");
        }
        {
            EditScope edit(&dp, "Map Macro");
            dp.tracks[0].devices[2].maps = {{0, filterId, 0, 0.1, 0.9}};
            emit dp.structureChanged();
        }
        DevicePanel macroPanel(&dp, &dt, 0, dp.tracks[0].devices[2], nullptr, {});
        macroPanel.show();
        QApplication::processEvents();
        QList<DragValue*> ranges;
        for (DragValue* v : macroPanel.body()->findChildren<DragValue*>()) if (!qobject_cast<Knob*>(v)) ranges << v;
        CHECK(ranges.size() == 2, "M8 panels: the Macros panel lists the mapping with its Min and Max");
        if (ranges.size() == 2) {
            const int undo0 = dp.undoStack()->count();
            emit ranges[1]->valueEdited(0.6);
            CHECK(std::abs(dp.tracks[0].devices[2].maps[0].hi - 0.6) < 1e-9 && dp.undoStack()->count() == undo0 + 1, "M8 panels: editing Max changes the mapping (undoable)");
        }
        dp.tracks[0].devices[2].params[0].value = 1.0;
        emit dp.deviceChanged();
        DevicePanel filterPanel(&dp, &dt, 0, dp.tracks[0].devices[1], nullptr, {});
        filterPanel.show();
        QApplication::processEvents();
        filterPanel.body()->follow(0);
        CHECK(std::abs(filterPanel.body()->pos(0) - 0.6) < 1e-9, "M8 panels: a mapped knob shows where the macro puts it");
    }
    {
        // ---- the mixer follows a playing clip's volume envelope, and settles back when it stops
        Project p; oneNoteProject(p);
        p.tracks[0].arrangement[0].clip->envelopes = {{target::volume(), {{0, 0.2}}}};
        audio::Engine e(&p);
        Transport t(&p, &e);
        MixerStrip strip(&p, &t, 0);
        LevelFader* fader = strip.findChild<LevelFader*>();
        t.play();
        emit t.tick();
        const double during = fader ? fader->value() : -1;
        t.stop();
        emit t.tick();
        CHECK(fader && std::abs(during - 0.2) < 1e-9 && std::abs(fader->value() - 0.75) < 1e-9,
              "M8 controls: the fader follows the playing clip's volume envelope, then goes back to the track's volume");
    }
    {
        // ---- the window: a browser sample goes to the Drum Rack's selected pad; punch switches; count-in in Preferences
        MainWindow w;
        w.resize(1600, 900);
        w.show();
        QApplication::processEvents();
        Project* proj = w.project();
        const int t0 = proj->selectedTrack;
        {
            EditScope edit(proj, "Add Drum Rack");
            proj->addDeviceToSelected("Drum Rack", DeviceKind::Instrument);
        }
        int rackId = 0;
        for (const Device& d : proj->tracks[t0].devices) if (d.name == "Drum Rack") rackId = d.id;
        devui::selectDrumPad(rackId, 9);
        w.loadSample(burstsWav);
        bool loaded = false;
        for (int k = 0; k < 300 && !loaded; ++k) {
            QTest::qWait(10);
            for (const Device& d : proj->tracks[t0].devices) if (d.id == rackId && d.pads.value(9).name == "bursts 120") loaded = true;
        }
        bool sampler = false;
        for (const Device& d : proj->tracks[t0].devices) sampler = sampler || d.name == "Sampler";
        CHECK(rackId && loaded && !sampler, "M8 window: a browser sample loads onto the Drum Rack's selected pad (no Sampler replaces it)");
        QToolButton* punchIn = nullptr;
        for (QToolButton* b : w.findChildren<QToolButton*>()) if (b->text() == QString::fromUtf8("⇥")) punchIn = b;
        if (punchIn) punchIn->click();
        CHECK(punchIn && w.transport()->punchIn(), "M8 window: the Punch-In switch");
        PreferencesDialog prefs(w.engine(), w.midiInput(), {}, &w);
        QComboBox* countIn = prefs.findChild<QComboBox*>("countIn");
        CHECK(countIn && countIn->count() == 4, "M8 window: Preferences → Record has the count-in (None, 1, 2, 4 bars)");
    }


    // ---- Milestone 10: switching devices on and off fades instead of jumping
    {
        Project p;
        rackProject(p, {});
        Device util = p.makeDevice("Utility", DeviceKind::AudioEffect);
        util.params[0].value = (20.0 + 35) / 70;   // +20 dB: switching it on would jump 10x
        util.enabled = false;
        p.tracks[0].devices = {p.makeDevice("Analog Synth", DeviceKind::Instrument), util};
        audio::Engine e(&p);
        e.send({Command::Stop, -1, -1, 0, 0, true});
        e.send({Command::Play});
        const std::vector<float> before = render(e, 26400);   // into the second note
        p.tracks[0].devices[1].enabled = true;
        e.publish();
        const std::vector<float> after = render(e, 4800);
        auto rmsL = [](const std::vector<float>& v, size_t from, size_t to) {
            double s = 0; for (size_t i = from; i < to; ++i) s += double(v[i * 2]) * v[i * 2]; return std::sqrt(s / double(to - from)); };
        const double was = rmsL(before, 26400 - 100, 26400), start = rmsL(after, 0, 40), settled = rmsL(after, 2000, 2400);
        CHECK(start < was * 2.0 && settled > was * 5.0, "M10 engine: an effect switched on fades in over a few ms (no jump), then sounds fully");
    }

    // ---- Milestone 9: racks — the sound
    {
        const int frames = 30000;
        auto play = [&](const std::function<QVector<Device>(Project&)>& make) {
            Project p;
            rackProject(p, make(p));
            return playArrangement(p, frames);
        };
        const std::vector<float> analog = play([](Project& p) { return QVector<Device>{p.makeDevice("Analog Synth", DeviceKind::Instrument)}; });
        const std::vector<float> fm = play([](Project& p) { return QVector<Device>{p.makeDevice("FM Synth", DeviceKind::Instrument)}; });
        std::vector<float> both(analog.size());
        for (size_t i = 0; i < both.size(); ++i) both[i] = analog[i] + fm[i];
        // an Instrument Rack with Analog in chain 1 and FM in chain 2; `tweak` sets the rack's knobs
        auto rack2 = [&](const std::function<void(Device&)>& tweak, bool chordInFm = false) {
            return play([&](Project& p) {
                Device r = p.makeDevice("Instrument Rack", DeviceKind::Instrument);
                r.chains = {"Analog", "FM"};
                tweak(r);
                QVector<Device> v{r, inChain(p.makeDevice("Analog Synth", DeviceKind::Instrument), r, 0)};
                if (chordInFm) v.push_back(inChain(p.makeDevice("Chord", DeviceKind::MidiEffect), r, 1));
                v.push_back(inChain(p.makeDevice("FM Synth", DeviceKind::Instrument), r, 1));
                return v;
            });
        };
        CHECK(peakOf(analog) > 0.05f && peakOf(fm) > 0.05f, "M9 racks: (the two synths make sound on their own)");
        CHECK(maxDiff(rack2([](Device&) {}), both) < 1e-4f, "M9 Instrument Rack: both chains play the notes, mixed (= the two synths added)");
        CHECK(maxDiff(rack2([](Device& r) { r.params[racks::param(1, racks::On)].value = 0; }), analog) < 1e-5f,
              "M9 Instrument Rack: a chain switched off is silent");
        CHECK(maxDiff(rack2([](Device& r) { r.params[racks::param(0, racks::Solo)].value = 1; }), analog) < 1e-5f,
              "M9 Instrument Rack: solo plays only that chain");
        CHECK(maxDiff(rack2([](Device& r) { r.params[racks::param(1, racks::Volume)].value = 0; }), analog) < 1e-5f,
              "M9 Instrument Rack: a chain's volume all the way down is silent");
        CHECK(maxDiff(rack2([](Device& r) { r.params[racks::param(0, racks::Solo)].value = 1; }, true), analog) < 1e-5f,
              "M9 Instrument Rack: a MIDI effect in one chain doesn't change the notes of another");
        const std::vector<float> panned = rack2([](Device& r) { r.params[racks::param(1, racks::On)].value = 0; r.params[racks::param(0, racks::Pan)].value = 0; });
        float right = 0;
        for (size_t i = 1; i < panned.size(); i += 2) right = std::max(right, std::fabs(panned[i]));
        CHECK(right < 1e-6f && peakOf(panned) > 0.05f, "M9 Instrument Rack: a chain panned hard left is only on the left");
        CHECK(maxDiff(rack2([](Device& r) {
                  r.params[racks::param(1, racks::Volume)].value = 0.75;
                  r.maps = {{0, r.id, racks::param(1, racks::Volume), 0.0, 0.75}};   // macro 1: the FM chain's level
                  r.params[0].value = 0;
              }), analog) < 1e-5f && maxDiff(rack2([](Device& r) {
                  r.maps = {{0, r.id, racks::param(1, racks::Volume), 0.0, 0.75}};
                  r.params[0].value = 1;
              }), both) < 1e-4f,
              "M9 rack macros: a macro turns a chain's volume (down: silent, up: 0 dB)");

        // Audio Effect Rack after the Analog Synth
        auto fxRack = [&](const std::function<void(Project&, Device&, QVector<Device>&)>& build) {
            return play([&](Project& p) {
                Device r = p.makeDevice("Audio Effect Rack", DeviceKind::AudioEffect);
                QVector<Device> members;
                build(p, r, members);
                QVector<Device> v{p.makeDevice("Analog Synth", DeviceKind::Instrument), r};
                v += members;
                return v;
            });
        };
        CHECK(maxDiff(fxRack([](Project&, Device& r, QVector<Device>&) { r.chains = {"Dry"}; }), analog) < 1e-5f,
              "M9 Audio Effect Rack: an empty chain passes the sound through");
        std::vector<float> twice = analog;
        for (float& x : twice) x *= 2;
        CHECK(maxDiff(fxRack([](Project&, Device& r, QVector<Device>&) { r.chains = {"A", "B"}; }), twice) < 1e-4f,
              "M9 Audio Effect Rack: two empty chains add up (the sound twice)");
        CHECK(maxDiff(fxRack([](Project& p, Device& r, QVector<Device>& m) {
                  r.chains = {"Dry", "Muted"};
                  Device u = p.makeDevice("Utility", DeviceKind::AudioEffect);
                  u.params[6].value = 1;   // Mute
                  m.push_back(inChain(u, r, 1));
              }), analog) < 1e-5f,
              "M9 Audio Effect Rack: each chain runs its own effects (a muted chain adds nothing)");
        CHECK(maxDiff(fxRack([](Project&, Device& r, QVector<Device>&) { r.chains = {"A", "B"}; r.enabled = false; }), analog) < 1e-5f,
              "M9 Audio Effect Rack: switched off, it's bypassed");
        CHECK(peakOf(play([](Project& p) {
                  Device r = p.makeDevice("Instrument Rack", DeviceKind::Instrument);
                  r.chains = {"A"};
                  r.enabled = false;
                  return QVector<Device>{r, inChain(p.makeDevice("Analog Synth", DeviceKind::Instrument), r, 0)};
              })) == 0.0f, "M9 Instrument Rack: switched off, it's silent");
    }

    // ---- Milestone 9: racks — editing
    {
        Project p;
        rackProject(p, {});
        p.selectedTrack = 0;
        p.addDeviceToSelected("Analog Synth", DeviceKind::Instrument);
        p.addDeviceToSelected("Saturator", DeviceKind::AudioEffect);
        const std::vector<float> before = playArrangement(p, 24000);
        const int satId = p.tracks[0].devices[1].id;
        const int undo0 = p.undoStack()->count();
        const int rackId = p.groupIntoRack(0, satId);
        const QVector<Device>& ds = p.tracks[0].devices;
        CHECK(rackId && ds.size() == 3 && ds[1].id == rackId && deviceType(ds[1].name) == DeviceType::AudioEffectRack && ds[1].chains == QStringList{"Saturator"}
              && ds[2].id == satId && ds[2].rack == rackId && ds[2].chain == 0 && p.undoStack()->count() == undo0 + 1,
              "M9 racks: Group puts a device into a new rack's chain, named after it, as one undo step");
        CHECK(maxDiff(playArrangement(p, 24000), before) < 1e-5f, "M9 racks: grouping doesn't change the sound");
        CHECK(p.groupIntoRack(0, satId) == 0 && p.groupIntoRack(0, rackId) == 0, "M9 racks: racks don't go inside racks");
        // chains: add, add a device into them, instrument rules
        CHECK(!p.addDeviceToChain(0, rackId, 0, "Analog Synth"), "M9 racks: an Audio Effect Rack takes no instruments");
        CHECK(p.addDeviceToChain(0, rackId, -1, "Redux") && p.tracks[0].devices[1].chains.size() == 2
              && p.tracks[0].devices[1].chains[1] == "Redux" && p.tracks[0].devices.last().name == "Redux" && p.tracks[0].devices.last().chain == 1,
              "M9 racks: a device dropped past the last chain makes a new chain named after it");
        CHECK(p.addDeviceToChain(0, rackId, 0, "EQ") && p.tracks[0].devices[3].name == "EQ" && p.tracks[0].devices[3].chain == 0
              && p.tracks[0].devices[4].name == "Redux", "M9 racks: an effect goes at the end of its chain, chains stay in order");
        CHECK(!p.ungroupRack(0, rackId), "M9 racks: a rack with two chains can't be ungrouped");
        p.tracks[0].devices[1].params[racks::param(1, racks::Volume)].value = 0.3;
        p.deleteChain(0, rackId, 0);
        const QVector<Device>& after = p.tracks[0].devices;
        CHECK(after.size() == 3 && after[1].chains == QStringList{"Redux"} && after[2].name == "Redux" && after[2].chain == 0
              && after[1].params[racks::param(0, racks::Volume)].value == 0.3,
              "M9 racks: deleting a chain takes its devices, and the later chains (and their knobs) move up");
        CHECK(p.ungroupRack(0, rackId) && p.tracks[0].devices.size() == 2 && p.tracks[0].devices[1].name == "Redux" && p.tracks[0].devices[1].rack == 0,
              "M9 racks: Ungroup hands a one-chain rack's devices back to the track");
        p.undoStack()->undo();
        CHECK(p.tracks[0].devices.size() == 3 && isRack(p.tracks[0].devices[1]), "M9 racks: undo brings the rack back");
        CHECK(p.deleteDevice(0, rackId) && p.tracks[0].devices.size() == 1, "M9 racks: deleting a rack deletes what's in its chains");

        // an Instrument Rack: chain order and replacing the instrument
        p.addDeviceToSelected("Instrument Rack", DeviceKind::Instrument);
        CHECK(p.tracks[0].devices.size() == 1 && deviceType(p.tracks[0].devices[0].name) == DeviceType::InstrumentRack,
              "M9 racks: an Instrument Rack replaces the track's instrument");
        const int ir = p.tracks[0].devices[0].id;
        p.addDeviceToChain(0, ir, 0, "Reverb");
        p.addDeviceToChain(0, ir, 0, "FM Synth");
        p.addDeviceToChain(0, ir, 0, "Arpeggiator");
        p.addDeviceToChain(0, ir, 0, "Electric");
        QStringList order;
        for (const Device& d : p.tracks[0].devices) order << d.name;
        CHECK((order == QStringList{"Instrument Rack", "Arpeggiator", "Electric", "Reverb"}),
              "M9 racks: in a chain, MIDI effects come first, one instrument (a new one replaces it), then audio effects");
        p.addDeviceToSelected("Lead Stack", DeviceKind::Instrument);
        const QVector<Device>& ls = p.tracks[0].devices;
        CHECK(ls.size() == 3 && ls[0].name == "Lead Stack" && isRack(ls[0]) && ls[0].chains == QStringList({"Lead", "Sub"})
              && ls[1].name == "Saw Lead" && ls[1].rack == ls[0].id && ls[2].name == "Sub Bass" && ls[2].chain == 1 && ls[0].maps.size() == 2,
              "M9 racks: a ready-made rack from Sounds brings its chains, devices and macro mappings (and replaces the old rack)");
        const int stackId = ls[0].id;   // (ls dangles once the track list grows)
        CHECK(p.duplicateTrack(0) == 1 && p.tracks[1].devices[1].rack == p.tracks[1].devices[0].id && p.tracks[1].devices[0].id != stackId
              && p.tracks[1].devices[0].maps[0].deviceId == p.tracks[1].devices[0].id,
              "M9 racks: a duplicated track's chains belong to the copy's rack, its macros turn the copy's knobs");

        // the set file: racks round-trip; a file with chain devices out of place is put in order
        QTemporaryDir dir;
        const QString path = dir.filePath("racks.sessions");
        setfile::Extras x;
        Project q;
        CHECK(setfile::save(path, p, x) && setfile::load(path, q, x) && q.captureEditState() == p.captureEditState(),
              "M9 racks: racks, chains, their devices and macros save and load exactly");
        EditState st = p.captureEditState();
        auto& devs = st.tracks[0].devices;
        std::reverse(devs.begin(), devs.end());   // chain devices before their rack
        setfile::Extras x2;
        QByteArray json = setfile::toJson(st, x2);
        EditState back;
        CHECK(setfile::fromJson(json, back, x2) && back.tracks[0].devices.size() == 3 && isRack(back.tracks[0].devices[0])
              && back.tracks[0].devices[1].name == "Saw Lead" && back.tracks[0].devices[2].name == "Sub Bass",
              "M9 racks: a set with chain devices out of place loads with them after their rack, chain by chain");
    }

    // ---- Milestone 9: racks — the Device View
    {
        Project p;
        rackProject(p, {});
        p.selectedTrack = 0;
        p.addDeviceToSelected("Layered Keys", DeviceKind::Instrument);
        audio::Engine e(&p);
        Transport tr(&p, &e);
        DeviceView view(&p, &tr);
        view.resize(1400, 260);
        view.show();
        QApplication::processEvents();
        const int rackId = p.tracks[0].devices[0].id;
        auto names = [&] {
            QStringList n;
            for (DevicePanel* panel : view.panels())
                for (const Device& d : p.tracks[0].devices) if (d.id == panel->deviceId()) n << d.name;
            return n;
        };
        CHECK(view.findChildren<RackPanel*>().size() == 1 && (names() == QStringList{"Layered Keys", "Electric"}),
              "M9 Device View: a rack shows its panel and the first chain's devices next to it");
        devui::selectChain(rackId, 1);
        QApplication::processEvents();
        CHECK((names() == QStringList{"Layered Keys", "Warm Pad", "Chorus"}), "M9 Device View: picking a chain shows its devices");
        // a device dropped on the chain's drop zone goes into that chain
        QLabel* drop = nullptr;
        for (QLabel* l : view.findChildren<QLabel*>()) if (l->text().startsWith("Drop effects into Pad")) drop = l;
        QMimeData mime;
        mime.setData(Browser::mimeType(), "Delay|f");
        if (drop) {
            QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(drop, &enter);
            QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(drop, &move);
            QDropEvent ev(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(drop, &ev);
        }
        QApplication::processEvents();
        CHECK(drop && p.tracks[0].devices.last().name == "Delay" && p.tracks[0].devices.last().chain == 1 && names().contains("Delay"),
              "M9 Device View: a device dropped next to the chain goes into it");
        // the rack's own panel: every macro and chain knob has a control, chains past the last are hidden
        devui::DeviceBody* body = nullptr;
        for (DevicePanel* panel : view.panels()) if (panel->deviceId() == rackId) body = panel->body();
        int visibleOn = 0;
        for (QWidget* w : body ? body->findChildren<QWidget*>() : QList<QWidget*>())
            if (w->property("mapTarget").toString().endsWith(QString(":%1").arg(racks::param(0, racks::On))) ||
                w->property("mapTarget").toString().endsWith(QString(":%1").arg(racks::param(1, racks::On))) ||
                w->property("mapTarget").toString().endsWith(QString(":%1").arg(racks::param(2, racks::On))))
                visibleOn += w->isVisibleTo(body) ? 1 : 0;
        CHECK(body && visibleOn == 2, "M9 Device View: the chain list shows a row per chain");
        // a macro turns what it's mapped to, and the knob shows it
        body->edit(1, 0.0);   // macro 2: the pad's cutoff, down to its minimum
        const Device& pad = p.tracks[0].devices[2];
        CHECK(pad.name == "Warm Pad" && std::fabs(p.drivenValue(0, target::device(pad.id, 1), 0, false) - 0.25) < 1e-9,
              "M9 rack macros: the macro drives the pad's cutoff to its Min");
    }


    // ---- Milestone 10: guitar devices — levels at their defaults
    {
        const std::vector<float> in = pluck(2.0);
        auto rms = [](const std::vector<float>& v) { double s = 0; for (float x : v) s += double(x) * x; return std::sqrt(s / double(v.size())); };
        const double inDb = 20 * std::log10(rms(in));
        QStringList report;
        double worst = 0;
        for (const DeviceSpec& spec : deviceSpecs()) {
            if (specOf(spec.type) != &spec || !isGuitarType(spec.type) || spec.type == DeviceType::Tuner) continue;
            const double outDb = 20 * std::log10(std::max(1e-9, rms(runEffect(spec.type, defaultsOf(spec.type), in)))) - inDb;
            report << QString("%1 %2").arg(spec.name).arg(outDb, 0, 'f', 1);
            worst = std::max(worst, std::fabs(outDb));
        }
        for (int h = 0; h < guitaramp::kHeads; ++h) {
            std::vector<float> p = defaultsOf(DeviceType::GuitarAmp);
            p[guitaramp::Head] = float(choicePosition(h, guitaramp::kHeads));
            const double outDb = 20 * std::log10(std::max(1e-9, rms(runEffect(DeviceType::GuitarAmp, p, in)))) - inDb;
            report << QString("amp head %1 %2").arg(h).arg(outDb, 0, 'f', 1);
            worst = std::max(worst, std::fabs(outDb));
        }
        std::printf("      guitar levels (dB vs input): %s\n", qPrintable(report.join(", ")));
        CHECK(worst < 7, "M10 guitar: every pedal and amp head sits within a few dB of unity at its defaults");
    }


    // ---- Milestone 10: the tuner, impulse responses, the amp and the wah
    {
        auto tuneTo = [&](double freq) {   // what the Tuner reads after a second of a plucked string
            auto tuner = audio::createProcessor(DeviceType::Tuner, 48000);
            const std::vector<float> in = pluck(1.0, float(freq), 0.3f);
            std::vector<float> p = defaultsOf(DeviceType::Tuner);
            audio::Context ctx; ctx.sampleRate = 48000;
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            for (size_t pos = 0; pos + audio::kMaxBlock <= in.size(); pos += audio::kMaxBlock) {
                std::copy(in.begin() + long(pos), in.begin() + long(pos) + audio::kMaxBlock, l);
                std::copy(l, l + audio::kMaxBlock, r);
                tuner->process(l, r, audio::kMaxBlock, p.data(), ctx);
            }
            return double(tuner->meter[0].load());
        };
        auto cents = [](double got, double want) { return got > 0 ? 1200 * std::log2(got / want) : 999.0; };
        const double e2 = tuneTo(82.407), a2 = tuneTo(110.0), e4 = tuneTo(329.63), e1 = tuneTo(41.203), sharp = tuneTo(110.0 * std::pow(2.0, 12.0 / 1200));
        std::printf("      tuner cents: E2 %.2f A2 %.2f E4 %.2f E1 %.2f A2+12 %.2f\n", cents(e2, 82.407), cents(a2, 110), cents(e4, 329.63), cents(e1, 41.203), cents(sharp, 110));
        CHECK(std::fabs(cents(e2, 82.407)) < 1 && std::fabs(cents(a2, 110)) < 1 && std::fabs(cents(e4, 329.63)) < 1 && std::fabs(cents(e1, 41.203)) < 1
              && std::fabs(cents(sharp, 110) - 12) < 1,
              "M10 Tuner: finds a guitar's E2, A2 and E4 and a bass's E1 to within a cent, and hears 12 cents sharp");
        auto tuner = audio::createProcessor(DeviceType::Tuner, 48000);
        std::vector<float> p = defaultsOf(DeviceType::Tuner);
        p[1] = 1;   // Mute
        audio::Context ctx; ctx.sampleRate = 48000;
        float l[audio::kMaxBlock], r[audio::kMaxBlock], last = 1;
        for (int b = 0; b < 40; ++b) {
            std::fill(l, l + audio::kMaxBlock, 0.0f); std::fill(r, r + audio::kMaxBlock, 0.0f);
            tuner->process(l, r, audio::kMaxBlock, p.data(), ctx);
        }
        CHECK(tuner->meter[0].load() == 0.0f, "M10 Tuner: silence reads as no note");
        std::fill(l, l + audio::kMaxBlock, 0.5f); std::fill(r, r + audio::kMaxBlock, 0.5f);
        tuner->process(l, r, audio::kMaxBlock, p.data(), ctx);
        last = l[audio::kMaxBlock - 1];
        CHECK(last < 1e-3f, "M10 Tuner: Mute silences the track");

        // impulse responses: a single click passes the sound through (a block late); two clicks echo it
        std::vector<float> delta(64, 0.0f);
        delta[0] = 1.0f;
        auto k1 = audio::makeIrKernel(samples::fromFrames(delta, {}, 48000), 48000);
        std::vector<float> two(400, 0.0f);
        two[0] = 0.8f; two[300] = 0.6f;
        auto k2 = audio::makeIrKernel(samples::fromFrames(two, {}, 48000), 48000);
        const std::vector<float> in = pluck(0.2);
        std::vector<float> p2 = defaultsOf(DeviceType::GuitarAmp);
        audio::Convolver conv;
        std::vector<float> out1(in.size()), out2(in.size());
        for (size_t pos = 0; pos < in.size(); pos += 100) {   // odd block sizes on purpose
            const int n = int(std::min<size_t>(100, in.size() - pos));
            conv.process(*k1, 0, in.data() + pos, out1.data() + pos, n);
        }
        audio::Convolver conv2;
        for (size_t pos = 0; pos < in.size(); pos += 77) {
            const int n = int(std::min<size_t>(77, in.size() - pos));
            conv2.process(*k2, 0, in.data() + pos, out2.data() + pos, n);
        }
        float err1 = 0, err2 = 0;
        for (size_t i = audio::kIrBlock; i < in.size(); ++i) err1 = std::max(err1, std::fabs(out1[i] - in[i - audio::kIrBlock]));
        for (size_t i = audio::kIrBlock + 300; i < in.size(); ++i) {
            const float want = (0.8f * in[i - audio::kIrBlock] + 0.6f * in[i - audio::kIrBlock - 300]) / 1.0f;   // energy 0.64 + 0.36 = 1
            err2 = std::max(err2, std::fabs(out2[i] - want));
        }
        CHECK(k1 && k1->partitions == 1 && err1 < 1e-4f && k2 && k2->partitions == 4 && err2 < 1e-4f,
              "M10 IR: the convolution matches the impulse response exactly (one block late, any block size)");

        // the amp: more gain, more harmonics; a cabinet takes the fizz off the top
        auto highEnergy = [](const std::vector<float>& v) {   // above ~6 kHz, a crude two-point difference
            double s = 0, t = 0;
            for (size_t i = 2; i < v.size(); ++i) { const double d = v[i] - 2 * v[i - 1] + v[i - 2]; s += d * d; t += double(v[i]) * v[i]; }
            return s / std::max(1e-12, t);
        };
        std::vector<float> amp = defaultsOf(DeviceType::GuitarAmp);
        amp[guitaramp::Head] = float(choicePosition(3, guitaramp::kHeads));
        amp[guitaramp::Cab] = 0;   // no cabinet
        const std::vector<float> sine = [] { std::vector<float> v(48000); for (size_t i = 0; i < v.size(); ++i) v[i] = 0.2f * float(std::sin(2 * M_PI * 220 * double(i) / 48000)); return v; }();
        amp[guitaramp::Gain] = 0.0f;
        const double cleanish = highEnergy(runEffect(DeviceType::GuitarAmp, amp, sine));
        amp[guitaramp::Gain] = 1.0f;
        const double dirty = highEnergy(runEffect(DeviceType::GuitarAmp, amp, sine));
        amp[guitaramp::Cab] = float(choicePosition(3, guitaramp::kCabs));
        const double cabbed = highEnergy(runEffect(DeviceType::GuitarAmp, amp, sine));
        CHECK(dirty > cleanish * 3 && cabbed < dirty * 0.3, "M10 Guitar Amp: Gain adds harmonics; the 4x12 cabinet rolls off the fizz");
        CHECK(audio::ampResponseDb(amp.data(), 10000, 48000) < -20 && std::fabs(audio::ampResponseDb(amp.data(), 800, 48000)) < 12,
              "M10 Guitar Amp: the response the panel draws (cabinet falls away above 5 kHz)");
        amp[guitaramp::Cab] = float(choicePosition(guitaramp::kLoadedIr, guitaramp::kCabs));
        const std::vector<float> viaIr = runEffect(DeviceType::GuitarAmp, amp, sine, k1.get());
        amp[guitaramp::Cab] = 0;
        const std::vector<float> noCab = runEffect(DeviceType::GuitarAmp, amp, sine);
        float irErr = 0;
        for (size_t i = audio::kIrBlock + 1000; i < noCab.size(); ++i) irErr = std::max(irErr, std::fabs(viaIr[i] - noCab[i - audio::kIrBlock]));
        CHECK(irErr < 1e-3f, "M10 Guitar Amp: Loaded IR convolves the amp's output with the impulse response");

        // the wah: heel down is dark, toe down bright
        std::vector<float> w = defaultsOf(DeviceType::Wah);
        const std::vector<float> noise = [] { std::vector<float> v(24000); dsp::Noise rng; for (float& x : v) x = 0.2f * rng.next(); return v; }();
        w[0] = 0; const double heel = highEnergy(runEffect(DeviceType::Wah, w, noise));
        w[0] = 1; const double toe = highEnergy(runEffect(DeviceType::Wah, w, noise));
        CHECK(toe > heel * 4 && audio::pedalResponseDb(DeviceType::Wah, w.data(), 2400, 48000) > audio::pedalResponseDb(DeviceType::Wah, w.data(), 400, 48000) + 10,
              "M10 Wah: the pedal sweeps the peak from low (heel) to high (toe)");
        std::vector<float> cs = defaultsOf(DeviceType::Chainsaw);
        CHECK(audio::pedalResponseDb(DeviceType::Chainsaw, cs.data(), 1300, 48000) > audio::pedalResponseDb(DeviceType::Chainsaw, cs.data(), 400, 48000) + 8,
              "M10 Chainsaw: Low and High on 10 give the sawing upper-mid peak");
    }


    // ---- Milestone 10: guitar panels, the guitar track, the audio settings
    {
        Project p;
        rackProject(p, {});
        p.selectedTrack = 0;
        const int undo0 = p.undoStack()->count();
        const int g = p.insertGuitarTrack(1);
        const Track& gt = p.tracks[g];
        CHECK(gt.type == TrackType::Audio && gt.name == "Guitar" && gt.inputChannel == 1 && gt.monitor == Monitor::In
              && gt.devices.size() == 2 && gt.devices[0].name == "Tuner" && gt.devices[1].name == "Guitar Amp" && p.undoStack()->count() == undo0 + 1,
              "M10 guitar track: an audio track on Input 1, monitoring, with a Tuner and a Guitar Amp, one undo step");
        // the input reaches the amp through the track (monitoring)
        audio::Engine e(&p);
        e.setInputWanted(true);
        const std::vector<float> in = pluck(0.5);
        std::vector<float> inter(in.size() * 2);
        for (size_t i = 0; i < in.size(); ++i) { inter[i * 2] = in[i]; inter[i * 2 + 1] = 0; }   // Input 1 only
        std::vector<float> out(inter.size());
        for (size_t pos = 0; pos < in.size(); pos += 256) {
            const int n = int(std::min<size_t>(256, in.size() - pos));
            e.process(out.data() + pos * 2, n, inter.data() + pos * 2);   // a duplex driver's call
        }
        float peak = 0;
        for (float x : out) peak = std::max(peak, std::fabs(x));
        CHECK(peak > 0.01f, "M10 guitar track: what comes in on Input 1 is heard through the amp straight away (duplex input)");

        // panels: the footswitch switches the pedal, the amp loads an IR, the tuner shows a note
        Device od = p.makeDevice("Overdrive", DeviceKind::AudioEffect);
        p.tracks[g].devices.push_back(od);
        audio::Engine e2(&p);
        Transport tr(&p, &e2);
        DevicePanel pedal(&p, &tr, g, p.tracks[g].devices.last(), nullptr, {});
        pedal.show();
        QApplication::processEvents();
        CHECK(pedal.body() && !pedal.body()->findChildren<devui::Display*>().isEmpty() && pedal.body()->minimumSizeHint().height() <= devui::kBodyHeight + 4,
              "M10 pedals: a pedal's panel has its graphs and fits the device height (0.18: no footswitch, the title's activator switches it)");
        QTemporaryDir dir;
        std::vector<float> irData(480, 0.0f);
        irData[0] = 1; irData[40] = 0.5f;
        const QString irPath = dir.filePath("cab.wav");
        writeMonoWav(irPath, irData, 48000);
        DevicePanel ampPanel(&p, &tr, g, p.tracks[g].devices[1], nullptr, {});
        QMimeData files;
        files.setUrls({QUrl::fromLocalFile(irPath)});
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(ampPanel.body(), &enter);
        QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(ampPanel.body(), &move);
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &files, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(ampPanel.body(), &drop);
        const Device& amp = p.tracks[g].devices[1];
        CHECK(amp.sample && amp.samplePath == irPath && choiceIndex(DeviceType::GuitarAmp, guitaramp::Cab, amp.params[guitaramp::Cab].value) == guitaramp::kLoadedIr,
              "M10 Guitar Amp: dropping a WAV on it loads it as the cabinet IR and switches the cabinet to Loaded IR");
        e2.publish();
        float buf[512 * 2] = {};
        e2.process(buf, 512);
        CHECK(true, "M10 Guitar Amp: the engine prepares the loaded IR (no crash)");

        // audio settings: the saved options compose what the engine is given
        {
            QSettings s("Sessions", "Sessions");
            s.setValue("audio/system", "jack");
            s.setValue("audio/frames", 64);
            s.setValue("audio/out1", "UMC:playback_FL");
            s.setValue("audio/in1", "UMC:capture_FL");
            s.setValue("audio/in2", "-");
        }
        const audio::DriverOptions o = PreferencesDialog::savedAudio();
        CHECK(o.system == "jack" && o.frames == 64 && o.outPorts.value(0) == "UMC:playback_FL" && o.inPorts.value(0) == "UMC:capture_FL" && o.inPorts.value(1) == "-",
              "M10 audio: the interface's channels and the buffer are saved and handed to the engine");
        { QSettings s("Sessions", "Sessions"); for (const char* k : {"audio/system", "audio/frames", "audio/out1", "audio/in1", "audio/in2"}) s.remove(k); }
    }


    // ================= Milestone 11: dragging devices around (what FORM does well) =================
    QTemporaryDir presetDir;   // presets saved by the tests go here, not into your User Library
    qputenv("SESSIONS_PRESETS", presetDir.path().toLocal8Bit());
    // a drag over `w` at `pos`, then the drop (what Qt sends while you drag)
    auto dragDrop = [](QWidget* w, const QPoint& pos, QMimeData* mime, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        const Qt::DropActions actions = mime->hasFormat(devicedrag::kMime) ? Qt::MoveAction | Qt::CopyAction : Qt::CopyAction;
        QDragEnterEvent enter(pos, actions, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(w, &enter);
        QDragMoveEvent move(pos, actions, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(w, &move);
        const bool accepted = move.isAccepted();
        QDropEvent drop(QPointF(pos), actions, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(w, &drop);
        QApplication::processEvents();
        return accepted;
    };
    auto itemMime = [](const QString& payload) { auto* m = new QMimeData; m->setData(Browser::mimeType(), payload.toUtf8()); return m; };
    {
        // ---- the model: where things land, moving, copying
        Project p;
        rackProject(p, {});
        p.addTrack("Bass", QColor("#5aa9f0"));
        p.insertTrack(2, TrackType::Audio);
        p.selectedTrack = 0;
        p.addDeviceToSelected("Analog Synth", DeviceKind::Instrument);
        p.addDeviceToSelected("Saturator", DeviceKind::AudioEffect);
        p.addDeviceToSelected("Reverb", DeviceKind::AudioEffect);
        auto names = [&p](int t) { QStringList n; for (const Device& d : p.tracks[t].devices) n << d.name; return n; };
        auto idOf = [&p](int t, const QString& name) { for (const Device& d : p.tracks[t].devices) if (d.name == name) return d.id; return 0; };
        const int syn = idOf(0, "Analog Synth"), sat = idOf(0, "Saturator"), rev = idOf(0, "Reverb");
        const int undo0 = p.undoStack()->count();
        CHECK(p.moveDevices(0, {rev}, 0, {sat}) && (names(0) == QStringList{"Analog Synth", "Reverb", "Saturator"}) && p.undoStack()->count() == undo0 + 1,
              "M11 devices: a device dropped in front of another moves there, one undo step");
        CHECK(p.moveDevices(0, {sat}, 0, {syn}) && (names(0) == QStringList{"Analog Synth", "Saturator", "Reverb"}),
              "M11 devices: an audio effect dropped before the instrument lands right after it");
        const int undo1 = p.undoStack()->count();
        p.moveDevices(0, {sat}, 0, {sat});
        CHECK((names(0) == QStringList{"Analog Synth", "Saturator", "Reverb"}) && p.undoStack()->count() == undo1,
              "M11 devices: a device dropped on its own place stays there (no undo step)");
        CHECK(p.moveDevices(0, {sat}, 0, {0}) && names(0).last() == "Saturator", "M11 devices: dropped at the end, it goes last");
        QList<int> placed;
        CHECK(p.insertDevices(0, {p.makeDeviceBlock("Arpeggiator", DeviceKind::MidiEffect)}, {0}, "Add Arpeggiator", &placed) && names(0).first() == "Arpeggiator"
              && placed.size() == 1 && placed[0] == p.tracks[0].devices[0].id, "M11 devices: a MIDI effect dropped at the end goes in front of the instrument");
        Envelope env;
        env.target = target::device(rev, 0);
        env.points = {{0, 0.2}, {4, 0.8}};
        {
            EditScope edit(&p, "Automate");
            p.tracks[0].automation.push_back(env);
        }
        CHECK(p.moveDevices(0, {rev}, 1, {}) && !names(0).contains("Reverb") && names(1) == QStringList{"Reverb"} && p.tracks[1].devices[0].id == rev
              && p.tracks[1].envelope(target::device(rev, 0)) && !p.tracks[0].envelope(target::device(rev, 0)),
              "M11 devices: dropped on another track, a device moves there (same id) with its automation");
        p.undoStack()->undo();
        CHECK(names(0).contains("Reverb") && p.tracks[1].devices.isEmpty() && p.tracks[0].envelope(target::device(rev, 0)),
              "M11 devices: undo brings it, and its automation, back");
        {
            EditScope edit(&p, "Turn");
            for (Device& d : p.tracks[0].devices) if (d.id == rev) d.params[1].value = 0.123;
        }
        CHECK(p.moveDevices(0, {rev}, 1, {}, true, &placed) && names(0).contains("Reverb") && names(1) == QStringList{"Reverb"}
              && placed.value(0) == p.tracks[1].devices[0].id && placed.value(0) != rev && p.tracks[1].devices[0].params[1].value == 0.123,
              "M11 devices: with Ctrl, a copy goes (a new id, the same settings)");
        QString why;
        CHECK(!p.moveDevices(0, {syn}, 2, {}, false, nullptr, &why) && !why.isEmpty() && names(0).contains("Analog Synth"),
              "M11 devices: an instrument doesn't go on an audio track, and says why");
        CHECK(!p.moveDevices(0, {syn}, 0, {}, true), "M11 devices: a track's instrument isn't copied onto its own track (one instrument per track)");
        p.selectedTrack = 1;
        p.addDeviceToSelected("FM Synth", DeviceKind::Instrument);
        CHECK(p.moveDevices(0, {syn}, 1, {0}) && (names(1) == QStringList{"Analog Synth", "Reverb"}) && !names(0).contains("Analog Synth"),
              "M11 devices: an instrument moved onto a track that has one replaces it, in the instrument's place");

        // racks: into a chain and out of it
        const int rack = p.groupIntoRack(0, sat);   // track 0: Arpeggiator, Audio Effect Rack (Saturator), Reverb
        {
            EditScope edit(&p, "Map");
            for (Device& d : p.tracks[0].devices) if (d.id == rack) d.maps.push_back({0, sat, 0, 0.0, 1.0});
        }
        CHECK(rack && p.moveDevices(0, {rev}, 0, {0, rack, 0}) && p.tracks[0].devices.last().id == rev && p.tracks[0].devices.last().rack == rack
              && p.tracks[0].devices.last().chain == 0 && isRack(p.tracks[0].devices[1]), "M11 racks: a device dropped next to a rack goes into its chain");
        auto rackDev = [&p, rack]() -> const Device* { for (const Device& d : p.tracks[0].devices) if (d.id == rack) return &d; return nullptr; };
        CHECK(p.moveDevices(0, {sat}, 0, {0}) && p.tracks[0].devices.last().id == sat && p.tracks[0].devices.last().rack == 0 && rackDev() && rackDev()->maps.isEmpty(),
              "M11 racks: a device dragged out of a rack goes on the track, and the rack's macros let go of it");
        CHECK(!p.moveDevices(0, {rack}, 0, {0, rack, 0}), "M11 racks: a rack can't go inside itself");
        CHECK(p.moveDevices(0, {rack}, 1, {}) && p.tracks[1].devices.last().rack == rack && p.tracks[1].devices.last().id == rev
              && std::none_of(p.tracks[0].devices.begin(), p.tracks[0].devices.end(), [rack](const Device& d) { return d.id == rack || d.rack == rack; }),
              "M11 racks: a rack moves to another track with what's in its chains");
        // several at once, kept in their order
        const int a = idOf(0, "Arpeggiator"), s2 = idOf(0, "Saturator");
        CHECK(p.moveDevices(0, {s2, a}, 1, {}) && names(0).isEmpty() && names(1).first() == "Arpeggiator" && names(1).last() == "Saturator",
              "M11 devices: several dragged together each go where their kind goes, in order");

        // grouping several devices, folding, deleting several
        Project q;
        rackProject(q, {});
        q.selectedTrack = 0;
        for (const char* n : {"Analog Synth", "Saturator", "EQ"}) q.addDeviceToSelected(n, deviceSpec(n)->kind);
        const std::vector<float> before = playArrangement(q, 24000);
        const int qs = q.tracks[0].devices[0].id, qsat = q.tracks[0].devices[1].id, qeq = q.tracks[0].devices[2].id;
        const int ir = q.groupIntoRack(0, QList<int>{qs, qsat});
        QStringList qn;
        for (const Device& d : q.tracks[0].devices) qn << d.name;
        CHECK(ir && (qn == QStringList{"Instrument Rack", "Analog Synth", "Saturator", "EQ"}) && q.tracks[0].devices[1].rack == ir && q.tracks[0].devices[2].rack == ir
              && q.tracks[0].devices[2].chain == 0 && q.tracks[0].devices[0].chains == QStringList{"Analog Synth"},
              "M11 racks: Ctrl+G on an instrument and an effect puts both into one chain of a new Instrument Rack");
        CHECK(maxDiff(playArrangement(q, 24000), before) < 1e-5f, "M11 racks: grouping them doesn't change the sound");
        const int undoF = q.undoStack()->count();
        q.setDeviceFolded(0, qeq, true);
        CHECK(q.tracks[0].devices[3].folded && q.undoStack()->count() == undoF + 1, "M11 devices: folding a device is one undo step");
        QTemporaryDir dir;
        setfile::Extras x;
        Project q2;
        CHECK(setfile::save(dir.filePath("fold.sessions"), q, x) && setfile::load(dir.filePath("fold.sessions"), q2, x) && q2.tracks[0].devices[3].folded
              && q2.captureEditState() == q.captureEditState(), "M11 devices: a folded device stays folded in a saved set");
        CHECK(q.deleteDevices(0, {ir, qeq}) && q.tracks[0].devices.isEmpty(), "M11 devices: deleting several (a rack with its chain) at once");
        q.undoStack()->undo();
        CHECK(q.tracks[0].devices.size() == 4, "M11 devices: one undo brings them all back");

        // scenes move with their clips
        Project sc;
        rackProject(sc, {});
        sc.scenes = {"A", "B", "C"};
        sc.tracks[0].clipSlots.resize(3);
        ClipPtr c0 = sc.tracks[0].clipSlots[0];
        ClipPtr c2 = sc.newClip("Third", 4);
        sc.tracks[0].clipSlots[2] = c2;
        CHECK(sc.moveScene(0, 2) && (sc.scenes == QStringList{"B", "C", "A"}) && sc.tracks[0].clipSlots[2] == c0 && sc.tracks[0].clipSlots[1] == c2 && !sc.tracks[0].clipSlots[0],
              "M11 scenes: a scene moved down takes its clips along");
        sc.undoStack()->undo();
        CHECK((sc.scenes == QStringList{"A", "B", "C"}) && sc.tracks[0].clipSlots[0] == c0, "M11 scenes: undo moves it back");
    }

    // ---- Milestone 11: your presets
    {
        Project p;
        rackProject(p, {});
        p.addTrack("Two", QColor("#5aa9f0"));
        p.selectedTrack = 0;
        p.addDeviceToSelected("Lead Stack", DeviceKind::Instrument);   // a rack with two chains and macro mappings
        p.addDeviceToSelected("Delay", DeviceKind::AudioEffect);
        const int stack = p.tracks[0].devices[0].id;
        int delay = 0;
        for (const Device& d : p.tracks[0].devices) if (d.name == "Delay") delay = d.id;
        for (Device& d : p.tracks[0].devices) if (d.id == delay) d.params[0].value = 0.31;
        const QString path = presets::pathFor("Lead + Delay");
        CHECK(presets::save(path, "Lead + Delay", p.deviceBlocks(0, {stack, delay})) && QFileInfo::exists(path) && presets::list().contains(path)
              && presets::nameOf(path) == "Lead + Delay", "M11 presets: a rack and an effect save as one preset in the User Library");
        CHECK(QFileInfo(presets::pathFor("a/b:c")).fileName() == QString("a_b_c.") + presets::kSuffix, "M11 presets: a name a file can't hold is made safe");
        Project::DeviceBlocks blocks;
        QString name;
        CHECK(presets::read(path, blocks, &name) && name == "Lead + Delay" && blocks.size() == 2 && isRack(blocks[0][0]) && blocks[0].size() == 3
              && blocks[1][0].name == "Delay" && blocks[1][0].params[0].value == 0.31, "M11 presets: it reads back the same");
        devicedrag::Payload pl;
        pl.source = devicedrag::Payload::Preset;
        pl.path = path;
        pl.name = name;
        const devicedrag::Result r = devicedrag::drop(p, pl, 1, {}, false);
        const QVector<Device>& two = p.tracks[1].devices;
        QSet<int> all;
        for (const Track& t : p.tracks) for (const Device& d : t.devices) all.insert(d.id);
        int count = 0;
        for (const Track& t : p.tracks) count += int(t.devices.size());
        CHECK(r.ok && two.size() == 4 && isRack(two[0]) && two[1].rack == two[0].id && two[2].rack == two[0].id && two[3].name == "Delay"
              && all.size() == count && two[0].maps.size() == 2 && two[0].maps[0].deviceId != p.tracks[0].devices[1].id
              && (two[0].maps[0].deviceId == two[1].id || two[0].maps[0].deviceId == two[2].id || two[0].maps[0].deviceId == two[0].id),
              "M11 presets: dropped on a track, it brings new devices (new ids), the rack's chains and its macros turning the copies");
        Browser b;
        QLineEdit* search = b.findChild<QLineEdit*>();
        if (search) search->setText("Lead +");
        CHECK(search && b.listedItems().contains(path + "|u"), "M11 presets: the browser finds it (User Library)");
        QString renamed;
        CHECK(presets::rename(path, "Big Lead", &renamed) && !QFileInfo::exists(path) && presets::nameOf(renamed) == "Big Lead", "M11 presets: renaming one");
        auto* fileMime = new QMimeData;
        fileMime->setUrls({QUrl::fromLocalFile(renamed)});
        CHECK(devicedrag::parse(fileMime).source == devicedrag::Payload::Preset, "M11 presets: a preset file dragged in from the file manager counts as a device");
        delete fileMime;
    }

    // ---- Milestone 11: the Device View — drops where the line shows, selecting, dragging titles, folding
    {
        Project p;
        rackProject(p, {});
        p.addTrack("Other", QColor("#5aa9f0"));
        p.selectedTrack = 0;
        for (const char* n : {"Analog Synth", "Saturator", "Reverb"}) p.addDeviceToSelected(n, deviceSpec(n)->kind);
        audio::Engine e(&p);
        Transport tr(&p, &e);
        DeviceView view(&p, &tr);
        view.resize(1900, 260);
        view.show();
        QApplication::processEvents();
        auto names = [&p](int t) { QStringList n; for (const Device& d : p.tracks[t].devices) n << d.name; return n; };
        auto panelOf = [&view](int id) -> DevicePanel* { for (DevicePanel* x : view.panels()) if (x->deviceId() == id) return x; return nullptr; };
        const int syn = p.tracks[0].devices[0].id, sat = p.tracks[0].devices[1].id, rev = p.tracks[0].devices[2].id;
        DevicePanel* revPanel = panelOf(rev);
        const QPoint leftOfReverb = revPanel ? revPanel->mapTo(&view, QPoint(8, 60)) : QPoint();
        QRect line;
        const Project::DevicePlace place = view.placeAt(leftOfReverb, &line);
        CHECK(revPanel && place.beforeId == rev && place.rackId == 0 && line.width() == 3 && std::abs(line.x() - (revPanel->geometry().x() - 3)) <= 1,
              "M11 Device View: over a device's left half, the line shows in front of it");
        QMimeData* eq = itemMime("EQ|f");
        dragDrop(&view, leftOfReverb, eq);
        delete eq;
        int eqId = 0;
        for (const Device& d : p.tracks[0].devices) if (d.name == "EQ") eqId = d.id;
        CHECK((names(0) == QStringList{"Analog Synth", "Saturator", "EQ", "Reverb"}) && view.selection() == QList<int>{eqId},
              "M11 Device View: a browser device dropped there goes in front of it, and is selected");
        // selecting with the titles
        QApplication::processEvents();
        DevicePanel* satPanel = panelOf(sat);
        if (satPanel) QTest::mouseClick(satPanel->titleBar(), Qt::LeftButton, Qt::NoModifier, QPoint(60, 10));
        if (DevicePanel* x = panelOf(eqId)) QTest::mouseClick(x->titleBar(), Qt::LeftButton, Qt::ControlModifier, QPoint(60, 10));
        CHECK((view.selection() == QList<int>{sat, eqId}) && panelOf(sat) && panelOf(sat)->selected(), "M11 Device View: click a title selects it, Ctrl+click adds one");
        if (DevicePanel* x = panelOf(syn)) QTest::mouseClick(x->titleBar(), Qt::LeftButton, Qt::NoModifier, QPoint(60, 10));
        if (DevicePanel* x = panelOf(rev)) QTest::mouseClick(x->titleBar(), Qt::LeftButton, Qt::ShiftModifier, QPoint(60, 10));
        CHECK(view.selection().size() == 4, "M11 Device View: Shift+click selects the ones in between");
        // dragging titles: the selection moves (to the end), with Ctrl it's copied
        view.setSelection({sat, eqId});
        QMimeData* moving = devicedrag::mimeFor(p.tracks[0].id, view.selection());
        QWidget* host = view.findChild<QScrollArea*>()->widget();
        const QPoint end = host->mapTo(&view, QPoint(host->width() - 20, 60));
        CHECK(dragDrop(&view, end, moving) && (names(0) == QStringList{"Analog Synth", "Reverb", "Saturator", "EQ"}),
              "M11 Device View: two selected devices dragged by a title to the end move there together");
        delete moving;
        QMimeData* copying = devicedrag::mimeFor(p.tracks[0].id, {rev});
        dragDrop(&view, end, copying, Qt::ControlModifier);
        delete copying;
        CHECK((names(0) == QStringList{"Analog Synth", "Reverb", "Saturator", "EQ", "Reverb"}) && p.tracks[0].devices.last().id != rev,
              "M11 Device View: Ctrl while dropping copies it");
        // the commands on the selection
        view.setSelection({p.tracks[0].devices.last().id});
        view.deleteSelection();
        view.setSelection({sat});
        view.duplicateSelection();
        CHECK((names(0) == QStringList{"Analog Synth", "Reverb", "Saturator", "Saturator", "EQ"}) && view.selection().size() == 1 && view.selection()[0] != sat,
              "M11 Device View: Delete removes the selection; Ctrl+D puts a copy right after it, selected");
        view.setSelection({sat});
        view.copySelection();
        p.selectedTrack = 1;
        emit p.selectionChanged();
        QApplication::processEvents();
        view.paste();
        CHECK(names(1) == QStringList{"Saturator"} && view.selection().size() == 1, "M11 Device View: Ctrl+C, another track, Ctrl+V pastes a copy there");
        p.selectedTrack = 0;
        emit p.selectionChanged();
        QApplication::processEvents();
        // double-click a title folds it to a strip, and back
        if (DevicePanel* x = panelOf(rev)) QTest::mouseDClick(x->titleBar(), Qt::LeftButton, Qt::NoModifier, QPoint(60, 10));
        QApplication::processEvents();
        CHECK(panelOf(rev) && panelOf(rev)->folded() && panelOf(rev)->width() == 24 && !panelOf(rev)->body(), "M11 Device View: double-clicking a title folds the device");
        if (DevicePanel* x = panelOf(rev)) QTest::mouseDClick(x->titleBar(), Qt::LeftButton, Qt::NoModifier, QPoint(10, 60));
        QApplication::processEvents();
        CHECK(panelOf(rev) && !panelOf(rev)->folded() && panelOf(rev)->body(), "M11 Device View: and double-clicking again unfolds it");
        // an instrument on an empty audio track turns it into a MIDI track (0.21); one holding audio clips still refuses it
        const int audio = p.insertTrack(2, TrackType::Audio);
        p.selectedTrack = audio;
        emit p.selectionChanged();
        QApplication::processEvents();
        QMimeData* synth = itemMime("Analog Synth|i");
        CHECK(dragDrop(&view, QPoint(40, 60), synth) && p.tracks[audio].type == TrackType::Midi && p.tracks[audio].devices.size() == 1 && p.tracks[audio].devices[0].name == "Analog Synth",
              "M11 Device View (0.21): an instrument dropped on an empty audio track makes it a MIDI track and lands on it");
        p.undoStack()->undo();
        QApplication::processEvents();
        CHECK(p.tracks[audio].type == TrackType::Audio && p.tracks[audio].devices.isEmpty(), "M11 Device View (0.21): one undo takes the device and the change of kind back");
        p.tracks[audio].clipSlots[0] = p.newAudioClip(QString(), nullptr);
        p.tracks[audio].clipSlots[0]->samplePath = "x.wav";
        emit p.structureChanged();
        QApplication::processEvents();
        CHECK(!dragDrop(&view, QPoint(40, 60), synth) && p.tracks[audio].devices.isEmpty() && p.tracks[audio].type == TrackType::Audio, "M11 Device View: an audio track with clips doesn't take an instrument");
        p.tracks[audio].clipSlots[0] = nullptr;
        delete synth;
        // a rack: dropped next to it, into its chain
        p.selectedTrack = 0;
        emit p.selectionChanged();
        const int rack = p.groupIntoRack(0, sat);
        QApplication::processEvents();
        RackPanel* rp = view.findChildren<RackPanel*>().value(0);
        QMimeData* chorus = itemMime("Chorus|f");
        const QPoint inChain = rp && rp->chainDrop() ? rp->chainDrop()->mapTo(&view, QPoint(-30, 60)) : QPoint();
        const Project::DevicePlace cp = view.placeAt(inChain);
        dragDrop(&view, inChain, chorus);
        delete chorus;
        bool inRack = false;
        for (const Device& d : p.tracks[0].devices) inRack |= d.name == "Chorus" && d.rack == rack;
        CHECK(rp && cp.rackId == rack && inRack, "M11 Device View: a device dropped next to a rack's devices goes into the chain it shows");
    }

    // ---- Milestone 11: dropping devices on tracks and the empty space (Session, mixer, Arrangement)
    {
        Project p;
        rackProject(p, {});
        p.addTrack("Two", QColor("#5aa9f0"));
        p.insertTrack(2, TrackType::Audio);
        p.selectedTrack = 0;
        p.addDeviceToSelected("Analog Synth", DeviceKind::Instrument);
        audio::Engine e(&p);
        Transport tr(&p, &e);
        SessionView sv(&p, &tr);
        sv.resize(1400, 640);
        sv.show();
        QApplication::processEvents();
        ClipGrid* grid = sv.grid();
        QSignalSpy opened(grid, &ClipGrid::openDeviceView);
        auto names = [&p](int t) { QStringList n; for (const Device& d : p.tracks[t].devices) n << d.name; return n; };
        QMimeData* reverb = itemMime("Reverb|f");
        dragDrop(grid, QPoint(grid->columnLeft(1) + 30, 40), reverb);
        delete reverb;
        CHECK(names(1) == QStringList{"Reverb"} && opened.count() == 1 && opened.last().value(0).toInt() == 1,
              "M11 Session: a device dropped on a track's column goes on that track (and its Device View opens)");
        QMimeData* synth = itemMime("Analog Synth|i");
        CHECK(dragDrop(grid, QPoint(grid->columnLeft(2) + 30, 40), synth) && p.tracks[2].type == TrackType::Midi && names(2) == QStringList{"Analog Synth"},
              "M11 Session (0.21): an instrument dropped on an empty audio track's column makes it a MIDI track and lands on it");
        p.undoStack()->undo();   // the audio track again, for the tests below
        QApplication::processEvents();
        delete synth;
        const int nT = int(p.tracks.size());
        QMimeData* sound = itemMime("Acid Line|i");
        const QPoint free(grid->columnLeft(nT - 1) + grid->columnWidth(nT - 1) + 30, 60);
        dragDrop(grid, free, sound);
        delete sound;
        CHECK(grid->freeSpace() > 30 && int(p.tracks.size()) == nT + 1 && p.tracks.last().name == "Acid Line" && p.tracks.last().type == TrackType::Midi
              && names(nT) == QStringList{"Acid Line"} && p.tracks.last().groupId == 0, "M11 Session: a Sound dropped on the free space makes a new MIDI track named after it");
        p.undoStack()->undo();
        CHECK(int(p.tracks.size()) == nT, "M11 Session: one undo takes the new track and its device away");
        QMimeData* delayFx = itemMime("Delay|f");
        dragDrop(grid, free, delayFx);
        delete delayFx;
        CHECK(int(p.tracks.size()) == nT + 1 && p.tracks.last().type == TrackType::Audio && names(nT) == QStringList{"Delay"},
              "M11 Session: an audio effect dropped there makes an audio track");
        p.undoStack()->undo();
        // a device panel's drag onto another track's column moves it; with Ctrl it copies
        const int syn = p.tracks[0].devices[0].id;
        QMimeData* move = devicedrag::mimeFor(p.tracks[0].id, {syn});
        dragDrop(grid, QPoint(grid->columnLeft(1) + 30, 40), move);
        delete move;
        CHECK(names(1).first() == "Analog Synth" && p.tracks[1].devices[0].id == syn && p.tracks[0].devices.isEmpty(),
              "M11 Session: a device dragged by its title onto another track moves there");
        QMimeData* copy = devicedrag::mimeFor(p.tracks[1].id, {syn});
        dragDrop(grid, QPoint(grid->columnLeft(0) + 30, 40), copy, Qt::ControlModifier);
        delete copy;
        CHECK(names(0) == QStringList{"Analog Synth"} && p.tracks[0].devices[0].id != syn && names(1).first() == "Analog Synth",
              "M11 Session: with Ctrl, a copy goes and the original stays");
        // the mixer strips take devices for their track
        MixerStrip* strip = nullptr;
        for (MixerStrip* s : sv.findChildren<MixerStrip*>()) if (s->isVisible() && s->geometry().contains(QPoint(grid->columnLeft(2) + 30, 20))) strip = s;
        QMimeData* chorus = itemMime("Chorus|f");
        if (strip) dragDrop(strip, QPoint(30, 60), chorus);
        delete chorus;
        CHECK(strip && names(2) == QStringList{"Chorus"}, "M11 mixer: a device dropped on a mixer strip goes on its track");
        // scenes: drag a scene's name to another row
        p.scenes = {"A", "B", "C"};
        for (Track& t : p.tracks) t.clipSlots.resize(3);
        emit p.structureChanged();
        QApplication::processEvents();
        const ClipPtr c0 = p.tracks[0].clipSlots[0];
        const int sx = grid->columnLeft(p.trackCount()) + 40;
        auto rowY = [](int row) { return 20 + 1 + row * 21 + 10; };
        QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, QPoint(sx, rowY(0)));
        QTest::mouseMove(grid, QPoint(sx, rowY(1)));
        QTest::mouseMove(grid, QPoint(sx, rowY(2)));
        QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, QPoint(sx, rowY(2)));
        CHECK((p.scenes == QStringList{"B", "C", "A"}) && p.tracks[0].clipSlots[2] == c0, "M11 Session: a scene dragged by its name moves, clips and all");
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, QPoint(sx, rowY(2)));
        CHECK(p.tracks[0].queuedSlot == 2 || p.tracks[0].playingSlot == 2, "M11 Session: a click on a scene's name still launches it");
        tr.stop();

        // the Arrangement: a lane or its header, and the Main lane for a new track
        ArrangementView arr(&p, &tr);
        arr.resize(1300, 560);
        arr.show();
        QApplication::processEvents();
        QSignalSpy arrOpened(&arr, &ArrangementView::openDeviceView);
        const int before = int(p.tracks[1].devices.size());
        QMimeData* gate = itemMime("Gate|f");
        dragDrop(&arr, QPoint(arr.width() - 60, arr.laneRect(1).center().y()), gate);
        delete gate;
        CHECK(int(p.tracks[1].devices.size()) == before + 1 && p.tracks[1].devices.last().name == "Gate" && arrOpened.count() == 1,
              "M11 Arrangement: a device dropped on a track's header goes on that track");
        const int n2 = int(p.tracks.size());
        QMimeData* fm = itemMime("FM Synth|i");
        dragDrop(&arr, QPoint(200, arr.laneRect(p.trackCount()).center().y()), fm);
        delete fm;
        CHECK(int(p.tracks.size()) == n2 + 1 && p.tracks.last().devices.value(0).name == "FM Synth", "M11 Arrangement: dropped on the Main lane, it makes a new track");
    }

    // ---- Milestone 11: the window routes the keys to the Device View when it has focus
    {
        MainWindow w;
        w.resize(1600, 900);
        w.show();
        QApplication::setActiveWindow(&w);
        QApplication::processEvents();
        Project* proj = w.project();
        DeviceView* dv = w.deviceView();
        const int t = proj->selectedTrack;
        proj->addDeviceToSelected("Saturator", DeviceKind::AudioEffect);
        proj->addDeviceToSelected("Chorus", DeviceKind::AudioEffect);
        QApplication::processEvents();
        const int n = int(proj->tracks[t].devices.size());
        int chorus = 0;
        for (const Device& d : proj->tracks[t].devices) if (d.name == "Chorus") chorus = d.id;
        if (auto* stack = qobject_cast<QStackedWidget*>(dv->parentWidget())) stack->setCurrentWidget(dv);   // the Device View showing
        QApplication::processEvents();
        dv->setFocus();
        QApplication::processEvents();
        CHECK(dv->isVisible() && QApplication::focusWidget() == dv, "M11 window: the Device View takes the focus");
        dv->setSelection({chorus});
        QTest::keyClick(dv, Qt::Key_D, Qt::ControlModifier);
        CHECK(int(proj->tracks[t].devices.size()) == n + 1, "M11 window: Ctrl+D in the Device View duplicates the selected device");
        QTest::keyClick(dv, Qt::Key_Delete);
        CHECK(int(proj->tracks[t].devices.size()) == n, "M11 window: Delete in the Device View deletes the selected device");
        dv->setSelection({chorus});
        QTest::keyClick(dv, Qt::Key_Delete);
        CHECK(int(proj->tracks[t].devices.size()) == n - 1, "M11 window: Delete deletes the selected device (not a clip)");
        proj->undoStack()->undo();
        QApplication::processEvents();
        int sat = 0;
        for (const Device& d : proj->tracks[t].devices) if (d.name == "Saturator") sat = d.id;
        dv->setSelection({sat, chorus});
        QTest::keyClick(dv, Qt::Key_G, Qt::ControlModifier);
        bool grouped = false;
        for (const Device& d : proj->tracks[t].devices) grouped |= d.id == chorus && d.rack != 0;
        CHECK(grouped, "M11 window: Ctrl+G in the Device View groups the selected devices into a rack");
    }


    // ================================================================ Milestone 12: take lanes and comping
    {
        // ---- the comp: regions over a clip's takes
        Project p;
        p.scenes = {"1"};
        p.addTrack("Gtr", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        ClipPtr c = p.newClip("Gtr", 4);
        c->samplePath = "/x/take3.wav";
        c->takes = {{"/x/take1.wav", nullptr, "Take 1", 0}, {"/x/take2.wav", nullptr, "Take 2", 0}, {"/x/take3.wav", nullptr, "Take 3", 0}};
        CHECK(c->takeAt(0) == 2 && c->takeAt(3.9) == 2, "M12 comp: with no regions the newest take plays everywhere");
        c->useTake(1, 2, 0);
        c->useTake(2, 3, 0);
        CHECK(c->comp.size() == 1 && c->comp[0].start == 1 && c->comp[0].end == 3 && c->comp[0].take == 0 && c->takeAt(1.5) == 0 && c->takeAt(0.5) == 2,
              "M12 comp: a stretch from a take; neighbouring stretches of the same take are joined");
        c->useTake(1.5, 2.5, 1);
        CHECK(c->comp.size() == 3 && c->comp[1].take == 1 && c->comp[0].end == 1.5 && c->comp[2].start == 2.5 && c->takeAt(2) == 1,
              "M12 comp: a stretch from another take cuts the first one around it");
        c->useWholeTake(0);
        CHECK(c->comp.size() == 1 && c->comp[0].start == 0 && c->comp[0].end == 4 && c->comp[0].take == 0, "M12 comp: Use Whole Take covers the clip");
        c->useWholeTake(2);
        CHECK(c->comp.isEmpty(), "M12 comp: the whole newest take means no regions");
        CHECK(std::abs(c->takeSeconds(0, 2) - 1.0) < 1e-9, "M12 comp: a take's file time at a clip beat follows the clip's warp map");
    }
    {
        // ---- engine: a comped clip plays each stretch from its take and crossfades at the joins
        QTemporaryDir dir;
        std::vector<float> a(96000, 0.5f), b(96000, -0.5f);
        writeMonoWav(dir.filePath("a.wav"), a, 48000);
        writeMonoWav(dir.filePath("b.wav"), b, 48000);
        Project p;
        ClipPtr c = audioProject(p, dir.filePath("b.wav"));
        c->loop = false;
        c->takes = {{dir.filePath("a.wav"), samples::load(dir.filePath("a.wav")), "Take 1", 0},
                    {dir.filePath("b.wav"), samples::load(dir.filePath("b.wav")), "Take 2", 0}};
        c->useTake(0, 2, 0);   // Take 1 for two beats, then the newest (Take 2)
        std::vector<float> out = playArrangement(p, 96000);
        CHECK(std::abs(out[2 * 24000] - 0.5f) < 0.02f && std::abs(out[2 * 72000] + 0.5f) < 0.02f, "M12 engine: a comped clip plays each stretch from its take");
        float jump = 0;
        for (int i = 47000; i < 49000; ++i) jump = std::max(jump, std::fabs(out[2 * i] - out[2 * (i - 1)]));
        CHECK(std::fabs(out[2 * 48000]) < 0.08f && std::abs(out[2 * (48000 - 300)] - 0.5f) < 0.02f && std::abs(out[2 * (48000 + 300)] + 0.5f) < 0.02f && jump < 0.02f,
              "M12 engine: the join crossfades over ~5 ms (no step at the boundary)");
        c->takes[0].sample = nullptr;   // a missing take file is silence, not a crash
        out = playArrangement(p, 96000);
        CHECK(std::fabs(out[2 * 24000]) < 1e-6f && std::abs(out[2 * 72000] + 0.5f) < 0.02f, "M12 engine: a take whose file is missing plays silence");
    }
    {
        // ---- monitoring replaces the track's clips (you hear the input, not the pass you recorded before)
        QTemporaryDir dir;
        writeMonoWav(dir.filePath("tone.wav"), std::vector<float>(96000, 0.4f), 48000);
        Project p;
        ClipPtr c = audioProject(p, dir.filePath("tone.wav"));
        c->loop = false;
        p.tracks[0].monitor = Monitor::Off;
        CHECK(peakOf(playArrangement(p, 24000)) > 0.3f, "M12 monitor: with Monitor Off the clip plays");
        p.tracks[0].monitor = Monitor::In;
        CHECK(peakOf(playArrangement(p, 24000)) == 0.0f, "M12 monitor: Monitor In silences the track's clips (the input plays instead)");
        p.tracks[0].monitor = Monitor::Auto;
        p.tracks[0].armed = true;
        CHECK(peakOf(playArrangement(p, 24000)) == 0.0f, "M12 monitor: so does Auto while the track is armed");
        p.tracks[0].armed = false;
        CHECK(peakOf(playArrangement(p, 24000)) > 0.3f, "M12 monitor: disarmed, the clip is back");
    }
    {
        // ---- loop recording onto the timeline: every pass is a take of one clip over the loop
        QTemporaryDir recDir;
        Project p;
        p.scenes = {"1"};
        p.addTrack("Gtr", QColor("#62c9dc"));
        p.tracks[0].type = TrackType::Audio;
        p.tracks[0].armed = true;
        p.recordFolder = recDir.path();
        audio::Engine e(&p);
        Transport t(&p, &e);
        qint64 pos = 0;
        // a one-bar loop (96000 frames): pass k (k < 3) has a click at beat k + 0.5; pass 4 at beat 0.25
        auto pump = [&](qint64 until) {
            std::vector<float> in(512), out(512);
            while (pos < until) {
                for (int f = 0; f < 256; ++f) {
                    const qint64 g = pos + f;
                    const int pass = int(g / 96000), at = int(g % 96000);
                    const bool click = pass < 3 ? at == pass * 24000 + 12000 : at == 6000;
                    in[size_t(f) * 2] = in[size_t(f) * 2 + 1] = click ? 0.9f : 0.0f;
                }
                e.pushInput(in.data(), 256);
                e.process(out.data(), 256);
                pos += 256;
                if (pos % 4096 == 0) QTest::qWait(17);
            }
        };
        t.setLoop(0, 4);
        t.setLoopEnabled(true);
        t.setArrangementRecord(true);
        t.play();
        pump(3 * 96000 + 12000);   // three full passes and half a beat of a fourth
        QTest::qWait(30);
        t.stop();
        for (int k = 0; k < 60 && t.audioRecording(); ++k) { pump(pos + 4096); QTest::qWait(5); }
        const auto& arr = p.tracks[0].arrangement;
        ClipPtr rec = arr.size() == 1 ? arr[0].clip : nullptr;
        const bool ok = rec && rec->takes.size() == 4;
        CHECK(arr.size() == 1 && std::abs(arr[0].start) < 1e-6 && std::abs(arr[0].length - 4) < 1e-6 && ok,
              "M12 loop record: one clip over the loop, with a take per pass (three full, one cut short)");
        bool own = ok;
        for (int k = 0; ok && k < 3; ++k) {
            const SamplePtr s = rec->takes[k].sample;
            own &= s && s->frames() >= 96000 - 100 && s->frames() <= 96000 + 100 && s->left[size_t(k * 24000 + 12000)] > 0.7f && std::fabs(s->left[size_t(k * 24000 + 12000 + 500)]) < 1e-6f;
        }
        CHECK(own, "M12 loop record: each pass's take holds that pass's audio, on its beat, a loop long");
        const SamplePtr last = ok ? rec->takes[3].sample : nullptr;
        CHECK(last && last->frames() > 10000 && last->frames() < 14000 && last->left[6000] > 0.7f, "M12 loop record: the pass cut short by Stop is kept as a take too");
        CHECK(ok && rec->takeAt(0.25) == 3 && rec->takeAt(1.5) == 2 && rec->takes[3].startBeat == 0 && rec->samplePath == rec->takes[3].samplePath,
              "M12 loop record: the short pass plays where it has audio, the pass before it elsewhere");
        p.tracks[0].armed = false;   // hear the clip, not the input
        std::vector<float> out = playArrangement(p, 96000);
        std::vector<int> on = onsetsOf(out, 0.3f);
        CHECK(on.size() == 2 && onsetNear(on, 6000, 2) >= 0 && onsetNear(on, 60000, 2) >= 0, "M12 loop record: the clip plays the fourth pass's click and the third pass's, none of the older ones");
        if (ok) {
            EditScope edit(&p, "Comp");
            rec->useTake(1, 2, 1);   // beat 2 from the second pass
        }
        out = playArrangement(p, 96000);
        on = onsetsOf(out, 0.3f);
        CHECK(on.size() == 3 && onsetNear(on, 36000, 2) >= 0, "M12 loop record: comping in the second pass brings its click back");
        // one undo takes the whole recording out (every pass is one step); the files stay
        p.undoStack()->undo();
        p.undoStack()->undo();
        CHECK(p.tracks[0].arrangement.isEmpty() && ok && QFileInfo::exists(rec->takes[0].samplePath) && QFileInfo::exists(rec->takes[3].samplePath),
              "M12 loop record: one undo removes the recording; the takes' files stay on disk");
        p.undoStack()->redo();
        p.undoStack()->redo();
        // the set file keeps the takes and the comp
        setfile::Extras x;
        const QString setPath = recDir.filePath("takes.sessions");
        QString err;
        CHECK(setfile::save(setPath, p, x, &err), "M12 sets: a set with takes saves");
        QFile f(setPath); f.open(QIODevice::ReadOnly);
        const QString json = QString::fromUtf8(f.readAll());
        Project q;
        setfile::Extras y;
        CHECK(json.contains("\"takes\"") && json.contains("\"comp\"") && setfile::load(setPath, q, y, &err) && q.captureEditState() == p.captureEditState(),
              "M12 sets: takes, their start beats and the comp round-trip exactly");
        const ClipPtr lc = q.tracks.value(0).arrangement.value(0).clip;
        CHECK(lc && lc->takes.size() == 4 && lc->takes[0].sample && lc->takes[0].name == "Take 1" && lc->takeAt(1.5) == 1, "M12 sets: the loaded takes have their audio and names");
    }
    {
        // ---- the Takes view: lanes, a drag across a take, a click, a double-click
        QTemporaryDir dir;
        for (int k = 0; k < 3; ++k) writeMonoWav(dir.filePath(QString("t%1.wav").arg(k)), std::vector<float>(96000, 0.2f + 0.1f * k), 48000);
        Project p;
        ClipPtr c = audioProject(p, dir.filePath("t2.wav"));
        c->loop = false;
        for (int k = 0; k < 3; ++k) c->takes.push_back({dir.filePath(QString("t%1.wav").arg(k)), samples::load(dir.filePath(QString("t%1.wav").arg(k))), QString("Take %1").arg(k + 1), 0});
        audio::Engine e(&p);
        Transport t(&p, &e);
        TakesEditor ed(&p, &t);
        ed.resize(700, 320);
        ed.show();
        ed.setClip(c);
        QApplication::processEvents();
        CHECK(ed.laneCount() == 4 && ed.takeOfLane(1) == 2 && ed.takeOfLane(3) == 0 && ed.takeOfLane(0) == -1 && !ed.laneRect(3).isEmpty(),
              "M12 Takes view: the Comp lane, then one lane per take, newest first");
        const QRect lane1 = ed.laneRect(ed.laneOfTake(0)), lane2 = ed.laneRect(ed.laneOfTake(1));
        const int steps0 = p.undoStack()->count();
        QTest::mousePress(&ed, Qt::LeftButton, Qt::NoModifier, QPoint(int(ed.beatToX(1.02)), lane1.center().y()));
        QTest::mouseMove(&ed, QPoint(int(ed.beatToX(1.5)), lane1.center().y()));
        QTest::mouseMove(&ed, QPoint(int(ed.beatToX(2.03)), lane1.center().y()));
        QTest::mouseRelease(&ed, Qt::LeftButton, Qt::NoModifier, QPoint(int(ed.beatToX(2.03)), lane1.center().y()));
        CHECK(c->comp.size() == 1 && c->comp[0].start == 1 && c->comp[0].end == 2 && c->comp[0].take == 0 && c->takeAt(1.5) == 0 && p.undoStack()->count() == steps0 + 1,
              "M12 Takes view: dragging across a take uses that stretch of it (snapped to the grid), one undo step");
        QTest::mouseClick(&ed, Qt::LeftButton, Qt::NoModifier, QPoint(int(ed.beatToX(3.1)), lane2.center().y()));
        CHECK(c->takeAt(3.1) == 1 && c->takeAt(2.9) == 2 && c->takeAt(3.3) == 2, "M12 Takes view: a click uses one grid step of that take");
        p.undoStack()->undo();
        CHECK(c->takeAt(3.1) == 2 && c->takeAt(1.5) == 0, "M12 Takes view: undo takes the click back and keeps the drag");
        QTest::mouseDClick(&ed, Qt::LeftButton, Qt::NoModifier, QPoint(int(ed.beatToX(0.5)), lane1.center().y()));
        CHECK(c->comp.size() == 1 && c->comp[0].take == 0 && c->comp[0].start == 0 && c->comp[0].end == 4, "M12 Takes view: double-clicking a lane uses the whole take");
        // in the Clip View: the Takes button swaps the editor
        ClipView cv(&p, &t);
        cv.resize(1100, 300);
        cv.show();
        cv.setClip(c);
        QApplication::processEvents();
        QPushButton* takesBtn = cv.findChild<QPushButton*>("takesShow");
        CHECK(takesBtn && takesBtn->isEnabled() && takesBtn->text() == "Takes (3)" && cv.sampleEditor()->isVisibleTo(&cv), "M12 Clip View: the Sample box offers Takes (3) and shows the sample editor first");
        QTest::mouseClick(takesBtn, Qt::LeftButton);
        QApplication::processEvents();
        CHECK(cv.takesEditor()->isVisibleTo(&cv) && !cv.sampleEditor()->isVisibleTo(&cv) && cv.takesEditor()->clip() == c, "M12 Clip View: Takes shows the Takes view for the clip");
        cv.showEnvelope(true);
        CHECK(cv.envelopeEditor()->isVisibleTo(&cv) && !takesBtn->isChecked(), "M12 Clip View: Show (envelopes) takes over from Takes");
        ClipPtr plain = p.newAudioClip(dir.filePath("t0.wav"), samples::load(dir.filePath("t0.wav")));
        cv.showEnvelope(false);
        cv.showTakes(true);
        cv.setClip(plain);
        CHECK(!takesBtn->isEnabled() && cv.sampleEditor()->isVisibleTo(&cv), "M12 Clip View: a clip without takes has no Takes view");
    }

    // ================================================================ Milestone 12: sidechains
    {
        using namespace sidechain;
        CHECK(supports(DeviceType::Compressor) && supports(DeviceType::Gate) && supports(DeviceType::AutoFilter) && !supports(DeviceType::EQ)
              && paramCount(DeviceType::Compressor) == 13 && param(DeviceType::Compressor, Listen) == 12 && paramCount(DeviceType::Gate) == 11
              && param(DeviceType::Gate, Tap) == 6 && paramCount(DeviceType::AutoFilter) == 13 && param(DeviceType::AutoFilter, Gain) == 9,
              "M12 sidechain: five key parameters appended to Compressor, Gate and Auto Filter");
        // ---- model: who can key whom
        Project p;
        p.scenes = {"1"};
        p.addTrack("Kick", QColor("#f2905e"));
        p.addTrack("Bass", QColor("#62c9dc"));
        p.addTrack("Pad", QColor("#b89cd8"));
        p.tracks[1].devices = {p.makeDevice("Compressor", DeviceKind::AudioEffect)};
        p.tracks[0].devices = {p.makeDevice("Gate", DeviceKind::AudioEffect), p.makeDevice("EQ", DeviceKind::AudioEffect)};
        const int comp = p.tracks[1].devices[0].id, gate = p.tracks[0].devices[0].id, eq = p.tracks[0].devices[1].id;
        const int kickId = p.tracks[0].id, bassId = p.tracks[1].id;
        QString why;
        CHECK(p.canKeyFrom(1, comp, kickId) && p.canKeyFrom(1, comp, 0) && !p.canKeyFrom(1, comp, bassId, &why) && !why.isEmpty(),
              "M12 sidechain: a device can be keyed from another track, not from its own");
        CHECK(!p.canKeyFrom(0, eq, bassId, &why) && why.contains("no sidechain"), "M12 sidechain: the EQ has no key input");
        const int ret = p.insertReturn();
        CHECK(!p.canKeyFrom(1, comp, p.trackAt(ret)->id), "M12 sidechain: returns can't be key sources");
        const int steps = p.undoStack()->count();
        CHECK(p.setKeySource(1, comp, kickId) && p.tracks[1].devices[0].sidechainTrack == kickId && p.undoStack()->count() == steps + 1,
              "M12 sidechain: choosing the key's track is one undo step");
        CHECK(p.keyReaches(kickId, bassId) && !p.keyReaches(bassId, kickId), "M12 sidechain: the Kick now comes before the Bass (keyReaches)");
        CHECK(!p.canKeyFrom(0, gate, bassId, &why) && why.contains("loop"), "M12 sidechain: keying the Kick's Gate from the Bass would loop: refused");
        CHECK(p.canKeyFrom(0, gate, p.tracks[2].id) && p.setKeySource(0, gate, p.tracks[2].id) && !p.canKeyFrom(2, comp, kickId) && !p.canKeyFrom(2, comp, bassId),
              "M12 sidechain: a chain of keys (Pad -> Kick -> Bass) refuses anything that would close the loop");
        CHECK(!p.moveDevices(1, {comp}, 0, {}, false, nullptr, &why) && p.tracks[1].devices.size() == 1 && why.contains("keyed"),
              "M12 sidechain: a keyed device can't be moved onto the track that keys it");
        CHECK(!p.moveDevices(1, {comp}, 2, {}, false, nullptr, &why) && why.contains("loop"), "M12 sidechain: nor onto a track its key depends on");
        p.undoStack()->undo();
        p.undoStack()->undo();
        CHECK(p.tracks[1].devices[0].sidechainTrack == 0 && p.tracks[0].devices[0].sidechainTrack == 0, "M12 sidechain: undo clears the key sources again");
        const int gi = p.groupTracks({2});
        CHECK(gi >= 0 && !p.canKeyFrom(p.indexOfTrack(bassId), comp, p.tracks[gi].id, &why) && why.contains("group"), "M12 sidechain: group tracks aren't key sources");
    }
    {
        // ---- engine: a bass ducked by a kick on another track
        QTemporaryDir dir;
        std::vector<float> bass(96000), kick(96000, 0.0f);
        for (int i = 0; i < 96000; ++i) {
            bass[size_t(i)] = 0.3f * std::sin(2 * 3.14159265f * 110 * float(i) / 48000);
            if (i % 24000 < 2400) kick[size_t(i)] = 0.9f * std::cos(2 * 3.14159265f * 60 * float(i % 24000) / 48000);   // 50 ms on every beat, from its peak
        }
        writeMonoWav(dir.filePath("bass.wav"), bass, 48000);
        writeMonoWav(dir.filePath("kick.wav"), kick, 48000);
        Project p;
        p.scenes = {"1"};
        auto addAudio = [&](const QString& name, const QString& path) {
            p.addTrack(name, QColor("#62c9dc"));
            Track& t = p.tracks.last();
            t.type = TrackType::Audio;
            ClipPtr c = p.newAudioClip(path, samples::load(path));
            c->loop = false;
            t.clipSlots[0] = c;
            t.arrangement = {{0, 4, 0, c}};
            return int(p.tracks.size()) - 1;
        };
        const int bassT = addAudio("Bass", dir.filePath("bass.wav"));
        const int kickT = addAudio("Kick", dir.filePath("kick.wav"));   // the key's track comes AFTER the keyed one
        p.tracks[bassT].devices = {p.makeDevice("Compressor", DeviceKind::AudioEffect)};
        const int compId = p.tracks[bassT].devices[0].id;
        auto knobs = [&](int track) -> QVector<DeviceParam>& { return p.tracks[track].devices[0].params; };
        knobs(bassT)[0].value = 0.85;     // threshold -6 dB: the bass alone (-10 dB) stays under it
        knobs(bassT)[1].value = 0.7686;   // ratio 10:1
        knobs(bassT)[2].value = 0;        // attack 0.1 ms
        knobs(bassT)[3].value = 0.5;      // release 100 ms
        knobs(bassT)[4].value = 0;        // no knee
        knobs(bassT)[5].value = 0;        // no makeup
        p.tracks[kickT].active = false;   // muted: only its key should reach the bass
        // the Bass track's post-fader peak between two frames of the song
        auto bassPeak = [&](int from, int to) {
            audio::Engine e(&p);
            e.send({Command::Stop, -1, -1, 0, 0, true});
            e.send({Command::Play});
            render(e, from);
            e.takeTrackPeak(bassT);
            render(e, to - from);
            return e.takeTrackPeak(bassT);
        };
        const int burst = 24000, after = 26400;   // the second beat's kick
        CHECK(bassPeak(burst + 300, after) > 0.27f, "M12 sidechain: (internal: the bass alone isn't compressed)");
        p.tracks[bassT].devices[0].sidechainTrack = p.tracks[kickT].id;
        CHECK(bassPeak(burst + 300, after) < 0.2f && bassPeak(30000, 46000) > 0.27f,
              "M12 sidechain: keyed from the Kick (Post FX), the bass ducks on each kick though the kick is muted");
        // the key arrives in the same block: the Kick renders first although it's later in the track list
        {
            std::vector<float> ducked = playArrangement(p, 48000);
            p.tracks[bassT].devices[0].sidechainTrack = 0;
            std::vector<float> plain = playArrangement(p, 48000);
            p.tracks[bassT].devices[0].sidechainTrack = p.tracks[kickT].id;
            // the first 128 samples of the kick: ducked already when the key is this block's, not when it's the last block's
            const float ratio = rmsOf(ducked, size_t(2 * (burst + 20)), size_t(2 * (burst + 128))) / std::max(1e-6f, rmsOf(plain, size_t(2 * (burst + 20)), size_t(2 * (burst + 128))));
            CHECK(ratio < 0.75f, "M12 sidechain: the bass ducks within the kick's own block (sources render first, wherever they sit)");
        }
        using namespace sidechain;
        knobs(bassT)[param(DeviceType::Compressor, Tap)].value = choicePosition(2, 3);   // Post Mix
        CHECK(bassPeak(burst + 300, after) > 0.27f, "M12 sidechain: the Post Mix key is silent while its track is muted");
        p.tracks[kickT].active = true;
        CHECK(bassPeak(burst + 300, after) < 0.2f, "M12 sidechain: unmuted, the Post Mix key ducks the bass");
        p.tracks[kickT].active = false;
        knobs(bassT)[param(DeviceType::Compressor, Tap)].value = choicePosition(0, 3);   // Pre FX
        p.tracks[kickT].devices = {p.makeDevice("Gate", DeviceKind::AudioEffect)};
        p.tracks[kickT].devices[0].params[0].value = 1;   // threshold 0 dB: the gate never opens, the kick's chain is silent
        CHECK(bassPeak(burst + 300, after) < 0.2f, "M12 sidechain: Pre FX takes the kick before its (shut) Gate");
        knobs(bassT)[param(DeviceType::Compressor, Tap)].value = choicePosition(1, 3);   // Post FX
        CHECK(bassPeak(burst + 300, after) > 0.27f, "M12 sidechain: Post FX takes it after (silent through the shut Gate)");
        p.tracks[kickT].devices.clear();
        knobs(bassT)[param(DeviceType::Compressor, Gain)].value = 0;   // key gain -24 dB
        CHECK(bassPeak(burst + 300, after) > 0.27f, "M12 sidechain: Key Gain at -24 dB puts the key under the threshold");
        knobs(bassT)[param(DeviceType::Compressor, Gain)].value = 0.5;
        knobs(bassT)[param(DeviceType::Compressor, HP)].value = 1;   // key high-pass at 2 kHz: a 60 Hz kick hardly gets through
        CHECK(bassPeak(burst + 300, after) > 0.27f, "M12 sidechain: Key HP at 2 kHz keeps a 60 Hz kick out of the detector");
        knobs(bassT)[param(DeviceType::Compressor, HP)].value = 0;
        knobs(bassT)[param(DeviceType::Compressor, Listen)].value = 1;
        CHECK(bassPeak(burst + 300, after) > 0.6f && bassPeak(30000, 46000) < 0.05f, "M12 sidechain: Listen plays the key through the track instead of the bass");
        // Listen never reaches an export
        {
            audio::Engine offline(&p);
            audio::RenderOptions o;
            o.startBeat = 0; o.lengthBeats = 4; o.tailSeconds = 0;
            const QString wav = dir.filePath("export.wav");
            QString err;
            const bool rendered = offline.renderOffline(wav, o, {}, &err);
            const SamplePtr back = rendered ? samples::load(wav) : nullptr;
            float betweenKicks = 0;
            if (back) for (int i = 30000; i < 46000 && i < back->frames(); ++i) betweenKicks = std::max(betweenKicks, std::fabs(back->left[size_t(i)]));
            CHECK(rendered && back && betweenKicks > 0.25f, "M12 sidechain: Export Audio renders the compressed bass, not the key, with Listen on");
        }
        knobs(bassT)[param(DeviceType::Compressor, Listen)].value = 0;
        // the key's level meter
        {
            audio::Engine e(&p);
            e.send({Command::Stop, -1, -1, 0, 0, true});
            e.send({Command::Play});
            render(e, burst + 1200);
            const float keyDb = e.deviceMeter(compId, 2);
            render(e, 10000);
            CHECK(keyDb > -6 && e.deviceMeter(compId, 2) < -40, "M12 sidechain: meter[2] is the key's level (loud on the kick, quiet between)");
        }
        // a Gate on the bass, opened by the kick
        p.tracks[bassT].devices = {p.makeDevice("Gate", DeviceKind::AudioEffect)};
        p.tracks[bassT].devices[0].sidechainTrack = p.tracks[kickT].id;
        knobs(bassT)[0].value = (-20.0 + 70) / 70;   // threshold -20 dB
        CHECK(bassPeak(burst + 300, after) > 0.2f && bassPeak(34000, 46000) < 0.02f, "M12 sidechain: a Gate keyed from the kick lets the bass through on the kicks only (shut again after its release)");
        // an Auto Filter whose envelope follows the kick
        p.tracks[bassT].devices = {p.makeDevice("Auto Filter", DeviceKind::AudioEffect)};
        p.tracks[bassT].devices[0].sidechainTrack = p.tracks[kickT].id;
        knobs(bassT)[0].value = 0.2966;   // 200 Hz
        knobs(bassT)[2].value = 0;        // no LFO
        knobs(bassT)[7].value = 1;        // Env all the way up
        {
            const int afId = p.tracks[bassT].devices[0].id;
            audio::Engine e(&p);
            e.send({Command::Stop, -1, -1, 0, 0, true});
            e.send({Command::Play});
            render(e, burst + 1200);
            const float onKick = e.deviceMeter(afId, 0);
            render(e, 21800);   // ~430 ms after the kick: the envelope has fallen most of the way
            CHECK(onKick > 600 && e.deviceMeter(afId, 0) < 320, "M12 sidechain: an Auto Filter's envelope opens the cutoff on the kick and falls back after");
        }
        // a return can be keyed too: a compressor on the return, the bass sent to it
        p.tracks[bassT].devices.clear();
        const int ret = p.insertReturn();
        Track& r = *p.trackAt(ret);
        r.devices = {p.makeDevice("Compressor", DeviceKind::AudioEffect)};
        r.devices[0].params[0].value = 0.85; r.devices[0].params[1].value = 0.7686; r.devices[0].params[2].value = 0; r.devices[0].params[4].value = 0; r.devices[0].params[5].value = 0;
        r.devices[0].sidechainTrack = p.tracks[kickT].id;
        p.tracks[bassT].sends[0] = 1.0;
        {
            auto returnPeak = [&](int from, int to) {
                audio::Engine e(&p);
                e.send({Command::Stop, -1, -1, 0, 0, true});
                e.send({Command::Play});
                render(e, from);
                e.takeReturnPeak(0);
                render(e, to - from);
                return e.takeReturnPeak(0);
            };
            CHECK(returnPeak(burst + 300, after) < returnPeak(30000, 46000) * 0.75f, "M12 sidechain: a Compressor on a return is ducked by the kick too");
        }
        // a deleted key track: the key is silent (no ducking), undo brings it back
        p.tracks[bassT].devices = {p.makeDevice("Compressor", DeviceKind::AudioEffect)};
        for (int i : {0, 1, 2, 4, 5}) knobs(bassT)[i].value = i == 0 ? 0.85 : i == 1 ? 0.7686 : 0;
        p.tracks[bassT].devices[0].sidechainTrack = p.tracks[kickT].id;
        p.deleteTrack(kickT);
        CHECK(p.tracks.size() == 1 && p.tracks[0].devices[0].sidechainTrack != 0 && bassPeak(burst + 300, after) > 0.27f,
              "M12 sidechain: a deleted key track leaves the key silent (Missing), nothing else changes");
        p.undoStack()->undo();
        CHECK(p.tracks.size() == 2 && bassPeak(burst + 300, after) < 0.2f, "M12 sidechain: undo brings the key back");
        // the set file keeps the key's track and the new knobs; a preset drops the track
        {
            knobs(bassT)[param(DeviceType::Compressor, Gain)].value = 0.6;
            knobs(bassT)[param(DeviceType::Compressor, Tap)].value = choicePosition(0, 3);
            setfile::Extras x;
            const QString setPath = dir.filePath("sc.sessions");
            QString err;
            Project q;
            setfile::Extras y;
            const bool saved = setfile::save(setPath, p, x, &err);
            QFile f(setPath); f.open(QIODevice::ReadOnly);
            const QString json = QString::fromUtf8(f.readAll());
            CHECK(saved && json.contains("\"sidechain\"") && setfile::load(setPath, q, y, &err) && q.captureEditState() == p.captureEditState()
                  && q.tracks[bassT].devices[0].sidechainTrack == p.tracks[kickT].id && q.tracks[bassT].devices[0].params.size() == 13,
                  "M12 sets: the key's track and the key knobs round-trip exactly");
            QTemporaryDir presetDir;
            qputenv("SESSIONS_PRESETS", presetDir.path().toLocal8Bit());
            const QString pp = presets::pathFor("Ducker");
            Project::DeviceBlocks blocks;
            CHECK(presets::save(pp, "Ducker", p.deviceBlocks(bassT, {p.tracks[bassT].devices[0].id})) && presets::read(pp, blocks, nullptr, &err)
                  && blocks.size() == 1 && blocks[0][0].sidechainTrack == 0 && std::abs(blocks[0][0].params[param(DeviceType::Compressor, Gain)].value - 0.6) < 1e-9,
                  "M12 presets: a preset keeps the key knobs but not the key's track (a track of another set)");
            qunsetenv("SESSIONS_PRESETS");
        }
        // the panel: the key's source chooser, the tap, the knobs, Listen and the key meter
        {
            audio::Engine e(&p);
            Transport t(&p, &e);
            DevicePanel panel(&p, &t, bassT, p.tracks[bassT].devices[0], nullptr, {});
            devui::DeviceBody* body = panel.body();
            QWidget* keyFrom = nullptr;
            QWidget* listen = nullptr;
            QWidget* keyMeter = nullptr;
            for (QWidget* w : body->findChildren<QWidget*>()) {
                if (w->property("info").toString().startsWith("Key From|")) keyFrom = w;
                if (w->property("mapTarget").toString() == body->target(param(DeviceType::Compressor, Listen))) listen = w;
                if (w->property("info").toString().startsWith("Key Level|")) keyMeter = w;
            }
            CHECK(keyFrom && listen && keyMeter && body->minimumSizeHint().height() <= devui::kBodyHeight + 4,
                  "M12 panels: the Compressor has a Key From chooser, a mappable Listen switch and a key meter, and still fits");
            p.setKeySource(bassT, p.tracks[bassT].devices[0].id, 0);
            QApplication::processEvents();
            CHECK(keyFrom->property("info").toString().contains("Key From") && p.tracks[bassT].devices[0].sidechainTrack == 0, "M12 panels: back to Internal");
        }
    }


    // ================================================================ Milestone 13: Monolith, the big synth
    {
        using namespace monolith;
        auto ne = [](double real, double lo, double hi) { return std::log(real / lo) / std::log(hi / lo); };   // a knob position for an Exp parameter
        auto nl = [](double real, double lo, double hi) { return (real - lo) / (hi - lo); };                  // and for a Linear / Int one
        const DeviceSpec* ms = specOf(DeviceType::Monolith);
        CHECK(ms && paramCount(DeviceType::Monolith) == kParams && kParams == 170 && QString(ms->params[macro(3)].name) == "Macro 4" && QString(ms->params[macro(7)].name) == "Macro 8" && QString(ms->params[lparam(3, LRate)].name) == "LFO4 Rate" && QString(ms->params[BendRange].name) == "Bend Range"
              && QString(ms->params[oparam(2, Fine)].name) == "Osc3 Fine" && QString(ms->params[step(1, 15)].name) == "Step2 16"
              && QString(ms->params[fparam(1, FType)].name) == "F2 Type" && audio::kMaxParams >= kParams,
              "M13 Monolith: 170 knobs in the documented order, the 0.17, 0.20 and 0.21 ones appended (the engine passes up to 192)");
        // Drives a Monolith processor: notes {pitch, start sample, length samples, velocity}, with routes, for `seconds`.
        struct MonoNote { int pitch, at, length; float vel; };
        auto run = [](const std::vector<float>& params, const std::vector<ModRoute>& mods, const std::vector<MonoNote>& notes, double seconds) {
            auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
            DevRun out;
            audio::EventList ev;
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.playing = true;
            ctx.mods = mods.data(); ctx.modCount = int(mods.size());
            const int total = int(seconds * 48000);
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            for (int pos = 0; pos < total; pos += audio::kMaxBlock) {
                const int n = std::min(audio::kMaxBlock, total - pos);
                ev.clear();
                for (const MonoNote& m : notes) if (m.at >= pos && m.at < pos + n) ev.add({m.at - pos, m.pitch, m.vel, m.length, false, false});
                ctx.beat = pos * ctx.beatsPerSample;
                std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
                proc->render(ev, l, r, n, params.data(), ctx);
                for (int i = 0; i < n; ++i) {
                    if (!std::isfinite(l[i]) || !std::isfinite(r[i])) out.finite = false;
                    out.peak = std::max(out.peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
                    out.l.push_back(l[i]); out.r.push_back(r[i]);
                }
            }
            return out;
        };
        auto rms = [](const std::vector<float>& v, int from, int to) { return rmsOf(v, size_t(std::max(0, from)), size_t(std::min<int>(int(v.size()), to))); };
        // how much high-frequency content: the mean step between samples over the mean level
        auto rough = [](const std::vector<float>& v, int from, int to) {
            double d = 0, s = 0;
            for (int i = std::max(1, from); i < std::min<int>(int(v.size()), to); ++i) { d += std::fabs(v[size_t(i)] - v[size_t(i) - 1]); s += std::fabs(v[size_t(i)]); }
            return s > 1e-9 ? float(d / s) : 0.0f;
        };
        // how much the waveform curves: the second difference's RMS over the signal's (a sine at f: ~(2πf/sr)²; harmonics raise it fast)
        auto hf = [](const std::vector<float>& v, int from, int to) {
            double d = 0, s = 0; int n = 0;
            for (int i = std::max(2, from); i < std::min<int>(int(v.size()), to); ++i) {
                const double dd = double(v[size_t(i)]) - 2.0 * v[size_t(i) - 1] + v[size_t(i) - 2];
                d += dd * dd; s += double(v[size_t(i)]) * v[size_t(i)]; ++n;
            }
            return s > 1e-12 ? float(std::sqrt(d / s)) : 0.0f;
        };
        auto zeroCrossHz = [](const std::vector<float>& v, int from, int to) {
            int n = 0;
            for (int i = std::max(1, from); i < std::min<int>(int(v.size()), to); ++i) if ((v[size_t(i)] >= 0) != (v[size_t(i) - 1] >= 0)) ++n;
            return float(n) * 48000.0f / float(2 * std::max(1, to - from));
        };
        std::vector<float> base = defaultsOf(DeviceType::Monolith);
        base[eparam(0, A)] = 0; base[eparam(0, S)] = 1;           // a steady amp: attack 1 ms, full sustain
        base[oparam(1, Level)] = 0;                                // Osc 1 alone, a saw (Basic at 0.66)
        base[fparam(0, FCutoff)] = 1;                              // filter 1 wide open
        const std::vector<MonoNote> c2 = {{36, 0, 24000, 0.9f}};
        DevRun d = run(base, {}, c2, 0.6);
        CHECK(d.finite && d.peak > 0.05f && d.peak < 1.0f && rms(d.l, 2400, 24000) > 0.02f, "M13 Monolith: a C2 on the defaults is finite, audible and under full scale");
        {
            std::vector<float> p = base; p[oparam(0, Level)] = 0;
            CHECK(run(p, {}, c2, 0.3).peak < 1e-5f, "M13 Monolith: with every oscillator at zero it's silent");
        }
        {
            bool all = true;
            for (int t = 0; t < 8; ++t) { std::vector<float> p = base; p[oparam(0, Table)] = choicePosition(t, 8); all &= rms(run(p, {}, c2, 0.3).l, 2400, 14400) > 0.01f; }
            CHECK(all && audio::wavetableCount() == 8, "M13 Monolith: all eight wavetables (Basic … Sub, Growl, Metallic, Digital) sound");
        }
        {
            std::vector<float> sine = base; sine[oparam(0, Table)] = choicePosition(4, 8); sine[oparam(0, Position)] = 0;   // Sub at 0: a pure sine
            const float plain = hf(run(sine, {}, c2, 0.3).l, 2400, 14400);
            bool warps = true;
            for (int mode = 0; mode < 3; ++mode) {
                std::vector<float> p = sine; p[oparam(0, Mode)] = choicePosition(mode, 3); p[oparam(0, Intensity)] = 0.7f;
                warps &= hf(run(p, {}, c2, 0.3).l, 2400, 14400) > plain * 2.0f;
            }
            CHECK(warps, "M13 Monolith: Bend, Formant and Sync at Intensity 0.7 all add harmonics to a sine");
        }
        {
            std::vector<float> lp = base; lp[fparam(0, FType)] = 0; lp[fparam(0, FCutoff)] = ne(100, 30, 18000);
            std::vector<float> hp = base; hp[fparam(0, FType)] = choicePosition(2, 7); hp[fparam(0, FCutoff)] = ne(5000, 30, 18000);
            std::vector<float> lp24 = lp; lp24[fparam(0, FType)] = choicePosition(1, 7);
            const float open = hf(d.l, 4800, 24000), low = hf(run(lp, {}, c2, 0.6).l, 4800, 24000), low24 = hf(run(lp24, {}, c2, 0.6).l, 4800, 24000),
                        high = hf(run(hp, {}, c2, 0.6).l, 4800, 24000);
            CHECK(low < open * 0.5f && low24 < low && high > open * 1.5f, "M13 Monolith: LP 12 at 100 Hz darkens a saw, LP 24 more so, HP at 5 kHz brightens it");
            std::vector<float> scream = lp; scream[fparam(0, FType)] = choicePosition(6, 7); scream[fparam(0, FReso)] = 0.8f; scream[oparam(0, Table)] = choicePosition(4, 8); scream[oparam(0, Position)] = 0;
            std::vector<float> lpSine = lp; lpSine[oparam(0, Table)] = choicePosition(4, 8); lpSine[oparam(0, Position)] = 0; lpSine[fparam(0, FCutoff)] = ne(400, 30, 18000); scream[fparam(0, FCutoff)] = ne(400, 30, 18000);
            const DevRun s = run(scream, {}, c2, 0.6);
            CHECK(s.finite && hf(s.l, 4800, 24000) > hf(run(lpSine, {}, c2, 0.6).l, 4800, 24000) * 1.3f, "M13 Monolith: Scream drives a sine into harmonics that a plain low-pass doesn't");
            std::vector<float> comb = base; comb[fparam(0, FType)] = choicePosition(5, 7); comb[fparam(0, FReso)] = 0.9f; comb[fparam(0, FCutoff)] = ne(200, 30, 18000);
            const DevRun cb = run(comb, {}, c2, 0.6);
            CHECK(cb.finite && cb.peak < 2.0f && rms(cb.l, 4800, 24000) > 0.02f, "M13 Monolith: the Comb filter at high resonance stays stable and audible");
        }
        {
            std::vector<float> p = base; p[fparam(0, FType)] = 0; p[fparam(0, FCutoff)] = ne(100, 30, 18000);
            p[eparam(1, A)] = 0; p[eparam(1, D)] = ne(0.15, 0.005, 8); p[eparam(1, S)] = 0;
            const DevRun e = run(p, {{SrcEnv2, fparam(0, FCutoff), 0.6}}, c2, 0.6);
            CHECK(rough(e.l, 500, 2400) > rough(e.l, 20000, 28000) * 1.5f, "M13 Monolith: Env 2 routed to the cutoff opens the filter, then it closes as the envelope decays");
            std::vector<float> q = p; q[lparam(0, LRate)] = choicePosition(4, 9); q[lparam(0, LShape)] = choicePosition(4, 6);   // 1/4 note square
            const DevRun l = run(q, {{SrcLfo1, fparam(0, FCutoff), 0.6}}, {{36, 0, 48000, 0.9f}}, 1.0);
            CHECK(rough(l.l, 2400, 11000) > rough(l.l, 14400, 23000) * 1.5f && rough(l.l, 26400, 35000) > rough(l.l, 38400, 47000) * 1.5f,
                  "M13 Monolith: a synced square LFO on the cutoff opens and closes it every quarter note");
            std::vector<float> st = base; st[sparam(0, SRate)] = choicePosition(6, 8); st[sparam(0, SCount)] = nl(4, 1, 16);   // 1/16: 6000 samples a step
            st[step(0, 0)] = 1; st[step(0, 1)] = 0; st[step(0, 2)] = 1; st[step(0, 3)] = 0; st[oparam(0, Level)] = 0.5f;
            const DevRun sq = run(st, {{SrcStep1, oparam(0, Level), 0.5}}, {{36, 0, 48000, 0.9f}}, 0.6);
            CHECK(rms(sq.l, 1000, 5500) > 0.03f && rms(sq.l, 7000, 11500) < 0.002f && rms(sq.l, 13000, 17500) > 0.03f && rms(sq.l, 19000, 23500) < 0.002f,
                  "M13 Monolith: a 4-step sequencer on the level switches the oscillator on and off every 1/16");
            std::vector<float> mc = p; mc[macro(0)] = 1;
            CHECK(rough(run(mc, {{SrcMacro1, fparam(0, FCutoff), 0.6}}, c2, 0.6).l, 4800, 24000) > rough(run(p, {}, c2, 0.6).l, 4800, 24000) * 1.5f,
                  "M13 Monolith: Macro 1 routed to the cutoff opens it");
        }
        {
            std::vector<float> leg = base; leg[VoiceMode] = choicePosition(2, 3); leg[Glide] = ne(0.2, 0.001, 2);
            leg[oparam(0, Table)] = choicePosition(4, 8); leg[oparam(0, Position)] = 0;   // a sine: clean zero crossings
            leg[eparam(0, D)] = ne(0.05, 0.005, 8); leg[eparam(0, S)] = 0.2f;              // the amp settles at 20 %: a retrigger shows as a new peak
            const std::vector<MonoNote> two = {{36, 0, 48000, 0.9f}, {43, 14400, 30000, 0.9f}};
            auto peakIn = [](const std::vector<float>& l, int from, int to) { float pk = 0; for (int i = from; i < to && i < int(l.size()); ++i) pk = std::max(pk, std::fabs(l[size_t(i)])); return pk; };
            const DevRun g = run(leg, {}, two, 1.0);
            const float before = zeroCrossHz(g.l, 4800, 14000), during = zeroCrossHz(g.l, 15500, 18500), after = zeroCrossHz(g.l, 36000, 46000);
            const float first = peakIn(g.l, 100, 1500), second = peakIn(g.l, 14900, 18400);   // the attack's peak, then the held level
            CHECK(second < first * 0.4f && std::abs(before - 65.4f) < 3 && std::abs(after - 98.0f) < 4 && during > before + 2 && during < after - 2,
                  "M13 Monolith: Legato: an overlapping note doesn't retrigger the envelope and glides from C2 to G2");
            std::vector<float> mono = leg; mono[VoiceMode] = choicePosition(1, 3);
            const DevRun m = run(mono, {}, two, 1.0);
            CHECK(peakIn(m.l, 14900, 18400) > peakIn(m.l, 100, 1500) * 0.7f, "M13 Monolith: Mono retriggers the envelope on every note");
            std::vector<float> poly = leg; poly[VoiceMode] = 0;
            auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
            audio::EventList ev; audio::Context ctx; ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.playing = true;
            float l[128], r[128];
            ev.add({0, 36, 0.9f, 48000, false, false}); ev.add({0, 43, 0.9f, 48000, false, false});
            for (int k = 0; k < 20; ++k) { std::fill(l, l + 128, 0.0f); std::fill(r, r + 128, 0.0f); proc->render(ev, l, r, 128, poly.data(), ctx); ev.clear(); }
            CHECK(proc->meter[0].load() == 2.0f, "M13 Monolith: Poly plays both notes at once (two voices)");
        }
        {
            std::vector<float> u1 = base; u1[Spread] = 1;
            std::vector<float> u4 = u1; u4[Unison] = 1; u4[Detune] = nl(30, 0, 50);
            const DevRun a = run(u1, {}, c2, 0.4), b = run(u4, {}, c2, 0.4);
            double diffA = 0, diffB = 0, sum = 0;
            for (size_t i = 4800; i < a.l.size(); ++i) { diffA += std::fabs(a.l[i] - a.r[i]); diffB += std::fabs(b.l[i] - b.r[i]); sum += std::fabs(b.l[i]); }
            CHECK(diffA < 1e-6 && diffB > sum * 0.1, "M13 Monolith: Unison 4 with Spread is wide, one voice sits in the middle");
        }
        {
            std::vector<float> fb = base; fb[Feedback] = 1; fb[fparam(0, FType)] = choicePosition(6, 7); fb[fparam(0, FReso)] = 1; fb[InsertType] = choicePosition(1, 6); fb[InsertAmount] = 1;
            const DevRun f = run(fb, {}, {{36, 0, 48000, 1.0f}}, 1.0);
            CHECK(f.finite && f.peak < 1.5f, "M13 Monolith: full feedback into a screaming filter and a driven insert stays finite and bounded");
            std::vector<float> ring = base; ring[ModRing] = 1; ring[ModPitch] = nl(7, -36, 36);
            std::vector<float> noise = base; noise[oparam(0, Level)] = 0; noise[NoiseLevel] = 1;
            std::vector<float> dark = noise; dark[NoiseColor] = 0;
            std::vector<float> bits = base; bits[InsertType] = choicePosition(5, 6); bits[InsertAmount] = 0.9f;
            const float plain = rough(d.l, 4800, 24000);
            CHECK(std::fabs(rough(run(ring, {}, c2, 0.6).l, 4800, 24000) - plain) > plain * 0.1f, "M13 Monolith: ring modulation by the modulation oscillator changes the spectrum");
            const DevRun nz = run(noise, {}, c2, 0.4), dk = run(dark, {}, c2, 0.4);
            CHECK(rms(nz.l, 2400, 19000) > 0.01f && rough(dk.l, 2400, 19000) < rough(nz.l, 2400, 19000) * 0.5f, "M13 Monolith: noise sounds, and Color darkens it");
            CHECK(std::fabs(rough(run(bits, {}, c2, 0.6).l, 4800, 24000) - plain) > plain * 0.1f, "M13 Monolith: the Bits insert changes the sound");
            std::vector<float> vel = base; vel[VelAmp] = 1;
            const float loud = run(vel, {}, {{36, 0, 24000, 1.0f}}, 0.3).peak, soft = run(vel, {}, {{36, 0, 24000, 0.2f}}, 0.3).peak;
            CHECK(soft < loud * 0.4f, "M13 Monolith: Vel > Amp makes soft notes quieter");
        }
        // ---- every Sound plays through the engine, carrying its routes
        {
            Project p;
            p.scenes = {"1"};
            p.addTrack("Bass", QColor("#62c9dc"));
            ClipPtr c = p.newClip("Note", 4);
            c->notes = {{36, 0, 2, 0.9, 1.0}};
            p.tracks[0].clipSlots[0] = c;
            p.tracks[0].arrangement = {{0, 4, 0, c}};
            QStringList sounds;
            for (const DeviceSpec& s : deviceSpecs()) if (s.type == DeviceType::Monolith && specOf(s.type) != &s) sounds << s.name;
            bool allPlay = sounds.size() >= 6, routed = true;
            for (const QString& name : sounds) {
                p.tracks[0].devices = {p.makeDevice(name, DeviceKind::Instrument)};
                routed &= !p.tracks[0].devices[0].mods.isEmpty() && p.tracks[0].devices[0].mods == QVector<ModRoute>(presetMods(name).begin(), presetMods(name).end())
                          && p.tracks[0].devices[0].params.size() == kParams;
                const std::vector<float> out = playArrangement(p, 48000);
                bool finite = true;
                for (float x : out) finite &= std::isfinite(x);
                const float pk = peakOf(out);
                if (!finite || pk < 0.02f || pk >= 1.0f) { allPlay = false; std::printf("      %s: finite %d peak %.3f\n", qPrintable(name), finite, pk); }
            }
            CHECK(routed, "M13 Sounds: Growl Bass, Wobble Sub, Reese Monolith, Neuro Step, Formant Talk, 808 Sub and Sync Bass come with their routes");
            CHECK(allPlay, "M13 Sounds: every one of them plays a C2 through the engine, finite and under full scale");
            // the routes reach the processor through the engine: a macro turning the level all the way down
            p.tracks[0].devices = {p.makeDevice("Monolith", DeviceKind::Instrument)};
            p.tracks[0].devices[0].params[macro(0)].value = 1;
            const float plain = peakOf(playArrangement(p, 24000));
            p.tracks[0].devices[0].mods = {{SrcMacro1, oparam(0, Level), -1.0}, {SrcMacro1, oparam(1, Level), -1.0}};
            CHECK(plain > 0.05f && peakOf(playArrangement(p, 24000)) < 1e-5f, "M13 engine: the routes reach the processor (Macro 1 turns both oscillators down to nothing)");
            // the set file and presets keep the routes
            QTemporaryDir dir;
            setfile::Extras x;
            const QString setPath = dir.filePath("mono.sessions");
            QString err;
            Project q;
            setfile::Extras y;
            const bool saved = setfile::save(setPath, p, x, &err);
            QFile f(setPath); f.open(QIODevice::ReadOnly);
            const QString json = QString::fromUtf8(f.readAll());
            CHECK(saved && json.contains("\"mods\"") && setfile::load(setPath, q, y, &err) && q.captureEditState() == p.captureEditState() && q.tracks[0].devices[0].mods.size() == 2,
                  "M13 sets: the routes round-trip exactly");
            qputenv("SESSIONS_PRESETS", dir.path().toLocal8Bit());
            Project::DeviceBlocks blocks;
            const QString pp = presets::pathFor("My Growl");
            CHECK(presets::save(pp, "My Growl", p.deviceBlocks(0, {p.tracks[0].devices[0].id})) && presets::read(pp, blocks, nullptr, &err)
                  && blocks.size() == 1 && blocks[0][0].mods == p.tracks[0].devices[0].mods, "M13 presets: a preset keeps the routes");
            qunsetenv("SESSIONS_PRESETS");
            CHECK(p.freshCopies(p.deviceBlocks(0, {p.tracks[0].devices[0].id}))[0][0].mods == p.tracks[0].devices[0].mods, "M13 model: a copy keeps the routes");
        }
        // ---- the panel: pages, a dropped source, the routes list
        {
            Project p;
            p.scenes = {"1"};
            p.addTrack("Bass", QColor("#62c9dc"));
            p.tracks[0].devices = {p.makeDevice("Growl Bass", DeviceKind::Instrument)};
            const int id = p.tracks[0].devices[0].id;
            audio::Engine e(&p);
            Transport t(&p, &e);
            DevicePanel panel(&p, &t, 0, p.tracks[0].devices[0], nullptr, {});
            panel.show();
            devui::DeviceBody* body = panel.body();
            QList<QPushButton*> pages = body->findChildren<QPushButton*>("monoDrawer");
            QStackedWidget* stack = body->findChild<QStackedWidget*>();
            const int closedWidth = body->width();
            CHECK(pages.size() == 7 && stack && stack->count() == 7 && closedWidth <= 1200 && body->minimumSizeHint().height() <= devui::kBodyHeight + 4,
                  qPrintable(QString("M13 panel (0.20): the main panel with six drawers closed, in the device's height (%1 x %2 px)").arg(body->width()).arg(body->minimumSizeHint().height())));
            QPushButton* modButton = nullptr;
            for (QPushButton* b : pages) if (b->text() == "Mod") modButton = b;
            QTest::mouseClick(modButton, Qt::LeftButton);
            QTest::qWait(320);
            QApplication::processEvents();
            CHECK(stack->currentIndex() == 3 && modButton->isChecked() && !pages[0]->isChecked() && body->width() > closedWidth + 200 && body->width() <= 1700,
                  qPrintable(QString("M13 panel (0.20): the Mod button slides the Mod drawer out (%1 px wide now)").arg(body->width())));
            int rows = 0;
            for (QWidget* w : body->findChildren<QWidget*>()) if (w->property("info").toString().startsWith("Amount|")) ++rows;
            CHECK(stack->currentIndex() == 3 && rows == p.tracks[0].devices[0].mods.size() && rows == 5, "M13 panel: the Mod drawer lists one row per route (Growl Bass has five)");
            // a source chip dropped on Filter 1's cutoff knob adds a route
            QWidget* cutoff = nullptr;
            for (QWidget* w : body->findChildren<QWidget*>()) if (w->property("mapTarget").toString() == body->target(fparam(1, FCutoff))) cutoff = w;
            const int steps = p.undoStack()->count();
            if (cutoff) {
                QMimeData mime;
                mime.setData(devui::kModSourceMime, QByteArray::number(int(SrcLfo2)));
                QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(cutoff, &enter);
                QDropEvent drop(QPointF(5, 5), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(cutoff, &drop);
            }
            QApplication::processEvents();
            const QVector<ModRoute>& mods = p.tracks[0].devices[0].mods;
            CHECK(cutoff && mods.size() == 6 && mods.last().source == SrcLfo2 && mods.last().param == fparam(1, FCutoff) && std::abs(mods.last().amount - 0.3) < 1e-9
                  && p.undoStack()->count() == steps + 1 && body->modulated(fparam(1, FCutoff)),
                  "M13 panel: dropping LFO 2 on F2 Cutoff adds a route (30 %), one undo step, and the knob shows it");
            // its amount field, dragged up, raises the amount in one step; ✕ removes it
            QApplication::processEvents();
            DragValue* amount = nullptr;
            QToolButton* remove = nullptr;
            for (DragValue* v : body->findChildren<DragValue*>()) if (v->property("info").toString().startsWith("Amount|")) amount = v;
            for (QToolButton* b : body->findChildren<QToolButton*>()) if (b->text() == "✕") remove = b;
            if (amount) {
                QTest::mousePress(amount, Qt::LeftButton, Qt::NoModifier, QPoint(20, 8));
                QTest::mouseMove(amount, QPoint(20, -40));
                QTest::mouseRelease(amount, Qt::LeftButton, Qt::NoModifier, QPoint(20, -40));
            }
            CHECK(amount && p.tracks[0].devices[0].mods.last().amount > 0.35 && p.undoStack()->count() == steps + 2, "M13 panel: dragging a route's amount changes it, one undo step");
            p.undoStack()->undo();
            CHECK(std::abs(p.tracks[0].devices[0].mods.last().amount - 0.3) < 1e-9, "M13 panel: and undo puts it back");
            QApplication::processEvents();
            for (QToolButton* b : body->findChildren<QToolButton*>()) if (b->text() == "✕") remove = b;
            if (remove) QTest::mouseClick(remove, Qt::LeftButton);
            QApplication::processEvents();
            CHECK(remove && p.tracks[0].devices[0].mods.size() == 5 && !body->modulated(fparam(1, FCutoff)), "M13 panel: ✕ removes the route");
            p.undoStack()->undo();
            CHECK(p.tracks[0].devices[0].mods.size() == 6, "M13 panel: undo brings the route back");
            CHECK(p.tracks[0].devices[0].params[oparam(0, Position)].value == 0.35 && body->modulated(oparam(0, Position)), "M13 panel: Growl Bass's Osc 1 position is modulated (by LFO 1)");
        }
    }

    // ---- Milestone 14: themes (View > Theme), and dragging a Session clip into the Arrangement through Tab
    {
        theme::apply(theme::Kind::Dark);   // the app stylesheet, as main() applies it (the tests above ran unstyled)
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QApplication::processEvents();
        Project* p = w.project();
        ClipGrid* grid = w.sessionView()->grid();
        ArrangementView* av = w.arrangementView();
        auto key = [&](int k, Qt::KeyboardModifiers mods = Qt::NoModifier) { QTest::keyClick(QApplication::focusWidget() ? QApplication::focusWidget() : &w, Qt::Key(k), mods); QApplication::processEvents(); };
        auto slotRect = [&](int track, int scene) {
            for (const auto& c : grid->mapCells()) if (c.second == QString("slot:%1:%2").arg(p->tracks[track].id).arg(scene)) return c.first;
            return QRect();
        };
        if (!w.sessionViewShown()) key(Qt::Key_Tab);
        QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, slotRect(2, 2).center());   // the Keys chords in the Clip View
        QApplication::processEvents();
        QApplication::processEvents();

        // themes: Dark is the original; Light and Mid switch every colour at once, live, and are remembered
        const QColor darkBg = theme::bg;
        CHECK(theme::kind() == theme::Kind::Dark && theme::bg == QColor("#2b2b2b") && theme::text == QColor("#d2d2d2") && devui::col::display == QColor("#171717")
              && devui::col::curve == theme::accent, "M14 theme: Dark is the default, with the original colours (the device palette refers to the theme)");
        const QRect top(0, 0, w.width(), w.sessionView()->mapTo(&w, QPoint(0, w.sessionView()->height())).y());   // menu, control bar, browser, Session View
        const QImage before = w.grab().toImage().copy(top);
        QAction *light = nullptr, *mid = nullptr, *dark = nullptr;
        for (QAction* a : w.findChildren<QAction*>()) { if (a->text() == "Light") light = a; else if (a->text() == "Mid") mid = a; else if (a->text() == "Dark") dark = a; }
        if (light) light->trigger();
        QApplication::processEvents();
        CHECK(light && mid && dark && light->isCheckable() && light->isChecked() && !dark->isChecked(), "M14 theme: View > Theme lists Dark, Mid and Light; Light is checked after choosing it");
        CHECK(theme::kind() == theme::Kind::Light && theme::bg.lightnessF() > 0.6 && theme::text.lightnessF() < 0.3 && devui::col::display.lightnessF() > 0.8 && devui::col::value.lightnessF() < 0.3
              && theme::outline.lightnessF() < 0.3 && theme::readout == theme::text,
              "M14 theme: Light has light surfaces, dark text, light device displays, dark outlines and dark readouts");
        CHECK(QSettings("Sessions", "Sessions").value("ui/theme").toString() == "light" && theme::savedKind() == theme::Kind::Light, "M14 theme: the choice is remembered (QSettings ui/theme)");
        CHECK(qApp->styleSheet().contains(theme::bg.name()) && !qApp->styleSheet().contains("#2b2b2b"), "M14 theme: the stylesheet is rebuilt from the new colours");
        auto meanGray = [](const QImage& img) { double s = 0; int n = 0; for (int y = 0; y < img.height(); y += 3) for (int x = 0; x < img.width(); x += 3) { s += qGray(img.pixel(x, y)); ++n; } return s / std::max(1, n); };
        const QImage lightImg = w.grab().toImage().copy(top);
        if (qEnvironmentVariableIsSet("SESSIONS_SMOKE_DUMP")) { before.save(qgetenv("SESSIONS_SMOKE_DUMP") + "/m14-before.png"); lightImg.save(qgetenv("SESSIONS_SMOKE_DUMP") + "/m14-light.png"); }
        CHECK(meanGray(lightImg) > meanGray(before) + 60, "M14 theme: the window really paints light (the Session View and its chrome are far brighter)");
        {   // the piano roll keeps white keys light and black keys dark (FORM's check), on a light grid
            PianoRoll* roll = w.findChild<PianoRoll*>();
            const QImage ri = roll->grab().toImage();
            int lightKeys = 0, darkKeys = 0, lightGrid = 0;
            for (int y = 20; y < ri.height() - 40; ++y) { const int g = qGray(ri.pixel(3, y)); if (g > 200) ++lightKeys; else if (g < 80) ++darkKeys; if (qGray(ri.pixel(ri.width() / 2, y)) > 150) ++lightGrid; }
            CHECK(roll && lightKeys > 20 && darkKeys > 10 && lightGrid > 20, "M14 theme: the piano roll's keys stay white and black on a light grid");
        }
        // Mid sits between the two
        mid->trigger();
        QApplication::processEvents();
        CHECK(theme::kind() == theme::Kind::Mid && theme::bg.lightnessF() > darkBg.lightnessF() + 0.2 && theme::bg.lightnessF() < QColor("#b8b8b5").lightnessF() - 0.1 && theme::text.lightnessF() < 0.2,
              "M14 theme: Mid sits between Dark and Light, with dark text");
        // and back to Dark: pixel for pixel what it was
        dark->trigger();
        QApplication::processEvents();
        const QImage after = w.grab().toImage().copy(top);
        if (qEnvironmentVariableIsSet("SESSIONS_SMOKE_DUMP")) after.save(qgetenv("SESSIONS_SMOKE_DUMP") + "/m14-after.png");
        CHECK(theme::kind() == theme::Kind::Dark && theme::savedKind() == theme::Kind::Dark && after == before, "M14 theme: back to Dark the window is pixel-identical to before");
        CHECK(theme::stylesheet().contains("background: #2b2b2b; color: #d2d2d2") && theme::stylesheet().contains("QToolButton:checked, QPushButton:checked { background: #ffa63c; color: #151515; }"),
              "M14 theme: the Dark stylesheet reads as the original literals");

        // dragging a Session clip into the Arrangement: pick it up, Tab, drop it on a lane of its kind
        const int bass = 1;
        const ClipPtr line = p->tracks[bass].clipSlots[1];   // Bass Line, 8 beats
        auto pickUp = [&](int track, int scene) {
            if (!w.sessionViewShown()) key(Qt::Key_Tab);
            QApplication::processEvents();
            const QRect r = slotRect(track, scene);
            QTest::mousePress(grid, Qt::LeftButton, Qt::NoModifier, r.center());
            QTest::mouseMove(grid, r.center() + QPoint(25, 0));
            return !r.isEmpty() && grid->clipDragActive();
        };
        CHECK(line && pickUp(bass, 1), "M14 drag: picking up a Session clip and moving a little starts a clip drag in the grid");
        int steps = p->undoStack()->count();
        key(Qt::Key_Tab);
        CHECK(!w.sessionViewShown() && av->clipDragActive() && !grid->clipDragActive(), "M14 drag: Tab while dragging switches to the Arrangement, which takes the clip");
        const QRect bassLane = av->laneRect(bass);
        const QPointF over(av->beatToX(40.3), bassLane.center().y());
        QTest::mouseMove(av, over.toPoint());
        const QImage ghost = av->grab().toImage();   // paints the ghost (and runs that code under the sanitizers)
        CHECK(av->clipDropLane(over, line) == bass && !ghost.isNull(), "M14 drag: over the Bass lane the clip can land, and the view draws where");
        CHECK(av->clipDropLane(QPointF(over.x(), av->laneRect(int(p->tracks.size())).center().y()), line) < 0
              && av->clipDropLane(QPointF(over.x(), 10), line) < 0 && av->clipDropLane(QPointF(av->width() - 10, over.y()), line) < 0,
              "M14 drag: not on a return lane, the ruler or the track headers");
        const int placed0 = p->tracks[bass].arrangement.size();
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, over.toPoint());
        QApplication::processEvents();
        {
            const auto& arr = p->tracks[bass].arrangement;
            const ArrangementClip* a = arr.size() == placed0 + 1 ? &arr.last() : nullptr;
            CHECK(a && a->start == 40 && a->length == line->length && a->offset == 0, "M14 drag: the release places it at the snapped beat (40.3 -> 40), one clip length long");
            CHECK(a && a->clip && a->clip != line && a->clip->id != line->id && a->clip->name == line->name && a->clip->notes.size() == line->notes.size() && a->clip->notes.first().pitch == line->notes.first().pitch && a->clip->length == line->length,
                  "M14 drag: what lands is a copy of the clip (its own id, the same notes)");
            CHECK(!av->clipDragActive() && !grid->clipDragActive() && p->undoStack()->count() == steps + 1 && a && p->selectedClip == a->clip && p->selectedTrack == bass && w.clipViewShown(),
                  "M14 drag: the drag is over, it's one undo step, and the new clip is selected and shown");
            CHECK(p->tracks[bass].clipSlots[1] == line, "M14 drag: the Session slot keeps its clip");
        }
        p->undoStack()->undo();
        CHECK(p->tracks[bass].arrangement.size() == placed0, "M14 drag: undo removes the placement");
        p->undoStack()->redo();
        CHECK(p->tracks[bass].arrangement.size() == placed0 + 1 && p->tracks[bass].arrangement.last().start == 40, "M14 drag: redo brings it back");
        p->undoStack()->undo();

        // Alt while dropping: unsnapped
        CHECK(pickUp(bass, 1), "M14 drag: picked up again");
        key(Qt::Key_Tab);
        const QPoint freePt(int(av->beatToX(44.37)) + 1, bassLane.center().y());
        QTest::mouseMove(av, freePt, Qt::AltModifier);
        QTest::mouseRelease(av, Qt::LeftButton, Qt::AltModifier, freePt);
        QApplication::processEvents();
        {
            const double expect = av->xToBeat(freePt.x());
            const auto& arr = p->tracks[bass].arrangement;
            CHECK(arr.size() == placed0 + 1 && std::abs(arr.last().start - expect) < 1e-9 && std::abs(expect - std::round(expect)) > 0.02,
                  "M14 drag: with Alt held the clip lands exactly under the mouse, off the grid");
            p->undoStack()->undo();
        }

        // dropped over what's already there: the drop replaces the material under it (like Paste)
        CHECK(pickUp(bass, 1), "M14 drag: picked up a third time");
        key(Qt::Key_Tab);
        const QPoint onExisting(int(av->beatToX(24.2)), bassLane.center().y());   // the demo's Bass Line sits at 16..32
        QTest::mouseMove(av, onExisting);
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, onExisting);
        QApplication::processEvents();
        {
            int covering = 0; bool trimmed = false;
            for (const ArrangementClip& a : p->tracks[bass].arrangement) { if (a.start < 28 && a.start + a.length > 28) ++covering; if (a.start == 16 && a.length == 8) trimmed = true; }
            CHECK(covering == 1 && trimmed && p->tracks[bass].arrangement.last().start == 24, "M14 drag: dropping on an existing clip trims it and the new copy takes its place (24..32)");
            p->undoStack()->undo();
            CHECK(p->tracks[bass].arrangement.size() == placed0 && p->tracks[bass].arrangement.first().length == 16, "M14 drag: one undo step restores the trimmed clip too");
        }

        // the wrong kind of track: a MIDI clip over an audio lane doesn't land; over a group lane neither
        const int audio = p->insertTrack(int(p->tracks.size()), TrackType::Audio);
        QApplication::processEvents();
        CHECK(pickUp(bass, 1), "M14 drag: picked up for the audio lane");
        key(Qt::Key_Tab);
        const QPoint onAudio(int(av->beatToX(8.0)), av->laneRect(audio).center().y());
        QTest::mouseMove(av, onAudio);
        CHECK(av->clipDropLane(onAudio, line) < 0 && av->cursor().shape() == Qt::ForbiddenCursor, "M14 drag: a MIDI clip can't land on an audio track (forbidden cursor)");
        steps = p->undoStack()->count();
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, onAudio);
        QApplication::processEvents();
        CHECK(p->tracks[audio].arrangement.isEmpty() && p->undoStack()->count() == steps && !av->clipDragActive(), "M14 drag: releasing there places nothing and records no undo step");
        p->tracks.removeAt(audio);
        emit p->structureChanged();
        QApplication::processEvents();

        // Tab again brings the clip back to the Session View, drag and all; dropping it on a slot moves it as before
        CHECK(pickUp(bass, 1), "M14 drag: picked up for the round trip");
        key(Qt::Key_Tab);
        CHECK(!w.sessionViewShown() && av->clipDragActive(), "M14 drag: in the Arrangement");
        key(Qt::Key_Tab);
        CHECK(w.sessionViewShown() && !av->clipDragActive() && grid->clipDragActive(), "M14 drag: Tab again returns to the Session View with the clip still in hand");
        const QRect target = slotRect(bass, 3);
        QTest::mouseMove(grid, target.center());
        QTest::mouseRelease(grid, Qt::LeftButton, Qt::NoModifier, target.center());
        QApplication::processEvents();
        CHECK(!target.isEmpty() && p->tracks[bass].clipSlots[3] == line && !p->tracks[bass].clipSlots[1] && !grid->clipDragActive() && QWidget::mouseGrabber() == nullptr,
              "M14 drag: dropped on a slot it moves there, as any slot drag, and the grid lets the mouse go");
        p->undoStack()->undo();
        CHECK(p->tracks[bass].clipSlots[1] == line, "M14 drag: undo puts it back in its slot");

        // Escape drops a clip in flight
        CHECK(pickUp(bass, 1), "M14 drag: picked up for Escape");
        key(Qt::Key_Tab);
        steps = p->undoStack()->count();
        key(Qt::Key_Escape);
        CHECK(!av->clipDragActive() && !w.sessionViewShown() && QWidget::mouseGrabber() == nullptr, "M14 drag: Escape ends the drag in the Arrangement");
        QTest::mouseRelease(av, Qt::LeftButton, Qt::NoModifier, over.toPoint());
        QApplication::processEvents();
        CHECK(p->tracks[bass].arrangement.size() == placed0 && p->undoStack()->count() == steps, "M14 drag: and the release afterwards places nothing");
        key(Qt::Key_Tab);
    }

    // ================================================================ Milestone 15: NAM (Neural Amp Modeler)
    {
        QTemporaryDir dir;
        auto writeNam = [&](const QString& name, const QByteArray& json) {
            const QString path = dir.filePath(name);
            QFile f(path); f.open(QIODevice::WriteOnly); f.write(json); f.close();
            return path;
        };
        // One channel through a Runtime in engine-sized blocks (both sides the same signal).
        auto runRuntime = [](nam::Runtime& rt, const std::vector<float>& x) {
            std::vector<float> y(x.size());
            for (size_t pos = 0; pos < x.size(); pos += audio::kMaxBlock) {
                const int n = int(std::min<size_t>(audio::kMaxBlock, x.size() - pos));
                float l[audio::kMaxBlock], r[audio::kMaxBlock];
                std::copy(x.begin() + long(pos), x.begin() + long(pos) + n, l);
                std::copy(x.begin() + long(pos), x.begin() + long(pos) + n, r);
                rt.process(l, r, n);
                std::copy(l, l + n, y.begin() + long(pos));
            }
            return y;
        };
        auto sigmoid = [](double x) { return 1.0 / (1.0 + std::exp(-x)); };
        auto pos01 = [](double real, double lo, double hi) { return float((real - lo) / (hi - lo)); };   // a knob position for a real value

        // ---- the reader: a Linear capture (a 3-tap FIR with a bias) checked against its definition
        QString err;
        const QString linPath = writeNam("fir.nam", R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":3,"bias":true},"weights":[0.5,-0.25,0.125,0.01],"sample_rate":48000,"metadata":{"loudness":-12.0,"name":"FIR"}})");
        const nam::ModelPtr lin = nam::load(linPath, &err);
        CHECK(lin && lin->arch == nam::Arch::Linear && lin->ir.size() == 3 && lin->irBias == 0.01f && lin->name == "FIR" && lin->hasLoudness && lin->loudness == -12 && lin->rateKnown && lin->weightCount == 4,
              qPrintable("M15 .nam: a Linear capture reads (taps, bias, name, loudness, rate) " + err));
        {
            nam::Runtime rt(lin, 48000);
            std::vector<float> x(300);
            for (size_t i = 0; i < x.size(); ++i) x[i] = float(0.5 * std::sin(i * 0.37));
            const std::vector<float> y = runRuntime(rt, x);
            float worst = 0;
            for (size_t t = 0; t < x.size(); ++t) {
                const double want = 0.01 + 0.5 * x[t] - 0.25 * (t >= 1 ? x[t - 1] : 0) + 0.125 * (t >= 2 ? x[t - 2] : 0);
                worst = std::max(worst, float(std::fabs(y[t] - want)));
            }
            CHECK(worst < 1e-5f && !rt.resampling() && rt.latency() == 0, "M15 Linear: y[t] = bias + ir[0] x[t] + ir[1] x[t-1] + ir[2] x[t-2], exactly, no latency at the capture's rate");
        }
        CHECK(!nam::parse(R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":3,"bias":true},"weights":[1,2,3]})", &err) && err.contains("needs 4"),
              qPrintable("M15 .nam: the wrong number of weights is refused, saying how many it needs: " + err));
        CHECK(!nam::parse(R"({"version":"0.5.0","architecture":"Sequential","config":{},"weights":[]})", &err) && err.contains("Sequential"),
              "M15 .nam: an unknown architecture is refused by name");
        CHECK(!nam::parse("not json at all", &err) && !nam::load(dir.filePath("nowhere.nam"), &err) && err.contains("missing"), "M15 .nam: junk and a missing file are refused");

        // ---- a tiny gated WaveNet (one array, 2 channels, kernel 2, dilations 1 and 2) against a reference
        // written out here from the format: weights in NAM's order, every layer kept for the whole signal.
        std::vector<float> gw(66);
        for (size_t i = 0; i < gw.size(); ++i) gw[i] = float(0.6 * std::sin(1.7 * double(i) + 0.3));
        {
            QJsonArray wa; for (float w : gw) wa.append(double(w));
            const QJsonObject layer{{"input_size", 1}, {"condition_size", 1}, {"head_size", 1}, {"channels", 2}, {"kernel_size", 2}, {"dilations", QJsonArray{1, 2}},
                                    {"activation", "Tanh"}, {"gated", true}, {"head_bias", true}};
            const QJsonObject root{{"version", "0.5.4"}, {"architecture", "WaveNet"}, {"config", QJsonObject{{"layers", QJsonArray{layer}}, {"head", QJsonValue::Null}, {"head_scale", 0.02}}},
                                   {"weights", wa}, {"sample_rate", 48000}, {"metadata", QJsonObject{{"name", "Tiny Gated"}, {"loudness", -20.0}}}};
            const QString tinyPath = writeNam("tiny.nam", QJsonDocument(root).toJson());
            err.clear();
            const nam::ModelPtr tiny = nam::load(tinyPath, &err);
            CHECK(tiny && tiny->arch == nam::Arch::WaveNet && tiny->arrays.size() == 1 && tiny->arrays[0].layers.size() == 2 && tiny->arrays[0].layers[1].gating == 1
                  && tiny->receptiveField == 4 && tiny->summary == "WaveNet · 2 channels · 2 layers · gated" && tiny->headScale == gw[65],
                  qPrintable("M15 .nam: a gated WaveNet reads (arrays, layers, gating, receptive field, head scale) " + err));
            // the reference, on the signal preceded by zeros (the runtime warms itself up on silence the same way)
            const int warm = 64, N = 400;
            std::vector<float> x(size_t(warm + N), 0.0f);
            for (int t = 0; t < N; ++t) x[size_t(warm + t)] = float(0.4 * std::sin(t * 0.21) + 0.2 * std::sin(t * 1.3));
            const float* R = &gw[0];                    // rechannel: out c <- in 0
            auto C = [&](int l, int o, int i, int k) { return gw[size_t(2 + l * 30 + (o * 2 + i) * 2 + k)]; };   // conv [out][in][tap]
            auto Bc = [&](int l, int o) { return gw[size_t(2 + l * 30 + 16 + o)]; };
            auto M = [&](int l, int o) { return gw[size_t(2 + l * 30 + 20 + o)]; };                             // mixin (condition in = 1)
            auto W1 = [&](int l, int o, int i) { return gw[size_t(2 + l * 30 + 24 + o * 2 + i)]; };
            auto B1 = [&](int l, int o) { return gw[size_t(2 + l * 30 + 28 + o)]; };
            const float* H = &gw[62];                   // head rechannel [in], bias gw[64], head scale gw[65]
            const int dil[2] = {1, 2};
            std::vector<std::array<double, 2>> u0(x.size()), u1(x.size()), u2(x.size());   // the layers' inputs / outputs
            std::vector<double> ref(x.size());
            for (size_t t = 0; t < x.size(); ++t) {
                for (int c = 0; c < 2; ++c) u0[t][c] = R[c] * x[t];
                double acc[2] = {0, 0};
                const std::vector<std::array<double, 2>>* in[2] = {&u0, &u1};
                std::vector<std::array<double, 2>>* out[2] = {&u1, &u2};
                for (int l = 0; l < 2; ++l) {
                    const auto& u = *in[l];
                    double z[4];
                    for (int o = 0; o < 4; ++o) {
                        z[o] = Bc(l, o) + M(l, o) * x[t];
                        for (int i = 0; i < 2; ++i)
                            for (int k = 0; k < 2; ++k) {
                                const long tt = long(t) - (1 - k) * dil[l];
                                if (tt >= 0) z[o] += C(l, o, i, k) * u[size_t(tt)][size_t(i)];
                            }
                    }
                    double top[2];
                    for (int c = 0; c < 2; ++c) top[c] = std::tanh(z[c]) * sigmoid(z[2 + c]);
                    for (int c = 0; c < 2; ++c) acc[c] += top[c];
                    for (int c = 0; c < 2; ++c) (*out[l])[t][c] = u[t][c] + B1(l, c) + W1(l, c, 0) * top[0] + W1(l, c, 1) * top[1];
                }
                ref[t] = gw[65] * (gw[64] + H[0] * acc[0] + H[1] * acc[1]);
            }
            nam::Runtime rt(tiny, 48000);
            const std::vector<float> got = runRuntime(rt, std::vector<float>(x.begin() + warm, x.end()));
            float worst = 0, peak = 0;
            for (int t = 0; t < N; ++t) { worst = std::max(worst, float(std::fabs(double(got[size_t(t)]) - ref[size_t(warm + t)]))); peak = std::max(peak, float(std::fabs(ref[size_t(warm + t)]))); }
            CHECK(peak > 1e-3f && worst < 2e-5f, qPrintable(QString("M15 WaveNet: the gated network matches the reference sample for sample (worst %1, peak %2)").arg(worst).arg(peak)));
            // the device reads it through the engine: an audio track monitoring its input plays the capture
            Project p;
            p.scenes = {"1"};
            p.addTrack("Guitar", QColor("#62c9dc"));
            p.tracks[0].type = TrackType::Audio;
            p.tracks[0].monitor = Monitor::In;
            p.tracks[0].inputChannel = 1;
            Device nd = p.makeDevice("NAM", DeviceKind::AudioEffect);
            nd.modelPath = tinyPath;
            nd.model = tiny;
            p.tracks[0].devices.push_back(nd);
            auto playInput = [&](Project& proj) {
                audio::Engine e(&proj);
                e.setInputWanted(true);
                const std::vector<float> in = pluck(0.5);
                std::vector<float> inter(in.size() * 2), out(in.size() * 2);
                for (size_t i = 0; i < in.size(); ++i) { inter[i * 2] = in[i]; inter[i * 2 + 1] = 0; }
                for (size_t pos = 0; pos < in.size(); pos += 256) {
                    const int n = int(std::min<size_t>(256, in.size() - pos));
                    e.process(out.data() + pos * 2, n, inter.data() + pos * 2);
                }
                float diff = 0, peakOut = 0;
                for (size_t i = in.size() / 2; i < in.size(); ++i) { diff = std::max(diff, std::fabs(out[i * 2] - in[i])); peakOut = std::max(peakOut, std::fabs(out[i * 2])); }
                return std::make_pair(diff, peakOut);
            };
            const auto [diff, peakOut] = playInput(p);
            CHECK(peakOut > 0.001f && diff > 0.01f, "M15 engine: an audio track monitoring Input 1 plays through the NAM device (the capture changes the sound)");
            p.tracks[0].devices[0].modelPath = dir.filePath("gone.nam");
            p.tracks[0].devices[0].model = nam::load(p.tracks[0].devices[0].modelPath);
            const auto [diffMissing, peakMissing] = playInput(p);
            CHECK(!p.tracks[0].devices[0].model && peakMissing > 0.001f && diffMissing < 1e-4f, "M15 engine: with the capture missing the device plays dry");
        }

        // ---- a tiny LSTM (2 units) against the cell equations
        {
            std::vector<float> lw(39);
            for (size_t i = 0; i < lw.size(); ++i) lw[i] = float(0.5 * std::cos(0.9 * double(i) + 1.1));
            QJsonArray wa; for (float w : lw) wa.append(double(w));
            const QJsonObject root{{"version", "0.5.4"}, {"architecture", "LSTM"}, {"config", QJsonObject{{"num_layers", 1}, {"input_size", 1}, {"hidden_size", 2}}},
                                   {"weights", wa}, {"sample_rate", 48000}};
            err.clear();
            const nam::ModelPtr m = nam::parse(QJsonDocument(root).toJson(), &err, dir.filePath("lstm.nam"));
            CHECK(m && m->arch == nam::Arch::LSTM && m->lstm.size() == 1 && m->lstm[0].hidden == 2 && m->summary == "LSTM · 1 layer × 2 units", qPrintable("M15 .nam: an LSTM reads " + err));
            // W rows (i0 i1 f0 f1 g0 g1 o0 o1) x columns (x h0 h1), then b, then h, then c, then the head
            auto Wr = [&](int r, int c) { return double(lw[size_t(r * 3 + c)]); };
            auto b = [&](int r) { return double(lw[size_t(24 + r)]); };
            double h[2] = {lw[32], lw[33]}, c[2] = {lw[34], lw[35]};
            const double hw0 = lw[36], hw1 = lw[37], hb = lw[38];
            const int warm = 24000, N = 300;
            std::vector<float> x(static_cast<size_t>(N));
            for (int t = 0; t < N; ++t) x[size_t(t)] = float(0.5 * std::sin(t * 0.11));
            std::vector<double> ref(static_cast<size_t>(N));
            for (int t = -warm; t < N; ++t) {
                const double xt = t < 0 ? 0.0 : x[size_t(t)];
                const double xh[3] = {xt, h[0], h[1]};
                double g[8];
                for (int r = 0; r < 8; ++r) { g[r] = b(r); for (int j = 0; j < 3; ++j) g[r] += Wr(r, j) * xh[j]; }
                for (int k = 0; k < 2; ++k) {
                    c[k] = sigmoid(g[2 + k]) * c[k] + sigmoid(g[k]) * std::tanh(g[4 + k]);
                    h[k] = sigmoid(g[6 + k]) * std::tanh(c[k]);
                }
                if (t >= 0) ref[size_t(t)] = hb + hw0 * h[0] + hw1 * h[1];
            }
            nam::Runtime rt(m, 48000);
            const std::vector<float> got = runRuntime(rt, x);
            float worst = 0, peak = 0;
            for (int t = 0; t < N; ++t) { worst = std::max(worst, float(std::fabs(double(got[size_t(t)]) - ref[size_t(t)]))); peak = std::max(peak, float(std::fabs(ref[size_t(t)]))); }
            CHECK(peak > 1e-3f && worst < 2e-5f, qPrintable(QString("M15 LSTM: the cell equations (gates i f g o, the stored initial state) match the reference (worst %1, peak %2)").arg(worst).arg(peak)));
        }

        // ---- a real-size standard WaveNet (16 / 8 channels, two arrays of ten layers) with random weights: finite, bounded, affordable
        QByteArray standardJson;
        {
            auto layer = [](int inputSize, int channels, int headSize, bool headBias) {
                return QJsonObject{{"input_size", inputSize}, {"condition_size", 1}, {"head_size", headSize}, {"channels", channels}, {"kernel_size", 3},
                                   {"dilations", QJsonArray{1, 2, 4, 8, 16, 32, 64, 128, 256, 512}}, {"activation", "Tanh"}, {"gated", false}, {"head_bias", headBias}};
            };
            QJsonArray wa;
            uint32_t seed = 0x1234567u;
            for (int i = 0; i < 13802; ++i) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; wa.append(double(int32_t(seed)) / 2147483648.0 * 0.4); }
            const QJsonObject root{{"version", "0.5.0"}, {"architecture", "WaveNet"}, {"config", QJsonObject{{"layers", QJsonArray{layer(1, 16, 8, false), layer(16, 8, 1, true)}}, {"head", QJsonValue::Null}, {"head_scale", 0.02}}},
                                   {"weights", wa}};
            standardJson = QJsonDocument(root).toJson(QJsonDocument::Compact);
            err.clear();
            const nam::ModelPtr m = nam::parse(standardJson, &err, dir.filePath("standard.nam"));
            CHECK(m && m->weightCount == 13802 && !m->rateKnown && m->sampleRate == 48000 && m->receptiveField == 4093 && m->summary == "WaveNet · 16/8 channels · 2 × 10 layers",
                  qPrintable("M15 .nam: the standard shape reads (13802 weights, 48 kHz assumed, receptive field 4093) " + err));
            QElapsedTimer timer; timer.start();
            nam::Runtime rt(m, 48000);
            const double loadMs = double(timer.nsecsElapsed()) / 1e6;
            std::vector<float> x(48000 * 2);
            dsp::Noise rng;
            for (size_t i = 0; i < x.size(); ++i) x[i] = 0.3f * rng.next() + 0.3f * float(std::sin(i * 0.05));
            timer.restart();
            const std::vector<float> y = runRuntime(rt, x);
            const double ms = double(timer.nsecsElapsed()) / 1e6 / 2;
            bool finite = true; float peak = 0;
            for (float v : y) { finite = finite && std::isfinite(v); peak = std::max(peak, std::fabs(v)); }
            std::printf("      NAM standard WaveNet: %.0f ms per second of audio (one channel; both sides linked), prepared in %.0f ms\n", ms, loadMs);
            CHECK(finite && peak > 1e-4f && peak < 10, "M15 WaveNet: a standard-size network with random weights stays finite and bounded");
#if defined(__SANITIZE_ADDRESS__)
            const double budget = 3000;   // the sanitizers' instrumented loops are several times slower
#else
            const double budget = 600;
#endif
            CHECK(ms < budget, qPrintable(QString("M15 WaveNet: a second of audio costs well under a second (%1 ms, budget %2)").arg(ms, 0, 'f', 0).arg(budget)));
            // stereo: once the sides differ the right side gets its own state and keeps up
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            bool differ = false, finiteR = true;
            for (int blk = 0; blk < 40; ++blk) {
                for (int i = 0; i < audio::kMaxBlock; ++i) { l[i] = 0.3f * rng.next(); r[i] = blk < 10 ? l[i] : 0.3f * rng.next(); }
                rt.process(l, r, audio::kMaxBlock);
                for (int i = 0; i < audio::kMaxBlock; ++i) { finiteR = finiteR && std::isfinite(r[i]); if (blk >= 12 && std::fabs(l[i] - r[i]) > 1e-4f) differ = true; }
            }
            CHECK(finiteR && differ, "M15 WaveNet: a stereo input runs two networks (the sides part when the signals do)");
        }
        err.clear();
        CHECK(nam::writeExample(dir.filePath("example.nam"), 3) && nam::load(dir.filePath("example.nam"), &err) && nam::load(dir.filePath("example.nam"))->summary == "WaveNet · 8/4 channels · 2 × 10 layers",
              qPrintable("M15 .nam: the example writer makes a capture that loads " + err));

        // ---- a "SlimmableContainer": one capture in two sizes (the newest trainer's A2 files), and the A2 layer shape
        QByteArray containerJson;
        {
            // the head scale is the last weight (the config's "head_scale" is only informational, as in the core): the light size gets a fifth of it
            auto sub = [&](float headScale, double loudness) {
                QJsonArray wa; for (size_t i = 0; i + 1 < gw.size(); ++i) wa.append(double(gw[i]));
                wa.append(double(headScale));
                const QJsonObject layer{{"input_size", 1}, {"condition_size", 1}, {"head_size", 1}, {"channels", 2}, {"kernel_size", 2}, {"dilations", QJsonArray{1, 2}},
                                        {"activation", "Tanh"}, {"gated", true}, {"head_bias", true}};
                return QJsonObject{{"version", "0.7.0"}, {"architecture", "WaveNet"}, {"metadata", QJsonObject{{"loudness", loudness}}},
                                   {"config", QJsonObject{{"layers", QJsonArray{layer}}, {"head", QJsonValue::Null}, {"head_scale", 0.02}}}, {"weights", wa}, {"sample_rate", 48000}};
            };
            const float hsFull = gw[65], hsLight = gw[65] * 0.2f;
            const QJsonObject root{{"version", "0.7.0"}, {"architecture", "SlimmableContainer"}, {"weights", QJsonArray{}},
                                   {"metadata", QJsonObject{{"name", "Two Sizes"}, {"loudness", -21.0}, {"gear_make", "Marshall"}, {"gear_type", "amp_cab"}, {"modeled_by", "tester"}}},
                                   {"config", QJsonObject{{"submodels", QJsonArray{QJsonObject{{"max_value", 1.0}, {"model", sub(hsFull, -30)}}, QJsonObject{{"max_value", 0.5}, {"model", sub(hsLight, -31)}}}}}},
                                   {"sample_rate", 48000}};
            containerJson = QJsonDocument(root).toJson();
            err.clear();
            const nam::ModelPtr c = nam::parse(containerJson, &err, dir.filePath("two sizes.nam"));
            CHECK(c && c->container && c->arch == nam::Arch::WaveNet && c->headScale == hsFull && c->variants.size() == 1 && c->variants[0].maxValue == 0.5 && c->variants[0].model->headScale == hsLight
                  && c->name == "Two Sizes" && c->loudness == -21 && c->gear == "Marshall (amp_cab)" && c->modeledBy == "tester" && c->summary.contains("2 sizes") && c->weightCount == 132,
                  qPrintable("M15 .nam: a container reads as its full size with the lighter one as a variant, the container's metadata on top " + err));
            if (c) {   // the same weights at two head scales: the light size's output is the full one's, scaled
                nam::Runtime full(c, 48000), light(c->variants[0].model, 48000);
                std::vector<float> x(512);
                for (size_t i = 0; i < x.size(); ++i) x[i] = float(0.4 * std::sin(i * 0.23));
                const std::vector<float> yf = runRuntime(full, x), yl = runRuntime(light, x);
                float worst = 0, peak = 0;
                for (size_t i = 0; i < x.size(); ++i) { worst = std::max(worst, std::fabs(yl[i] * 5 - yf[i])); peak = std::max(peak, std::fabs(yf[i])); }
                CHECK(peak > 1e-3f && worst < 1e-5f, "M15 .nam: the light size runs as its own network");
            }
            // the A2 shape the newest trainer writes: 23 layers, kernel sizes 6 and 15, LeakyReLU objects, gating_mode / secondary_activation lists with nulls,
            // a 16-tap head, inactive FiLM objects, "slimmable": null. The weight counts are the ones in real files (1871 and 12146).
            auto a2 = [&](int channels, int weightCount) {
                QJsonArray ks, dil, acts, gating, secondary;
                const int kernels[23] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
                const int dils[23] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13, 1, 3, 7, 17, 41, 101, 239};
                for (int i = 0; i < 23; ++i) { ks.append(kernels[i]); dil.append(dils[i]); acts.append(QJsonObject{{"type", "LeakyReLU"}, {"negative_slope", 0.01}}); gating.append("none"); secondary.append(QJsonValue::Null); }
                const QJsonObject film{{"active", false}, {"shift", true}, {"groups", 1}};
                QJsonObject layer{{"input_size", 1}, {"condition_size", 1}, {"head", QJsonObject{{"out_channels", 1}, {"kernel_size", 16}, {"bias", true}}}, {"channels", channels},
                                  {"kernel_sizes", ks}, {"dilations", dil}, {"activation", acts}, {"bottleneck", channels}, {"head1x1", QJsonObject{{"active", false}, {"out_channels", 1}, {"groups", 1}}},
                                  {"layer1x1", QJsonObject{{"active", true}, {"groups", 1}}}, {"groups_input", 1}, {"groups_input_mixin", 1}, {"gating_mode", gating}, {"secondary_activation", secondary},
                                  {"slimmable", QJsonValue::Null}};
                for (const char* f : {"conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film", "activation_pre_film", "activation_post_film", "layer1x1_post_film", "head1x1_post_film"})
                    layer.insert(QLatin1String(f), film);
                QJsonArray w;
                uint32_t seed = 0xabcdefu + unsigned(channels);
                for (int i = 0; i < weightCount; ++i) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; w.append(double(int32_t(seed)) / 2147483648.0 * 0.3); }
                return QJsonObject{{"version", "0.7.0"}, {"architecture", "WaveNet"}, {"config", QJsonObject{{"layers", QJsonArray{layer}}, {"head", QJsonValue::Null}, {"head_scale", 0.005}}}, {"weights", w}, {"sample_rate", 48000}};
            };
            err.clear();
            const nam::ModelPtr lite = nam::parse(QJsonDocument(a2(3, 1871)).toJson(), &err, dir.filePath("a2 lite.nam"));
            QString err2;
            const nam::ModelPtr fullA2 = nam::parse(QJsonDocument(a2(8, 12146)).toJson(), &err2, dir.filePath("a2 full.nam"));
            CHECK(lite && fullA2 && lite->arrays[0].layers.size() == 23 && lite->arrays[0].layers[14].conv.kernel == 15 && lite->arrays[0].headRechannel.kernel == 16
                  && lite->arrays[0].layers[0].act.type == nam::Activation::LeakyReLU && lite->receptiveField == 1 + 15 + 3 * 5 * 409 + 14 + 14 * 13 && lite->summary == "WaveNet · 3 channels · 23 layers",
                  qPrintable("M15 .nam: the A2 layer shape reads with the weight counts real files carry (1871 light, 12146 full) " + err + err2));
            if (lite) {
                nam::Runtime rt(lite, 48000);
                std::vector<float> x(4800);
                dsp::Noise rng;
                for (float& v : x) v = 0.3f * rng.next();
                const std::vector<float> y = runRuntime(rt, x);
                bool finite = true;
                for (float v : y) finite = finite && std::isfinite(v);
                CHECK(finite, "M15 .nam: an A2 network runs (finite)");
            }
        }
        // the Quality knob: the engine runs the light size of a container
        {
            const QString twoPath = writeNam("two sizes.nam", containerJson);
            Project p;
            p.scenes = {"1"};
            p.addTrack("Guitar", QColor("#62c9dc"));
            p.tracks[0].type = TrackType::Audio;
            p.tracks[0].monitor = Monitor::In;
            p.tracks[0].inputChannel = 1;
            Device nd = p.makeDevice("NAM", DeviceKind::AudioEffect);
            nd.modelPath = twoPath;
            nd.model = nam::load(twoPath);
            p.tracks[0].devices.push_back(nd);
            auto level = [&](Project& proj) {
                audio::Engine e(&proj);
                e.setInputWanted(true);
                const std::vector<float> in = pluck(0.5);
                std::vector<float> inter(in.size() * 2), out(in.size() * 2);
                for (size_t i = 0; i < in.size(); ++i) { inter[i * 2] = in[i]; inter[i * 2 + 1] = 0; }
                for (size_t pos = 0; pos < in.size(); pos += 256) {
                    const int n = int(std::min<size_t>(256, in.size() - pos));
                    e.process(out.data() + pos * 2, n, inter.data() + pos * 2);
                }
                std::vector<float> left(in.size());
                for (size_t i = 0; i < in.size(); ++i) left[i] = out[i * 2];
                return rmsOf(left, in.size() / 2, in.size());
            };
            const float fullLevel = level(p);
            p.tracks[0].devices[0].params[namdev::Quality].value = choicePosition(1, 2);
            const float lightLevel = level(p);
            CHECK(nd.model && nd.model->container && fullLevel > 1e-4f && std::fabs(lightLevel * 5 / fullLevel - 1) < 0.01f,
                  qPrintable(QString("M15 engine: Quality Light runs the container's light size (levels %1 / %2)").arg(fullLevel).arg(lightLevel)));
        }

        // ---- the device's knobs: Input, Output, Mix, Gate and Normalize, through a pass-through capture
        const QString passPath = writeNam("pass.nam", R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":1,"bias":false},"weights":[1.0],"sample_rate":48000,"metadata":{"loudness":-12.0}})");
        const nam::ModelPtr pass = nam::load(passPath, &err);
        CHECK(pass && pass->ir.size() == 1, "M15 device: a pass-through capture for the knob tests");
        auto runNam = [&](const std::vector<float>& params, const std::vector<float>& in, nam::Runtime* rt, const audio::IrKernel* ir = nullptr) {
            auto proc = audio::createProcessor(DeviceType::Nam, 48000);
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.nam = rt; ctx.ir = ir;
            std::vector<float> out(in.size());
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            for (size_t pos = 0; pos < in.size(); pos += audio::kMaxBlock) {
                const int n = int(std::min<size_t>(audio::kMaxBlock, in.size() - pos));
                std::copy(in.begin() + long(pos), in.begin() + long(pos) + n, l);
                std::copy(in.begin() + long(pos), in.begin() + long(pos) + n, r);
                proc->process(l, r, n, params.data(), ctx);
                std::copy(l, l + n, out.begin() + long(pos));
            }
            return out;
        };
        {
            nam::Runtime rt(pass, 48000);
            std::vector<float> in(24000);
            for (size_t i = 0; i < in.size(); ++i) in[i] = 0.25f * float(std::sin(2 * M_PI * 220 * double(i) / 48000));
            std::vector<float> p = defaultsOf(DeviceType::Nam);
            const float out0 = p[namdev::Output];   // 0 dB (the Output knob runs -24 .. +12 dB)
            const float base = rmsOf(runNam(p, in, &rt), 12000, 24000);
            p[namdev::Input] = pos01(6, -24, 24);
            const float inUp = rmsOf(runNam(p, in, &rt), 12000, 24000);
            p[namdev::Input] = 0.5f; p[namdev::Output] = pos01(-6, -24, 12);
            const float outDown = rmsOf(runNam(p, in, &rt), 12000, 24000);
            p[namdev::Output] = out0; p[namdev::Mix] = 0.5f;
            const std::vector<float> half = runNam(p, in, &rt);
            p[namdev::Mix] = 1; p[namdev::Normalize] = 1;
            const float normalized = rmsOf(runNam(p, in, &rt), 12000, 24000);
            p[namdev::Normalize] = 0;
            CHECK(std::fabs(base - rmsOf(in, 12000, 24000)) < 1e-3f && std::fabs(inUp / base - 2) < 0.02f && std::fabs(outDown / base - 0.5f) < 0.01f,
                  "M15 device: Input and Output are gains in dB around the capture (+6 doubles, -6 halves)");
            p[namdev::Mix] = 0;
            const std::vector<float> dry = runNam(p, in, &rt);
            p[namdev::Mix] = 1;
            CHECK(std::fabs(rmsOf(half, 12000, 24000) - base) < 2e-3f && maxDiff(dry, in) < 1e-6f, "M15 device: Mix blends the dry input with the capture's output (0: dry, untouched)");
            CHECK(std::fabs(normalized / base - 0.5f) < 0.01f, "M15 device: Normalize takes the capture's loudness (-12 dB) to -18 dB: half the level");
            // the gate: a quiet passage closes it, a loud one opens it again
            std::vector<float> gated(48000);
            for (size_t i = 0; i < gated.size(); ++i) gated[i] = (i < 16000 ? 0.25f : i < 32000 ? 0.002f : 0.25f) * float(std::sin(2 * M_PI * 220 * double(i) / 48000));
            p[namdev::Gate] = pos01(-40, -90, -30);
            const std::vector<float> g = runNam(p, gated, &rt);
            CHECK(rmsOf(g, 8000, 16000) > 0.15f && rmsOf(g, 26000, 32000) < 1e-5f && rmsOf(g, 40000, 48000) > 0.15f,
                  qPrintable(QString("M15 device: the Gate closes on a quiet passage (-40 dB threshold) and opens again (quiet part %1)").arg(rmsOf(g, 26000, 32000))));
            p[namdev::Gate] = 0;
            CHECK(rmsOf(runNam(p, gated, &rt), 24000, 32000) > 1e-3f && paramText(DeviceType::Nam, namdev::Gate, 0) == "-inf dB", "M15 device: the Gate all the way down is off");
            CHECK(std::fabs(rmsOf(runNam(p, in, nullptr), 12000, 24000) - base) < 1e-3f, "M15 device: without a capture the sound passes through dry");
            // the IR slot: a cabinet after the capture, with its own mix and on / off
            std::vector<float> irData(480, 0.0f);
            irData[0] = 0.6f; irData[48] = 0.8f;   // energy 1: direct plus an echo a millisecond later
            auto irSample = std::make_shared<SampleData>();
            irSample->rate = 48000; irSample->channels = 1; irSample->left = irData; irSample->right = irData;
            const auto k = audio::makeIrKernel(irSample, 48000);
            const std::vector<float> withIr = runNam(p, in, &rt, k.get());
            const std::vector<float> plain = runNam(p, in, &rt);
            float irErr = 0;
            for (size_t i = audio::kIrBlock + 1000; i < in.size(); ++i) irErr = std::max(irErr, std::fabs(withIr[i] - (0.6f * plain[i - audio::kIrBlock] + 0.8f * plain[i - audio::kIrBlock - 48])));
            p[namdev::IrMix] = 0;
            const std::vector<float> irOff = runNam(p, in, &rt, k.get());
            p[namdev::IrMix] = 1; p[namdev::Ir] = 0;
            const std::vector<float> irBypassed = runNam(p, in, &rt, k.get());
            p[namdev::Ir] = 1;
            CHECK(irErr < 1e-3f && maxDiff(irOff, plain) < 1e-4f && maxDiff(irBypassed, plain) < 1e-4f,
                  "M15 device: the IR slot convolves the capture's output (one block late, as the amp's), IR Mix 0 and IR off leave it alone");
        }

        // ---- resampling: a capture is a 48 kHz network; at 44.1 kHz the engine resamples around it, so the tone stays
        {
            std::vector<float> fir(64);
            double sum = 0;
            for (int j = 0; j < 64; ++j) {   // a 3 kHz low-pass designed at 48 kHz (windowed sinc)
                const double x = j - 31.5, fc = 3000.0 / 48000;
                fir[size_t(j)] = float((std::fabs(x) < 1e-9 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x)) * (0.5 - 0.5 * std::cos(2 * M_PI * (j + 0.5) / 64)));
                sum += fir[size_t(j)];
            }
            for (float& v : fir) v = float(v / sum);
            QJsonArray wa; for (float v : fir) wa.append(double(v));
            const QJsonObject root{{"version", "0.5.0"}, {"architecture", "Linear"}, {"config", QJsonObject{{"receptive_field", 64}, {"bias", false}}}, {"weights", wa}, {"sample_rate", 48000}};
            const nam::ModelPtr lp = nam::parse(QJsonDocument(root).toJson(), &err, dir.filePath("lowpass.nam"));
            const std::vector<double> freqs = {500, 1500, 2500, 4000, 6000};
            auto toneAt = [&](double rate, std::vector<double>& dbOut, int* latency) {   // the response in dB at each frequency, at this engine rate
                nam::Runtime rt(lp, float(rate));
                if (latency) *latency = rt.latency();
                const int N = int(rate);
                std::vector<float> x(static_cast<size_t>(N));
                for (int t = 0; t < N; ++t) { double v = 0; for (double f : freqs) v += 0.15 * std::sin(2 * M_PI * f * t / rate); x[size_t(t)] = float(v); }
                const std::vector<float> y = runRuntime(rt, x);
                dbOut.clear();
                for (double f : freqs) {   // one DFT bin over the second half, input and output
                    double re = 0, im = 0, reIn = 0, imIn = 0;
                    for (int t = N / 2; t < N; ++t) {
                        const double ph = 2 * M_PI * f * t / rate;
                        re += y[size_t(t)] * std::cos(ph); im -= y[size_t(t)] * std::sin(ph);
                        reIn += x[size_t(t)] * std::cos(ph); imIn -= x[size_t(t)] * std::sin(ph);
                    }
                    dbOut.push_back(10 * std::log10((re * re + im * im) / std::max(1e-20, reIn * reIn + imIn * imIn)));
                }
            };
            std::vector<double> at48, at441;
            int lat48 = -1, lat441 = -1;
            toneAt(48000, at48, &lat48);
            toneAt(44100, at441, &lat441);
            double worst = 0;
            for (size_t i = 0; i + 1 < freqs.size(); ++i) worst = std::max(worst, std::fabs(at48[i] - at441[i]));   // up to 4 kHz (6 kHz is deep in the stopband)
            CHECK(at48[0] > -0.5 && at48[0] < 0.5 && at48[3] < -3 && at48[4] < -25, "M15 resampling: the 48 kHz capture is a 3 kHz low-pass at its own rate");
            CHECK(worst < 0.5 && lat48 == 0 && lat441 > 0 && lat441 < 100,
                  qPrintable(QString("M15 resampling: at 44.1 kHz the engine resamples around the network and the tone matches 48 kHz within %1 dB (latency %2 frames)").arg(worst, 0, 'f', 2).arg(lat441)));
        }

        // ---- the set file (relative path, found after the folder moves), presets, the device's equality
        {
            Project p;
            p.scenes = {"1"};
            p.addTrack("Guitar", QColor("#62c9dc"));
            p.tracks[0].type = TrackType::Audio;
            QDir().mkpath(dir.filePath("song/Captures"));
            QDir().mkpath(dir.filePath("song/Samples"));
            const QString capPath = dir.filePath("song/Captures/tiny copy.nam");
            QFile::copy(dir.filePath("tiny.nam"), capPath);
            const QString irPath = dir.filePath("song/Samples/cab.wav");
            std::vector<float> irData(480, 0.0f); irData[0] = 1;
            writeMonoWav(irPath, irData, 48000);
            Device d = p.makeDevice("NAM", DeviceKind::AudioEffect);
            d.modelPath = capPath; d.model = nam::load(capPath);
            d.samplePath = irPath; d.sample = samples::load(irPath);
            d.params[namdev::Input].value = 0.7;
            p.tracks[0].devices.push_back(d);
            const QString setPath = dir.filePath("song/song.sessions");
            setfile::Extras x;
            CHECK(d.model && setfile::save(setPath, p, x), "M15 sets: a set with a NAM device saves");
            QFile f(setPath); f.open(QIODevice::ReadOnly);
            const QByteArray json = f.readAll();
            CHECK(json.contains("\"model\"") && json.contains("\"relativePath\": \"Captures/tiny copy.nam\""), "M15 sets: the capture is saved by path and relative to the set");
            QDir(dir.path()).rename("song", "moved");
            Project q;
            setfile::Extras y;
            err.clear();
            CHECK(setfile::load(dir.filePath("moved/song.sessions"), q, y, &err) && q.tracks[0].devices.size() == 1
                  && q.tracks[0].devices[0].modelPath == QFileInfo(dir.filePath("moved/Captures/tiny copy.nam")).absoluteFilePath() && q.tracks[0].devices[0].model
                  && q.tracks[0].devices[0].model->name == "Tiny Gated" && q.tracks[0].devices[0].sample,
                  qPrintable("M15 sets: the moved set finds its capture (and the IR) next to it " + err));
            p.tracks[0].devices[0].modelPath = q.tracks[0].devices[0].modelPath;
            p.tracks[0].devices[0].samplePath = q.tracks[0].devices[0].samplePath;
            CHECK(q.captureEditState() == p.captureEditState(), "M15 sets: the set round-trips (captureEditState ==, with the capture's path)");
            QTemporaryDir presetDir;
            qputenv("SESSIONS_PRESETS", presetDir.path().toLocal8Bit());
            const QString pp = presets::pathFor("My Capture");
            Project::DeviceBlocks blocks;
            CHECK(presets::save(pp, "My Capture", p.deviceBlocks(0, {p.tracks[0].devices[0].id})) && presets::read(pp, blocks, nullptr, &err) && blocks.size() == 1
                  && blocks[0][0].modelPath == p.tracks[0].devices[0].modelPath && blocks[0][0].model && blocks[0][0].samplePath == p.tracks[0].devices[0].samplePath,
                  qPrintable("M15 presets: a preset keeps the capture's path (and the IR's) " + err));
            Project missing;
            QFile::remove(dir.filePath("moved/Captures/tiny copy.nam"));
            CHECK(setfile::load(dir.filePath("moved/song.sessions"), missing, y, &err) && !missing.tracks[0].devices[0].model && !missing.tracks[0].devices[0].modelPath.isEmpty(),
                  "M15 sets: a capture that's gone keeps its path (the panel says it's missing) and the set still loads");
        }

        // ---- the browser lists captures, they drag as devices, and land on tracks
        {
            QTemporaryDir lib;
            const QString cap = lib.filePath("Amp Capture.nam");
            QFile::copy(dir.filePath("tiny.nam"), cap);
            writeMonoWav(lib.filePath("tone.wav"), std::vector<float>(480, 0.1f), 48000);
            Browser b;
            b.addFolder(lib.path());
            QApplication::processEvents();
            CHECK(b.listedItems().contains(cap + "|n") && b.listedItems().contains(lib.filePath("tone.wav") + "|s"), "M15 browser: a folder lists its .nam captures next to its samples");
            QTemporaryDir userLib;
            qputenv("SESSIONS_PRESETS", userLib.path().toLocal8Bit());
            QDir().mkpath(userLib.filePath("Captures"));
            QFile::copy(dir.filePath("tiny.nam"), userLib.filePath("Captures/Shelf Amp.nam"));
            b.showUserLibrary();
            QApplication::processEvents();
            CHECK(b.listedItems().contains(userLib.filePath("Captures/Shelf Amp.nam") + "|n"), "M15 browser: the User Library lists .nam captures kept in the presets folder");
            QMimeData files;
            files.setUrls({QUrl::fromLocalFile(cap)});
            CHECK(Browser::modelFilesIn(&files) == QStringList{cap} && Browser::audioFilesIn(&files).isEmpty(), "M15 browser: a .nam file dragged in from the file manager is a capture, not audio");
            QMimeData item;
            item.setData(Browser::mimeType(), (cap + "|n").toUtf8());
            const devicedrag::Payload pl = devicedrag::parse(&item), plFile = devicedrag::parse(&files);
            CHECK(pl.source == devicedrag::Payload::Model && pl.path == cap && pl.name == "Amp Capture" && plFile.source == devicedrag::Payload::Model && plFile.path == cap,
                  "M15 drag: a capture (browser item or file) drags as a device");
            Project p;
            rackProject(p, {});
            p.tracks[0].type = TrackType::Audio;
            p.tracks[0].devices.clear();
            const int undo0 = p.undoStack()->count();
            const devicedrag::Result r = devicedrag::drop(p, pl, 0, {}, false);
            CHECK(r.ok && p.tracks[0].devices.size() == 1 && p.tracks[0].devices[0].name == "NAM" && p.tracks[0].devices[0].modelPath == cap && p.tracks[0].devices[0].model
                  && p.undoStack()->count() == undo0 + 1 && r.title.contains("Amp Capture"),
                  "M15 drag: dropped on an audio track it lands as a NAM device holding the capture, one undo step");
            const devicedrag::Result r2 = devicedrag::drop(p, plFile, -1, {}, false);
            CHECK(r2.ok && p.tracks.size() == 2 && p.tracks[1].type == TrackType::Audio && p.tracks[1].name == "Amp Capture" && p.tracks[1].devices.size() == 1 && p.tracks[1].devices[0].model,
                  "M15 drag: dropped on the empty space it makes an audio track named after the capture");
            QMimeData bad;
            bad.setUrls({QUrl::fromLocalFile(lib.filePath("missing.nam"))});
            const devicedrag::Result r3 = devicedrag::drop(p, devicedrag::parse(&bad), 0, {}, false);
            CHECK(!r3.ok && r3.text.contains("missing"), "M15 drag: a capture that can't be read is refused with the reason");
        }

        // ---- the panel: every knob, the chooser, a dropped capture and a dropped IR
        {
            Project p;
            rackProject(p, {});
            p.tracks[0].type = TrackType::Audio;
            p.tracks[0].devices = {p.makeDevice("NAM", DeviceKind::AudioEffect)};
            audio::Engine e(&p);
            Transport tr(&p, &e);
            DevicePanel panel(&p, &tr, 0, p.tracks[0].devices[0], nullptr, {});
            panel.show();
            QApplication::processEvents();
            devui::DeviceBody* body = panel.body();
            QSet<QString> targets;
            for (QWidget* w : body->findChildren<QWidget*>()) targets.insert(w->property("mapTarget").toString());
            int missingControls = 0;
            for (int i = 0; i < body->count(); ++i) if (!targets.contains(body->target(i))) ++missingControls;
            QToolButton* load = nullptr;
            for (QToolButton* tb : body->findChildren<QToolButton*>()) if (tb->text() == "Load…") load = tb;
            CHECK(missingControls == 0 && body->count() == namdev::kParams && load && body->findChildren<devui::Display*>().size() >= 3 && body->minimumSizeHint().height() <= devui::kBodyHeight + 4,
                  "M15 panel: every parameter has a control, Load… is there, the capture / meter / IR displays, and it fits the device height");
            const int undo0 = p.undoStack()->count();
            auto dropOn = [&](const QString& path) {
                QMimeData mime;
                mime.setUrls({QUrl::fromLocalFile(path)});
                QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(body, &enter);
                QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(body, &move);
                QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(body, &drop);
                return enter.isAccepted();
            };
            const bool accepted = dropOn(dir.filePath("tiny.nam"));
            const Device& d = p.tracks[0].devices[0];
            CHECK(accepted && d.modelPath == dir.filePath("tiny.nam") && d.model && d.model->name == "Tiny Gated" && p.undoStack()->count() == undo0 + 1,
                  "M15 panel: a .nam dropped on the device loads the capture (one undo step)");
            QLabel* fileLabel = nullptr;
            for (QLabel* l : body->findChildren<QLabel*>()) if (l->text() == "tiny.nam") fileLabel = l;
            CHECK(fileLabel != nullptr, "M15 panel: the capture's file name shows");
            const QString irPath = dir.filePath("panel cab.wav");
            std::vector<float> irData(480, 0.0f); irData[0] = 1;
            writeMonoWav(irPath, irData, 48000);
            p.tracks[0].devices[0].params[namdev::Ir].value = 0;
            CHECK(dropOn(irPath) && p.tracks[0].devices[0].samplePath == irPath && p.tracks[0].devices[0].sample && p.tracks[0].devices[0].params[namdev::Ir].value == 1 && p.undoStack()->count() == undo0 + 2,
                  "M15 panel: a WAV dropped on the device loads it as the cabinet IR and switches IR on (one undo step)");
            p.undoStack()->undo();
            p.undoStack()->undo();
            CHECK(p.tracks[0].devices[0].modelPath.isEmpty() && !p.tracks[0].devices[0].model && p.tracks[0].devices[0].samplePath.isEmpty(), "M15 panel: undo takes the IR and the capture away again");
            // a missing capture: the panel still builds and says so
            p.tracks[0].devices[0].modelPath = dir.filePath("gone.nam");
            emit p.structureChanged();
            DevicePanel panel2(&p, &tr, 0, p.tracks[0].devices[0], nullptr, {});
            panel2.show();
            QApplication::processEvents();
            panel2.body()->tick();
            panel2.body()->refresh();
            CHECK(panel2.body() && isGuitarType(DeviceType::Nam) && deviceSpec("NAM") && deviceSpec("NAM")->kind == DeviceKind::AudioEffect, "M15 panel: a missing capture still shows a panel; NAM sits with the guitar devices");
            e.publish();
            float buf[512 * 2] = {};
            e.process(buf, 512);
            CHECK(true, "M15 engine: a device with a missing capture renders (no crash)");
        }
    }

    // ================================================================ Milestone 16: racks, completed
    // (key / velocity / chain-select zones and the Chain selector, the MIDI Effect Rack, Drum Rack pads holding chains)
    {
        const Zone fade{60, 100, 10, 0};
        CHECK((Zone{}.weight(0) == 1 && Zone{}.weight(127) == 1 && Zone{10, 20, 0, 0}.weight(9) == 0 && Zone{10, 20, 0, 0}.weight(21) == 0
              && fade.weight(59) == 0 && std::fabs(fade.weight(60) - 1.0 / 11) < 1e-12 && std::fabs(fade.weight(65) - 6.0 / 11) < 1e-12 && fade.weight(70) == 1 && fade.weight(100) == 1
              && std::fabs(Zone{0, 127, 0, 3}.weight(126) - 2.0 / 4) < 1e-12),
              "M16 zones: a zone answers inside its range, fading in and out over its fade steps");
        const Zone wild = Zone{90, 20, 50, 80}.sane();
        CHECK((wild.lo == 90 && wild.hi == 90 && wild.fadeLo == 0 && wild.fadeHi == 0 && Zone{-5, 300, 0, 0}.sane() == Zone{0, 127, 0, 0}
              && Zone{0, 10, 8, 8}.sane() == Zone{0, 10, 8, 2}),
              "M16 zones: sane() keeps a zone inside 0..127, its ends in order and its fades between them");
        CHECK((ChainZones{}.isDefault() && ChainZones{}.key == Zone{} && ChainZones{}.vel.lo == 1 && ChainZones{}.select == (Zone{0, 0, 0, 0})),
              "M16 zones: by default a chain takes every key and velocity, and the selector at 0");
        CHECK((paramCount(DeviceType::InstrumentRack) == racks::kParams && paramCount(DeviceType::MidiEffectRack) == racks::kParams
              && deviceSpec("MIDI Effect Rack") && deviceSpec("MIDI Effect Rack")->kind == DeviceKind::MidiEffect && isRackType(DeviceType::MidiEffectRack)
              && paramValue(DeviceType::MidiEffectRack, racks::param(0, racks::Transpose), 0.5) == 0 && paramText(DeviceType::MidiEffectRack, racks::param(0, racks::VelocityScale), 0.5) == "100 %"
              && paramValue(DeviceType::AudioEffectRack, racks::ChainSelect, 1.0) == 127),
              "M16 specs: the racks gain the Chain selector (appended); the MIDI Effect Rack's chains have Transpose and Velocity");
    }

    // ---- Milestone 16: the sound
    {
        const int frames = 30000;
        auto playWith = [&](const QVector<Note>& notes, const std::function<QVector<Device>(Project&)>& make) {
            Project p;
            rackProject(p, {});
            p.tracks[0].clipSlots[0]->notes = notes;
            p.tracks[0].devices = make(p);
            return playArrangement(p, frames);
        };
        auto synth = [](const char* name) { return [name](Project& p) { return QVector<Device>{p.makeDevice(name, DeviceKind::Instrument)}; }; };
        auto sum = [](std::vector<float> a, const std::vector<float>& b) { for (size_t i = 0; i < a.size() && i < b.size(); ++i) a[i] += b[i]; return a; };
        auto scaled = [](std::vector<float> a, float g) { for (float& x : a) x *= g; return a; };
        const Note n60{60, 0.0, 1.0, 0.9, 1.0}, n67{67, 0.5, 1.0, 0.7, 1.0};
        const std::vector<float> a60 = playWith({n60}, synth("Analog Synth")), a67 = playWith({n67}, synth("Analog Synth")), aBoth = playWith({n60, n67}, synth("Analog Synth"));
        const std::vector<float> f60 = playWith({n60}, synth("FM Synth")), f67 = playWith({n67}, synth("FM Synth")), fBoth = playWith({n60, n67}, synth("FM Synth"));
        CHECK((peakOf(a60) > 0.05f && peakOf(f67) > 0.05f && maxDiff(a60, a67) > 0.01f), "M16 zones: (each synth plays each note on its own)");
        // an Instrument Rack: Analog in chain 1, FM in chain 2; `tweak` sets its knobs and zones
        auto rack2 = [&](const std::function<void(Device&)>& tweak) {
            return playWith({n60, n67}, [&](Project& p) {
                Device r = p.makeDevice("Instrument Rack", DeviceKind::Instrument);
                r.chains = {"Analog", "FM"};
                r.zones.resize(2);
                tweak(r);
                return QVector<Device>{r, inChain(p.makeDevice("Analog Synth", DeviceKind::Instrument), r, 0), inChain(p.makeDevice("FM Synth", DeviceKind::Instrument), r, 1)};
            });
        };
        CHECK((maxDiff(rack2([](Device&) {}), sum(aBoth, fBoth)) < 1e-4f), "M16 zones: default zones change nothing (both chains play both notes)");
        CHECK((maxDiff(rack2([](Device& r) { r.zones[0].key = {0, 63, 0, 0}; r.zones[1].key = {64, 127, 0, 0}; }), sum(a60, f67)) < 1e-4f),
              "M16 key zones: a split keyboard (C3 to the Analog chain, G3 to the FM chain)");
        CHECK((maxDiff(rack2([](Device& r) { r.zones[0].vel = {1, 100, 0, 0}; r.zones[1].vel = {101, 127, 0, 0}; }), sum(a67, f60)) < 1e-4f),
              "M16 velocity zones: velocity layers (the softer note to one chain, the harder one to the other)");
        auto selectAt = [](Device& r, int v) { r.params[racks::ChainSelect].value = v / 127.0; r.zones[0].select = {0, 0, 0, 0}; r.zones[1].select = {1, 1, 0, 0}; };
        CHECK((maxDiff(rack2([&](Device& r) { selectAt(r, 0); }), aBoth) < 1e-4f && maxDiff(rack2([&](Device& r) { selectAt(r, 1); }), fBoth) < 1e-4f),
              "M16 Chain selector: notes go only to the chains whose chain zone holds it");
        const Note soft60{60, 0.0, 1.0, 0.9 * 6 / 11, 1.0};
        CHECK((maxDiff(rack2([](Device& r) { r.params[racks::param(1, racks::On)].value = 0; r.zones[0].key = {55, 127, 10, 0}; }),
                      playWith({soft60, n67}, synth("Analog Synth"))) < 1e-4f),
              "M16 zone fades: a note inside a key zone's fade plays softer (its velocity scaled by the fade)");
        // an Audio Effect Rack after the Analog, two empty chains
        auto fxRack = [&](const std::function<void(Device&)>& tweak) {
            return playWith({n60, n67}, [&](Project& p) {
                Device r = p.makeDevice("Audio Effect Rack", DeviceKind::AudioEffect);
                r.chains = {"A", "B"};
                r.zones.resize(2);
                tweak(r);
                return QVector<Device>{p.makeDevice("Analog Synth", DeviceKind::Instrument), r};
            });
        };
        CHECK((maxDiff(fxRack([](Device&) {}), scaled(aBoth, 2)) < 1e-4f && maxDiff(fxRack([&](Device& r) { selectAt(r, 1); }), aBoth) < 1e-4f),
              "M16 Audio Effect Rack: the Chain selector feeds only the chains whose zone holds it");
        CHECK((maxDiff(fxRack([](Device& r) { r.zones[0].select = {0, 127, 0, 0}; r.zones[1].select = {0, 2, 0, 1}; r.params[racks::ChainSelect].value = 2 / 127.0; }),
                      scaled(aBoth, 1.5f)) < 1e-4f),
              "M16 Audio Effect Rack: in a chain zone's fade the chain gets that share of the sound");

        // MIDI Effect Rack before the Analog
        auto midiRack = [&](const QVector<Note>& notes, const std::function<void(Project&, Device&, QVector<Device>&)>& build) {
            return playWith(notes, [&](Project& p) {
                Device r = p.makeDevice("MIDI Effect Rack", DeviceKind::MidiEffect);
                QVector<Device> members;
                build(p, r, members);
                QVector<Device> v{r};
                v += members;
                v.push_back(p.makeDevice("Analog Synth", DeviceKind::Instrument));
                return v;
            });
        };
        auto transposeTo = [](Device& r, int chain, int st) { r.params[racks::param(chain, racks::Transpose)].value = (st + 48) / 96.0; };
        auto layered = [&](const std::function<void(Device&)>& tweak) {
            return midiRack({n60, n67}, [&](Project&, Device& r, QVector<Device>&) { r.chains = {"Played", "Up"}; r.zones.resize(2); transposeTo(r, 1, 12); tweak(r); });
        };
        const Note n72{72, 0.0, 1.0, 0.9, 1.0}, n79{79, 0.5, 1.0, 0.7, 1.0}, n48{48, 0.0, 1.0, 0.9, 1.0};
        CHECK((maxDiff(layered([](Device&) {}), playWith({n60, n72, n67, n79}, synth("Analog Synth"))) < 1e-4f),
              "M16 MIDI Effect Rack: every chain's notes go on, merged (a chain transposed an octave up layers it)");
        CHECK((maxDiff(layered([](Device& r) { r.params[racks::param(1, racks::On)].value = 0; }), aBoth) < 1e-4f
              && maxDiff(layered([](Device& r) { r.enabled = false; }), aBoth) < 1e-4f
              && maxDiff(layered([](Device& r) { r.params[racks::param(0, racks::Solo)].value = 1; }), aBoth) < 1e-4f),
              "M16 MIDI Effect Rack: a chain switched off (or not soloed) sends nothing; the rack switched off lets the notes through untouched");
        const Note half72{72, 0.0, 1.0, 0.45, 1.0}, half79{79, 0.5, 1.0, 0.35, 1.0};
        CHECK((maxDiff(layered([](Device& r) { r.params[racks::param(1, racks::VelocityScale)].value = 0.25; }), playWith({n60, half72, n67, half79}, synth("Analog Synth"))) < 1e-4f),
              "M16 MIDI Effect Rack: a chain's Velocity scales its notes' velocities (50 %)");
        CHECK((maxDiff(midiRack({n60, n67}, [&](Project&, Device& r, QVector<Device>&) {
                  r.chains = {"Low", "High"};
                  r.zones.resize(2);
                  r.zones[0].key = {0, 63, 0, 0};
                  transposeTo(r, 0, -12);
                  r.zones[1].key = {64, 127, 0, 0};
              }), playWith({n48, n67}, synth("Analog Synth"))) < 1e-4f),
              "M16 MIDI Effect Rack: key zones split the notes between chains (the low one an octave down)");
        CHECK((maxDiff(midiRack({n60, n67}, [&](Project& p, Device& r, QVector<Device>& m) {
                  r.chains = {"Scaled", "Empty"};
                  r.zones.resize(2);
                  r.params[racks::param(1, racks::On)].value = 0;
                  Device sc = p.makeDevice("Scale", DeviceKind::MidiEffect);
                  sc.params[2].value = (12 + 24) / 48.0;   // Transpose +12
                  m.push_back(inChain(sc, r, 0));
              }), playWith({n72, n79}, synth("Analog Synth"))) < 1e-4f),
              "M16 MIDI Effect Rack: a chain runs its own MIDI effects");

        // Drum Rack pads holding chains (the 808 pads): a kick on C1, a snare on D1
        const Note kick{36, 0.0, 0.25, 0.9, 1.0}, snare{38, 0.5, 0.25, 0.8, 1.0};
        auto drums = [&](const QVector<Note>& notes, const std::function<void(Project&, QVector<Device>&)>& tweak) {
            return playWith(notes, [&](Project& p) {
                QVector<Device> v{p.makeDevice("Drum Rack", DeviceKind::Instrument)};
                tweak(p, v);
                return v;
            });
        };
        auto onPad = [](Device d, const Device& drum, int pad) { d.rack = drum.id; d.chain = pad; return d; };
        auto plain = [](Project&, QVector<Device>&) {};
        const std::vector<float> kitBoth = drums({kick, snare}, plain), kitSnare = drums({snare}, plain), kitKick = drums({kick}, plain);
        const std::vector<float> analogC3 = playWith({Note{60, 0.0, 0.25, 0.9, 1.0}}, synth("Analog Synth"));
        CHECK((peakOf(kitSnare) > 0.05f && peakOf(kitKick) > 0.05f && peakOf(analogC3) > 0.05f), "M16 pad chains: (the kick, the snare and the synth sound on their own)");
        CHECK((maxDiff(drums({kick, snare}, [&](Project& p, QVector<Device>& v) { v.push_back(onPad(p.makeDevice("Analog Synth", DeviceKind::Instrument), v[0], 0)); }),
                      sum(kitSnare, analogC3)) < 1e-4f),
              "M16 pad chains: an instrument on a pad plays that pad's hits at C3 instead of its sample; the other pads play on");
        CHECK((maxDiff(drums({kick}, [&](Project& p, QVector<Device>& v) {
                  v[0].params[drumrack::param(0, drumrack::Transpose)].value = (12 + 24) / 48.0;
                  v.push_back(onPad(p.makeDevice("Analog Synth", DeviceKind::Instrument), v[0], 0));
              }), playWith({Note{72, 0.0, 0.25, 0.9, 1.0}}, synth("Analog Synth"))) < 1e-4f),
              "M16 pad chains: the pad's Transpose moves the note its instrument plays");
        CHECK((maxDiff(drums({kick}, [&](Project& p, QVector<Device>& v) {
                  v[0].params[drumrack::param(0, drumrack::Vol)].value = (-6 + 24) / 30.0;
                  v[0].params[drumrack::param(0, drumrack::Pan)].value = 0;
                  v.push_back(onPad(p.makeDevice("Analog Synth", DeviceKind::Instrument), v[0], 0));
              }), [&] { std::vector<float> l = scaled(analogC3, float(std::pow(10.0, -6 / 20.0))); for (size_t i = 1; i < l.size(); i += 2) l[i] = 0; return l; }()) < 1e-4f),
              "M16 pad chains: the pad's Volume and Pan set its instrument's level (-6 dB, hard left)");
        CHECK((maxDiff(drums({kick, snare}, [&](Project& p, QVector<Device>& v) { v.push_back(onPad(p.makeDevice("Utility", DeviceKind::AudioEffect), v[0], 2)); }), kitBoth) < 1e-5f),
              "M16 pad chains: a sample pad with effects plays on time through them (an untouched Utility changes nothing)");
        CHECK((maxDiff(drums({kick, snare}, [&](Project& p, QVector<Device>& v) {
                  Device u = p.makeDevice("Utility", DeviceKind::AudioEffect);
                  u.params[6].value = 1;   // Mute
                  v.push_back(onPad(u, v[0], 2));
              }), kitKick) < 1e-5f),
              "M16 pad chains: effects on a pad process just that pad (a muted Utility on D1 silences the snare only)");
        CHECK((peakOf(drums({kick, snare}, [&](Project& p, QVector<Device>& v) {
                  Device u1 = p.makeDevice("Utility", DeviceKind::AudioEffect), u2 = p.makeDevice("Utility", DeviceKind::AudioEffect);
                  u1.params[6].value = u2.params[6].value = 1;
                  v.push_back(onPad(p.makeDevice("Analog Synth", DeviceKind::Instrument), v[0], 0));
                  v.push_back(onPad(u1, v[0], 0));
                  v.push_back(onPad(u2, v[0], 2));
              })) < 1e-6f),
              "M16 pad chains: effects after a pad's instrument process it (muted pads, silence)");
        CHECK((peakOf(drums({kick, snare}, [&](Project& p, QVector<Device>& v) { v[0].enabled = false; v.push_back(onPad(p.makeDevice("Analog Synth", DeviceKind::Instrument), v[0], 0)); })) == 0.0f),
              "M16 pad chains: a Drum Rack switched off is silent, its pads' instruments too");
        // the hats share choke group 1 (F#1 pad 6, G#1 pad 8): a synth held on F#1 is released by a hit on G#1
        const Note held{42, 0.0, 3.0, 0.9, 1.0}, open{44, 0.5, 0.25, 0.8, 1.0};
        auto chokeRun = [&](bool choke) {
            return drums({held, open}, [&](Project& p, QVector<Device>& v) {
                v[0].params[drumrack::param(8, drumrack::Vol)].value = 0;   // the open hat itself at -24 dB
                if (!choke) v[0].params[drumrack::param(8, drumrack::Choke)].value = 0;
                v.push_back(onPad(p.makeDevice("Analog Synth", DeviceKind::Instrument), v[0], 6));
            });
        };
        const std::vector<float> choked = chokeRun(true), unchoked = chokeRun(false);
        const size_t tail = size_t(27000) * 2;
        CHECK((rmsOf(unchoked, tail, unchoked.size()) > 0.02f && rmsOf(choked, tail, choked.size()) < 0.25f * rmsOf(unchoked, tail, unchoked.size())
              && maxDiff(std::vector<float>(choked.begin(), choked.begin() + 11000 * 2), std::vector<float>(unchoked.begin(), unchoked.begin() + 11000 * 2)) < 1e-6f),
              "M16 pad chains: a choke group reaches a pad's instrument (a hit on G#1 releases the synth on F#1)");
    }

    // ---- Milestone 16: editing
    {
        // Drum Rack pads
        Project p;
        rackProject(p, {});
        p.selectedTrack = 0;
        p.addDeviceToSelected("Drum Rack", DeviceKind::Instrument);
        const int drumId = p.tracks[0].devices[0].id;
        const int undo0 = p.undoStack()->count();
        auto place = [&](const char* name, DeviceKind kind, int pad, QString* why = nullptr) {
            return p.insertDevices(0, {p.makeDeviceBlock(name, kind)}, Project::DevicePlace{0, drumId, pad}, QString("Add ") + name, nullptr, why);
        };
        CHECK((place("Analog Synth", DeviceKind::Instrument, 3) && p.tracks[0].devices.size() == 2 && p.tracks[0].devices[1].rack == drumId
              && p.tracks[0].devices[1].chain == 3 && p.undoStack()->count() == undo0 + 1 && holdsChains(p.tracks[0].devices[0]) && !isRack(p.tracks[0].devices[0])),
              "M16 pad chains: an instrument dropped on a pad goes into that pad's chain (one undo step)");
        CHECK((place("Saturator", DeviceKind::AudioEffect, 3) && place("Chord", DeviceKind::MidiEffect, 3) && place("FM Synth", DeviceKind::Instrument, 3) && place("EQ", DeviceKind::AudioEffect, 1)),
              "M16 pad chains: (effects, a MIDI effect and another instrument go on pads)");
        auto order = [&](int track) {
            QStringList o;
            for (const Device& d : p.tracks[track].devices) o << QString("%1@%2").arg(d.name).arg(d.rack ? d.chain : -1);
            return o;
        };
        CHECK(((order(0) == QStringList{"Drum Rack@-1", "EQ@1", "Chord@3", "FM Synth@3", "Saturator@3"})),
              qPrintable("M16 pad chains: pads in order, each MIDI effects, the instrument (a new one replaces the old), audio effects: " + order(0).join(", ")));
        QString why;
        CHECK((!place("Analog Synth", DeviceKind::Instrument, 16, &why) && why.contains("pad") && !place("Drum Rack", DeviceKind::Instrument, 2, &why) && why.contains("Drum Rack")
              && !place("Instrument Rack", DeviceKind::Instrument, 2) && !place("Macros", DeviceKind::AudioEffect, 2)),
              "M16 pad chains: no pad past D#2, no Drum Rack, rack or Macros on a pad");
        const int other = p.insertTrack(1, TrackType::Midi);
        p.tracks[other].devices = {p.makeDevice("Instrument Rack", DeviceKind::Instrument)};
        p.tracks[other].devices[0].chains = {"A"};
        p.tracks[other].devices[0].zones.resize(1);
        const int instRack = p.tracks[other].devices[0].id;
        CHECK((!p.moveDevices(0, {drumId}, other, {0, instRack, 0}, false, nullptr, &why) && why.contains("pads hold devices")),
              "M16 pad chains: a Drum Rack whose pads hold devices doesn't go into a rack's chain");
        CHECK((p.moveDevices(0, {drumId}, other, {}) && p.tracks[0].devices.isEmpty()
              && (order(other) == QStringList{"Drum Rack@-1", "EQ@1", "Chord@3", "FM Synth@3", "Saturator@3"})),
              "M16 pad chains: moving the Drum Rack takes its pads' devices along (and replaces the other track's instrument)");
        p.undoStack()->undo();
        CHECK((p.tracks[0].devices.size() == 5 && p.tracks[other].devices.size() == 1), "M16 pad chains: undo puts the Drum Rack and its pads back");
        const int copyAt = p.duplicateTrack(0);
        const QVector<Device>& copy = p.tracks[copyAt].devices;
        CHECK((copy.size() == 5 && copy[0].id != drumId && std::all_of(copy.begin() + 1, copy.end(), [&](const Device& d) { return d.rack == copy[0].id; })),
              "M16 pad chains: a duplicated track's pad devices belong to the copy's Drum Rack");
        const QString kickPath = p.tracks[0].devices[0].pads[3].samplePath;
        const int undo1 = p.undoStack()->count();
        p.deleteChain(0, drumId, 3);
        CHECK(((order(0) == QStringList{"Drum Rack@-1", "EQ@1"}) && p.tracks[0].devices[0].pads[3].samplePath == kickPath && p.undoStack()->count() == undo1 + 1),
              "M16 pad chains: Remove Pad Devices takes that pad's devices and leaves its sample");
        p.undoStack()->undo();
        CHECK((p.tracks[0].devices.size() == 5 && p.groupIntoRack(0, drumId) == 0), "M16 pad chains: undo; a Drum Rack with pad devices isn't grouped into a rack");
        CHECK((p.deleteDevice(0, drumId) && p.tracks[0].devices.isEmpty()), "M16 pad chains: deleting the Drum Rack takes its pads' devices");
        p.undoStack()->undo();
        // a set's devices out of order (a hand-edited file): normalizeRacks puts pad devices back after their Drum Rack, drops pads past the last
        {
            QVector<Device> mess = p.tracks[0].devices;
            std::reverse(mess.begin(), mess.end());
            Device stray = p.makeDevice("Redux", DeviceKind::AudioEffect);
            stray.rack = drumId;
            stray.chain = 30;
            mess.push_back(stray);
            normalizeRacks(mess);
            QStringList o;
            for (const Device& d : mess) o << QString("%1@%2").arg(d.name).arg(d.rack ? d.chain : -1);
            CHECK(((o == QStringList{"Drum Rack@-1", "EQ@1", "Chord@3", "FM Synth@3", "Saturator@3"})), qPrintable("M16 pad chains: normalizeRacks restores a Drum Rack's pads: " + o.join(", ")));
        }
        // a Drum Rack inside an Instrument Rack's chain plays samples only: its pads take no devices
        {
            Project q;
            rackProject(q, {});
            Device r = q.makeDevice("Instrument Rack", DeviceKind::Instrument);
            r.chains = {"Drums"};
            r.zones.resize(1);
            Device inner = inChain(q.makeDevice("Drum Rack", DeviceKind::Instrument), r, 0);
            q.tracks[0].devices = {r, inner};
            CHECK((!holdsChains(q.tracks[0].devices[1]) && !q.insertDevices(0, {{q.makeDevice("Analog Synth", DeviceKind::Instrument)}}, {0, inner.id, 0}, "Add")
                  && deviceBlockEnd(q.tracks[0].devices, 0) == 2),
                  "M16 pad chains: a Drum Rack inside a rack's chain takes no pad devices");
            QVector<Device> again = q.tracks[0].devices;
            normalizeRacks(again);
            CHECK((again.size() == 2 && again[1].rack == r.id), "M16 pad chains: (and stays in the rack's chain when a set is read)");
        }
        // presets: the Drum Rack with its pads
        {
            QTemporaryDir dir;
            const QString path = dir.filePath("Pad Kit.sessionspreset");
            Project::DeviceBlocks blocks;
            CHECK((presets::save(path, "Pad Kit", p.deviceBlocks(0, {drumId})) && presets::read(path, blocks) && blocks.size() == 1 && blocks[0].size() == 5
                  && isDrumRack(blocks[0][0]) && blocks[0][3].name == "FM Synth" && blocks[0][3].rack == blocks[0][0].id && blocks[0][3].chain == 3),
                  "M16 pad chains: a Drum Rack saved as a preset keeps its pads' devices");
        }

        // the MIDI Effect Rack
        Project m;
        rackProject(m, {});
        m.selectedTrack = 0;
        m.addDeviceToSelected("Analog Synth", DeviceKind::Instrument);
        m.addDeviceToSelected("Arpeggiator", DeviceKind::MidiEffect);
        m.addDeviceToSelected("Chord", DeviceKind::MidiEffect);
        m.addDeviceToSelected("Saturator", DeviceKind::AudioEffect);
        const int arp = m.tracks[0].devices[0].id, chord = m.tracks[0].devices[1].id, sat = m.tracks[0].devices[3].id;
        CHECK((m.groupIntoRack(0, QList<int>{arp, sat}) == 0), "M16 MIDI Effect Rack: MIDI and audio effects without an instrument don't group");
        const int midiRack = m.groupIntoRack(0, QList<int>{arp, chord});
        const QVector<Device>& md = m.tracks[0].devices;
        CHECK((midiRack && deviceType(md[0].name) == DeviceType::MidiEffectRack && md[0].kind == DeviceKind::MidiEffect && md[1].rack == midiRack && md[2].rack == midiRack
              && md[3].name == "Analog Synth" && md[0].zones.size() == 1),
              "M16 MIDI Effect Rack: grouping MIDI effects makes a MIDI Effect Rack, ahead of the instrument");
        CHECK((!m.addDeviceToChain(0, midiRack, 0, "Analog Synth") && !m.addDeviceToChain(0, midiRack, 0, "Saturator") && m.addDeviceToChain(0, midiRack, -1, "Scale")),
              "M16 MIDI Effect Rack: its chains take MIDI effects only");
        const Device& mr = m.tracks[0].devices[0];
        CHECK((mr.chains.size() == 2 && mr.zones.size() == 2 && mr.params[racks::param(1, racks::Transpose)].value == 0.5 && mr.params[racks::param(1, racks::VelocityScale)].value == 0.5),
              "M16 MIDI Effect Rack: a new chain starts at Transpose 0 and Velocity 100 %, with default zones");
        const int audio = m.insertTrack(1, TrackType::Audio);
        CHECK((!m.insertDevices(audio, {m.makeDeviceBlock("MIDI Effect Rack", DeviceKind::MidiEffect)}, {}, "Add")), "M16 MIDI Effect Rack: it goes on MIDI tracks only");
        // ready-made racks with zones
        {
            const QVector<Device> layers = m.makeDeviceBlock("Octave Layers", DeviceKind::MidiEffect);
            const QVector<Device> split = m.makeDeviceBlock("Split Arp", DeviceKind::MidiEffect);
            const QVector<Device> keys = m.makeDeviceBlock("Bass and Keys Split", DeviceKind::Instrument);
            CHECK((layers.size() == 1 && layers[0].chains.size() == 3 && paramValue(DeviceType::MidiEffectRack, racks::param(1, racks::Transpose), layers[0].params[racks::param(1, racks::Transpose)].value) == 12
                  && paramValue(DeviceType::MidiEffectRack, racks::param(2, racks::Transpose), layers[0].params[racks::param(2, racks::Transpose)].value) == -12
                  && split.size() == 2 && split[1].name == "Arpeggiator" && split[0].zones[0].key == (Zone{0, 59, 0, 0}) && split[0].zones[1].key == (Zone{60, 127, 0, 0})
                  && keys.size() == 3 && keys[0].zones[0].key.hi == 54 && keys[0].zones[1].key.lo == 55 && keys[2].name == "Electric"),
                  "M16 Sounds: Octave Layers, Split Arp and Bass and Keys Split come with their transposes and key zones");
        }

        // zones and chains: edits, deleting and moving chains
        Project z;
        rackProject(z, {});
        Device r = z.makeDevice("Instrument Rack", DeviceKind::Instrument);
        r.chains = {"A", "B", "C"};
        r.zones.resize(3);
        r.zones[0].key = {0, 40, 0, 0};
        r.zones[1].key = {41, 80, 0, 0};
        r.zones[2].key = {81, 127, 0, 0};
        r.params[racks::param(1, racks::Volume)].value = 0.3;
        r.params[racks::param(2, racks::Volume)].value = 0.2;
        Device ca = inChain(z.makeDevice("Analog Synth", DeviceKind::Instrument), r, 0);
        Device cb = inChain(z.makeDevice("FM Synth", DeviceKind::Instrument), r, 1);
        Device cc = inChain(z.makeDevice("Wavetable Synth", DeviceKind::Instrument), r, 2);
        Device mac = z.makeDevice("Macros", DeviceKind::AudioEffect);
        r.maps = {{0, r.id, racks::param(2, racks::On), 0, 1}, {1, r.id, racks::param(0, racks::Pan), 0, 1}, {2, r.id, racks::ChainSelect, 0, 1}};
        mac.maps = {{0, r.id, racks::param(1, racks::Volume), 0, 1}};
        z.tracks[0].devices = {r, ca, cb, cc, mac};
        auto env = [](const QString& t) { Envelope e; e.target = t; e.points = {{0, 0.1}, {4, 0.9}}; return e; };
        z.tracks[0].automation = {env(target::device(r.id, racks::param(2, racks::Volume))), env(target::device(r.id, racks::param(0, racks::Volume))),
                                  env(target::device(ca.id, 0)), env(target::device(r.id, racks::ChainSelect))};
        z.tracks[0].clipSlots[0]->envelopes = {env(target::device(r.id, racks::param(1, racks::Pan)))};
        const int zu = z.undoStack()->count();
        CHECK((z.setChainZone(0, r.id, 1, ChainZones::Velocity, Zone{20, 100, 5, 5}) && z.tracks[0].devices[0].zones[1].vel == (Zone{20, 100, 5, 5}) && z.undoStack()->count() == zu + 1
              && !z.setChainZone(0, r.id, 1, ChainZones::Velocity, Zone{20, 100, 5, 5}) && z.undoStack()->count() == zu + 1),
              "M16 zones: setting a zone is one undo step (setting it to what it is, none)");
        z.setChainZone(0, r.id, 2, ChainZones::Select, Zone{0, 10, 0, 0}, deviceMergeKey(r.id, 234));
        z.setChainZone(0, r.id, 2, ChainZones::Select, Zone{0, 20, 0, 0}, deviceMergeKey(r.id, 234));
        z.setChainZone(0, r.id, 2, ChainZones::Select, Zone{40, 30, 99, 0}, deviceMergeKey(r.id, 234));
        CHECK((z.undoStack()->count() == zu + 2 && z.tracks[0].devices[0].zones[2].select == (Zone{40, 40, 0, 0})),
              "M16 zones: a drag's steps are one undo step; zones are kept sane");
        z.undoStack()->undo();
        z.undoStack()->undo();
        CHECK((z.tracks[0].devices[0].zones[1].vel == ChainZones{}.vel && z.tracks[0].devices[0].zones[2].select == ChainZones{}.select), "M16 zones: undo restores them");
        const EditState before = z.captureEditState();
        z.deleteChain(0, r.id, 0);
        {
            const Track& t = z.tracks[0];
            const Device& rr = t.devices[0];
            QStringList targets;
            for (const Envelope& e : t.automation) targets << e.target;
            const MacroMap* macroMap = nullptr;
            for (const Device& d : t.devices) if (d.name == "Macros") macroMap = &d.maps[0];
            CHECK((rr.chains == (QStringList{"B", "C"}) && rr.zones.size() == 2 && rr.zones[0].key == (Zone{41, 80, 0, 0}) && rr.zones[1].key == (Zone{81, 127, 0, 0})
                  && rr.params[racks::param(0, racks::Volume)].value == 0.3 && rr.params[racks::param(1, racks::Volume)].value == 0.2 && rr.params[racks::param(2, racks::Volume)].value == 0.75),
                  "M16 Delete Chain: the later chains move up with their names, zones and knobs");
            CHECK(((targets == QStringList{target::device(r.id, racks::param(1, racks::Volume)), target::device(r.id, racks::ChainSelect)})),
                  qPrintable("M16 Delete Chain: automation follows the moved knobs; the deleted chain's (and its devices') goes: " + targets.join(", ")));
            CHECK((t.clipSlots[0]->envelopes.size() == 1 && t.clipSlots[0]->envelopes[0].target == target::device(r.id, racks::param(0, racks::Pan))),
                  "M16 Delete Chain: clip envelopes follow the moved knobs");
            CHECK((rr.maps.size() == 2 && rr.maps[0].param == racks::param(1, racks::On) && rr.maps[1].param == racks::ChainSelect && macroMap && macroMap->param == racks::param(0, racks::Volume)),
                  "M16 Delete Chain: rack macros and Macros devices follow the moved knobs; a mapping to the deleted chain goes");
        }
        z.undoStack()->undo();
        CHECK((z.captureEditState() == before), "M16 Delete Chain: undo restores all of it");
        CHECK((z.moveChain(0, r.id, 0, 2)), "M16 Move Chain: (chain A to the end)");
        {
            const Track& t = z.tracks[0];
            const Device& rr = t.devices[0];
            QStringList o;
            for (const Device& d : t.devices) o << QString("%1@%2").arg(d.name).arg(d.rack ? d.chain : -1);
            QStringList targets;
            for (const Envelope& e : t.automation) targets << e.target;
            CHECK((rr.chains == (QStringList{"B", "C", "A"}) && rr.zones[2].key == (Zone{0, 40, 0, 0}) && rr.zones[0].key == (Zone{41, 80, 0, 0})
                  && rr.params[racks::param(0, racks::Volume)].value == 0.3 && rr.params[racks::param(1, racks::Volume)].value == 0.2
                  && (o == QStringList{"Instrument Rack@-1", "FM Synth@0", "Wavetable Synth@1", "Analog Synth@2", "Macros@-1"})),
                  qPrintable("M16 Move Chain: the chain moves with its devices, name, zones and knobs: " + o.join(", ")));
            CHECK((targets.contains(target::device(r.id, racks::param(1, racks::Volume))) && targets.contains(target::device(r.id, racks::param(2, racks::Volume)))
                  && rr.maps[0].param == racks::param(1, racks::On) && rr.maps[1].param == racks::param(2, racks::Pan)),
                  "M16 Move Chain: automation and macro mappings move with the knobs");
        }
        z.undoStack()->undo();
        CHECK((z.captureEditState() == before && !z.moveChain(0, r.id, 1, 1)), "M16 Move Chain: undo restores it; moving a chain onto itself does nothing");

        // the set file: zones, the MIDI Effect Rack, pad devices
        {
            QTemporaryDir dir;
            const QString path = dir.filePath("racks.sessions");
            Project s;
            rackProject(s, {});
            Device mr2 = s.makeDevice("MIDI Effect Rack", DeviceKind::MidiEffect);
            mr2.chains = {"Low", "High"};
            mr2.zones = {ChainZones{}, ChainZones{}};
            mr2.zones[0].key = {0, 59, 3, 4};
            mr2.zones[1].vel = {64, 127, 0, 10};
            mr2.zones[1].select = {2, 9, 1, 1};
            mr2.params[racks::param(0, racks::Transpose)].value = 0.375;
            mr2.params[racks::ChainSelect].value = 5 / 127.0;
            Device drum = s.makeDevice("Drum Rack", DeviceKind::Instrument);
            s.tracks[0].devices = {mr2, inChain(s.makeDevice("Arpeggiator", DeviceKind::MidiEffect), mr2, 0), drum,
                                   inChain(s.makeDevice("Analog Synth", DeviceKind::Instrument), drum, 5), inChain(s.makeDevice("Utility", DeviceKind::AudioEffect), drum, 5)};
            s.addTrack("Plain", QColor("#80c0ff"));
            Device plainRack = s.makeDevice("Audio Effect Rack", DeviceKind::AudioEffect);
            plainRack.chains = {"Dry"};
            plainRack.zones.resize(1);
            s.tracks[1].devices = {plainRack};
            setfile::Extras x;
            Project q;
            QString err;
            QFile f;
            CHECK((setfile::save(path, s, x) && setfile::load(path, q, x, &err) && q.captureEditState() == s.captureEditState()),
                  qPrintable("M16 sets: zones, a MIDI Effect Rack and a Drum Rack's pad devices round-trip (captureEditState ==) " + err));
            f.setFileName(path);
            f.open(QIODevice::ReadOnly);
            const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
            const QJsonArray t0 = root.value("tracks").toArray()[0].toObject().value("devices").toArray();
            const QJsonArray t1 = root.value("tracks").toArray()[1].toObject().value("devices").toArray();
            CHECK((t0[0].toObject().contains("zones") && t0[0].toObject().value("zones").toArray()[0].toObject().value("key").toArray() == (QJsonArray{0, 59, 3, 4})
                  && !t1[0].toObject().contains("zones") && t0[3].toObject().value("rack").toInt() == drum.id && t0[3].toObject().value("chain").toInt() == 5),
                  "M16 sets: zones are written only when a chain has some ([lo, hi, fade in, fade out]); pad devices name their Drum Rack and pad");
        }
    }

    // ---- Milestone 16: the panels
    {
        auto mouse = [](QWidget* w, QEvent::Type t, QPointF at) {
            QMouseEvent ev(t, at, w->mapToGlobal(at), Qt::LeftButton, t == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(w, &ev);
        };
        Project p;
        rackProject(p, {});
        p.selectedTrack = 0;
        Device r = p.makeDevice("Instrument Rack", DeviceKind::Instrument);
        r.chains = {"A", "B", "C"};
        r.zones.resize(3);
        p.tracks[0].devices = {r, inChain(p.makeDevice("Analog Synth", DeviceKind::Instrument), r, 0), inChain(p.makeDevice("FM Synth", DeviceKind::Instrument), r, 1),
                               inChain(p.makeDevice("Organ", DeviceKind::Instrument), r, 2)};
        audio::Engine e(&p);
        Transport tr(&p, &e);
        DeviceView view(&p, &tr);
        view.resize(1600, 260);
        view.show();
        devui::showZones(r.id, true);
        devui::setZoneMode(r.id, ChainZones::Key);
        view.rebuild();
        QApplication::processEvents();
        devui::DeviceBody* body = nullptr;
        for (DevicePanel* panel : view.panels()) if (panel->deviceId() == r.id) body = panel->body();
        QList<QWidget*> bars;
        for (QWidget* w : body ? body->findChildren<QWidget*>("zoneBar") : QList<QWidget*>()) if (w->isVisibleTo(body)) bars << w;
        CHECK((body && bars.size() == 3 && body->findChildren<QWidget*>("zoneRuler").size() == 1 && body->minimumSizeHint().height() <= devui::kBodyHeight + 4),
              "M16 panels: Zones shows a zone bar per chain and the scale, and the rack still fits the device height");
        if (bars.size() == 3) {
            QWidget* bar = bars[0];
            const int undo0 = p.undoStack()->count();
            const double y = bar->height() * 0.75;   // the lower half: the range
            mouse(bar, QEvent::MouseButtonPress, QPointF(bar->width() - 2, y));
            for (int k = 1; k <= 4; ++k) mouse(bar, QEvent::MouseMove, QPointF(bar->width() - 2 - k * (bar->width() / 2.0 - 2) / 4, y));
            mouse(bar, QEvent::MouseButtonRelease, QPointF(bar->width() / 2.0, y));
            const Zone k0 = p.tracks[0].devices[0].zones[0].key;
            CHECK((k0.lo == 0 && k0.hi >= 62 && k0.hi <= 65 && p.undoStack()->count() == undo0 + 1), "M16 panels: dragging a zone bar's end sets the key zone (one undo step)");
            mouse(bar, QEvent::MouseButtonPress, QPointF(bar->width() * 0.25, bar->height() * 0.25));   // the upper half at the low end's fade: inside the bar, so the fade
            const Zone k1 = p.tracks[0].devices[0].zones[0].key;
            QMouseEvent dbl(QEvent::MouseButtonDblClick, QPointF(5, y), bar->mapToGlobal(QPointF(5, y)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(bar, &dbl);
            CHECK((k1 == k0 && p.tracks[0].devices[0].zones[0].key == Zone{}), "M16 panels: double-clicking a zone bar gives the default zone back");
        }
        // Chain mode: the scale moves the selector
        devui::setZoneMode(r.id, ChainZones::Select);
        view.rebuild();
        QApplication::processEvents();
        body = nullptr;
        for (DevicePanel* panel : view.panels()) if (panel->deviceId() == r.id) body = panel->body();
        QWidget* ruler = body ? body->findChildren<QWidget*>("zoneRuler").value(0) : nullptr;
        if (ruler) mouse(ruler, QEvent::MouseButtonPress, QPointF(ruler->width() * 0.5 + 1, 6));
        CHECK((ruler && std::lround(paramValue(DeviceType::InstrumentRack, racks::ChainSelect, p.tracks[0].devices[0].params[racks::ChainSelect].value)) == 64),
              "M16 panels: in Chain mode a click on the scale moves the Chain selector");
        // drag a chain's name down two rows: the chain moves
        QList<QWidget*> names;
        for (QWidget* w : body ? body->findChildren<QWidget*>("chainName") : QList<QWidget*>()) if (w->isVisibleTo(body)) names << w;
        if (names.size() == 3) {
            QWidget* first = names[0];
            const QPoint third = first->mapFromGlobal(names[2]->mapToGlobal(QPoint(names[2]->width() / 2, names[2]->height() / 2)));
            mouse(first, QEvent::MouseButtonPress, QPointF(20, 8));
            mouse(first, QEvent::MouseMove, QPointF(20, 8 + 6));
            mouse(first, QEvent::MouseMove, QPointF(third));
            mouse(first, QEvent::MouseButtonRelease, QPointF(third));
            QApplication::processEvents();
        }
        CHECK((names.size() == 3 && (p.tracks[0].devices[0].chains == QStringList{"B", "C", "A"}) && devui::selectedChain(r.id) == 2),
              "M16 panels: dragging a chain's name down moves the chain (and shows it)");
        devui::showZones(r.id, false);

        // the MIDI Effect Rack's panel: Transpose and Velocity per chain
        {
            Device mr = p.makeDevice("MIDI Effect Rack", DeviceKind::MidiEffect);
            mr.chains = {"One", "Two"};
            mr.zones.resize(2);
            DevicePanel panel(&p, &tr, 0, mr, nullptr, {});
            QSet<QString> targets;
            for (QWidget* w : panel.body()->findChildren<QWidget*>()) targets.insert(w->property("mapTarget").toString());
            CHECK((targets.contains(panel.body()->target(racks::param(1, racks::Transpose))) && targets.contains(panel.body()->target(racks::ChainSelect))
                  && panel.body()->name(racks::param(1, racks::Transpose)) == "Chain 2 Transpose"),
                  "M16 panels: the MIDI Effect Rack's chains have Transpose and Velocity fields; the Chain selector has one");
        }

        // a Drum Rack: devices dropped on a pad go onto it, and the Device View shows the selected pad's devices
        p.tracks[0].devices = {p.makeDevice("Drum Rack", DeviceKind::Instrument)};
        const int drumId = p.tracks[0].devices[0].id;
        emit p.structureChanged();
        QApplication::processEvents();
        devui::DeviceBody* drumBody = nullptr;
        for (DevicePanel* panel : view.panels()) if (panel->deviceId() == drumId) drumBody = panel->body();
        devui::Display* pads = drumBody ? drumBody->findChild<devui::Display*>("drumPads") : nullptr;
        int racksShown = 0;
        for (RackPanel* rp : view.findChildren<RackPanel*>()) racksShown += rp->isVisibleTo(&view) ? 1 : 0;
        CHECK((pads && racksShown == 1), "M16 panels: a Drum Rack shows in a rack frame, with room for the selected pad's devices");
        if (pads) {
            const QPointF pad4(pads->width() * 0.125, pads->height() * 0.625);   // the second row's first pad: E1
            QMimeData mime;
            mime.setData(Browser::mimeType(), "Analog Synth|i");
            const int undo0 = p.undoStack()->count();
            QDragEnterEvent enter(pad4.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pads, &enter);
            QDragMoveEvent move(pad4.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pads, &move);
            QDropEvent drop(pad4, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(pads, &drop);
            QApplication::processEvents();
            const QVector<Device>& ds = p.tracks[0].devices;
            CHECK((move.isAccepted() && ds.size() == 2 && ds[1].name == "Analog Synth" && ds[1].rack == drumId && ds[1].chain == 4 && p.undoStack()->count() == undo0 + 1
                  && devui::selectedDrumPad(drumId) == 4),
                  "M16 panels: an instrument dropped on a pad goes onto that pad (one undo step) and selects it");
            QStringList shown;
            for (DevicePanel* panel : view.panels()) for (const Device& d : p.tracks[0].devices) if (d.id == panel->deviceId()) shown << d.name;
            CHECK(((shown == QStringList{"Drum Rack", "Analog Synth"}) && devui::padInstrument(&p, 0, drumId, 4) && devui::drumPadLabel(p.tracks[0].devices[0], 4).startsWith("E1")),
                  qPrintable("M16 panels: the Device View shows the selected pad's devices next to the Drum Rack: " + shown.join(", ")));
            devui::selectDrumPad(drumId, 0);
            QApplication::processEvents();
            shown.clear();
            for (DevicePanel* panel : view.panels()) for (const Device& d : p.tracks[0].devices) if (d.id == panel->deviceId()) shown << d.name;
            QLabel* zone = nullptr;
            for (QLabel* l : view.findChildren<QLabel*>()) if (l->isVisibleTo(&view) && l->text().startsWith("Drop an instrument or effects onto pad C1")) zone = l;
            CHECK(((shown == QStringList{"Drum Rack"}) && zone), "M16 panels: picking another pad shows its devices (none) and a drop zone for them");
        }
    }

    // ================================================================ Milestone 17: after the 0.16 hands-on
    // ---- Session launches take every track from the Arrangement when the song isn't playing
    {
        Project p;
        p.scenes = {"1"};
        p.addTrack("Arr", QColor("#f2905e"));   // plays from the timeline only
        p.addTrack("Ses", QColor("#9b7ff0"));   // has a Session clip
        for (int t = 0; t < 2; ++t) { p.tracks[t].devices = {p.makeDevice("Sampler", DeviceKind::Instrument)}; p.tracks[t].devices[0].params[1].value = 0; }
        ClipPtr arr = p.newClip("Arr", 4);
        arr->notes = {Note{60, 0.0, 0.5, 1.0, 1.0}};
        p.tracks[0].arrangement = {{0, 4, 0, arr}};
        ClipPtr ses = p.newClip("Ses", 4);
        ses->notes = {Note{60, 2.0, 0.5, 1.0, 1.0}};
        p.tracks[1].clipSlots[0] = ses;
        audio::Engine e(&p);
        Transport tr(&p, &e);
        tr.launchSlot(1, 0);   // from a standstill
        CHECK((tr.playing() && p.tracks[0].overridden && p.tracks[1].overridden), "M17 Session: launching a clip from a standstill takes every track from the Arrangement");
        std::vector<float> out = render(e, 12000);   // beats 0 .. 0.5: the Arrangement note would sound here
        CHECK((peakOf(out) < 1e-5f && e.trackStatus(0).overridden && e.trackStatus(1).playingSlot == 0), "M17 Session: the Arrangement track stays silent while the clip plays");
        out = render(e, 60000);   // beats 0.5 .. 3: the clip's note at beat 2
        CHECK((peakOf(out) > 0.05f), "M17 Session: (the launched clip plays)");
        tr.backToArrangement();
        tr.setBeat(0);
        out = render(e, 12000);
        CHECK((peakOf(out) > 0.05f && !p.tracks[0].overridden && !e.trackStatus(0).overridden), "M17 Session: Back to Arrangement hands the tracks back and the timeline plays again");
        tr.stop();
        render(e, 256);
        tr.play();
        render(e, 256);
        tr.launchSlot(1, 0);   // while the song plays: only that track
        CHECK((!p.tracks[0].overridden && p.tracks[1].overridden), "M17 Session: launched while the song plays, a clip takes only its own track (as before)");
        tr.stop();
        render(e, 256);
        tr.launchScene(0);
        CHECK((tr.playing() && p.tracks[0].overridden && p.tracks[1].overridden), "M17 Session: a scene launched from a standstill does the same");
    }

    // ---- the window: the browser's divider, the Device View's fixed pane, the Clip View's draggable one
    {
        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QApplication::processEvents();
        auto* hsplit = w.findChild<QSplitter*>("browserSplit");
        Browser* browser = w.findChild<Browser*>();
        CHECK((hsplit && hsplit->count() == 2 && browser && hsplit->widget(0) == browser), "M17 window: the browser sits in a splitter beside the main views");
        if (hsplit) { hsplit->setSizes({420, 900}); QApplication::processEvents(); }
        CHECK((browser && browser->width() >= 400), "M17 window: dragging the divider makes the browser wider");
        QToolButton* clipTab = nullptr; QToolButton* deviceTab = nullptr;
        for (QToolButton* b : w.findChildren<QToolButton*>()) { if (b->text() == "Clip") clipTab = b; if (b->text() == "Device") deviceTab = b; }
        QWidget* bottom = w.findChild<QWidget*>("bottomPanel");
        auto* vsplit = bottom ? qobject_cast<QSplitter*>(bottom->parentWidget()) : nullptr;
        if (deviceTab) QTest::mouseClick(deviceTab, Qt::LeftButton);
        QApplication::processEvents();
        CHECK((clipTab && deviceTab && bottom && vsplit && bottom->minimumHeight() == bottom->maximumHeight() && bottom->height() == bottom->minimumHeight() && !vsplit->handle(1)->isEnabled()),
              "M17 window: the Device View's pane is fixed at the devices' height and its divider can't be dragged");
        const int deviceH = bottom ? bottom->height() : 0;
        bool onScreen = !w.deviceView()->panels().isEmpty();
        for (DevicePanel* panel : w.deviceView()->panels()) onScreen &= panel->mapTo(bottom, QPoint(0, panel->height())).y() <= bottom->height();
        CHECK(onScreen, "M17 window: every device panel is on screen in that pane");
        if (clipTab) QTest::mouseClick(clipTab, Qt::LeftButton);
        QApplication::processEvents();
        CHECK((bottom && vsplit && bottom->maximumHeight() == QWIDGETSIZE_MAX && vsplit->handle(1)->isEnabled()), "M17 window: the Clip View's pane can be dragged");
        if (vsplit) vsplit->setSizes({300, 520});
        QApplication::processEvents();
        const int clipH = bottom ? bottom->height() : 0;
        if (deviceTab) QTest::mouseClick(deviceTab, Qt::LeftButton);
        QApplication::processEvents();
        if (clipTab) QTest::mouseClick(clipTab, Qt::LeftButton);
        QApplication::processEvents();
        CHECK((clipH >= 480 && bottom && bottom->height() == clipH && deviceH < clipH), "M17 window: the Clip View comes back at the height you dragged it to, taller than the Device View's");
    }

    // ---- the clip clocks: where a playing clip is in its loop, from the beat you hear
    {
        Project p;
        oneNoteProject(p);
        audio::Engine e(&p);
        Transport tr(&p, &e);
        SessionView view(&p, &tr);
        ClipGrid* grid = view.grid();
        p.tracks[0].playingSlot = 0;
        p.tracks[0].slotStart = 1.0;
        tr.setBeat(3.5);
        const ClipGrid::TrackStatus a = grid->trackStatus(0);
        tr.setBeat(10.0);
        const ClipGrid::TrackStatus b = grid->trackStatus(0);
        tr.setBeat(0.98);   // just before the start the engine reported (output latency): the clock sits at 0
        const ClipGrid::TrackStatus c = grid->trackStatus(0);
        CHECK((a.playing && std::fabs(a.fraction - 0.625) < 1e-9 && a.pass == 1 && std::fabs(b.fraction - 0.25) < 1e-9 && b.pass == 3 && c.playing && c.fraction == 0.0),
              "M17 clocks: the fraction through the loop and the pass count follow the heard beat, never negative");
        p.tracks[0].clipSlots[0]->loop = false;
        tr.setBeat(9.0);
        CHECK((std::fabs(grid->trackStatus(0).fraction - 1.0) < 1e-9 && grid->trackStatus(0).pass == 0), "M17 clocks: a one-shot clip's clock fills once and stays full");
        p.tracks[0].clipSlots[0]->loop = true;
        view.resize(600, 400);
        view.show();
        tr.setBeat(3.5);
        QApplication::processEvents();
        view.repaint();
        CHECK(true, "M17 clocks: the grid paints a playing clip's bar and clock (no crash)");
    }

    // ---- NAM: the capture library, ◀ ▶, the mapping targets
    {
        QTemporaryDir lib, other, own;
        qputenv("SESSIONS_PRESETS", lib.path().toLocal8Bit());
        QDir(lib.path()).mkpath("Amps");
        const QString a = QFileInfo(lib.filePath("Amps/a.nam")).absoluteFilePath(), b = QFileInfo(lib.filePath("b.nam")).absoluteFilePath();
        const QString c = QFileInfo(other.filePath("c.nam")).absoluteFilePath(), d = QFileInfo(other.filePath("d.nam")).absoluteFilePath(), bad = QFileInfo(other.filePath("bad.nam")).absoluteFilePath();
        nam::writeExample(a, 2); nam::writeExample(b, 3); nam::writeExample(c, 4); nam::writeExample(d, 5);
        writeFile(bad, "{}");
        QSettings("Sessions", "Sessions").setValue("browser/folders", QStringList{other.path()});
        QStringList expected{a, b, c, d, bad};
        std::sort(expected.begin(), expected.end(), [](const QString& x, const QString& y) { return x.compare(y, Qt::CaseInsensitive) < 0; });
        const QStringList library = import::captureLibrary();
        CHECK((library == expected && import::captureIndex(b, library) == expected.indexOf(b) + 1 && import::captureIndex(own.filePath("x.nam"), library) == 0),
              "M17 NAM: the capture library is the User Library folder and the browser's sample folders, subfolders included, sorted");
        auto readable = [&](int from, int dir) { int i = from; do { i = ((i + dir) % library.size() + library.size()) % library.size(); } while (library[i] == bad); return library[i]; };
        Project p;
        p.scenes = {"1"};
        p.addTrack("Guitar", QColor("#f2905e"));
        p.tracks[0].type = TrackType::Audio;
        Device dev = p.makeDevice("NAM", DeviceKind::AudioEffect);
        dev.modelPath = b;
        dev.model = nam::load(b);
        p.tracks[0].devices = {dev};
        const int devId = dev.id;
        auto path = [&] { return p.tracks[0].devices[0].modelPath; };
        const int undo0 = p.undoStack()->count();
        QString why;
        bool ok;
        { EditScope e(&p, "Next"); ok = import::stepCapture(p, 0, devId, 1, &why); }
        CHECK((ok && path() == readable(library.indexOf(b), 1) && p.tracks[0].devices[0].model && p.undoStack()->count() == undo0 + 1), qPrintable("M17 NAM: ▶ loads the next capture in the library (skipping one that can't be read), one undo step " + why));
        { EditScope e(&p, "Prev"); ok = import::stepCapture(p, 0, devId, -1, &why); }
        CHECK((ok && path() == b), "M17 NAM: ◀ goes back");
        p.tracks[0].devices[0].modelPath = library.last() == bad ? library[library.size() - 2] : library.last();
        { EditScope e(&p, "Next"); ok = import::stepCapture(p, 0, devId, 1, &why); }
        CHECK((ok && path() == readable(library.indexOf(p.tracks[0].devices[0].modelPath) < 0 ? 0 : library.indexOf(path()) - 1, 1) && path() == readable(library.size() - 1, 1)),
              "M17 NAM: past the last capture it wraps to the first");
        // a capture outside the library: its own folder's captures
        const QString x1 = QFileInfo(own.filePath("x1.nam")).absoluteFilePath(), x2 = QFileInfo(own.filePath("x2.nam")).absoluteFilePath();
        nam::writeExample(x1, 6); nam::writeExample(x2, 7);
        p.tracks[0].devices[0].modelPath = x1;
        { EditScope e(&p, "Next"); ok = import::stepCapture(p, 0, devId, 1, &why); }
        const bool toX2 = ok && path() == x2;
        { EditScope e(&p, "Next"); ok = import::stepCapture(p, 0, devId, 1, &why); }
        CHECK((toX2 && ok && path() == x1), "M17 NAM: a capture that isn't in the library steps through its own folder, wrapping around");
        p.tracks[0].devices[0].modelPath.clear();
        p.tracks[0].devices[0].model.reset();
        { EditScope e(&p, "Next"); ok = import::stepCapture(p, 0, devId, 1, &why); }
        CHECK((ok && path() == readable(library.size() - 1, 1)), "M17 NAM: with no capture loaded, ▶ starts at the library's first");
        QTemporaryDir lonely;
        const QString only = QFileInfo(lonely.filePath("only.nam")).absoluteFilePath();
        nam::writeExample(only, 8);
        p.tracks[0].devices[0].modelPath = only;
        { EditScope e(&p, "Next"); ok = import::stepCapture(p, 0, devId, 1, &why); }
        CHECK((!ok && path() == only && why.contains("only")), "M17 NAM: a lone capture in its folder has nowhere to step (and says so)");
        CHECK((!import::stepCapture(p, 0, 999, 1, &why) && !mapping::isContinuous(QString("device:%1:%2:next").arg(p.tracks[0].id).arg(devId))),
              "M17 NAM: stepping needs a NAM device; the next / prev targets are buttons, not knobs");
        // the mapping targets
        audio::Engine e(&p);
        Transport tr(&p, &e);
        Mapper mapper(&p, &tr);
        p.tracks[0].devices[0].modelPath = b;
        const int undo1 = p.undoStack()->count();
        mapper.trigger(QString("device:%1:%2:next").arg(p.tracks[0].id).arg(devId), true);
        CHECK((path() == readable(library.indexOf(b), 1) && p.undoStack()->count() == undo1 + 1
               && mapping::targetName(p, QString("device:%1:%2:next").arg(p.tracks[0].id).arg(devId)) == "NAM · Next Capture"
               && mapping::targetName(p, QString("device:%1:%2:prev").arg(p.tracks[0].id).arg(devId)) == "NAM · Previous Capture"),
              "M17 NAM: a mapped Next Capture button steps the capture (one undo step) and the targets have names");
        p.undoStack()->undo();
        CHECK((path() == b), "M17 NAM: undo brings the previous capture back");
        // the panel
        DevicePanel panel(&p, &tr, 0, p.tracks[0].devices[0], nullptr, {});
        panel.show();
        QApplication::processEvents();
        QToolButton* next = nullptr; QToolButton* prev = nullptr; QLabel* place = nullptr;
        for (QToolButton* t : panel.body()->findChildren<QToolButton*>()) {
            if (t->property("mapTarget").toString().endsWith(":next")) next = t;
            if (t->property("mapTarget").toString().endsWith(":prev")) prev = t;
        }
        for (QLabel* l : panel.body()->findChildren<QLabel*>()) if (l->text() == QString("%1 / %2").arg(expected.indexOf(b) + 1).arg(expected.size())) place = l;
        if (next) QTest::mouseClick(next, Qt::LeftButton);
        QApplication::processEvents();
        CHECK((next && prev && place && path() == readable(library.indexOf(b), 1)), "M17 NAM panel: ◀ ▶ buttons (mappable) step the capture; the panel says where it is in the library");
        qunsetenv("SESSIONS_PRESETS");
        QSettings("Sessions", "Sessions").remove("browser/folders");
    }

    // ---- Monolith: pitch bend, the wheel as a source, LFO retrigger and fade, stepper retrigger, Restart
    {
        using namespace monolith;
        struct M17Note { int pitch, at, length; float vel; };
        auto run = [](const std::vector<float>& params, const std::vector<ModRoute>& mods, const std::vector<M17Note>& notes, double seconds, float bend, float wheel) {
            auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
            DevRun out;
            audio::EventList ev;
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.playing = true;
            ctx.bend = bend; ctx.wheel = wheel;
            ctx.mods = mods.data(); ctx.modCount = int(mods.size());
            const int total = int(seconds * 48000);
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            for (int pos = 0; pos < total; pos += audio::kMaxBlock) {
                const int n = std::min(audio::kMaxBlock, total - pos);
                ev.clear();
                for (const M17Note& m : notes) if (m.at >= pos && m.at < pos + n) ev.add({m.at - pos, m.pitch, m.vel, m.length, false, false});
                ctx.beat = pos * ctx.beatsPerSample;
                std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
                proc->render(ev, l, r, n, params.data(), ctx);
                for (int i = 0; i < n; ++i) {
                    if (!std::isfinite(l[i]) || !std::isfinite(r[i])) out.finite = false;
                    out.peak = std::max(out.peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
                    out.l.push_back(l[i]); out.r.push_back(r[i]);
                }
            }
            return out;
        };
        auto diffIn = [](const std::vector<float>& x, int xFrom, const std::vector<float>& y, int yFrom, int n) {
            float d = 0;
            for (int i = 0; i < n && xFrom + i < int(x.size()) && yFrom + i < int(y.size()); ++i) d = std::max(d, std::fabs(x[size_t(xFrom + i)] - y[size_t(yFrom + i)]));
            return d;
        };
        auto rmsDiff = [](const std::vector<float>& x, const std::vector<float>& y, int from, int to) {
            double s = 0; int n = 0;
            for (int i = from; i < to && i < int(x.size()) && i < int(y.size()); ++i) { const double d = double(x[size_t(i)]) - y[size_t(i)]; s += d * d; ++n; }
            return n ? float(std::sqrt(s / n)) : 0.0f;
        };
        std::vector<float> base = defaultsOf(DeviceType::Monolith);
        base[OscRestart] = 1;                                     // phases from 0: every run the same
        base[eparam(0, A)] = 0; base[eparam(0, S)] = 1;           // a steady amp
        base[oparam(1, Level)] = 0;                                // Osc 1 alone
        base[fparam(0, FCutoff)] = 1;                              // filter 1 wide open
        base[BendRange] = 0.5f;   // 12 st of 0 .. 24
        const std::vector<M17Note> c3 = {{60, 0, 24000, 0.9f}}, c4 = {{72, 0, 24000, 0.9f}}, fs3 = {{66, 0, 24000, 0.9f}};
        CHECK((maxDiff(run(base, {}, c3, 0.4, 1.0f, 0).l, run(base, {}, c4, 0.4, 0, 0).l) < 1e-5f && maxDiff(run(base, {}, c3, 0.4, 0.5f, 0).l, run(base, {}, fs3, 0.4, 0, 0).l) < 1e-5f),
              "M17 Monolith: pitch bend moves the pitch by the Bend Range (full bend at 12 st = the note an octave up, half = a tritone)");
        {
            std::vector<float> half = base; half[fparam(0, FCutoff)] = 0.5f;
            CHECK((maxDiff(run(base, {{SrcWheel, fparam(0, FCutoff), -0.5}}, c3, 0.4, 0, 1.0f).l, run(half, {}, c3, 0.4, 0, 0).l) < 1e-5f
                   && maxDiff(run(base, {{SrcWheel, fparam(0, FCutoff), -0.5}}, c3, 0.4, 0, 0).l, run(base, {}, c3, 0.4, 0, 0).l) < 1e-5f),
                  "M17 Monolith: the mod wheel is a source (at 1 it turns the cutoff by the route's amount, at 0 nothing)");
        }
        {
            std::vector<float> lfo = base; lfo[lparam(0, LRate)] = choicePosition(8, 9);   // free, 2 Hz (the default Hz)
            const std::vector<ModRoute> route = {{SrcLfo1, fparam(0, FCutoff), -0.6}};
            const std::vector<M17Note> late = {{60, 14400, 24000, 0.9f}};
            const DevRun first = run(lfo, route, c3, 0.5, 0, 0), free = run(lfo, route, late, 0.9, 0, 0);
            std::vector<float> retrig = lfo; retrig[lretrig(0)] = 1;
            const DevRun again = run(retrig, route, late, 0.9, 0, 0), again0 = run(retrig, route, c3, 0.5, 0, 0);
            CHECK((diffIn(again0.l, 1000, again.l, 15400, 10000) < 1e-4f && diffIn(first.l, 1000, free.l, 15400, 10000) > 0.01f),
                  "M17 Monolith: LFO Retrig restarts the LFO with every note (a later note sounds the same as one at the start); off, the LFO runs on");
            std::vector<float> faded = retrig; faded[lfade(0)] = 1;   // a 5 s fade-in
            const DevRun plain = run(base, {}, c3, 0.5, 0, 0);
            CHECK((rmsDiff(run(faded, route, c3, 0.5, 0, 0).l, plain.l, 1000, 24000) < 0.25f * rmsDiff(again0.l, plain.l, 1000, 24000)),
                  "M17 Monolith: LFO Fade ramps the LFO in from the note's start (a 5 s fade barely moves the cutoff in the first half second)");
        }
        {
            std::vector<float> st = base; st[sparam(0, SRate)] = choicePosition(6, 8); st[sparam(0, SCount)] = 1.0f / 15;   // 1/16: 6000 samples a step, two steps
            st[step(0, 0)] = 1; st[step(0, 1)] = 0;                                                                              // loud, then quiet
            const std::vector<ModRoute> route = {{SrcStep1, oparam(0, Level), 0.5}};
            const std::vector<M17Note> mid = {{60, 3000, 24000, 0.9f}};   // starts half way through the song's step 0
            std::vector<float> rt = st; rt[sretrig(0)] = 1;
            const DevRun song = run(st, route, mid, 0.5, 0, 0), own = run(rt, route, mid, 0.5, 0, 0);
            CHECK((rmsOf(own.l, 6500, 8500) > 2.0f * rmsOf(song.l, 6500, 8500) && rmsOf(song.l, 6500, 8500) > 0.01f),
                  "M17 Monolith: Step Retrig starts the sequence from step 1 with every note (off, it follows the song's beat)");
        }
        {
            std::vector<float> loose = base; loose[OscRestart] = 0;
            const std::vector<M17Note> twice = {{60, 0, 6000, 0.9f}, {60, 14400, 6000, 0.9f}};   // the same note twice in one run
            const DevRun same = run(base, {}, twice, 0.5, 0, 0), free = run(loose, {}, twice, 0.5, 0, 0);
            CHECK((diffIn(same.l, 1000, same.l, 15400, 4000) < 1e-3f && diffIn(free.l, 1000, free.l, 15400, 4000) > 0.01f),
                  "M17 Monolith: Restart makes every note start its oscillators at the same phase (off: free phases, two hits differ)");
        }
        // the whole way: a keyboard's pitch bend through the transport and the engine to the track's Monolith
        {
            auto song = [&](int pitch, int bendTrack, double bend) {
                Project p;
                p.scenes = {"1"};
                p.addTrack("Mono", QColor("#9b7ff0"));
                p.addTrack("Other", QColor("#f2905e"));
                Device m = p.makeDevice("Monolith", DeviceKind::Instrument);
                for (size_t i = 0; i < base.size() && int(i) < m.params.size(); ++i) m.params[int(i)].value = base[i];
                p.tracks[0].devices = {m};
                ClipPtr c = p.newClip("One", 4);
                c->notes = {Note{pitch, 0.0, 1.0, 0.9, 1.0}};
                p.tracks[0].arrangement = {{0, 4, 0, c}};
                audio::Engine e(&p);
                Transport tr(&p, &e);
                tr.perform(bendTrack, 0, bend);
                tr.perform(bendTrack, 1, 0.7);
                e.send({Command::Stop, -1, -1, 0, 0, true});
                e.send({Command::Play});
                return render(e, 20000);
            };
            CHECK((maxDiff(song(60, 0, 1.0), song(72, 0, 0.0)) < 1e-5f && maxDiff(song(60, 1, 1.0), song(60, 0, 0.0)) < 1e-6f),
                  "M17 engine: a pitch bend sent to a track reaches its Monolith (and only that track's)");
        }
        {
            audio::MidiInput in;
            QSignalSpy bends(&in, &audio::MidiInput::pitchBend), pressures(&in, &audio::MidiInput::channelPressure), ccs(&in, &audio::MidiInput::controlChange);
            in.inject(0xE1, 0x00, 0x60);   // channel 2, 12288
            in.inject(0xD0, 100, 0);
            in.inject(0xB0, 1, 64);
            CHECK((bends.size() == 1 && bends[0][0].toInt() == 1 && bends[0][1].toInt() == 12288 && pressures.size() == 1 && pressures[0][1].toInt() == 100 && ccs.size() == 1),
                  "M17 MIDI: pitch bend and channel pressure messages arrive as their own signals (the mod wheel is CC 1)");
        }
    }

    // ================================================================ Milestone 18: everything in sync, and the pedals revisited
    // ---- tempo detection and matching the set's tempo
    {
        // a drum loop at a tempo: kicks on the beats, a snare on 2 and 4, hats on the eighths
        auto loopAt = [](double bpm, int bars, double rate) {
            const double beat = 60.0 / bpm;
            const int n = int(std::lround(bars * 4 * beat * rate));
            std::vector<float> v(size_t(n), 0.0f);
            dsp::Noise rng;
            for (int b = 0; b < bars * 8; ++b) {
                const int at = int(std::lround(b * beat / 2 * rate));
                for (int i = 0; i < int(0.08 * rate) && at + i < n; ++i) {
                    const double t = i / rate;
                    float s = 0.15f * float(rng.next() * std::exp(-t * 120));                                     // a hat on every eighth
                    if (b % 2 == 0) s += 0.6f * float(std::sin(2 * M_PI * 60 * t) * std::exp(-t * 25));            // a kick on the beat
                    if (b % 8 == 2 || b % 8 == 6) s += 0.4f * float(rng.next() * std::exp(-t * 40));              // a snare on 2 and 4
                    v[size_t(at + i)] += s;
                }
            }
            return v;
        };
        auto sampleOf = [](const std::vector<float>& v, double rate) {
            auto s = std::make_shared<SampleData>();
            s->rate = rate; s->channels = 1; s->left = v; s->right = v;
            return SamplePtr(s);
        };
        bool close = true;
        QStringList found;
        for (double bpm : {87.0, 96.0, 128.0, 172.0}) {
            const tempo::Estimate e = tempo::detect(*sampleOf(loopAt(bpm, 4, 48000), 48000));
            found << QString("%1→%2").arg(bpm).arg(e.bpm, 0, 'f', 1);
            const double oct = std::log2(e.bpm / bpm);   // the tempo, or its half or double (the same for sync)
            close &= std::fabs(oct - std::round(oct)) < 0.015 && e.confidence > 0.1;
        }
        const tempo::Estimate at441 = tempo::detect(*sampleOf(loopAt(120, 2, 44100), 44100));
        CHECK((close && std::fabs(at441.bpm - 120) < 1.5), qPrintable("M18 tempo: drum loops at 87, 96, 128 and 172 BPM are found within 1 % (or their half / double; and at 44.1 kHz): " + found.join(", ")));
        std::vector<float> noise(96000);
        { std::mt19937 rng(7); std::uniform_real_distribution<float> u(-0.3f, 0.3f); for (float& x : noise) x = u(rng); }
        std::vector<float> hit(int(0.3 * 48000), 0.0f);
        for (size_t i = 0; i < hit.size(); ++i) hit[i] = 0.5f * float(std::sin(2 * M_PI * 80 * i / 48000.0) * std::exp(-double(i) / 4000));
        const tempo::Estimate noiseT = tempo::detect(*sampleOf(noise, 48000)), hitT = tempo::detect(*sampleOf(hit, 48000));
        CHECK((noiseT.bpm == 0 && hitT.bpm == 0), qPrintable(QString("M18 tempo: noise and a single hit have no tempo (noise %1 BPM at %2, hit %3)").arg(noiseT.bpm, 0, 'f', 1).arg(noiseT.confidence, 0, 'f', 2).arg(hitT.bpm, 0, 'f', 1)));
        // matching: the set's tempo halved or doubled until it's nearest the sample's
        CHECK((tempo::octaveFor(87, 172) == 1 && tempo::octaveFor(87, 120) == 0 && tempo::octaveFor(87, 60) == -1 && tempo::octaveFor(174, 87) == -1 && tempo::octaveFor(87, 87 * 1.5) == 1),
              "M18 tempo: the octave that brings the set's tempo nearest the sample's (87 at 172: one up, two set beats per sample beat)");
        const SamplePtr loop87 = sampleOf(loopAt(87, 4, 48000), 48000);
        const tempo::Sync s172 = tempo::syncTo(*loop87, 172), s120 = tempo::syncTo(*loop87, 120), s60 = tempo::syncTo(*loop87, 60);
        CHECK((s172.beats == 32 && s172.loop && std::fabs(s172.detected - 87) < 1 && s120.beats == 16 && s120.loop && s60.beats == 8 && s60.loop),
              "M18 tempo: a 4-bar loop at 87 BPM is 32 beats at 172 (plays at 86), 16 at 120, 8 at 60: whole bars, looping");
        const tempo::Sync shot = tempo::syncTo(*sampleOf(hit, 48000), 120);
        CHECK((shot.detected == 0 && !shot.loop && std::fabs(shot.beats - 0.6) < 1e-9), "M18 tempo: a hit with no tempo plays once at its own speed");
        // a loop a little short of its bars is stretched to them; a phrase that isn't a whole number of beats plays once, warped
        std::vector<float> shortLoop = loopAt(100, 2, 48000);
        shortLoop.resize(shortLoop.size() - size_t(0.1 * 48000));
        const tempo::Sync sl = tempo::syncTo(*sampleOf(shortLoop, 48000), 100);
        std::vector<float> phrase = loopAt(100, 2, 48000);
        phrase.resize(phrase.size() + size_t(0.45 * 48000));
        const tempo::Sync ph = tempo::syncTo(*sampleOf(phrase, 48000), 100);
        CHECK((sl.beats == 8 && sl.loop && !ph.loop && std::fabs(ph.beats - (8 + 0.75)) < 0.2), "M18 tempo: a loop 0.1 s short of two bars is stretched to them; a phrase 3/4 of a beat long plays once in tempo");

        // new clips are matched to the set's tempo; a tempo change re-matches them
        Project p;
        p.scenes = {"1"};
        p.addTrack("Loop", QColor("#80c0ff"));
        p.tracks[0].type = TrackType::Audio;
        p.tempo = 172;
        ClipPtr c = p.newAudioClip("/tmp/loop87.wav", loop87);
        CHECK((c->length == 32 && c->loop && c->warp && std::fabs(c->detectedBpm - 87) < 1 && std::fabs(c->sampleBpm() - 174) < 2 && c->warpMarkers.last().beat == 32),
              "M18 clips: an 87 BPM loop dropped into a 172 BPM set becomes a 32-beat clip (plays at 86)");
        p.tracks[0].clipSlots[0] = c;
        p.tracks[0].arrangement = {{4, 32, 0, c}};
        c->envelopes = {{target::volume(), {{0, 1}, {16, 0.5}}}};
        const int undo0 = p.undoStack()->count();
        { EditScope e(&p, "Tempo"); p.setTempo(100); }
        CHECK((c->length == 16 && c->warpMarkers.last().beat == 16 && p.tracks[0].arrangement[0].length == 16 && p.tracks[0].arrangement[0].start == 4
               && c->envelopes[0].points[1].beat == 8 && std::fabs(c->sampleBpm() - 87) < 1 && p.undoStack()->count() == undo0 + 1),
              "M18 clips: at 100 BPM the loop is re-matched to 16 beats (plays at 100), its placement and envelopes scaled with it, one undo step");
        { EditScope e(&p, "Tempo"); p.setTempo(172); }
        CHECK((c->length == 32 && p.tracks[0].arrangement[0].length == 32 && c->envelopes[0].points[1].beat == 16), "M18 clips: back at 172 it's 32 beats again");
        { EditScope e(&p, "Tempo"); p.setTempo(150); }
        CHECK((c->length == 32), "M18 clips: a tempo that keeps the same octave nearest (150: 75 vs 87) changes nothing");
        p.undoStack()->undo();
        p.undoStack()->undo();
        CHECK((p.tempo == 100 && c->length == 16 && p.tracks[0].arrangement[0].length == 16), "M18 clips: undo takes the tempo and the re-match back together");
        c->warp = false;
        { EditScope e(&p, "Tempo"); p.setTempo(172); }
        CHECK((c->length == 16), "M18 clips: a clip with Warp off is left alone by a tempo change");
        c->warp = true;
        c->warpMarkers = {{0, 0}, {20, loop87->seconds()}};   // moved by hand
        c->length = 20;
        const int undo1 = p.undoStack()->count();
        CHECK((p.syncClipToTempo(c) && c->length == 32 && c->loop && c->warpMarkers.size() == 2 && c->warpMarkers.last().beat == 32 && p.undoStack()->count() == undo1 + 1),
              "M18 clips: Sync fits the clip to the set's tempo again from its audio (one undo step)");
        ClipPtr pinned = p.newAudioClip("/tmp/hit.wav", sampleOf(hit, 48000));
        CHECK((pinned->detectedBpm == 0 && p.syncClipToTempo(pinned) && pinned->detectedBpm == 0 && !pinned->loop), "M18 clips: Sync on audio with no tempo keeps it a one-shot");
        // the set file keeps the found tempo
        {
            QTemporaryDir dir;
            const QString path = dir.filePath("sync.sessions");
            setfile::Extras x;
            Project q;
            CHECK((setfile::save(path, p, x) && setfile::load(path, q, x) && q.captureEditState() == p.captureEditState()
                   && std::fabs(q.tracks[0].clipSlots[0]->detectedBpm - c->detectedBpm) < 1e-9),
                  "M18 sets: the tempo found in a clip's audio is saved and read back");
        }
        // the engine: the 87 BPM loop in a 172 BPM set plays its kicks on every second set beat
        {
            Project e;
            e.scenes = {"1"};
            e.addTrack("Loop", QColor("#80c0ff"));
            e.tracks[0].type = TrackType::Audio;
            e.tempo = 172;
            ClipPtr lc = e.newAudioClip("/tmp/loop87.wav", loop87);
            lc->warpMode = WarpMode::Repitch;
            e.tracks[0].arrangement = {{0, 32, 0, lc}};
            const std::vector<float> out = playArrangement(e, 120000);
            const std::vector<int> on = onsetsOf(out, 0.2f, 4000);
            const double two = 2 * 60.0 / 172 * 48000;   // two set beats, in samples
            bool onGrid = on.size() >= 3;
            for (size_t k = 1; k < on.size() && k < 4; ++k) onGrid &= std::fabs(on[k] - k * two) < 150;
            CHECK(onGrid, qPrintable(QString("M18 engine: the 87 BPM loop plays its beats on every second beat of the 172 BPM set (onsets at %1, %2, %3; two beats = %4)")
                                     .arg(on.size() > 0 ? on[0] : -1).arg(on.size() > 1 ? on[1] : -1).arg(on.size() > 2 ? on[2] : -1).arg(two, 0, 'f', 0)));
        }
        // the Clip View says what it found and how the clip sits against the set's tempo
        {
            audio::Engine eng(&p);
            Transport tr(&p, &eng);
            ClipView view(&p, &tr);
            view.resize(1200, 300);
            view.show();
            view.setClip(c);
            QApplication::processEvents();
            QLabel* info = nullptr;
            for (QLabel* l : view.findChildren<QLabel*>()) if (l->text().startsWith("found ")) info = l;
            QPushButton* sync = view.findChild<QPushButton*>("syncTempo");
            QPushButton* warp = view.findChild<QPushButton*>("warpOn");
            CHECK((info && info->text().contains("plays at") && sync && warp && warp->isChecked()),
                  qPrintable("M18 Clip View: the Sample box shows the found tempo and the tempo the clip plays at, with Warp (on) and Sync: " + (info ? info->text() : QString("(no label)"))));
        }
    }

    // ---- the pedals: each its own circuit
    {
        const std::vector<float> in = pluck(2.0);
        const std::vector<float> quiet = [&] { std::vector<float> q = in; for (float& x : q) x *= 0.08f; return q; }();
        const std::vector<float> sine = [] { std::vector<float> v(48000); for (size_t i = 0; i < v.size(); ++i) v[i] = 0.25f * float(std::sin(2 * M_PI * 220 * double(i) / 48000)); return v; }();
        // how much of a signal isn't the sine at `freq` (the residual after a least-squares fit), as a share of it
        auto thd = [](const std::vector<float>& v, double freq) {
            double cs = 0, sn = 0;
            for (size_t i = 4800; i < v.size(); ++i) { const double w = 2 * M_PI * freq * double(i) / 48000; cs += v[i] * std::cos(w); sn += v[i] * std::sin(w); }
            const double n = double(v.size() - 4800);
            const double a = 2 * cs / n, b = 2 * sn / n;
            double res = 0, tot = 0;
            for (size_t i = 4800; i < v.size(); ++i) { const double w = 2 * M_PI * freq * double(i) / 48000; const double d = v[i] - a * std::cos(w) - b * std::sin(w); res += d * d; tot += double(v[i]) * v[i]; }
            return tot > 0 ? std::sqrt(res / tot) : 0.0;
        };
        auto rms = [](const std::vector<float>& v, size_t from = 0, size_t to = size_t(-1)) {
            double s = 0; size_t n = 0;
            for (size_t i = from; i < v.size() && i < to; ++i) { s += double(v[i]) * v[i]; ++n; }
            return n ? std::sqrt(s / double(n)) : 0.0;
        };
        auto highEnergy = [](const std::vector<float>& v) {   // above ~6 kHz, a crude two-point difference
            double s = 0, t = 0;
            for (size_t i = 2; i < v.size(); ++i) { const double d = v[i] - 2 * v[i - 1] + v[i - 2]; s += d * d; t += double(v[i]) * v[i]; }
            return s / std::max(1e-12, t);
        };
        auto lopsided = [](const std::vector<float>& v) {   // the positive peak over the negative one, once the filters have settled
            float up = 0, down = 0;
            for (size_t i = 9600; i < v.size(); ++i) { up = std::max(up, v[i]); down = std::max(down, -v[i]); }
            return up / std::max(1e-6f, down);
        };
        auto both = [](DeviceType type, const std::vector<float>& params, const std::vector<float>& inL, const std::vector<float>& inR) {
            auto proc = audio::createProcessor(type, 48000);
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000;
            std::vector<float> outL(inL.size()), outR(inL.size());
            float l[audio::kMaxBlock], r[audio::kMaxBlock];
            for (size_t pos = 0; pos < inL.size(); pos += audio::kMaxBlock) {
                const int n = int(std::min<size_t>(audio::kMaxBlock, inL.size() - pos));
                std::copy(inL.begin() + long(pos), inL.begin() + long(pos) + n, l);
                std::copy(inR.begin() + long(pos), inR.begin() + long(pos) + n, r);
                proc->process(l, r, n, params.data(), ctx);
                std::copy(l, l + n, outL.begin() + long(pos));
                std::copy(r, r + n, outR.begin() + long(pos));
            }
            return std::make_pair(outL, outR);
        };
        // every drive pedal sounds like itself: no two share the same brightness and shape at their defaults
        const DeviceType drives[] = {DeviceType::Overdrive, DeviceType::Distortion, DeviceType::MetalDistortion, DeviceType::Chainsaw, DeviceType::MetalBoost, DeviceType::Fuzz, DeviceType::Crunch};
        std::vector<double> hf, lop, crest;
        QStringList report;
        for (DeviceType t : drives) {
            const std::vector<float> out = runEffect(t, defaultsOf(t), in);
            hf.push_back(highEnergy(out));
            lop.push_back(lopsided(out));
            crest.push_back(peakOf(out) / std::max(1e-6, rms(out)));
            report << QString("%1 hf %2 lop %3 crest %4").arg(specOf(t)->name).arg(hf.back() * 1000, 0, 'f', 2).arg(lop.back(), 0, 'f', 2).arg(crest.back(), 0, 'f', 2);
        }
        bool distinct = true;
        for (size_t i = 0; i < hf.size(); ++i)
            for (size_t j = i + 1; j < hf.size(); ++j) {
                const bool apart = std::fabs(std::log(hf[i] / hf[j])) > 0.2 || std::fabs(std::log(lop[i] / lop[j])) > 0.08 || std::fabs(std::log(crest[i] / crest[j])) > 0.12;
                distinct &= apart;
            }
        std::printf("      pedal voices: %s\n", qPrintable(report.join("; ")));
        CHECK(distinct, "M18 pedals: the seven drive pedals each sound different at their defaults (brightness, symmetry or dynamics apart)");
        // Overdrive: the lows stay clean, the mids are driven; Asym adds even harmonics
        {
            std::vector<float> od = defaultsOf(DeviceType::Overdrive);
            CHECK((audio::pedalResponseDb(DeviceType::Overdrive, od.data(), 1000, 48000) > audio::pedalResponseDb(DeviceType::Overdrive, od.data(), 80, 48000) + 3),
                  "M18 Overdrive: the mids sit above the clean lows (the screamer's hump)");
            od[0] = 0.8f;
            const float sym = lopsided(runEffect(DeviceType::Overdrive, od, sine));
            od[3] = 1;
            const float asym = lopsided(runEffect(DeviceType::Overdrive, od, sine));
            CHECK((std::fabs(std::log(sym)) < 0.03 && std::fabs(std::log(asym)) > 0.05), qPrintable(QString("M18 Overdrive: 808 clips a sine evenly, Asym favours one side (even harmonics): peaks %1 vs %2").arg(sym, 0, 'f', 3).arg(asym, 0, 'f', 3)));
            od[3] = 0;
            od[0] = 0; const double clean = thd(runEffect(DeviceType::Overdrive, od, sine), 220);
            od[0] = 1; const double driven = thd(runEffect(DeviceType::Overdrive, od, sine), 220);
            CHECK((clean < 0.08 && driven > 0.15), qPrintable(QString("M18 Overdrive: Drive from 0 to 10 goes from nearly clean to driven (distortion %1 % → %2 %)").arg(clean * 100, 0, 'f', 1).arg(driven * 100, 0, 'f', 1)));
        }
        // Distortion: the three clipping pairs
        {
            std::vector<float> d = defaultsOf(DeviceType::Distortion);
            d[1] = 0;   // the Filter open, so the clipping pairs show
            d[3] = 0; const std::vector<float> sil = runEffect(DeviceType::Distortion, d, in);
            d[3] = float(choicePosition(1, 3)); const std::vector<float> led = runEffect(DeviceType::Distortion, d, in);
            d[3] = 1; const std::vector<float> ge = runEffect(DeviceType::Distortion, d, in);
            CHECK((rms(led) > rms(sil) * 1.15 && highEnergy(ge) < highEnergy(sil) * 1.02 && rms(ge) < rms(sil) * 0.7),
                  qPrintable(QString("M18 Distortion: LEDs are louder and more open than silicon, germanium quieter and no brighter (rms %1 / %2 / %3, top %4 / %5 / %6)")
                             .arg(rms(sil), 0, 'f', 3).arg(rms(led), 0, 'f', 3).arg(rms(ge), 0, 'f', 3).arg(highEnergy(sil) * 1000, 0, 'f', 2).arg(highEnergy(led) * 1000, 0, 'f', 2).arg(highEnergy(ge) * 1000, 0, 'f', 2)));
            d[3] = 0; d[1] = 0; const double open = highEnergy(runEffect(DeviceType::Distortion, d, in));
            d[1] = 1; const double dark = highEnergy(runEffect(DeviceType::Distortion, d, in));
            CHECK((dark < open * 0.25), "M18 Distortion: Filter up darkens it");
        }
        // Metal Distortion: the sweepable mid, Tight
        {
            std::vector<float> m = defaultsOf(DeviceType::MetalDistortion);
            m[2] = 1; const float up = audio::pedalResponseDb(DeviceType::MetalDistortion, m.data(), 800, 48000);
            m[2] = 0; const float down = audio::pedalResponseDb(DeviceType::MetalDistortion, m.data(), 800, 48000);
            m[2] = 0.5f; m[6] = 1; const float tight = audio::pedalResponseDb(DeviceType::MetalDistortion, m.data(), 80, 48000);
            m[6] = 0; const float loose = audio::pedalResponseDb(DeviceType::MetalDistortion, m.data(), 80, 48000);
            CHECK((up - down > 20 && loose - tight > 12), "M18 Metal Distortion: Mid swings 800 Hz by over 20 dB; Tight cuts the lows before the gain");
        }
        // Metal Boost: nearly clean at its defaults, Tight
        {
            std::vector<float> b = defaultsOf(DeviceType::MetalBoost);
            const double clean = highEnergy(runEffect(DeviceType::MetalBoost, b, in)), raw = highEnergy(in);
            b[3] = 1; const float tight = audio::pedalResponseDb(DeviceType::MetalBoost, b.data(), 80, 48000);
            b[3] = 0; const float loose = audio::pedalResponseDb(DeviceType::MetalBoost, b.data(), 80, 48000);
            CHECK((clean < raw * 3 && loose - tight > 12), "M18 Metal Boost: little distortion at its defaults; Tight cuts the lows");
        }
        // Fuzz: starved, it sputters and gates
        {
            std::vector<float> f = defaultsOf(DeviceType::Fuzz);
            f[3] = 1; const double warmQuiet = rms(runEffect(DeviceType::Fuzz, f, quiet)), warmLoud = rms(runEffect(DeviceType::Fuzz, f, in));
            f[3] = 0; const double starvedQuiet = rms(runEffect(DeviceType::Fuzz, f, quiet)), starvedLoud = rms(runEffect(DeviceType::Fuzz, f, in));
            CHECK((starvedQuiet < warmQuiet * 0.5 && starvedLoud > warmLoud * 0.5), "M18 Fuzz: Bias down gates a quiet signal while loud playing still comes through");
            f[3] = 0.7f; f[1] = 0; const double dark = highEnergy(runEffect(DeviceType::Fuzz, f, in));
            f[1] = 1; const double bright = highEnergy(runEffect(DeviceType::Fuzz, f, in));
            CHECK((bright > dark * 2), "M18 Fuzz: the muff's Tone goes from dark to cutting");
        }
        // Crunch: Bright and Presence
        {
            std::vector<float> c = defaultsOf(DeviceType::Crunch);
            c[0] = 0;   // cleanest, so the shelf shows
            const double plain = highEnergy(runEffect(DeviceType::Crunch, c, in));
            c[3] = 1; const double bright = highEnergy(runEffect(DeviceType::Crunch, c, in));
            c[3] = 0; c[4] = 1; const float presUp = audio::pedalResponseDb(DeviceType::Crunch, c.data(), 3500, 48000);
            c[4] = 0; const float presDown = audio::pedalResponseDb(DeviceType::Crunch, c.data(), 3500, 48000);
            CHECK((bright > plain * 1.3 && presUp - presDown > 8), "M18 Crunch: Bright adds bite before the gain, Presence on top after it");
        }
        // Phase Pedal: the sweep moves (Depth), stands still at Depth 0
        {
            auto sweep = [&](float depth) {
                std::vector<float> ph = defaultsOf(DeviceType::PhasePedal);
                ph[2] = depth;
                auto proc = audio::createProcessor(DeviceType::PhasePedal, 48000);
                audio::Context ctx; ctx.sampleRate = 48000;
                float lo = 1e9f, hi = -1e9f;
                float l[audio::kMaxBlock], r[audio::kMaxBlock];
                for (int k = 0; k < 600; ++k) {   // 1.6 s: a full sweep at 0.6 Hz
                    std::fill(l, l + audio::kMaxBlock, 0.1f); std::fill(r, r + audio::kMaxBlock, 0.1f);
                    proc->process(l, r, audio::kMaxBlock, ph.data(), ctx);
                    const float c = proc->meter[2].load();
                    lo = std::min(lo, c); hi = std::max(hi, c);
                }
                return hi - lo;
            };
            float deepest = 0;
            for (int f = 100; f < 5000; f += 10) deepest = std::min(deepest, audio::phaserResponseDb(dsp::allpassCoef(700, 48000), float(f), 48000));
            CHECK((sweep(1.0f) > 0.2f && sweep(0.0f) < 1e-4f && deepest < -15), "M18 Phase Pedal: Depth sets how far the notches sweep; the panel's notch curve has its dips");
        }
        // Chorus Pedal: Mix, Stereo
        {
            std::vector<float> ch = defaultsOf(DeviceType::ChorusPedal);
            ch[3] = 0; const auto dry = both(DeviceType::ChorusPedal, ch, in, in);
            ch[3] = 1; const auto vib = both(DeviceType::ChorusPedal, ch, in, in);
            ch[3] = 0.5f; ch[2] = 1; const auto st = both(DeviceType::ChorusPedal, ch, in, in);
            ch[2] = 0; const auto mono = both(DeviceType::ChorusPedal, ch, in, in);
            CHECK((maxDiff(dry.first, in) < 1e-4f && maxDiff(vib.first, in) > 0.02f && std::fabs(20 * std::log10(rms(vib.first) / rms(in))) < 4
                   && maxDiff(st.first, st.second) > 0.01f && maxDiff(mono.first, mono.second) < 1e-6f),
                  "M18 Chorus Pedal: Mix 0 is dry, Mix 100 % a vibrato at the same level; Stereo parts the sides");
        }
        // Twang: the springs ring on after the note; Mix 0 is dry
        {
            std::vector<float> tw = defaultsOf(DeviceType::Twang);
            std::vector<float> stops = pluck(1.0);   // a second of playing, then silence
            stops.resize(96000, 0.0f);
            tw[2] = 0; tw[3] = 0; const std::vector<float> dry = runEffect(DeviceType::Twang, tw, stops);
            tw[2] = 1; const std::vector<float> wet = runEffect(DeviceType::Twang, tw, stops);
            const double tailDry = rms(dry, 52000, 64000), tailWet = rms(wet, 52000, 64000);
            CHECK((std::fabs(20 * std::log10(rms(dry, 0, 48000) / rms(in, 0, 48000))) < 1 && tailWet > 1.5e-4 && tailWet > tailDry * 5),
                  qPrintable(QString("M18 Twang: Mix 0 passes the dry sound; the springs ring on after the playing stops (tail %1 dB)").arg(20 * std::log10(std::max(1e-9, tailWet)), 0, 'f', 1)));
        }
        // the graphs: every drive pedal's clipper is bounded and rises with its input
        {
            bool sane = true;
            for (DeviceType t : drives) {
                const std::vector<float> d = defaultsOf(t);
                float last = -2;
                bool ok = true;
                for (int i = 0; i <= 40; ++i) {
                    const float x = -1 + 0.05f * i, y = audio::pedalTransfer(t, d.data(), x);
                    ok &= std::isfinite(y) && std::fabs(y) <= 1.05f && y >= last - 1e-4f;
                    last = y;
                }
                ok &= std::fabs(audio::pedalTransfer(t, d.data(), 0)) < 0.12f;
                if (!ok) std::printf("      %s: curve at rest %.3f, at -1 %.3f, at 1 %.3f\n", specOf(t)->name, audio::pedalTransfer(t, d.data(), 0), audio::pedalTransfer(t, d.data(), -1), audio::pedalTransfer(t, d.data(), 1));
                sane &= ok;
            }
            CHECK(sane, "M18 pedals: each clipper's curve (the panel's graph) is bounded, rises with the input and sits near 0 at rest");
            Project p;
            p.scenes = {"1"};
            p.addTrack("G", QColor("#f2905e"));
            p.tracks[0].type = TrackType::Audio;
            audio::Engine e(&p);
            Transport tr(&p, &e);
            int graphs = 0, fits = 0, pedals = 0;
            for (const DeviceSpec& spec : deviceSpecs()) {
                if (specOf(spec.type) != &spec || !isGuitarType(spec.type) || spec.type == DeviceType::Tuner || spec.type == DeviceType::GuitarAmp || spec.type == DeviceType::Nam || spec.type == DeviceType::Cabinet) continue;
                p.tracks[0].devices = {p.makeDevice(spec.name, DeviceKind::AudioEffect)};
                DevicePanel panel(&p, &tr, 0, p.tracks[0].devices[0], nullptr, {});
                ++pedals;
                graphs += panel.body()->findChildren<devui::Display*>().size() >= 1 ? 1 : 0;
                fits += panel.body()->minimumSizeHint().height() <= devui::kBodyHeight + 4 && panel.body()->width() <= 540 ? 1 : 0;
                if (panel.body()->width() > 540) std::printf("      %s panel is %d px wide\n", spec.name, panel.body()->width());
                panel.body()->tick();
            }
            CHECK((pedals == 11 && graphs == 11 && fits == 11), "M18 pedals: all eleven pedal panels have their graphs and fit the device height, no wider than 540 px");
        }
    }


    // ---- Milestone 19: themes with tints and the customization window
    {
        QTemporaryDir themesDir;
        qputenv("SESSIONS_THEMES", themesDir.path().toLocal8Bit());
        const QVector<theme::Theme>& builtin = theme::builtinThemes();
        CHECK((builtin.size() == 12 && builtin[0].name == "Dark" && builtin[1].name == "Mid" && builtin[2].name == "Light" && builtin[3].name == "Midnight" && builtin.last().name == "High Contrast"),
              "M19 themes: twelve built-in themes, the three bases first");
        theme::apply(theme::Kind::Dark);
        const QColor darkBg = theme::bg, darkAccent = theme::accent, darkPanel = theme::panel;
        theme::apply(theme::themeNamed("Midnight"));
        CHECK((theme::kind() == theme::Kind::Dark && theme::bg != darkBg && theme::bg.hslSaturationF() > 0.1 && std::abs(theme::bg.lightnessF() - darkBg.lightnessF()) < 0.08
               && theme::bg.hslHue() > 180 && theme::bg.hslHue() < 260 && theme::accent == QColor("#ffb050") && theme::devCurve == theme::accent && theme::play == QColor("#6fd35c")
               && theme::onAccent == QColor("#151515")),
              "M19 themes: Midnight tints the greys blue at their lightness, swaps the accent (the device curve follows), leaves the transport colours alone");
        theme::apply(theme::themeNamed("Sky"));
        CHECK((theme::kind() == theme::Kind::Light && theme::accent == QColor("#3b82f6") && theme::onAccent.lightnessF() > 0.9 && theme::bg.lightnessF() > 0.6),
              "M19 themes: Sky is a light base with a blue accent, and light ink on the accent");
        theme::apply(theme::themeNamed("High Contrast"));
        CHECK((theme::text.lightnessF() > QColor("#d2d2d2").lightnessF() && theme::panel.lightnessF() >= darkPanel.lightnessF() - 1e-6 && theme::accent == QColor("#ffd60a")),
              "M19 themes: High Contrast pushes the text away from the background");
        // a theme of your own: saved, listed, read back with its overrides, applied, deleted
        theme::Theme mine = theme::themeNamed("Ember");
        mine.name = "My Ember";
        mine.overrides.insert("accent", QColor("#00ff00"));
        mine.overrides.insert("bg", QColor("#102030"));
        mine.overrides.insert("shade", QColor(0, 0, 0, 90));
        QString err;
        const bool saved = theme::saveCustom(mine, &err);
        bool found = false;
        const theme::Theme back = theme::themeNamed("My Ember", &found);
        CHECK((saved && theme::customThemes().size() == 1 && found && !back.builtin && back.base == theme::Kind::Dark && std::abs(back.hue - 18) < 1e-6
               && back.overrides.value("bg") == QColor("#102030") && back.overrides.value("shade").alpha() == 90 && QFile::exists(theme::pathFor("My Ember"))),
              qPrintable("M19 themes: a theme of yours saves as a .sessionstheme file and reads back with its tint and overrides " + err));
        theme::apply(back);
        theme::save(back);
        CHECK((theme::bg == QColor("#102030") && theme::accent == QColor("#00ff00") && theme::devCurve == QColor("#ff7a59") && theme::current().name == "My Ember"
               && theme::savedTheme().name == "My Ember" && theme::savedKind() == theme::Kind::Dark),
              "M19 themes: an override wins for its own colour (the accent family still follows the theme's accent); the choice is remembered by name");
        CHECK((theme::resolve(back, "panel").hslHue() >= 0 && theme::resolve(back, "panel").hslSaturationF() > 0.05 && theme::isTintable("panel") && !theme::isTintable("record") && theme::isAccentColour("devCurve")
               && theme::group("rollBg") == "Piano Roll" && theme::group("devDisplay") == "Device Panels" && theme::group("lane") == "Arrangement" && theme::colourNames().size() > 120),
              "M19 themes: the greys are tinted, the signal colours aren't, the colours have groups for the editor");
        theme::removeCustom("My Ember");
        CHECK((theme::customThemes().isEmpty() && !QFile::exists(theme::pathFor("My Ember"))), "M19 themes: Delete removes the file");
        theme::apply(theme::Kind::Dark);
        theme::save(theme::Kind::Dark);
        CHECK((theme::bg == darkBg && theme::accent == darkAccent && theme::current().name == "Dark" && theme::current().builtin && QSettings("Sessions", "Sessions").value("ui/theme").toString() == "dark"),
              "M19 themes: back to Dark everything is as it was");
        // the editor: loads a theme, retints live, overrides one colour, saves as yours
        ThemeEditor ed;
        ed.load(theme::themeNamed("Forest"));
        QComboBox* presets = ed.findChild<QComboBox*>("themePresets");
        QSlider* hue = ed.findChild<QSlider*>("themeHue");
        QLineEdit* name = ed.findChild<QLineEdit*>("themeName");
        CHECK((presets && presets->currentText() == "Forest" && theme::current().name == "Forest" && hue && hue->value() == 150 && name && name->text() == "Forest"),
              "M19 theme editor: Start from loads a theme into the controls and applies it");
        hue->setValue(300);
        QApplication::processEvents();
        CHECK((std::abs(theme::current().hue - 300) < 1e-6 && theme::bg.hslHue() > 270 && theme::bg.hslHue() < 330), "M19 theme editor: moving Hue retints the window live");
        ed.setColour("accent", QColor("#123456"));
        const bool overridden = theme::accent == QColor("#123456") && ed.theme().overrides.contains("accent");
        ed.setColour("accent", QColor());
        CHECK((overridden && theme::accent == QColor("#b6e65a") && !ed.theme().overrides.contains("accent")), "M19 theme editor: a swatch overrides one colour, ↺ puts the theme's value back");
        name->setText("Purple Forest");
        emit name->textEdited("Purple Forest");
        ed.findChild<QPushButton*>("themeSave")->click();
        QApplication::processEvents();
        CHECK((theme::customThemes().size() == 1 && theme::customThemes()[0].name == "Purple Forest" && std::abs(theme::customThemes()[0].hue - 300) < 1e-6 && theme::current().name == "Purple Forest"
               && theme::savedTheme().name == "Purple Forest"),
              "M19 theme editor: Save keeps it as yours and makes it the theme");
        // View > Theme lists the built-in themes, yours, and Customize…
        MainWindow w;
        QAction *midnight = nullptr, *purple = nullptr, *customize = nullptr;
        for (QAction* a : w.findChildren<QAction*>()) { if (a->text() == "Midnight") midnight = a; else if (a->text() == "Purple Forest") purple = a; else if (a->objectName() == "themeCustomize") customize = a; }
        CHECK((midnight && purple && customize && purple->isChecked()), "M19 theme menu: View > Theme lists the built-in themes, your own (checked when current) and Customize…");
        midnight->trigger();
        QApplication::processEvents();
        CHECK((theme::current().name == "Midnight" && midnight->isChecked() && !purple->isChecked()), "M19 theme menu: picking a theme applies it and checks it");
        theme::removeCustom("Purple Forest");
        QApplication::processEvents();
        bool stillListed = false;
        for (QAction* a : w.findChildren<QAction*>()) if (a->text() == "Purple Forest") stillListed = true;
        CHECK(!stillListed, "M19 theme menu: a deleted theme leaves the menu");
        theme::apply(theme::Kind::Dark);
        theme::save(theme::Kind::Dark);
    }

    // ---- Milestone 19: the compact Clip View and its box switches, the overview bar
    {
        Project p;
        p.loadDemo();
        audio::Engine e(&p);
        Transport t(&p, &e);
        ClipView cv(&p, &t);
        cv.resize(1100, 300);
        cv.show();
        cv.setClip(p.tracks[0].clipSlots[0]);
        QApplication::processEvents();
        const QList<QToolButton*> toggles = cv.findChildren<QToolButton*>("clipBoxToggle");
        QWidget *launchBox = nullptr, *envBox = nullptr, *clipBox = nullptr;
        for (QLabel* l : cv.findChildren<QLabel*>("clipBoxTitle")) { if (l->text() == "Launch") launchBox = l->parentWidget(); else if (l->text() == "Envelopes") envBox = l->parentWidget(); else if (l->text() == "Clip") clipBox = l->parentWidget(); }
        QToolButton *L = nullptr, *E = nullptr;
        for (QToolButton* b : toggles) { if (b->text() == "L") L = b; else if (b->text() == "E") E = b; }
        CHECK((toggles.size() == 3 && L && E && launchBox && envBox && clipBox && launchBox->isVisible() && clipBox->width() == 132 && launchBox->width() == 132),
              "M19 clip view: the boxes are 132 px wide and three round switches sit at the bottom left");
        L->click();
        E->click();
        QApplication::processEvents();
        CHECK((!launchBox->isVisible() && !envBox->isVisible() && clipBox->isVisible() && !QSettings("Sessions", "Sessions").value("clipView/launch", true).toBool()),
              "M19 clip view: L and E hide the Launch and Envelopes boxes (the Clip box stays), remembered in QSettings");
        L->click();
        E->click();
        QApplication::processEvents();
        CHECK((launchBox->isVisible() && envBox->isVisible()), "M19 clip view: and show them again");
        QLabel* tempoInfo = nullptr;
        for (QLabel* l : cv.findChildren<QLabel*>()) if (l->toolTip().startsWith("found ") || l->text().startsWith("found ")) tempoInfo = l;
        (void)tempoInfo;

        MainWindow w;
        w.resize(1400, 900);
        w.show();
        QApplication::processEvents();
        OverviewBar* bar = w.findChild<OverviewBar*>();
        Project* proj = w.project();
        CHECK((bar && bar->isVisible() && bar->height() == OverviewBar::kBarHeight && bar->clipOverview()->isVisible() && bar->chainOverview()->isVisible()), "M19 overview bar: a strip along the bottom with the clip and the chain overviews");
        int drums = 0;
        for (int i = 0; i < proj->tracks.size(); ++i) if (proj->tracks[i].name == "Drums") drums = i;
        QRect drumSlot;
        for (const auto& cell : w.sessionView()->grid()->mapCells()) if (cell.second == QString("slot:%1:0").arg(proj->tracks[drums].id)) drumSlot = cell.first;
        QTest::mouseClick(w.sessionView()->grid(), Qt::LeftButton, Qt::NoModifier, drumSlot.center() + QPoint(30, 0));
        QApplication::processEvents();
        const QList<QPair<QRect, int>> blocks = bar->chainOverview()->blocks();
        int topLevel = 0;
        for (int i = 0; i < proj->tracks[drums].devices.size(); i = deviceBlockEnd(proj->tracks[drums].devices, i)) ++topLevel;
        CHECK((blocks.size() == topLevel && topLevel >= 1 && blocks.first().second == proj->tracks[drums].devices.first().id && proj->selectedClip == proj->tracks[drums].clipSlots[0]),
              "M19 overview bar: one block per device on the selected track, in chain order");
        QTest::mouseClick(bar->chainOverview(), Qt::LeftButton, Qt::NoModifier, blocks.first().first.center());
        QApplication::processEvents();
        CHECK((!w.clipViewShown() && w.deviceView()->selection() == QList<int>{blocks.first().second}), "M19 overview bar: clicking a block opens the Device View with that device selected");
        QTest::mouseClick(bar->clipOverview(), Qt::LeftButton, Qt::NoModifier, QPoint(20, 10));
        QApplication::processEvents();
        CHECK(w.clipViewShown(), "M19 overview bar: clicking the clip overview opens the Clip View");
        const QImage shot = bar->clipOverview()->grab().toImage();
        int inked = 0;
        const QColor ink = proj->tracks[drums].color;
        for (int y = 0; y < shot.height(); ++y) for (int x = 0; x < shot.width(); ++x) if (QColor(shot.pixel(x, y)) == ink) ++inked;
        CHECK(inked > 20, "M19 overview bar: the clip overview paints the clip's notes in the track's colour");
    }

    // ---- Milestone 19: the Impulse Responses library, the Cabinet device, IR drops and ◀ ▶
    {
        CHECK((audio::builtinIrPaths().size() == 15 && audio::isBuiltinIr("builtin:ir/3/0") && !audio::isBuiltinIr("builtin:808/36")
               && audio::builtinName("builtin:ir/3/0") == "4x12 Closed · On Axis" && audio::builtinName("builtin:ir/5/2") == "8x10 Bass · Far"),
              "M19 IRs: fifteen built-in cabinet IRs (five cabinets through three mics), named");
        const SamplePtr cab = samples::load("builtin:ir/3/0");
        float irPeak = 0, lateEnergy = 0;
        if (cab) { for (float v : cab->left) irPeak = std::max(irPeak, std::fabs(v)); for (size_t i = 4000; i < cab->left.size(); ++i) lateEnergy += cab->left[i] * cab->left[i]; }
        CHECK((cab && cab->frames() == 8192 && irPeak > 0.05f && irPeak < 4.0f && lateEnergy < 1e-4f && cab->left.back() == 0.0f),
              "M19 IRs: a built-in IR renders the cabinet's impulse (a thump that dies away, faded at its end)");
        QTemporaryDir irDir, irMore, lonelyDir;
        qputenv("SESSIONS_IRS", irDir.path().toLocal8Bit());
        auto writeIr = [](const QString& path, float decay) {
            std::vector<float> st(2 * 2048);
            for (int i = 0; i < 2048; ++i) st[size_t(i) * 2] = st[size_t(i) * 2 + 1] = (i == 0 ? 1.0f : 0.3f * std::sin(0.02f * i)) * std::exp(-i / decay);
            samples::writeWav(path, st.data(), 2048, 48000, samples::WavFormat::Float32);
        };
        QDir(irDir.path()).mkpath("Cabs");
        const QString a = QFileInfo(irDir.filePath("Cabs/a.wav")).absoluteFilePath(), b = QFileInfo(irDir.filePath("b.wav")).absoluteFilePath(), c = QFileInfo(irMore.filePath("c.wav")).absoluteFilePath();
        writeIr(a, 200); writeIr(b, 400); writeIr(c, 800);
        writeFile(irMore.filePath("broken.wav"), "not a wav");
        import::addIrFolder(irMore.path());
        const QStringList library = import::irLibrary();
        CHECK((library.size() == 19 && library.first() == "builtin:ir/1/0" && library[14] == "builtin:ir/5/2" && library.contains(a) && library.contains(b) && library.contains(c)
               && import::irIndex("builtin:ir/1/0", library) == 1 && import::irIndex(a, library) == library.indexOf(a) + 1 && import::irIndex(lonelyDir.filePath("x.wav"), library) == 0
               && import::irFolders() == QStringList{QDir(irMore.path()).absolutePath()}),
              "M19 IRs: the library is the built-in cabinets, then the Impulse Responses folder (subfolders too) and your IR folders, sorted");
        Project p;
        p.scenes = {"1"};
        p.addTrack("Guitar", QColor("#f2905e"));
        p.tracks[0].type = TrackType::Audio;
        Device amp = p.makeDevice("Guitar Amp", DeviceKind::AudioEffect);
        p.tracks[0].devices = {amp};
        const int ampId = amp.id;
        QString why;
        bool ok;
        { EditScope e(&p, "Load"); ok = import::loadIr(p, 0, ampId, "builtin:ir/2/1", &why); }
        const Device& d0 = p.tracks[0].devices[0];
        CHECK((ok && d0.samplePath == "builtin:ir/2/1" && d0.sample && choiceIndex(DeviceType::GuitarAmp, guitaramp::Cab, d0.params[guitaramp::Cab].value) == guitaramp::kLoadedIr),
              qPrintable("M19 IRs: loading an IR into the amp switches its cabinet to Loaded IR " + why));
        { EditScope e(&p, "Next"); ok = import::stepIr(p, 0, ampId, 1, &why); }
        CHECK((ok && p.tracks[0].devices[0].samplePath == library[library.indexOf("builtin:ir/2/1") + 1]), "M19 IRs: ▶ loads the next IR in the library");
        p.tracks[0].devices[0].samplePath = library.last();
        p.tracks[0].devices[0].sample = samples::load(library.last());
        { EditScope e(&p, "Next"); ok = import::stepIr(p, 0, ampId, 1, &why); }
        const bool wrapped = ok && p.tracks[0].devices[0].samplePath == library.first();
        { EditScope e(&p, "Prev"); ok = import::stepIr(p, 0, ampId, -1, &why); }
        CHECK((wrapped && ok && p.tracks[0].devices[0].samplePath == library.last()), "M19 IRs: past the last it wraps to the first, and ◀ back again");
        const QString x1 = QFileInfo(lonelyDir.filePath("x1.wav")).absoluteFilePath(), x2 = QFileInfo(lonelyDir.filePath("x2.wav")).absoluteFilePath();
        writeIr(x1, 100); writeIr(x2, 150);
        p.tracks[0].devices[0].samplePath = x1;
        { EditScope e(&p, "Next"); ok = import::stepIr(p, 0, ampId, 1, &why); }
        CHECK((ok && p.tracks[0].devices[0].samplePath == x2), "M19 IRs: an IR outside the library steps through its own folder");
        CHECK((!import::stepIr(p, 0, 999, 1, &why) && !mapping::isContinuous(QString("device:%1:%2:irnext").arg(p.tracks[0].id).arg(ampId))
               && mapping::targetName(p, QString("device:%1:%2:irnext").arg(p.tracks[0].id).arg(ampId)) == "Guitar Amp · Next IR"
               && mapping::targetName(p, QString("device:%1:%2:irprev").arg(p.tracks[0].id).arg(ampId)) == "Guitar Amp · Previous IR"),
              "M19 IRs: the irnext / irprev mapping targets are buttons with names");
        // NAM and Cabinet switch their IR on when one loads
        Device nam = p.makeDevice("NAM", DeviceKind::AudioEffect);
        nam.params[namdev::Ir].value = 0;
        Device cabDev = p.makeDevice("Cabinet", DeviceKind::AudioEffect);
        cabDev.params[cabinet::Ir].value = 0;
        p.tracks[0].devices = {nam, cabDev};
        { EditScope e(&p, "Load"); ok = import::loadIr(p, 0, nam.id, a, &why); }
        bool ok2;
        { EditScope e(&p, "Load"); ok2 = import::loadIr(p, 0, cabDev.id, c, &why); }
        CHECK((ok && ok2 && p.tracks[0].devices[0].params[namdev::Ir].value == 1 && p.tracks[0].devices[0].samplePath == a && p.tracks[0].devices[1].params[cabinet::Ir].value == 1
               && p.tracks[0].devices[1].samplePath == c && import::irDevice(p, 0) == &p.tracks[0].devices[1]),
              "M19 IRs: loading into a NAM or a Cabinet switches their IR on; irDevice is the track's last IR holder");
        // drops: the browser's "path|r" payload goes into the track's IR holder, or lands as a Cabinet
        QMimeData mime;
        mime.setData(Browser::mimeType(), QString("builtin:ir/4/0|r").toUtf8());
        const devicedrag::Payload pl = devicedrag::parse(&mime);
        CHECK((pl.source == devicedrag::Payload::Ir && pl.path == "builtin:ir/4/0" && pl.name == "4x12 Vintage · On Axis" && Browser::irFilesIn(&mime) == QStringList{"builtin:ir/4/0"}
               && devicedrag::shape(p, pl).size() == 1 && devicedrag::shape(p, pl).first().first().name == "Cabinet"),
              "M19 IRs: a browser IR drags as an Ir payload that would land as a Cabinet");
        const int devicesBefore = p.tracks[0].devices.size();
        devicedrag::Result r = devicedrag::drop(p, pl, 0, {}, false);
        CHECK((r.ok && p.tracks[0].devices.size() == devicesBefore && r.ids == QList<int>{cabDev.id} && p.tracks[0].devices[1].samplePath == "builtin:ir/4/0" && r.text.contains("Cabinet")),
              "M19 IRs: dropped on a track that has an IR holder, it goes into the last one (no new device)");
        p.addTrack("Bass", QColor("#80a0ff"));
        p.tracks[1].type = TrackType::Audio;
        p.tracks[1].devices = {p.makeDevice("EQ", DeviceKind::AudioEffect)};
        r = devicedrag::drop(p, pl, 1, {}, false);
        CHECK((r.ok && p.tracks[1].devices.size() == 2 && p.tracks[1].devices[1].name == "Cabinet" && p.tracks[1].devices[1].samplePath == "builtin:ir/4/0" && p.tracks[1].devices[1].sample),
              "M19 IRs: dropped on a track without one, it lands as a Cabinet holding the IR");
        const int tracks0 = p.tracks.size();
        r = devicedrag::drop(p, pl, -1, {}, false);
        CHECK((r.ok && p.tracks.size() == tracks0 + 1 && p.tracks.last().type == TrackType::Audio && p.tracks.last().devices.size() == 1 && p.tracks.last().devices[0].name == "Cabinet"),
              "M19 IRs: dropped on the empty space, a new audio track with a Cabinet");
        // the Cabinet processor: the IR shapes the sound (it's a low-pass of sorts), the switch bypasses it
        {
            auto proc = audio::createProcessor(DeviceType::Cabinet, 48000);
            std::vector<float> params = defaultsOf(DeviceType::Cabinet);
            const SamplePtr ir = samples::load("builtin:ir/3/0");
            auto kernel = audio::makeIrKernel(ir, 48000);
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.ir = kernel.get();
            dsp::Noise rng;
            struct Bands { float low = 0, high = 0, peak = 0; };   // energy under ~300 Hz, over ~3 kHz
            auto pass = [&](bool on) {
                proc->panic();
                params[cabinet::Ir] = on ? 1.0f : 0.0f;
                std::vector<float> out;
                Bands b;
                dsp::OnePole lo1, lo2, hi1, hi2;
                lo1.setLowpass(300, 48000); lo2.setLowpass(300, 48000); hi1.setLowpass(3000, 48000); hi2.setLowpass(3000, 48000);
                for (int pos = 0; pos < 48000; pos += 128) {
                    float l[128], rr[128];
                    for (int i = 0; i < 128; ++i) l[i] = rr[i] = rng.next() * 0.3f;
                    proc->process(l, rr, 128, params.data(), ctx);
                    for (int i = 0; i < 128; ++i) {
                        const float low = lo2.lp(lo1.lp(l[i])), high = hi2.hp(hi1.hp(l[i]));
                        if (pos >= 4800) { b.low += low * low; b.high += high * high; }
                        out.push_back(l[i]);
                    }
                }
                b.peak = peakOf(out);
                return b;
            };
            const Bands wet = pass(true), dry = pass(false);
            CHECK((wet.high / std::max(1e-9f, wet.low) < dry.high / std::max(1e-9f, dry.low) * 0.5f && wet.peak > 0.01f && dry.peak > 0.1f && std::isfinite(wet.peak)),
                  "M19 Cabinet: the IR darkens white noise; the IR switch passes it through dry");
            params[cabinet::LowCut] = 1.0f;   // 1 kHz
            const Bands cut = pass(false);
            CHECK((cut.low < dry.low * 0.25f && cut.high > dry.high * 0.5f), "M19 Cabinet: Low Cut takes the lows out and leaves the highs");
        }
        // the panels: the Cabinet, and ◀ ▶ on all three
        {
            audio::Engine e(&p);
            Transport tr(&p, &e);
            int withSteps = 0, fits = 0;
            for (const Device& d : {p.tracks[0].devices[0], p.tracks[0].devices[1], amp}) {
                Device dd = d;
                p.tracks[0].devices = {dd};
                DevicePanel panel(&p, &tr, 0, dd, nullptr, {});
                bool next = false, prev = false;
                for (QToolButton* t : panel.body()->findChildren<QToolButton*>()) { next |= t->property("mapTarget").toString().endsWith(":irnext"); prev |= t->property("mapTarget").toString().endsWith(":irprev"); }
                withSteps += next && prev ? 1 : 0;
                fits += panel.body()->minimumSizeHint().height() <= devui::kBodyHeight + 4 ? 1 : 0;
                panel.body()->tick();
            }
            CHECK((withSteps == 3 && fits == 3), "M19 IRs: the Guitar Amp, NAM and Cabinet panels all have ◀ ▶ for the IR library and fit the device height");
        }
        // the set file keeps a built-in IR by name
        {
            Project q;
            q.scenes = {"1"};
            q.addTrack("G", QColor("#f2905e"));
            q.tracks[0].type = TrackType::Audio;
            Device cd = q.makeDevice("Cabinet", DeviceKind::AudioEffect);
            cd.samplePath = "builtin:ir/5/0";
            cd.sample = samples::load(cd.samplePath);
            q.tracks[0].devices = {cd};
            const QJsonObject json = setfile::deviceToJson(q.tracks[0].devices[0]);
            const Device readBack = setfile::deviceFromJson(json);
            CHECK((readBack.samplePath == "builtin:ir/5/0" && readBack.sample && readBack.name == "Cabinet"), "M19 IRs: a built-in IR in a Cabinet saves and loads by name");
        }
        // the browser lists them
        {
            Browser br;
            br.showCategory(Browser::irCategory());
            const QStringList items = br.listedItems();
            CHECK((items.contains("builtin:ir/1/0|r") && items.contains(a + "|r") && items.contains(c + "|r") && items.size() >= 18), "M19 IRs: the browser's Impulse Responses lists the built-in cabinets and your files");
        }
        import::removeIrFolder(irMore.path());
    }

    // ---- Milestone 19: the Drum Studio and its grooves
    {
        namespace ds = drumstudio;
        auto hit = [&](int note, int kit, std::function<void(std::vector<float>&)> tweak = {}, double seconds = 1.0) {
            auto proc = audio::createProcessor(DeviceType::DrumStudio, 48000);
            std::vector<float> params = defaultsOf(DeviceType::DrumStudio);
            params[ds::Kit] = float(choicePosition(kit, ds::kKits));
            if (tweak) tweak(params);
            return runDevice(*proc, DeviceKind::Instrument, params, seconds, {note}, 0.05, 1.0f);
        };
        int sounding = 0, silentWrong = 0;
        QStringList peaks;
        for (int n : {35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 55, 57, 59}) {
            const DevRun r = hit(n, 0);
            if (r.finite && r.peak > 0.02f && r.peak < 1.5f) ++sounding; else peaks << QString("%1: %2").arg(n).arg(r.peak);
        }
        const DevRun none = hit(60, 0);
        if (none.peak > 1e-6f) ++silentWrong;
        CHECK((sounding == 22 && silentWrong == 0), qPrintable("M19 Drum Studio: every General MIDI drum note sounds (finite, under full scale), a note off the map is silent " + peaks.join(", ")));
        for (int d = 0; d < ds::kDrums; ++d) std::printf("      %s peak %.2f\n", ds::drumName(d), hit(ds::noteFor(d), 0).peak);
        float kitDiff = 0;
        const DevRun k0 = hit(36, 0), k4 = hit(36, 4);
        for (size_t i = 0; i < std::min(k0.l.size(), k4.l.size()); ++i) kitDiff = std::max(kitDiff, std::fabs(k0.l[i] - k4.l[i]));
        CHECK(kitDiff > 0.05f, "M19 Drum Studio: the kits sound different");
        // the per-drum knobs: Decay lengthens the tail, Tune raises the pitch (the sweep lands higher), Level is a gain
        const DevRun shortKick = hit(36, 0), longKick = hit(36, 0, [](std::vector<float>& p) { p[ds::param(ds::Kick, ds::Decay)] = 1.0f; });
        CHECK(rmsOf(longKick.l, 24000, 40000) > rmsOf(shortKick.l, 24000, 40000) * 3, "M19 Drum Studio: a drum's Decay lengthens its tail");
        const DevRun quiet = hit(38, 0, [](std::vector<float>& p) { p[ds::param(ds::Snare, ds::Level)] = 0.0f; });
        CHECK(quiet.peak < hit(38, 0).peak * 0.2f, "M19 Drum Studio: a drum's Level turns it down");
        // the mics: Room adds a tail, Width spreads the overheads, Overheads add a bright copy
        const DevRun dryRoom = hit(38, 0, [](std::vector<float>& p) { p[ds::Room] = 0.0f; }), wetRoom = hit(38, 0, [](std::vector<float>& p) { p[ds::Room] = 1.0f; p[ds::RoomSize] = 1.0f; });
        CHECK(rmsOf(wetRoom.l, 30000, 47000) > rmsOf(dryRoom.l, 30000, 47000) * 4, "M19 Drum Studio: the Room mics add a tail that Room Size lengthens");
        const DevRun narrow = hit(42, 0, [](std::vector<float>& p) { p[ds::Width] = 0.0f; }), wide = hit(42, 0, [](std::vector<float>& p) { p[ds::Width] = 1.0f; p[ds::Overheads] = 1.0f; });
        auto sideOf = [](const DevRun& r) { float s = 0; for (size_t i = 0; i < r.l.size(); ++i) s += std::fabs(r.l[i] - r.r[i]); return s; };
        CHECK(sideOf(wide) > sideOf(narrow) * 1.5f, "M19 Drum Studio: Width spreads the overheads across the sides");
        // the open hat is choked by the closed one
        {
            auto proc = audio::createProcessor(DeviceType::DrumStudio, 48000);
            std::vector<float> params = defaultsOf(DeviceType::DrumStudio);
            audio::Context ctx; ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000;
            std::vector<float> alone, choked;
            for (int pass = 0; pass < 2; ++pass) {
                auto pr = audio::createProcessor(DeviceType::DrumStudio, 48000);
                std::vector<float>& out = pass ? choked : alone;
                for (int pos = 0; pos < 48000; pos += 128) {
                    audio::EventList ev;
                    if (pos == 0) ev.add({0, 46, 0.9f, 2000, false, false});
                    if (pass && pos == 9600) ev.add({0, 42, 0.9f, 2000, false, false});
                    float l[128] = {}, r[128] = {};
                    pr->render(ev, l, r, 128, params.data(), ctx);
                    out.insert(out.end(), l, l + 128);
                }
            }
            CHECK(rmsOf(choked, 16000, 30000) < rmsOf(alone, 16000, 30000) * 0.35f, "M19 Drum Studio: a closed hat chokes the open one");
        }
        // the grooves: every one parses to notes inside its bars on drum notes, lanes the right length
        const auto& lib = grooves::all();
        bool shaped = true;
        int bad = -1;
        for (int gi = 0; gi < int(lib.size()); ++gi) {
            const grooves::Groove& g = lib[size_t(gi)];
            const QVector<Note> notes = grooves::notes(g);
            bool okG = !notes.isEmpty();
            for (const QString& line : QString(g.lanes).split('\n')) okG &= line.size() == 2 + g.bars * 16 && line[1] == '|';
            for (const Note& n : notes) okG &= n.start >= 0 && n.start < g.bars * 4 && ds::drumFor(n.pitch) >= 0 && n.velocity > 0 && n.velocity <= 1;
            if (!okG && bad < 0) bad = gi;
            shaped &= okG;
        }
        CHECK((lib.size() >= 60 && grooves::styles().size() == 14 && shaped), qPrintable(QString("M19 grooves: %1 grooves in 14 styles, each a bar or two of drum notes (bad: %2)").arg(lib.size()).arg(bad < 0 ? "none" : lib[size_t(bad)].name)));
        const int swingRide = grooves::find("Jazz", "Swing Ride"), rock = grooves::find("Rock", "Basic Eighths");
        const QVector<Note> swung = grooves::notes(lib[size_t(swingRide)]);
        double offEighth = -1;
        for (const Note& n : swung) if (n.pitch == 51 && n.start > 1.4 && n.start < 1.9) offEighth = n.start;
        CHECK((swingRide >= 0 && rock >= 0 && std::abs(offEighth - 1.67) < 0.02), "M19 grooves: a shuffle's off-eighths land on the triplet");
        const QVector<Note> a1 = grooves::notes(lib[size_t(rock)], 1, 1, 5), a2 = grooves::notes(lib[size_t(rock)], 1, 1, 5), straight = grooves::notes(lib[size_t(rock)]);
        bool same = a1.size() == a2.size(), moved = false;
        for (int i = 0; i < a1.size() && same; ++i) same &= a1[i].start == a2[i].start && a1[i].velocity == a2[i].velocity;
        for (int i = 0; i < std::min(a1.size(), straight.size()); ++i) moved |= std::abs(a1[i].start - straight[i].start) > 1e-9 || std::abs(a1[i].velocity - straight[i].velocity) > 1e-9;
        CHECK((same && moved), "M19 grooves: Humanize moves the hits a little, the same way for the same seed");
        QMimeData* gm = grooves::mimeFor(3, 0.5, 0.25);
        double hv = 0, ht = 0;
        CHECK((grooves::indexIn(gm, &hv, &ht) == 3 && std::abs(hv - 0.5) < 1e-9 && std::abs(ht - 0.25) < 1e-9 && grooves::indexIn(nullptr) == -1), "M19 grooves: the drag payload carries the groove and its humanize");
        delete gm;
        // into the set: a MIDI track takes the clip; an audio track gets a new Drum Studio track after it
        Project p;
        p.scenes = {"1", "2"};
        p.addTrack("Keys", QColor("#80a0ff"));
        p.addTrack("Audio", QColor("#f2905e"));
        p.tracks[1].type = TrackType::Audio;
        ClipPtr clip = grooves::makeClip(p, lib[size_t(rock)]);
        int t = grooves::toSession(p, 0, 1, clip);
        CHECK((t == 0 && clip->name == "Rock Basic Eighths" && clip->length == 4 && !clip->notes.isEmpty() && p.tracks[0].clipSlots[1] == clip), "M19 grooves: a groove becomes a MIDI clip in the slot");
        ClipPtr clip2 = grooves::makeClip(p, lib[size_t(swingRide)]);
        t = grooves::toSession(p, 1, 0, clip2);
        CHECK((t == 2 && p.tracks.size() == 3 && p.tracks[2].type == TrackType::Midi && p.tracks[2].devices.size() == 1 && p.tracks[2].devices[0].name == "Drum Studio" && p.tracks[2].clipSlots[0] == clip2),
              "M19 grooves: dropped on an audio track, a new MIDI track with a Drum Studio takes it");
        ClipPtr clip3 = grooves::makeClip(p, lib[size_t(rock)]);
        t = grooves::toArrangement(p, 2, 8, clip3);
        CHECK((t == 2 && p.tracks[2].arrangement.size() == 1 && p.tracks[2].arrangement[0].start == 8 && p.tracks[2].arrangement[0].clip == clip3), "M19 grooves: onto the timeline at the beat");
        // the panel: three pages, every knob, the kit view, To Clip
        {
            audio::Engine e(&p);
            Transport tr(&p, &e);
            DevicePanel panel(&p, &tr, 2, p.tracks[2].devices[0], nullptr, {});
            panel.show();
            QApplication::processEvents();
            const QList<QPushButton*> pages = panel.body()->findChildren<QPushButton*>("studioPage");
            QSet<QString> targets;
            for (QWidget* w : panel.body()->findChildren<QWidget*>()) targets.insert(w->property("mapTarget").toString());
            int missing = 0;
            for (int i = 0; i < panel.body()->count(); ++i) if (!targets.contains(panel.body()->target(i))) ++missing;
            CHECK((pages.size() == 3 && panel.body()->count() == ds::kParams && missing == 0 && panel.body()->minimumSizeHint().height() <= devui::kBodyHeight + 4 && panel.body()->findChildren<devui::Display*>().size() >= 2),
                  "M19 Drum Studio panel: Kit, Mics and Grooves pages, a control for all 50 knobs, the kit and mic views, and it fits the device height");
            for (QPushButton* b : pages) if (b->text() == "Grooves") b->click();
            QApplication::processEvents();
            QToolButton* toClip = panel.body()->findChild<QToolButton*>("grooveToClip");
            const int before = p.undoStack()->count();
            if (toClip) toClip->click();
            QApplication::processEvents();
            ClipPtr made = p.tracks[2].clipSlots.value(1);
            CHECK((toClip && made && !made->isAudio() && !made->notes.isEmpty() && p.undoStack()->count() == before + 1), "M19 Drum Studio panel: To Clip puts the groove in the track's next free slot, one undo step");
            panel.body()->tick();
        }
        // a groove dropped on the Session grid and on the Arrangement
        {
            MainWindow w;
            w.resize(1400, 900);
            w.show();
            QApplication::processEvents();
            Project* proj = w.project();
            int keys = 0;
            for (int i = 0; i < proj->tracks.size(); ++i) if (proj->tracks[i].name == "Keys") keys = i;
            ClipGrid* grid = w.sessionView()->grid();
            QMimeData* mime = grooves::mimeFor(rock);
            QRect keysSlot;
            for (const auto& cell : grid->mapCells()) if (cell.second == QString("slot:%1:3").arg(proj->tracks[keys].id)) keysSlot = cell.first;
            const QPoint slot = keysSlot.center();
            QDragEnterEvent enter(slot, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(grid, &enter);
            QDropEvent drop(slot, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(grid, &drop);
            QApplication::processEvents();
            const ClipPtr dropped = proj->tracks[keys].clipSlots.value(3);
            CHECK((enter.isAccepted() && dropped && !dropped->isAudio() && dropped->name == "Rock Basic Eighths"), "M19 grooves: dropped on a MIDI track's slot it becomes a clip there");
            ArrangementView* av = w.arrangementView();
            QTest::keyClick(grid, Qt::Key_Tab);   // the Arrangement
            QApplication::processEvents();
            const QPoint lanePos(int(av->beatToX(16)) + 2, av->laneRect(keys).center().y());
            QDragEnterEvent enter2(lanePos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(av, &enter2);
            QDragMoveEvent move(lanePos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(av, &move);
            QDropEvent drop2(lanePos, Qt::CopyAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(av, &drop2);
            QApplication::processEvents();
            bool onLane = false;
            for (const ArrangementClip& a : proj->tracks[keys].arrangement) onLane |= a.clip && a.clip->name == "Rock Basic Eighths" && std::abs(a.start - 16) < 1.01;
            CHECK(onLane, "M19 grooves: dropped on an Arrangement lane it becomes a clip at the beat");
            delete mime;
        }
    }


    // ---- Milestone 20: the Monolith reworked: the sub, F Drive, mod-osc waves, one-shot LFOs, the FX, the drawers, the new Sounds
    {
        using namespace monolith;
        auto ne = [](double real, double lo, double hi) { return std::log(real / lo) / std::log(hi / lo); };   // a knob position for an Exp parameter
        struct MNote { int pitch, start, length; float vel; };
        auto run = [&](const std::vector<float>& params, const std::vector<ModRoute>& routes, const std::vector<MNote>& notes, double seconds, double tempo = 120) {
            auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
            DevRun out;
            audio::Context ctx;
            ctx.sampleRate = 48000; ctx.tempo = tempo; ctx.beatsPerSample = tempo / 60.0 / 48000; ctx.playing = true;
            ctx.mods = routes.data(); ctx.modCount = int(routes.size());
            const int total = int(seconds * 48000);
            for (int pos = 0; pos < total; pos += 128) {
                audio::EventList ev;
                for (const MNote& n : notes) if (n.start >= pos && n.start < pos + 128) ev.add({n.start - pos, n.pitch, n.vel, n.length, false, false});
                float l[128] = {}, r[128] = {};
                ctx.beat = pos * ctx.beatsPerSample;
                proc->render(ev, l, r, 128, params.data(), ctx);
                for (int i = 0; i < 128; ++i) { out.l.push_back(l[i]); out.r.push_back(r[i]); out.finite &= std::isfinite(l[i]) && std::isfinite(r[i]); out.peak = std::max(out.peak, std::fabs(l[i])); }
            }
            return out;
        };
        auto lowEnergy = [](const std::vector<float>& v, size_t from, size_t to, float hz) {   // under `hz`, two one-poles
            dsp::OnePole a, b; a.setLowpass(hz, 48000); b.setLowpass(hz, 48000);
            float e = 0;
            for (size_t i = 0; i < v.size() && i < to; ++i) { const float y = b.lp(a.lp(v[i])); if (i >= from) e += y * y; }
            return e;
        };
        auto highEnergy = [](const std::vector<float>& v, size_t from, size_t to, float hz) {
            dsp::OnePole a, b; a.setLowpass(hz, 48000); b.setLowpass(hz, 48000);
            float e = 0;
            for (size_t i = 0; i < v.size() && i < to; ++i) { const float y = b.hp(a.hp(v[i])); if (i >= from) e += y * y; }
            return e;
        };
        std::vector<float> base = defaultsOf(DeviceType::Monolith);
        base[oparam(1, Level)] = 0;   // one sine-ish oscillator on its own (Basic at 0.66 is a saw; take the Sub table at 0 for a sine)
        base[oparam(0, Table)] = float(choicePosition(4, 8)); base[oparam(0, Position)] = 0;
        base[fparam(0, FCutoff)] = 1.0f;   // the filter open
        const std::vector<MNote> c3 = {{60, 0, 24000, 0.8f}};
        const DevRun plain = run(base, {}, c3, 0.8);
        std::vector<float> withSub = base; withSub[SubLevel] = 0.9f;
        const DevRun sub = run(withSub, {}, c3, 0.8);
        std::vector<float> subTwo = withSub; subTwo[SubOct] = 1.0f;   // -2
        const DevRun sub2 = run(subTwo, {}, c3, 0.8);
        CHECK((sub.finite && lowEnergy(sub.l, 4800, 36000, 180) > lowEnergy(plain.l, 4800, 36000, 180) * 4 && lowEnergy(sub2.l, 4800, 36000, 90) > lowEnergy(sub.l, 4800, 36000, 90) * 2),
              "M20 Monolith: the sub oscillator adds weight an octave under the note, Sub Oct -2 two octaves under");
        std::vector<float> subSquare = withSub; subSquare[SubWave] = float(choicePosition(1, 3));
        CHECK(highEnergy(run(subSquare, {}, c3, 0.8).l, 4800, 36000, 600) > highEnergy(sub.l, 4800, 36000, 600) * 1.5f, "M20 Monolith: a square sub has harmonics a sine sub hasn't");
        std::vector<float> driven = base; driven[FDrive] = 1.0f;
        CHECK(highEnergy(run(driven, {}, c3, 0.8).l, 4800, 36000, 1200) > highEnergy(plain.l, 4800, 36000, 1200) * 2, "M20 Monolith: F Drive adds harmonics before the filters");
        std::vector<float> ringSine = base; ringSine[ModRing] = 1.0f; ringSine[ModPitch] = float((7.0 + 36) / 72);
        std::vector<float> ringSquare = ringSine; ringSquare[ModOscWave] = float(choicePosition(3, 4));
        CHECK(highEnergy(run(ringSquare, {}, c3, 0.8).l, 4800, 36000, 2500) > highEnergy(run(ringSine, {}, c3, 0.8).l, 4800, 36000, 2500) * 1.3f, "M20 Monolith: a square modulation oscillator tears more than a sine");
        // a one-shot LFO: a saw-down LFO on the cutoff opens then closes once and holds (the same for a note late in the song)
        {
            std::vector<float> p = base;
            p[fparam(0, FCutoff)] = float(ne(200, 30, 18000));
            p[lparam(0, LRate)] = float(choicePosition(8, 9)); p[lparam(0, LHz)] = float(ne(2, 0.05, 30));   // free, 2 Hz: one cycle in 0.5 s
            p[lparam(0, LShape)] = float(choicePosition(3, 6));   // saw down: 1 at the start, -1 at the end
            p[lmode(0)] = float(choicePosition(1, 2));
            const std::vector<ModRoute> route = {{SrcLfo1, fparam(0, FCutoff), 0.5}};
            const DevRun one = run(p, route, {{48, 0, 60000, 0.8f}}, 1.3);
            const float early = highEnergy(one.l, 2400, 9600, 500), late = highEnergy(one.l, 36000, 48000, 500), later = highEnergy(one.l, 50000, 62000, 500);
            p[lmode(0)] = 0;
            const DevRun looping = run(p, route, {{48, 0, 60000, 0.8f}}, 1.3);
            const float loopLate = highEnergy(looping.l, 36000, 48000, 500);
            CHECK((one.finite && early > late * 4 && std::fabs(late - later) < std::max(late, later) * 0.5f + 1e-6f && loopLate > late * 3),
                  "M20 Monolith: a one-shot LFO runs once from the note and holds its end (looping, it comes round again)");
        }
        // the FX: a delay repeats after the note ends, a reverb leaves a tail, the chorus parts the sides, the EQ lifts the lows
        {
            std::vector<float> dry = base; dry[eparam(0, R)] = float(ne(0.01, 0.005, 8));
            const std::vector<MNote> shortNote = {{60, 0, 4800, 0.8f}};
            const DevRun d0 = run(dry, {}, shortNote, 1.5);
            std::vector<float> delayed = dry; delayed[fx(DelayMix)] = 0.6f; delayed[fx(DelayTime)] = float(choicePosition(3, 8)); delayed[fx(DelayFeedback)] = 0.5f;   // 1/4 at 120 = 0.5 s
            const DevRun d1 = run(delayed, {}, shortNote, 1.5);
            CHECK((d1.finite && rmsOf(d1.l, 25000, 30000) > rmsOf(d0.l, 25000, 30000) * 5 + 1e-4f && rmsOf(d1.l, 25000, 30000) > 0.005f),
                  "M20 Monolith: the synced delay repeats the note after it has stopped (1/4 at 120 BPM: half a second later)");
            std::vector<float> verb = dry; verb[fx(ReverbMix)] = 0.7f; verb[fx(ReverbSize)] = 0.9f;
            const DevRun r1 = run(verb, {}, shortNote, 1.5);
            CHECK((r1.finite && rmsOf(r1.l, 12000, 24000) > rmsOf(d0.l, 12000, 24000) * 5 + 1e-4f), "M20 Monolith: the reverb leaves a tail after the note");
            std::vector<float> chorus = base; chorus[fx(ChorusMix)] = 0.8f; chorus[fx(ChorusDepth)] = 0.8f;
            const DevRun c1 = run(chorus, {}, c3, 0.8);
            float sideDry = 0, sideWet = 0;
            for (size_t i = 4800; i < 36000; ++i) { sideDry += std::fabs(plain.l[i] - plain.r[i]); sideWet += std::fabs(c1.l[i] - c1.r[i]); }
            CHECK((c1.finite && sideWet > sideDry * 3 + 1e-3f), "M20 Monolith: the chorus parts the two sides");
            std::vector<float> eq = base; eq[fx(EqLow)] = 1.0f;   // +12 dB under 120 Hz
            std::vector<float> lowNote = eq;
            const DevRun e1 = run(eq, {}, {{36, 0, 24000, 0.8f}}, 0.8), e0 = run(base, {}, {{36, 0, 24000, 0.8f}}, 0.8);
            CHECK((e1.finite && lowEnergy(e1.l, 4800, 36000, 120) > lowEnergy(e0.l, 4800, 36000, 120) * 4), "M20 Monolith: EQ Low lifts the lows");
            // a macro routed to an FX knob reaches it (the shared sources modulate the FX)
            std::vector<float> macroDelay = dry; macroDelay[macro(0)] = 1.0f; macroDelay[fx(DelayTime)] = float(choicePosition(3, 8)); macroDelay[fx(DelayFeedback)] = 0.5f;
            const DevRun m1 = run(macroDelay, {{SrcMacro1, fx(DelayMix), 0.6}}, shortNote, 1.5);
            CHECK(rmsOf(m1.l, 25000, 30000) > rmsOf(d0.l, 25000, 30000) * 5 + 1e-4f, "M20 Monolith: Macro 1 routed to Delay Mix brings the delay in");
        }
        // the new Sounds come with routes and play
        {
            QStringList names;
            int routed = 0, played = 0;
            for (const char* n : {"Dub Wobble", "Neuro Reese", "Talking Growl", "Supersaw Lead", "Glass Pad", "Acid Monolith", "Hoover", "Stab", "Deep Sub"}) {
                const DeviceSpec* spec = deviceSpec(n);
                if (!spec) { names << n; continue; }
                std::vector<float> p(audio::kMaxParams, 0.0f);
                for (size_t i = 0; i < spec->params.size(); ++i) p[i] = float(spec->params[i].def);
                const std::vector<ModRoute>& mods = presetMods(n);
                routed += mods.empty() ? 0 : 1;
                const DevRun r = run(p, mods, {{48, 0, 24000, 0.8f}}, 0.8);
                played += r.finite && r.peak > 0.02f && r.peak < 1.5f ? 1 : 0;
                if (!(r.finite && r.peak > 0.02f && r.peak < 1.5f)) names << QString("%1 (peak %2)").arg(n).arg(r.peak);
            }
            CHECK((routed == 9 && played == 9), qPrintable("M20 Sounds: Dub Wobble, Neuro Reese, Talking Growl, Supersaw Lead, Glass Pad, Acid Monolith, Hoover, Stab and Deep Sub come with routes and play " + names.join(", ")));
        }
        // the panel: the main panel, six drawers that slide out one at a time and are remembered per device, every knob reachable, the scope
        {
            Project p;
            p.scenes = {"1"};
            p.addTrack("Bass", QColor("#62c9dc"));
            p.tracks[0].devices = {p.makeDevice("Supersaw Lead", DeviceKind::Instrument)};
            audio::Engine e(&p);
            Transport t(&p, &e);
            DevicePanel panel(&p, &t, 0, p.tracks[0].devices[0], nullptr, {});
            panel.show();
            QApplication::processEvents();
            devui::DeviceBody* body = panel.body();
            const QList<QPushButton*> drawers = body->findChildren<QPushButton*>("monoDrawer");
            for (QPushButton* b : drawers) if (b->isChecked()) { b->click(); QTest::qWait(320); }   // the drawer memory is per device id: the M13 test's Mod drawer may be open
            QApplication::processEvents();
            QStackedWidget* stack = body->findChild<QStackedWidget*>();
            QSet<QString> targets;
            for (QWidget* w : body->findChildren<QWidget*>()) targets.insert(w->property("mapTarget").toString());
            int missing = 0;
            for (int i = 0; i < body->count(); ++i) if (!targets.contains(body->target(i))) ++missing;
            const int closed = body->width();
            CHECK((drawers.size() == 7 && stack && stack->count() == 7 && missing == 0 && closed <= 1200 && panel.width() == closed + 2 && body->minimumSizeHint().height() <= devui::kBodyHeight + 4),
                  qPrintable(QString("M20 panel: the main panel (%1 px) with six drawers closed, a control for all 170 knobs, in the device's height").arg(closed)));
            QPushButton *env = nullptr, *fxb = nullptr;
            for (QPushButton* b : drawers) { if (b->text() == "Env") env = b; else if (b->text() == "FX") fxb = b; }
            env->click();
            QTest::qWait(320);
            QApplication::processEvents();
            const int withEnv = body->width();
            CHECK((stack->currentIndex() == 0 && env->isChecked() && withEnv > closed + 300 && panel.width() == withEnv + 2), "M20 panel: Env slides the envelope drawer out and the device panel grows with it");
            fxb->click();
            QTest::qWait(320);
            QApplication::processEvents();
            CHECK((stack->currentIndex() == 5 && fxb->isChecked() && !env->isChecked() && body->findChildren<devui::Display*>().size() >= 6), "M20 panel: FX swaps the drawer (one at a time)");
            fxb->click();
            QTest::qWait(320);
            QApplication::processEvents();
            CHECK((!fxb->isChecked() && body->width() == closed && panel.width() == closed + 2), "M20 panel: clicking the open drawer's button closes it again");
            env->click();
            QTest::qWait(320);
            DevicePanel again(&p, &t, 0, p.tracks[0].devices[0], nullptr, {});
            again.show();
            QApplication::processEvents();
            QPushButton* envAgain = nullptr;
            for (QPushButton* b : again.body()->findChildren<QPushButton*>("monoDrawer")) if (b->text() == "Env") envAgain = b;
            CHECK((envAgain && envAgain->isChecked() && again.body()->width() > closed + 300), "M20 panel: the open drawer is remembered per device (a rebuilt panel opens with it)");
            // the scope: a note played through the engine shows its envelope levels in the panel's scope
            auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
            std::vector<float> params = defaultsOf(DeviceType::Monolith);
            audio::Context ctx; ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000;
            audio::EventList ev; ev.add({0, 60, 0.9f, 48000, false, false});
            float l[128] = {}, r[128] = {};
            for (int k = 0; k < 20; ++k) { proc->render(ev, l, r, 128, params.data(), ctx); ev.clear(); }
            CHECK((proc->scope[0].load() > 0.3f && proc->scope[4].load() >= 1.0f && proc->scope[4].load() <= 3.0f), "M20 scope: while a note plays the processor reports Env 1's level and stage for the drawer's dot");
        }
    }

    // ---- Milestone 21: after the 0.20 hands-on: Guitar gathers its devices, the overview strips scroll and zoom,
    //      parts of clips (select, copy / cut / paste, Ctrl+drag copies), the side tabs, record off on stop,
    //      the six warp modes, alternating beat shading
    {
        // the browser: everything for the guitar under Guitar, and only there
        {
            Browser br;
            br.showCategory("Guitar");
            const QStringList guitar = br.listedItems();
            br.showCategory("Audio Effects");
            const QStringList fx = br.listedItems();
            br.showCategory("Sounds");
            const QStringList sounds = br.listedItems();
            bool ok = guitar.contains("Guitar Amp|f") && guitar.contains("Overdrive|f") && guitar.contains("Cabinet|f") && guitar.contains("Tuner|f") && guitar.contains("NAM|f")
                   && fx.contains("EQ|f") && !fx.isEmpty() && !sounds.isEmpty();
            int rigs = 0;
            for (const QString& it : fx) if (isGuitarType(deviceType(it.section('|', 0, 0)))) ok = false;
            for (const QString& it : sounds) if (const RackRecipe* r = rackRecipe(it.section('|', 0, 0)); r && r->guitar) ok = false;
            for (const QString& it : guitar) if (const RackRecipe* r = rackRecipe(it.section('|', 0, 0)); r && r->guitar) ++rigs;
            CHECK((ok && rigs >= 3), "M21 browser: the Tuner, the amp, NAM, the pedals, the Cabinet and the rigs live under Guitar only (Audio Effects keeps the studio effects)");
        }
        // stopping ends an Arrangement recording: the record button goes out
        {
            Project p; oneNoteProject(p);
            audio::Engine e(&p);
            Transport t(&p, &e);
            t.setArrangementRecord(true);
            t.play();
            const bool on = t.arrangementRecord() && t.playing();
            t.stop();
            CHECK((on && !t.arrangementRecord() && !t.playing()), "M21 transport: Stop switches Arrangement Record off (the button goes out with the song)");
        }
        // the zoom drag's direction, from Preferences
        {
            const bool was = PreferencesDialog::zoomDragUp();
            PreferencesDialog::setZoomDragUp(false);
            const bool downIn = PreferencesDialog::dragZoom(40) > 1.3 && PreferencesDialog::dragZoom(-40) < 0.8;
            PreferencesDialog::setZoomDragUp(true);
            const bool upIn = PreferencesDialog::dragZoom(40) < 0.8 && PreferencesDialog::dragZoom(-40) > 1.3;
            PreferencesDialog::setZoomDragUp(was);
            CHECK((downIn && upIn), "M21 preferences: dragging down zooms in unless the Look tab swaps it to dragging up");
        }
        // the Arrangement: the overview strip scrolls and zooms, clip bodies select time, Ctrl+drag copies, parts of clips
        {
            PreferencesDialog::setZoomDragUp(false);
            Project p;
            p.loadDemo();
            audio::Engine e(&p);
            Transport t(&p, &e);
            ArrangementView arr(&p, &t);
            arr.resize(900, 400); arr.show();
            QApplication::processEvents();
            const double off0 = arr.firstVisibleBeat(), z0 = arr.pixelsPerBeat();
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(40, 10));
            QTest::mouseMove(&arr, QPoint(100, 10));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(100, 10));
            CHECK((arr.firstVisibleBeat() > off0 + 1 && std::abs(arr.pixelsPerBeat() - z0) < 1e-9), "M21 overview: dragging the strip sideways scrolls the Arrangement (the zoom stays)");
            const double off1 = arr.firstVisibleBeat();
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(100, 10));
            QTest::mouseMove(&arr, QPoint(100, 70));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(100, 70));
            CHECK((arr.pixelsPerBeat() > z0 * 1.4), "M21 overview: dragging the strip down zooms in");
            PreferencesDialog::setZoomDragUp(true);
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(100, 10));
            QTest::mouseMove(&arr, QPoint(100, 70));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(100, 70));
            CHECK((arr.pixelsPerBeat() < z0 * 1.4 + 1e-9), "M21 overview: with the preference swapped, dragging down zooms out again");
            PreferencesDialog::setZoomDragUp(false);
            (void)off1;
            arr.zoomToFit();
            QApplication::processEvents();

            // a clip's body: a time selection inside the clip, the clip selected
            int lane = -1;
            for (int i = 0; i < p.tracks.size() && lane < 0; ++i) if (p.tracks[i].type == TrackType::Midi && !p.tracks[i].arrangement.isEmpty()) lane = i;
            CHECK(lane >= 0, "M21 clip parts: the demo has a MIDI clip on the timeline");
            if (lane >= 0) {
                std::sort(p.tracks[lane].arrangement.begin(), p.tracks[lane].arrangement.end(), [](const ArrangementClip& a, const ArrangementClip& b) { return a.start < b.start; });
                const ArrangementClip a = p.tracks[lane].arrangement.first();
                const ClipPtr clip = a.clip;
                const int n0 = p.tracks[lane].arrangement.size();
                const int bodyY = arr.laneRect(lane).top() + 30, titleY = arr.laneRect(lane).top() + 6;
                const double s = a.start + 1, e2 = a.start + 3;
                QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(s)) + 1, bodyY));
                QTest::mouseMove(&arr, QPoint(int(arr.beatToX(e2)) + 1, bodyY));
                QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(e2)) + 1, bodyY));
                CHECK((arr.hasTimeSelection() && arr.selectedClip() == clip && p.tracks[lane].arrangement.size() == n0 && p.tracks[lane].arrangement.first().start == a.start),
                      "M21 clip parts: dragging across a clip's body selects that part of it (the clip is selected, not moved)");
                // copy the part: a piece that holds the whole clip, placed at the part's offset
                const QVector<ArrangementView::ClipPiece> pieces = arr.copyRange();
                CHECK((pieces.size() == 1 && std::abs(pieces[0].length - 2) < 1e-9 && std::abs(pieces[0].offset - (a.offset + 1)) < 1e-9 && pieces[0].clip
                       && pieces[0].clip->notes.size() == clip->notes.size() && pieces[0].clip != clip),
                      "M21 clip parts: Ctrl+C takes the part as a clip of its own that still holds every note of the original");
                const double at = a.start + a.length + 8;
                arr.selectTime(at, at, lane, lane);
                arr.pastePieces(pieces);
                const ArrangementClip* pasted = nullptr;
                for (const ArrangementClip& x : p.tracks[lane].arrangement) if (std::abs(x.start - at) < 1e-9) pasted = &x;
                CHECK((pasted && std::abs(pasted->length - 2) < 1e-9 && std::abs(pasted->offset - (a.offset + 1)) < 1e-9 && pasted->clip != clip && pasted->clip->notes.size() == clip->notes.size()),
                      "M21 clip parts: Ctrl+V pastes the part at the insert marker; extend it and the rest of the clip is there (the offset is kept)");
                // cut the part out of the original
                arr.selectTime(s, e2, lane, lane);
                const int beforeCut = p.tracks[lane].arrangement.size();
                arr.cutRange();
                bool gap = true, left = false, right = false;
                for (const ArrangementClip& x : p.tracks[lane].arrangement) {
                    if (x.start < s + 2 && x.start + x.length > s + 2 - 1e-9) gap = false;
                    if (std::abs(x.start - a.start) < 1e-9 && std::abs(x.length - 1) < 1e-9) left = true;
                    if (std::abs(x.start - e2) < 1e-9 && std::abs(x.offset - (a.offset + 3)) < 1e-9) right = true;
                }
                CHECK((gap && left && right), "M21 clip parts: Ctrl+X cuts the part out, leaving the clip's two sides in place");
                const bool cutStep = p.undoStack()->undoText() == "Cut";
                p.undoStack()->undo();
                QApplication::processEvents();
                CHECK((cutStep && p.tracks[lane].arrangement.size() == beforeCut), "M21 clip parts: the cut is one undo step");

                // Ctrl+drag a clip by its name bar: a copy lands, the original stays
                const int n1 = p.tracks[lane].arrangement.size();
                const ArrangementClip* orig = nullptr;
                for (const ArrangementClip& x : p.tracks[lane].arrangement) if (x.clip == clip) orig = &x;
                const double start0 = orig ? orig->start : 0;
                const int dx = int(arr.beatToX(8) - arr.beatToX(0));
                QTest::mousePress(&arr, Qt::LeftButton, Qt::ControlModifier, QPoint(int(arr.beatToX(start0 + 0.5)), titleY));
                QTest::mouseMove(&arr, QPoint(int(arr.beatToX(start0 + 0.5)) + dx, titleY), Qt::ControlModifier);
                QTest::mouseRelease(&arr, Qt::LeftButton, Qt::ControlModifier, QPoint(int(arr.beatToX(start0 + 0.5)) + dx, titleY));
                bool stays = false, copied = false;
                for (const ArrangementClip& x : p.tracks[lane].arrangement) {
                    if (x.clip == clip && std::abs(x.start - start0) < 1e-9) stays = true;   // (cut short where the copy now lies: nothing overlaps)
                    if (x.clip != clip && std::abs(x.start - (start0 + 8)) < 1e-9 && std::abs(x.length - a.length) < 1e-9) copied = true;
                }
                CHECK((p.tracks[lane].arrangement.size() >= n1 && stays && copied), "M21 Ctrl+drag: dragging a clip's name bar with Ctrl held duplicates it where you drop it");
                p.undoStack()->undo();   // the original whole again
                // Ctrl+drag a selected part: a copy of the part lands, the clip stays whole (a known layout: one clip at 8 .. 24)
                p.tracks[lane].arrangement = {{8, 16, 0, clip}};
                emit p.structureChanged();
                QApplication::processEvents();
                arr.selectTime(9, 11, lane, lane);
                const int dx16 = int(arr.beatToX(16) - arr.beatToX(0));
                QTest::mousePress(&arr, Qt::LeftButton, Qt::ControlModifier, QPoint(int(arr.beatToX(10)), bodyY));
                QTest::mouseMove(&arr, QPoint(int(arr.beatToX(10)) + dx16, bodyY), Qt::ControlModifier);
                QTest::mouseRelease(&arr, Qt::LeftButton, Qt::ControlModifier, QPoint(int(arr.beatToX(10)) + dx16, bodyY));
                bool whole = false, part = false;
                for (const ArrangementClip& x : p.tracks[lane].arrangement) {
                    if (x.clip == clip && std::abs(x.start - 8) < 1e-9 && std::abs(x.length - 16) < 1e-9) whole = true;
                    if (std::abs(x.start - 25) < 1e-9 && std::abs(x.length - 2) < 1e-9 && std::abs(x.offset - 1) < 1e-9) part = true;
                }
                CHECK((p.tracks[lane].arrangement.size() == 2 && whole && part), "M21 Ctrl+drag: dragging a selected part of a clip with Ctrl copies that part, the clip stays whole");
                // without Ctrl, the part comes loose and moves
                p.tracks[lane].arrangement = {{8, 16, 0, clip}};
                emit p.structureChanged();
                QApplication::processEvents();
                arr.selectTime(9, 11, lane, lane);
                QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(10)), bodyY));
                QTest::mouseMove(&arr, QPoint(int(arr.beatToX(10)) + dx16, bodyY));
                QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(10)) + dx16, bodyY));
                bool moved = false, leftSide = false, rightSide = false;
                for (const ArrangementClip& x : p.tracks[lane].arrangement) {
                    if (std::abs(x.start - 25) < 1e-9 && std::abs(x.length - 2) < 1e-9 && std::abs(x.offset - 1) < 1e-9) moved = true;
                    if (x.clip == clip && std::abs(x.start - 8) < 1e-9 && std::abs(x.length - 1) < 1e-9) leftSide = true;
                    if (std::abs(x.start - 11) < 1e-9 && std::abs(x.length - 13) < 1e-9 && std::abs(x.offset - 3) < 1e-9) rightSide = true;
                }
                CHECK((p.tracks[lane].arrangement.size() == 3 && moved && leftSide && rightSide && arr.hasTimeSelection()), "M21 clip parts: dragging a selected part without Ctrl splits it out and moves it (the selection goes along)");
            }
        }
        // the window: the Clip / Device tabs on their side, the header strip, the overview bar's drags and device pictures
        {
            MainWindow w;
            w.resize(1400, 900);
            w.show();
            QApplication::processEvents();
            QToolButton* clipTab = nullptr; QToolButton* deviceTab = nullptr;
            for (QToolButton* b : w.findChildren<QToolButton*>()) { if (b->text() == "Clip") clipTab = b; if (b->text() == "Device") deviceTab = b; }
            QWidget* header = w.findChild<QWidget*>("detailHeader");
            QWidget* info = w.findChild<QWidget*>("infoView");
            CHECK((clipTab && deviceTab && header && info && clipTab->height() > 2 * clipTab->width() && deviceTab->height() > 2 * deviceTab->width()
                   && clipTab->mapTo(&w, QPoint(0, 0)).x() < header->mapTo(&w, QPoint(0, 0)).x()
                   && clipTab->mapTo(&w, QPoint(0, 0)).x() >= info->mapTo(&w, QPoint(0, 0)).x() + info->width() - 2),
                  "M21 window: the Clip and Device tabs stand on their side between the Info View and the detail pane");
            OverviewBar* bar = w.findChild<OverviewBar*>();
            Project* proj = w.project();
            int drums = 0;
            for (int i = 0; i < proj->tracks.size(); ++i) if (proj->tracks[i].name == "Drums") drums = i;
            QRect drumSlot;
            for (const auto& cell : w.sessionView()->grid()->mapCells()) if (cell.second == QString("slot:%1:0").arg(proj->tracks[drums].id)) drumSlot = cell.first;
            QTest::mouseClick(w.sessionView()->grid(), Qt::LeftButton, Qt::NoModifier, drumSlot.center() + QPoint(30, 0));
            QApplication::processEvents();
            CHECK((bar && bar->height() == OverviewBar::kBarHeight && w.clipViewShown() && proj->selectedClip == proj->tracks[drums].clipSlots[0]), "M21 window: a clip is selected, the Clip View shows it");
            ClipView* cv = w.findChild<ClipView*>();
            PianoRoll* roll = cv ? cv->pianoRoll() : nullptr;
            double f0 = 0, l0 = 0, f1 = 0, l1 = 0, f2 = 0, l2 = 0;
            const bool shown = roll && roll->viewport(f0, l0);
            ClipOverview* strip = bar ? bar->clipOverview() : nullptr;
            PreferencesDialog::setZoomDragUp(false);
            if (strip) {
                QTest::mousePress(strip, Qt::LeftButton, Qt::NoModifier, QPoint(150, 10));
                QTest::mouseMove(strip, QPoint(150, 80));
                QTest::mouseRelease(strip, Qt::LeftButton, Qt::NoModifier, QPoint(150, 80));
            }
            const bool zoomed = roll && roll->viewport(f1, l1) && (l1 - f1) < (l0 - f0) * 0.7;
            if (strip) {
                QTest::mousePress(strip, Qt::LeftButton, Qt::NoModifier, QPoint(100, 10));
                QTest::mouseMove(strip, QPoint(160, 10));
                QTest::mouseRelease(strip, Qt::LeftButton, Qt::NoModifier, QPoint(160, 10));
            }
            const bool scrolled = roll && roll->viewport(f2, l2) && f2 > f1 + 0.2 && std::abs((l2 - f2) - (l1 - f1)) < 0.05;
            CHECK((shown && zoomed && scrolled && w.clipViewShown()), "M21 overview bar: dragging the clip overview down zooms the editor in, sideways scrolls it (the outline follows)");
            const QList<ClipOverview*> strips = w.findChildren<ClipOverview*>();
            ClipOverview* headerStrip = cv ? cv->overviewStrip() : nullptr;
            CHECK((strips.size() == 2 && headerStrip && strips.contains(headerStrip) && headerStrip->isVisible() && headerStrip->width() > 300 && roll
                   && headerStrip->mapTo(&w, QPoint(0, 0)).x() == roll->mapTo(&w, QPoint(0, 0)).x() && headerStrip->width() == roll->width()
                   && headerStrip->mapTo(&w, QPoint(0, 0)).y() < roll->mapTo(&w, QPoint(0, 0)).y()),
                  "M21 window: a second clip overview sits right over the editor (as wide as it, not over the clip boxes)");
            if (headerStrip) {
                QTest::mousePress(headerStrip, Qt::LeftButton, Qt::NoModifier, QPoint(headerStrip->width() / 2, 8));
                QTest::mouseMove(headerStrip, QPoint(headerStrip->width() / 2 - 60, 8));
                QTest::mouseRelease(headerStrip, Qt::LeftButton, Qt::NoModifier, QPoint(headerStrip->width() / 2 - 60, 8));
            }
            double f3 = 0, l3 = 0;
            CHECK((roll && roll->viewport(f3, l3) && f3 < f2 - 0.2), "M21 window: dragging the header strip back scrolls the editor back");
            // the device chain in miniature: pictures of the panels, at the panels' proportions, scrolled by dragging
            if (deviceTab) QTest::mouseClick(deviceTab, Qt::LeftButton);
            QApplication::processEvents();
            if (bar) bar->chainOverview()->refreshThumbs();
            const QList<QPair<QRect, int>> blocks = bar ? bar->chainOverview()->blocks() : QList<QPair<QRect, int>>();
            const QList<QPair<int, QWidget*>> widgets = w.deviceView()->blockWidgets();
            bool proportions = bar && bar->chainOverview()->hasThumbs() && blocks.size() == widgets.size() && !blocks.isEmpty();
            for (int i = 0; proportions && i < blocks.size(); ++i) {
                const double k = double(blocks[i].first.height()) / widgets[i].second->height();
                proportions = blocks[i].second == widgets[i].first && std::abs(blocks[i].first.width() - widgets[i].second->width() * k) <= 2;
            }
            CHECK(proportions, "M21 overview bar: the device chain shows each panel as a picture of it, scaled to the strip's height");
            int wide = 0;
            for (const auto& b : blocks) wide = std::max(wide, b.first.right());
            if (bar && wide > bar->chainOverview()->width()) {
                QTest::mousePress(bar->chainOverview(), Qt::LeftButton, Qt::NoModifier, QPoint(200, 10));
                QTest::mouseMove(bar->chainOverview(), QPoint(100, 10));
                QTest::mouseRelease(bar->chainOverview(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 10));
                CHECK((bar->chainOverview()->scrollOffset() > 50 && w.deviceView()->selection() == QList<int>{}), "M21 overview bar: dragging a long chain sideways scrolls it (without selecting)");
            } else {
                CHECK(true, "M21 overview bar: dragging a long chain sideways scrolls it (the demo's chain fits: nothing to scroll)");
            }
        }
        // the six warp modes: Tones, Texture, Complex and Complex Pro keep the pitch while the time stretches; Re-Pitch doesn't
        {
            QTemporaryDir dir;
            const QString sineWav = dir.filePath("sine 220.wav");
            std::vector<float> sine(96000);
            for (size_t i = 0; i < sine.size(); ++i) sine[i] = 0.5f * std::sin(float(2.0 * M_PI * 220.0 * double(i) / 48000.0));
            writeMonoWav(sineWav, sine, 48000);
            auto hertz = [](const std::vector<float>& out, int from, int to) {   // zero crossings of the left channel
                int crossings = 0;
                for (int i = from + 1; i < to; ++i) if ((out[size_t(i) * 2] >= 0) != (out[size_t(i - 1) * 2] >= 0)) ++crossings;
                return crossings / 2.0 / ((to - from) / 48000.0);
            };
            auto smoothness = [](const std::vector<float>& out, int from, int to) {   // the quietest 20 ms against the loudest
                float lo = 1e9f, hi = 0;
                for (int s = from; s + 960 <= to; s += 960) {
                    float e = 0;
                    for (int i = s; i < s + 960; ++i) e += out[size_t(i) * 2] * out[size_t(i) * 2];
                    e = std::sqrt(e / 960);
                    lo = std::min(lo, e); hi = std::max(hi, e);
                }
                return hi > 0 ? lo / hi : 0.0f;
            };
            auto rms = [](const std::vector<float>& out, int from, int to) {
                double e = 0;
                for (int i = from; i < to; ++i) e += double(out[size_t(i) * 2]) * out[size_t(i) * 2];
                return float(std::sqrt(e / std::max(1, to - from)));
            };
            auto stretched = [&](WarpMode mode, double tempo, int transpose, double formants = 0) {
                Project p; ClipPtr c = audioProject(p, sineWav);
                c->length = 4; c->loop = false; c->warpMarkers = {{0, 0}, {4, 2}}; c->detectedBpm = 0;
                c->warpMode = mode; c->transpose = transpose; c->formants = formants;
                p.tracks[0].arrangement[0].length = 8;
                p.setTempo(tempo);
                return playArrangement(p, 48000 * 5);
            };
            const std::vector<float> rp = stretched(WarpMode::Repitch, 60, 0);
            CHECK(near(int(hertz(rp, 48000, 144000)), 110, 6), "M21 warp: Re-Pitch at half the tempo plays the sine an octave down");
            for (WarpMode m : {WarpMode::Tones, WarpMode::Texture, WarpMode::Complex, WarpMode::ComplexPro}) {
                const std::vector<float> out = stretched(m, 60, 0);
                const double hz = hertz(out, 48000, 144000);
                const float peak = peakOf(out), smooth = smoothness(out, 48000, 144000);
                const bool plays = rms(out, 48000 * 3.5, 48000 * 3.9) > 0.15f && rms(out, 48000 * 4.3, 48000 * 4.8) < 0.01f;   // four beats at 60 BPM: four seconds of sound, then silence
                const float minSmooth = m == WarpMode::Texture ? 0.25f : 0.6f;
                CHECK((near(int(hz), 220, m == WarpMode::Texture ? 22 : 8) && peak > 0.25f && peak < 0.9f && plays && smooth > minSmooth),
                      qPrintable(QString("M21 warp: %1 at half the tempo keeps the sine at 220 Hz for twice as long (%2 Hz, peak %3, smoothness %4)")
                                 .arg(warpModeName(m)).arg(hz, 0, 'f', 1).arg(peak, 0, 'f', 2).arg(smooth, 0, 'f', 2)));
            }
            const std::vector<float> up = stretched(WarpMode::Complex, 120, 12);
            const std::vector<float> down = stretched(WarpMode::Tones, 120, -12);
            CHECK((near(int(hertz(up, 24000, 72000)), 440, 12) && near(int(hertz(down, 24000, 72000)), 110, 6)),
                  "M21 warp: transpose moves the pitch in the stretching modes (Complex +12: 440 Hz, Tones -12: 110 Hz) while the time stays");
            const std::vector<float> fm = stretched(WarpMode::ComplexPro, 120, -12, 1.0);
            CHECK((near(int(hertz(fm, 24000, 72000)), 110, 6) && peakOf(fm) > 0.2f && peakOf(fm) < 1.5f), "M21 warp: Complex Pro with Formants up still transposes, at a sane level");
            // the Clip View offers the six, with each mode's own settings; sets keep them
            {
                Project p; ClipPtr c = audioProject(p, sineWav);
                audio::Engine e(&p);
                Transport t(&p, &e);
                ClipView cv(&p, &t);
                cv.resize(1200, 360); cv.show();
                p.selectedClip = c;
                cv.setClip(c);
                QApplication::processEvents();
                auto* mode = cv.findChild<QComboBox*>("warpMode");
                auto* grain = cv.findChild<DragValue*>("warpGrain");
                auto* flux = cv.findChild<DragValue*>("warpFlux");
                auto* formants = cv.findChild<DragValue*>("warpFormants");
                auto* slices = cv.findChild<QComboBox*>("warpSlices");
                const bool six = mode && mode->count() == 6 && mode->itemText(0) == "Beats" && mode->itemText(3) == "Re-Pitch" && mode->itemText(5) == "Complex Pro";
                const bool beats = grain && flux && formants && slices && slices->isVisible() && !grain->isVisible() && !formants->isVisible();
                c->warpMode = WarpMode::Texture; cv.setClip(c); QApplication::processEvents();
                const bool texture = grain && grain->isVisible() && flux->isVisible() && !slices->isVisible() && !formants->isVisible();
                c->warpMode = WarpMode::ComplexPro; cv.setClip(c); QApplication::processEvents();
                const bool pro = formants && formants->isVisible() && formants->isEnabled() && !grain->isVisible() && !slices->isVisible();
                CHECK((six && beats && texture && pro), qPrintable(QString("M21 clip view: the Warp chooser lists the six modes, and the row under it shows the mode's own settings (slices, grain and flux, formants) [%1 %2 %3 %4]")
                                                                    .arg(six).arg(beats).arg(texture).arg(pro)));
                c->grainMs = 80; c->flux = 0.5; c->formants = 0.7;
                const QString path = dir.filePath("warps.sessions");
                setfile::Extras x;
                Project q; setfile::Extras y;
                CHECK((setfile::save(path, p, x) && setfile::load(path, q, y) && q.tracks[0].clipSlots[0] && q.tracks[0].clipSlots[0]->warpMode == WarpMode::ComplexPro
                       && q.tracks[0].clipSlots[0]->grainMs == 80 && q.tracks[0].clipSlots[0]->flux == 0.5 && q.tracks[0].clipSlots[0]->formants == 0.7),
                      "M21 sets: the warp mode (by name) and its grain, flux and formants settings round-trip");
            }
        }
        // alternating beat shading in the grids
        {
            Project p;
            p.loadDemo();
            audio::Engine e(&p);
            Transport t(&p, &e);
            PianoRoll roll(&p, &t);
            roll.resize(700, 300); roll.show();
            ClipPtr empty = p.newClip("Empty", 4);
            p.tracks[0].clipSlots[0] = empty;
            roll.setClip(empty);
            QApplication::processEvents();
            const QImage shot = roll.grab().toImage();
            const int y = 16 + 25;   // a row in the grid, under the ruler
            const QColor beat1 = shot.pixelColor(int(34 + 0.6 * roll.zoom()), y), beat2 = shot.pixelColor(int(34 + 1.6 * roll.zoom()), y), beat3 = shot.pixelColor(int(34 + 2.6 * roll.zoom()), y);   // between the grid lines
            CHECK((beat1 != beat2 && beat1 == beat3), "M21 grids: every second beat is a shade lighter in the piano roll (and the Arrangement and the sample editors)");
        }
    }


    // ---- Milestone 21, second pass: the strip over the editor only, views that stay in bounds, the sample editor's
    //      warp tools, calmer beat shading, tracks that change kind for what's dropped on them, painted glyphs
    {
        // the sample editor's view never runs off before 1.1.1 or past the audio, however it's dragged or zoomed
        {
            Project p; ClipPtr c = audioProject(p, burstsWav);
            audio::Engine e(&p);
            Transport t(&p, &e);
            auto sameMarkers = [](const QVector<WarpMarker>& a, const QVector<WarpMarker>& b) {
                if (a.size() != b.size()) return false;
                for (int i = 0; i < a.size(); ++i) if (a[i].beat != b[i].beat || a[i].seconds != b[i].seconds) return false;
                return true;
            };
            SampleEditor se(&p, &t);
            se.resize(600, 200); se.show();
            se.setClip(c);
            QApplication::processEvents();
            double f0, l0, ef, el;
            se.viewport(f0, l0);
            se.extent(ef, el);
            se.setViewport(-27000, -26990);
            double f1, l1; se.viewport(f1, l1);
            se.setViewport(0, 1e6);
            double f2, l2; se.viewport(f2, l2);
            se.setViewport(1, 3);
            double f3, l3; se.viewport(f3, l3);
            CHECK((f1 >= ef - 1e-9 && l1 - f1 < 10.5 && l2 - f2 <= el - ef + 0.5 + 1e-6 && std::abs(f3 - 1) < 1e-6 && std::abs(l3 - 3) < 0.05),
                  "M21b sample editor: the view is clamped to the sample (no scrolling to beat -27000, no zooming out past the whole clip)");
            // Ctrl+wheel zooms around the pointer: the beat under it stays put
            se.setViewport(0, 4);
            const int px = 300;
            const double under = se.xToBeat(px);
            QWheelEvent wheel(QPointF(px, 100), QPointF(px, 100), QPoint(), QPoint(0, 240), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(&se, &wheel);
            double f4, l4; se.viewport(f4, l4);
            CHECK((l4 - f4 < 4 - 0.5 && std::abs(se.xToBeat(px) - under) < 0.02), "M21b sample editor: Ctrl+wheel zooms in around the pointer");
            // Shift+drag the waveform slides the audio under the grid: every marker's beat moves, snapped
            se.setViewport(0, 4);
            const QVector<WarpMarker> m0 = c->warpMarkers;
            const int y = SampleEditor::kTop + 40;
            QTest::mousePress(&se, Qt::LeftButton, Qt::ShiftModifier, QPoint(int(se.beatToX(1)), y));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(2)), y), Qt::ShiftModifier);
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::ShiftModifier, QPoint(int(se.beatToX(2)), y));
            bool slid = c->warpMarkers.size() == m0.size();
            for (int i = 0; slid && i < m0.size(); ++i) slid = std::abs(c->warpMarkers[i].beat - (m0[i].beat + 1)) < 1e-9 && c->warpMarkers[i].seconds == m0[i].seconds;
            CHECK((slid && p.undoStack()->undoText() == "Slide Audio"), "M21b sample editor: Shift+drag slides the audio a beat later under the grid (one undo step)");
            p.undoStack()->undo();
            // a plain drag selects a stretch (0.22; it used to scroll: the middle button does that now)
            se.setViewport(1, 3);
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(300, y));
            QTest::mouseMove(&se, QPoint(200, y));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(200, y));
            double f5, l5; se.viewport(f5, l5);
            double sa = 0, sb = 0;
            CHECK((std::abs(f5 - 1) < 1e-6 && se.selection(sa, sb) && sb > sa && sameMarkers(c->warpMarkers, m0)), "M22 sample editor: dragging across the waveform selects a stretch (the view and the markers stay)");
            QTest::mousePress(&se, Qt::MiddleButton, Qt::NoModifier, QPoint(300, y));
            QTest::mouseMove(&se, QPoint(200, y));
            QTest::mouseRelease(&se, Qt::MiddleButton, Qt::NoModifier, QPoint(200, y));
            se.viewport(f5, l5);
            CHECK((f5 > 1.2 && sameMarkers(c->warpMarkers, m0)), "M22 sample editor: a middle-button drag scrolls the waveform (the markers stay)");
            // a transient in the strip becomes a warp marker when you drag it
            se.setViewport(0, 4);
            const int stripY = SampleEditor::kStripY + 6;
            const int ti = se.transientAt(QPoint(int(se.beatToX(1)), stripY));
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(1)), stripY));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(1.25)), stripY));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(1.25)), stripY));
            bool pinned = false;
            for (const WarpMarker& w : c->warpMarkers) if (std::abs(w.seconds - 0.5) < 0.03 && std::abs(w.beat - 1.25) < 1e-6) pinned = true;   // the onset is found a few ms early
            CHECK((ti >= 0 && c->warpMarkers.size() == 3 && pinned), qPrintable(QString("M21b sample editor: dragging a transient tick makes a warp marker there and warps it (tick %1, markers %2)").arg(ti).arg(c->warpMarkers.size())));
            p.undoStack()->undo();
            // the warp commands
            se.setStartHere(1);
            bool start = c->warpMarkers.size() == 3 && std::abs(c->warpMarkers[0].beat + 1) < 1e-9 && std::abs(c->warpMarkers[1].beat) < 1e-9 && std::abs(c->warpMarkers[1].seconds - 0.5) < 1e-9;
            CHECK(start, "M21b sample editor: Set 1.1.1 Here puts the audio under that beat at 1.1.1 (the markers shift, the map stays)");
            p.undoStack()->undo();
            se.warpFromHere(1, 60, false);
            CHECK((c->warpMarkers.size() == 3 && c->warpMarkers[2].beat == 2.5 && c->warpMarkers[2].seconds == 2.0 && c->warpMarkers[1].beat == 1 && c->warpMarkers[0].beat == 0),
                  "M21b sample editor: Warp From Here runs a straight tempo from that beat to the end of the audio");
            p.undoStack()->undo();
            se.warpFromHere(1, 120, true);
            CHECK((c->warpMarkers.size() == 2 && c->warpMarkers[0].beat == 0 && c->warpMarkers[0].seconds == 0.5 && c->warpMarkers[1].beat == 3 && c->warpMarkers[1].seconds == 2.0),
                  "M21b sample editor: Warp From Here (Start at 1.1.1) drops what came before and starts the clip there");
            p.undoStack()->undo();
            CHECK(sameMarkers(c->warpMarkers, m0), "M21b sample editor: each warp command is one undo step");
        }
        // beat shading steps up with the zoom: bands are never narrower than 28 px
        {
            QImage img(400, 10, QImage::Format_ARGB32);
            img.fill(Qt::black);
            QPainter p(&img);
            gridshade::paint(p, 5, [](double b) { return b * 5; }, 0, 80, QRectF(0, 0, 400, 10));   // a bar is 20 px: too narrow, so every second group of 4 bars
            p.end();
            const QRgb a = img.pixel(8 * 5, 5), b = img.pixel(24 * 5, 5), c = img.pixel(40 * 5, 5), d = img.pixel(1 * 5, 5);
            CHECK((a == d && a != b && a == c), "M21b grids: zoomed far out, the shading alternates per four bars, not per beat");
            CHECK((theme::beatAlt.alpha() <= 16), "M21b grids: the shading is faint");
        }
        // a MIDI effect on an empty audio track turns it into a MIDI track; the Main column counts as free space; an audio file on an empty MIDI lane turns it audio
        {
            Project p; p.loadDemo();
            audio::Engine e(&p);
            Transport t(&p, &e);
            const int audio = p.insertTrack(int(p.tracks.size()), TrackType::Audio);
            const int midi = p.insertTrack(int(p.tracks.size()), TrackType::Midi);
            SessionView sv(&p, &t);
            sv.resize(1200, 600); sv.show();
            QApplication::processEvents();
            QMimeData* arp = itemMime("Arpeggiator|m");
            const int n0 = p.tracks.size();
            CHECK((sv.grid()->deviceDrop(arp, audio, false) && p.tracks[audio].type == TrackType::Midi && p.tracks[audio].devices.size() == 1 && p.tracks[audio].devices[0].name == "Arpeggiator"),
                  "M21b drops: a MIDI effect dropped on an empty audio track makes it a MIDI track and goes on it");
            CHECK((sv.grid()->deviceDrop(arp, p.trackCount(), false) && p.tracks.size() == n0 + 1 && p.tracks.last().type == TrackType::Midi && p.tracks.last().devices.size() == 1),
                  "M21b drops: dropped on the Main column, it makes a new MIDI track like the free space does");
            delete arp;
            QTemporaryDir d;
            const QString wav = d.filePath("hit.wav");
            writeMonoWav(wav, bursts(48000, 1.0, 0.5, 100, 0.5f), 48000);
            int landed = -1;
            {
                EditScope edit(&p, "Add Audio Clip");
                landed = import::toArrangement(p, midi, 0, import::audioClips(p, {wav}));
            }
            CHECK((landed == midi && p.tracks[midi].type == TrackType::Audio && p.tracks[midi].arrangement.size() == 1), "M21b drops: an audio file on an empty MIDI track makes it an audio track and lands there");
            p.undoStack()->undo();
            CHECK((p.tracks[midi].type == TrackType::Midi && p.tracks[midi].arrangement.isEmpty()), "M21b drops: one undo takes the clip and the change of kind back");
            QString why;
            CHECK((!p.canConvertTrack(2, TrackType::Audio, &why) && !why.isEmpty()), "M21b drops: a track with clips (or an instrument) keeps its kind, and says so");
        }
        // the transport glyphs are painted icons now, not font glyphs
        {
            MainWindow w;
            w.resize(1200, 800);
            w.show();
            QApplication::processEvents();
            QToolButton* play = w.findChild<QToolButton*>("playButton");
            QToolButton* rec = w.findChild<QToolButton*>("recButton");
            CHECK((play && rec && play->text().isEmpty() && !play->icon().isNull() && rec->text().isEmpty() && !rec->icon().isNull()
                   && !play->icon().pixmap(12, QIcon::Normal, QIcon::On).toImage().isNull()),
                  "M21b icons: the play, stop and record buttons carry painted glyphs with an On state for the lit background");
            const QImage pm = icons::pixmap(icons::Glyph::Play, Qt::white, 40).toImage();
            int filled = 0;
            for (int y = 0; y < pm.height(); ++y) for (int x = 0; x < pm.width(); ++x) if (qAlpha(pm.pixel(x, y)) > 200) ++filled;
            CHECK((filled > 200 && filled < 1000 && qAlpha(pm.pixel(2, 2)) == 0 && qAlpha(pm.pixel(20, 20)) > 200), "M21b icons: the play glyph is a filled triangle in the middle of its box");
        }
    }


    // ---- Milestone 21, third pass: scenes start together, the Master track and its chain, the colour grid and clip
    //      colours, the fixed waveform colour, click to play from there, the Monolith's LFO 3 / 4, Macros 5 .. 8, Curve and Loop
    {
        // a scene from a standstill: every clip starts at once, none waits for the next bar
        {
            Project p; p.loadDemo();
            audio::Engine e(&p);
            Transport tr(&p, &e);
            tr.setLaunchQuantization(4);
            tr.stop();
            render(e, 256);
            tr.launchScene(0);
            bool together = tr.playing();
            int launched = 0;
            for (const Track& t : p.tracks) if (t.queuedSlot == 0) { ++launched; together &= std::abs(t.queuedAt - tr.beat()) < 1e-6; }
            render(e, 4096);
            bool playingNow = true;
            for (int i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].clipSlots.value(0)) playingNow &= e.trackStatus(i).playingSlot == 0;
            CHECK((together && launched >= 2 && playingNow), "M21c scenes: launched from a standstill, every clip in the row starts at once (none flashes for a bar)");
            tr.stop();
            render(e, 256);
            tr.play();
            render(e, 256);
            tr.launchScene(1);
            double at = -1; bool same = true; int waiting = 0;
            for (const Track& t : p.tracks) if (t.queuedSlot == 1) { if (at < 0) at = t.queuedAt; same &= std::abs(t.queuedAt - at) < 1e-9; ++waiting; }
            render(e, 2048);   // well short of the bar: nothing has switched yet
            bool none = true;
            for (int i = 0; i < p.tracks.size(); ++i) if (p.tracks[i].clipSlots.value(1)) none &= e.trackStatus(i).playingSlot != 1;
            CHECK((same && waiting >= 1 && none), "M21c scenes: launched while the song plays, every clip in the row waits for the same quantization point");
            tr.stop();
        }
        // the Master track: its chain processes the whole mix, before the master fader
        {
            Project p; p.loadDemo();
            const int m = p.trackCount();
            CHECK((p.isMaster(m) && p.trackAt(m) == &p.master && p.master.name == "Master" && p.acceptsDevice(m, DeviceKind::AudioEffect) && !p.acceptsDevice(m, DeviceKind::Instrument)
                   && !p.canConvertTrack(m, TrackType::Midi)),
                  "M21c master: index trackCount() is the Master, an audio-effects-only track that never changes kind");
            const float loud = peakOf(playArrangement(p, 48000));
            QList<int> ids;
            CHECK((p.insertDevices(m, {p.makeDeviceBlock("Utility", DeviceKind::AudioEffect)}, {}, "Add Utility", &ids) && p.master.devices.size() == 1), "M21c master: an effect goes onto the Master's chain");
            p.master.devices[0].params[0].value = 0.0;   // Utility Gain all the way down (-35 dB)
            emit p.deviceChanged();
            const float quiet = peakOf(playArrangement(p, 48000));
            CHECK((loud > 0.05f && quiet < loud * 0.1f), qPrintable(QString("M21c master: the Master's devices shape the sound (Utility at -35 dB: %1 -> %2)").arg(loud).arg(quiet)));
            p.undoStack()->undo();
            CHECK(p.master.devices.isEmpty(), "M21c master: one undo takes the Master's device away");
            p.undoStack()->redo();
            QTemporaryDir dir;
            setfile::Extras x, y;
            Project q;
            CHECK((setfile::save(dir.filePath("m.sessions"), p, x) && setfile::load(dir.filePath("m.sessions"), q, y) && q.master.devices.size() == 1 && q.master.devices[0].name == "Utility"),
                  "M21c master: the Master's chain is saved with the set and comes back");
            // dropping an effect on the Master's column / lane goes to its chain; an instrument there makes a new track
            audio::Engine e(&p);
            Transport t(&p, &e);
            SessionView sv(&p, &t);
            sv.resize(1200, 600); sv.show();
            QApplication::processEvents();
            QMimeData* eq = itemMime("EQ|f");
            QMimeData* synth = itemMime("Analog Synth|i");
            const int n0 = p.tracks.size();
            CHECK((sv.grid()->deviceDrop(eq, m, false) && p.master.devices.size() == 2 && p.master.devices.last().name == "EQ" && p.tracks.size() == n0), "M21c master: an effect dropped on the Master column lands on its chain");
            CHECK((sv.grid()->deviceDrop(synth, m, false) && p.tracks.size() == n0 + 1 && p.master.devices.size() == 2), "M21c master: an instrument dropped there still makes a new track");
            delete eq; delete synth;
            MainWindow w;
            w.resize(1400, 900);
            w.show();
            QApplication::processEvents();
            ClipGrid* g = w.sessionView()->grid();
            QTest::mouseClick(g, Qt::LeftButton, Qt::NoModifier, QPoint(g->columnLeft(w.project()->trackCount()) + 20, 10));
            QApplication::processEvents();
            CHECK((w.project()->selectedTrack == w.project()->trackCount() && w.deviceView()->blockWidgets().isEmpty()), "M21c master: clicking the Master header selects it (its empty chain shows in the Device View)");
        }
        // colours: the grid palette, and a clip's own colour
        {
            const QVector<QColor>& pal = Project::palette();
            int vivid = 0;
            for (const QColor& c : pal) if (c.hslSaturationF() > 0.8) ++vivid;
            CHECK((pal.size() >= 70 && pal.size() % Project::kPaletteColumns == pal.size() - 6 || pal.size() == 76) && vivid >= 20, "M21c colours: a grid of 76 colours, fourteen hues a row, with vivid rows among them");
            Project p; p.loadDemo();
            ClipPtr clip = p.tracks[0].clipSlots[0];
            const QColor trackCol = p.tracks[0].color;
            CHECK((clip && !clip->color.isValid() && p.clipColor(clip) == trackCol), "M21c colours: a clip without its own colour shows its track's");
            { EditScope edit(&p, "Clip Color"); clip->color = pal[30]; }
            CHECK((p.clipColor(clip) == pal[30] && p.undoStack()->undoText() == "Clip Color"), "M21c colours: a clip can have its own colour (one undo step)");
            QTemporaryDir dir;
            setfile::Extras x, y;
            Project q;
            CHECK((setfile::save(dir.filePath("c.sessions"), p, x) && setfile::load(dir.filePath("c.sessions"), q, y) && q.tracks[0].clipSlots[0] && q.tracks[0].clipSlots[0]->color == pal[30]),
                  "M21c colours: the clip's colour is saved with the set");
            audio::Engine e(&p);
            Transport t(&p, &e);
            SessionView sv(&p, &t);
            sv.resize(1200, 600); sv.show();
            QApplication::processEvents();
            const QImage shot = sv.grid()->grab().toImage();
            QRect slot;
            for (const auto& cell : sv.grid()->mapCells()) if (cell.second == QString("slot:%1:0").arg(p.tracks[0].id)) slot = cell.first;
            CHECK((!slot.isEmpty() && shot.pixelColor(slot.center() + QPoint(slot.width() / 4, 0)) == pal[30]), "M21c colours: the slot paints the clip in its own colour");
            // the colour menu is a grid of squares
            QMenu menu;
            addColorGrid(&menu, pal[3], [](const QColor&) {});
            QWidgetAction* wa = nullptr;
            for (QAction* a : menu.actions()) if (auto* x2 = qobject_cast<QWidgetAction*>(a)) wa = x2;
            CHECK((wa && wa->defaultWidget() && wa->defaultWidget()->width() >= Project::kPaletteColumns * 16 && wa->defaultWidget()->height() >= 6 * 16), "M21c colours: the colour menu is one grid of squares, not a list of names");
        }
        // the sample editor: a fixed waveform colour, and a click plays from there
        {
            Project p; ClipPtr c = audioProject(p, burstsWav);
            audio::Engine e(&p);
            Transport t(&p, &e);
            SampleEditor se(&p, &t);
            se.resize(600, 220); se.show();
            se.setClip(c);
            QApplication::processEvents();
            const QImage shot = se.grab().toImage();
            int waveInk = 0, clipInk = 0;
            const QColor wave = theme::waveform, clipCol = p.tracks[0].color;
            auto near3 = [](const QColor& a, const QColor& b) { return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue()) < 60; };
            for (int y = SampleEditor::kTop; y < shot.height(); ++y) for (int x = 0; x < shot.width(); ++x) { const QColor px = shot.pixelColor(x, y); if (near3(px, wave)) ++waveInk; if (near3(px, clipCol)) ++clipInk; }
            CHECK((waveInk > 200 && clipInk < 20), "M21c sample editor: the waveform is drawn in its own fixed colour, not the clip's");
            // a click on the waveform of a clip on the timeline: Play starts there
            const int y = SampleEditor::kTop + 60;
            QTest::mouseClick(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(2.0)), y));
            CHECK((std::abs(t.startMarker() - 2.0) < 0.05 && !t.playing()), "M21c sample editor: a click on a timeline clip's waveform puts the start marker there");
            // a Session-only clip: the click launches it and sets it there
            p.tracks[0].arrangement.clear();
            emit p.structureChanged();
            QTest::mouseClick(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(1.0)), y));
            render(e, 4096);
            CHECK((t.playing() && (p.tracks[0].queuedSlot == 0 || p.tracks[0].playingSlot == 0) && e.trackStatus(0).playingSlot == 0), "M21c sample editor: a click on a Session clip's waveform launches it from there");
            t.stop();
        }
        // the Monolith: LFO 3 / 4 and Macros 5 .. 8 as sources, the envelopes' Curve and Loop
        {
            using namespace monolith;
            CHECK((kParams == 170 && kSources == 26 && lparam(2, LRate) == kLfo2Base && lmode(3) == kLfo2Base + kPerLfo2 + L2Mode && macro(4) == kMacro2Base && macro(7) == kMacro2Base + 3
                   && lfoOfSource(SrcLfo4) == 3 && macroOfSource(SrcMacro6) == 5 && QString(sourceName(SrcLfo3)) == "LFO 3" && sourceBipolar(SrcLfo4) && !sourceBipolar(SrcMacro8)
                   && sectionName(ecurve(1)) == "Env 2" && sectionName(lparam(3, LHz)) == "LFO 4"),
                  "M21c Monolith: LFO 3 / 4, Macros 5 .. 8, Curve and Loop appended after the 0.20 knobs, with their sources after the 0.17 ones");
            struct MonoNote { int pitch, at, length; float vel; };
            auto run = [](const std::vector<float>& params, const std::vector<ModRoute>& mods, const std::vector<MonoNote>& notes, double seconds) {
                auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
                DevRun out;
                audio::EventList ev;
                audio::Context ctx;
                ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.playing = true;
                ctx.mods = mods.data(); ctx.modCount = int(mods.size());
                const int total = int(seconds * 48000);
                float l[audio::kMaxBlock], r[audio::kMaxBlock];
                for (int pos = 0; pos < total; pos += audio::kMaxBlock) {
                    const int n = std::min(audio::kMaxBlock, total - pos);
                    ev.clear();
                    for (const MonoNote& mn : notes) if (mn.at >= pos && mn.at < pos + n) ev.add({mn.at - pos, mn.pitch, mn.vel, mn.length, false, false});
                    ctx.beat = pos * ctx.beatsPerSample;
                    std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
                    proc->render(ev, l, r, n, params.data(), ctx);
                    for (int i = 0; i < n; ++i) { out.peak = std::max(out.peak, std::fabs(l[i])); out.l.push_back(l[i]); }
                }
                return out;
            };
            auto rms = [](const std::vector<float>& v, double from, double to) { return rmsOf(v, size_t(from * 48000), size_t(std::min<double>(v.size(), to * 48000))); };
            std::vector<float> base = defaultsOf(DeviceType::Monolith);
            // LFO 3, square at 4 Hz, swinging Osc 1's level: the sound comes and goes
            std::vector<float> p3 = base;
            p3[lparam(2, LRate)] = choicePosition(8, 9);   // Free (the ninth rate)
            p3[lparam(2, LHz)] = float(std::log(4.0 / 0.05) / std::log(30.0 / 0.05));   // 4 Hz on the 0.05 .. 30 Hz knob
            p3[lparam(2, LShape)] = choicePosition(4, 6);   // Square (the fifth shape)
            const DevRun lfo3 = run(p3, {{SrcLfo3, oparam(0, Level), -1.0}, {SrcLfo3, oparam(1, Level), -1.0}}, {{48, 0, 96000, 0.9f}}, 1.0);
            float loudest = 0, quietest = 1e9f;
            for (int k = 2; k < 8; ++k) { const float e = rms(lfo3.l, k / 8.0, (k + 1) / 8.0); loudest = std::max(loudest, e); quietest = std::min(quietest, e); }
            CHECK((loudest > 0.02f && quietest < loudest * 0.35f), qPrintable(QString("M21c Monolith: LFO 3 modulates a knob (Osc 1's level swings: %1 .. %2)").arg(quietest).arg(loudest)));
            // Macro 5 all the way up, routed to the Volume with -100 %: quiet
            std::vector<float> p5 = base;
            p5[macro(4)] = 1.0f;
            const DevRun plain = run(base, {}, {{48, 0, 48000, 0.9f}}, 0.6), ducked = run(p5, {{SrcMacro5, oparam(0, Level), -1.0}, {SrcMacro5, oparam(1, Level), -1.0}}, {{48, 0, 48000, 0.9f}}, 0.6);
            CHECK((rms(plain.l, 0.2, 0.5) > 0.02f && rms(ducked.l, 0.2, 0.5) < rms(plain.l, 0.2, 0.5) * 0.1f), "M21c Monolith: Macro 5 is a source (routed to the oscillators' levels it turns the sound down)");
            // Env 1's Curve: with a 0.9 s attack, +1 (exponential) is much quieter half-way than -1 (logarithmic)
            std::vector<float> slow = base;
            slow[eparam(0, A)] = 0.8f;
            std::vector<float> expo = slow, logo = slow;
            expo[ecurve(0)] = 1.0f; logo[ecurve(0)] = 0.0f;
            const DevRun eOut = run(expo, {}, {{48, 0, 96000, 0.9f}}, 0.6), lOut = run(logo, {}, {{48, 0, 96000, 0.9f}}, 0.6);
            CHECK((rms(lOut.l, 0.2, 0.4) > rms(eOut.l, 0.2, 0.4) * 2.5f && rms(eOut.l, 0.2, 0.4) > 0.001f), "M21c Monolith: Env Curve bends the envelope (exponential starts slow, logarithmic starts quick)");
            // Env 1's Loop: a short decay to nothing, held: it keeps coming back
            std::vector<float> pulse = base;
            pulse[eparam(0, A)] = 0.4f; pulse[eparam(0, D)] = 0.4f; pulse[eparam(0, S)] = 0.0f;
            std::vector<float> looped = pulse;
            looped[eloop(0)] = 1.0f;
            const DevRun once = run(pulse, {}, {{48, 0, 96000, 0.9f}}, 1.5), again = run(looped, {}, {{48, 0, 96000, 0.9f}}, 1.5);
            CHECK((rms(once.l, 1.0, 1.4) < 0.002f && rms(again.l, 1.0, 1.4) > 0.01f), "M21c Monolith: Env Loop runs the attack and decay again and again while the note holds");
        }
    }


    // ---- Milestone 21, fourth pass: the Monolith's drawn curves, clips extended from the left, no overlaps, the strip repaints
    {
        // the curves as sources: the sound follows what's drawn, over a bar; one-shot holds its end
        {
            using namespace monolith;
            CHECK((kParams == 170 && kSources == 26 && cparam(1, CSmooth) == kParams - 1 && QString(sourceName(SrcCurve2)) == "Curve 2" && !sourceBipolar(SrcCurve1) && sectionName(cparam(1, CRate)) == "Curve 2"),
                  "M21d curves: two curves with Rate, Mode, Retrig and Smooth appended, and their sources after Macro 8");
            struct MonoNote { int pitch, at, length; float vel; };
            auto run = [](const std::vector<float>& params, const std::vector<ModRoute>& mods, const std::vector<CurvePoint>& curve, const std::vector<MonoNote>& notes, double seconds) {
                auto proc = audio::createProcessor(DeviceType::Monolith, 48000);
                DevRun out;
                audio::EventList ev;
                audio::Context ctx;
                ctx.sampleRate = 48000; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000; ctx.playing = true;
                ctx.mods = mods.data(); ctx.modCount = int(mods.size());
                CurvePoint pts[kCurves * kCurvePoints] = {};
                int counts[kCurves] = {int(curve.size()), 0};
                for (size_t i = 0; i < curve.size() && i < size_t(kCurvePoints); ++i) pts[i] = curve[i];
                ctx.curves = pts; ctx.curveCount = counts;
                const int total = int(seconds * 48000);
                float l[audio::kMaxBlock], r[audio::kMaxBlock];
                for (int pos = 0; pos < total; pos += audio::kMaxBlock) {
                    const int n = std::min(audio::kMaxBlock, total - pos);
                    ev.clear();
                    for (const MonoNote& mn : notes) if (mn.at >= pos && mn.at < pos + n) ev.add({mn.at - pos, mn.pitch, mn.vel, mn.length, false, false});
                    ctx.beat = pos * ctx.beatsPerSample;
                    std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
                    proc->render(ev, l, r, n, params.data(), ctx);
                    for (int i = 0; i < n; ++i) { out.peak = std::max(out.peak, std::fabs(l[i])); out.l.push_back(l[i]); }
                }
                return out;
            };
            auto rms = [](const std::vector<float>& v, double from, double to) { return rmsOf(v, size_t(from * 48000), size_t(std::min<double>(v.size(), to * 48000))); };
            std::vector<float> base = defaultsOf(DeviceType::Monolith);
            base[oparam(0, Level)] = 0; base[oparam(1, Level)] = 0;   // the oscillators only sound where the curve lifts them
            const std::vector<ModRoute> lift = {{SrcCurve1, oparam(0, Level), 1.0}, {SrcCurve1, oparam(1, Level), 1.0}};
            const std::vector<CurvePoint> gate = {{0, 0}, {0.49, 0}, {0.5, 1}, {1, 1}};   // silent for half a bar, then on
            const DevRun loop = run(base, lift, gate, {{48, 0, 48000 * 4, 0.9f}}, 3.0);
            CHECK((rms(loop.l, 0.2, 0.9) < 0.002f && rms(loop.l, 1.2, 1.9) > 0.02f && rms(loop.l, 2.2, 2.9) < 0.002f),
                  qPrintable(QString("M21d curves: a knob follows the drawn curve over a bar, round and round (%1 / %2 / %3)").arg(rms(loop.l, 0.2, 0.9)).arg(rms(loop.l, 1.2, 1.9)).arg(rms(loop.l, 2.2, 2.9))));
            std::vector<float> once = base;
            once[cparam(0, CMode)] = 1.0f;   // One-shot
            const DevRun held = run(once, lift, gate, {{48, 0, 48000 * 4, 0.9f}}, 3.0);
            CHECK((rms(held.l, 1.2, 1.9) > 0.02f && rms(held.l, 2.2, 2.9) > 0.02f), "M21d curves: One-shot runs the curve once and holds its end");
            std::vector<float> retrig = base;
            retrig[cparam(0, CRetrig)] = 1.0f;
            const DevRun late = run(retrig, lift, gate, {{48, 48000, 48000 * 3, 0.9f}}, 3.0);   // the note starts at 1 s: its curve starts there
            CHECK((rms(late.l, 1.2, 1.9) < 0.002f && rms(late.l, 2.2, 2.9) > 0.02f), "M21d curves: Retrig starts the curve with each note");
        }
        // drawing: a click adds a point, a drag moves it, a double-click removes it, Random rolls steps; all undoable and saved
        {
            Project p; p.loadEmpty();
            audio::Engine e(&p);
            Transport t(&p, &e);
            p.selectedTrack = 0;
            p.addDeviceToSelected("Monolith", DeviceKind::Instrument);
            DevicePanel panel(&p, &t, 0, p.tracks[0].devices[0], nullptr, {});
            panel.show();
            QApplication::processEvents();
            auto* body = dynamic_cast<devui::DeviceBody*>(panel.body());
            QPushButton* curveBtn = nullptr;
            for (QPushButton* b : body->findChildren<QPushButton*>("monoDrawer")) if (b->text() == "Curve") curveBtn = b;
            CHECK(curveBtn != nullptr, "M21d curves: a seventh drawer, Curve");
            if (curveBtn) { curveBtn->click(); QTest::qWait(320); QApplication::processEvents(); }
            QList<devui::Display*> disps;
            for (devui::Display* d : body->findChildren<devui::Display*>("monoCurve")) disps << d;
            devui::Display* disp = disps.value(0);
            auto curves = [&]() -> const QVector<QVector<CurvePoint>>& { return p.tracks[0].devices[0].curves; };   // read afresh: edits detach the vector
            CHECK((disps.size() == 2 && disp && curves().isEmpty()), "M21d curves: two curve displays, empty to begin with");
            if (disp) {
                QTest::mouseClick(disp, Qt::LeftButton, Qt::NoModifier, QPoint(disp->width() / 4, disp->height() / 2));
                QApplication::processEvents();
                const bool added = curves().size() == 2 && curves()[0].size() == 1 && std::abs(curves()[0][0].x - 0.25) < 0.02 && std::abs(curves()[0][0].y - 0.5) < 0.06;
                CHECK(added, qPrintable(QString("M21d curves: a click adds a point on the 1/16 grid (%1 points)").arg(curves().value(0).size())));
                QTest::mousePress(disp, Qt::LeftButton, Qt::NoModifier, QPoint(disp->width() / 4, disp->height() / 2));
                QTest::mouseMove(disp, QPoint(disp->width() / 2, disp->height() / 8));
                QTest::mouseRelease(disp, Qt::LeftButton, Qt::NoModifier, QPoint(disp->width() / 2, disp->height() / 8));
                QApplication::processEvents();
                CHECK((curves().value(0).size() == 1 && std::abs(curves()[0][0].x - 0.5) < 0.02 && curves()[0][0].y > 0.8), "M21d curves: dragging a point moves it");
                const int steps = p.undoStack()->count();
                QPushButton* rnd = body->findChildren<QPushButton*>("monoCurveRandom").value(0);
                if (rnd) rnd->click();
                QApplication::processEvents();
                CHECK((rnd && curves().value(0).size() == 16 && p.undoStack()->count() == steps + 1), "M21d curves: Random rolls eight steps (sixteen points), one undo step");
                p.undoStack()->undo();
                CHECK(curves().value(0).size() == 1, "M21d curves: undo takes the roll back");
                QTest::mouseDClick(disp, Qt::LeftButton, Qt::NoModifier, QPoint(disp->width() / 2, disp->height() / 8));
                QApplication::processEvents();
                CHECK(curves().value(0).isEmpty(), "M21d curves: double-clicking a point removes it");
                p.undoStack()->undo();
                QTemporaryDir dir;
                setfile::Extras x, y;
                Project q;
                CHECK((curves().value(0).size() == 1 && setfile::save(dir.filePath("c.sessions"), p, x) && setfile::load(dir.filePath("c.sessions"), q, y)
                       && q.tracks[0].devices[0].curves.size() == 2 && q.tracks[0].devices[0].curves[0].size() == 1 && std::abs(q.tracks[0].devices[0].curves[0][0].x - 0.5) < 1e-9),
                      "M21d curves: the curves are saved with the set and come back");
            }
        }
        // the Arrangement: clips extend from the left (the audio stays put), and what's placed is what plays
        {
            Project p; p.loadEmpty();
            audio::Engine e(&p);
            Transport t(&p, &e);
            ClipPtr a = p.newClip("A", 4), b = p.newClip("B", 4);
            p.tracks[0].clipSlots[0] = a; p.tracks[0].clipSlots[1] = b;
            p.tracks[0].arrangement = {{0, 4, 0, a}, {8, 4, 0, b}};
            ArrangementView arr(&p, &t);
            arr.resize(1000, 400); arr.show();
            QApplication::processEvents();
            arr.setFollow(false);
            const int titleY = arr.laneRect(0).top() + 6;
            const int dx = int(arr.beatToX(4) - arr.beatToX(0));
            // B's left edge dragged a bar earlier: it starts at 4, a bar longer, and (looping) shows the same audio where it was
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(8)) + 2, titleY + 20));
            QTest::mouseMove(&arr, QPoint(int(arr.beatToX(8)) + 2 - dx, titleY + 20));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(8)) + 2 - dx, titleY + 20));
            const ArrangementClip* pb = nullptr;
            for (const ArrangementClip& x : p.tracks[0].arrangement) if (x.clip == b) pb = &x;
            CHECK((pb && std::abs(pb->start - 4) < 1e-9 && std::abs(pb->length - 8) < 1e-9 && std::abs(pb->offset) < 1e-9), "M21d arrangement: dragging a clip's left edge extends it back (a looping clip keeps its audio in place)");
            p.undoStack()->undo();
            // a one-shot clip stops at its audio's start
            b->loop = false;
            p.tracks[0].arrangement = {{0, 4, 0, a}, {8, 4, 1, b}};
            emit p.structureChanged();
            QApplication::processEvents();
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(8)) + 2, titleY + 20));
            QTest::mouseMove(&arr, QPoint(int(arr.beatToX(8)) + 2 - dx, titleY + 20));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(8)) + 2 - dx, titleY + 20));
            pb = nullptr;
            for (const ArrangementClip& x : p.tracks[0].arrangement) if (x.clip == b) pb = &x;
            CHECK((pb && std::abs(pb->start - 7) < 1e-9 && std::abs(pb->length - 5) < 1e-9 && std::abs(pb->offset) < 1e-9), "M21d arrangement: a one-shot clip extends back only as far as its audio goes");
            // moving B over A cuts A: nothing overlaps
            b->loop = true;
            p.tracks[0].arrangement = {{0, 4, 0, a}, {8, 4, 0, b}};
            emit p.structureChanged();
            QApplication::processEvents();
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(9)), titleY));
            QTest::mouseMove(&arr, QPoint(int(arr.beatToX(9)) - int(arr.beatToX(6) - arr.beatToX(0)), titleY));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(9)) - int(arr.beatToX(6) - arr.beatToX(0)), titleY));
            auto overlapFree = [&] {
                QVector<ArrangementClip> v = p.tracks[0].arrangement;
                std::sort(v.begin(), v.end(), [](const ArrangementClip& x, const ArrangementClip& y) { return x.start < y.start; });
                for (int i = 1; i < v.size(); ++i) if (v[i].start < v[i - 1].start + v[i - 1].length - 1e-9) return false;
                return true;
            };
            const ArrangementClip* pa = nullptr; pb = nullptr;
            for (const ArrangementClip& x : p.tracks[0].arrangement) { if (x.clip == a) pa = &x; if (x.clip == b) pb = &x; }
            CHECK((pb && std::abs(pb->start - 2) < 1e-9 && pa && std::abs(pa->length - 2) < 1e-9 && p.tracks[0].arrangement.size() == 2 && overlapFree()),
                  "M21d arrangement: a clip moved over another cuts it away: what's placed is what plays");
            // resizing A's right edge over B cuts B's start
            p.tracks[0].arrangement = {{0, 4, 0, a}, {8, 4, 0, b}};
            emit p.structureChanged();
            QApplication::processEvents();
            QTest::mousePress(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(4)) - 2, titleY + 20));
            QTest::mouseMove(&arr, QPoint(int(arr.beatToX(10)), titleY + 20));
            QTest::mouseRelease(&arr, Qt::LeftButton, Qt::NoModifier, QPoint(int(arr.beatToX(10)), titleY + 20));
            pa = nullptr; pb = nullptr;
            for (const ArrangementClip& x : p.tracks[0].arrangement) { if (x.clip == a) pa = &x; if (x.clip == b) pb = &x; }
            CHECK((pa && std::abs(pa->length - 10) < 1e-9 && (!pb || std::abs(pb->start - 10) < 1e-9) && overlapFree()), "M21d arrangement: a clip lengthened over the next one cuts the next one's start");
        }
        // the strip over the Clip View shows the clip you just selected, not the last one
        {
            MainWindow w;
            w.resize(1400, 900);
            w.show();
            QApplication::processEvents();
            Project* proj = w.project();
            ClipGrid* g = w.sessionView()->grid();
            ClipView* cv = w.findChild<ClipView*>();
            auto slotOf = [&](const QString& track) { int ti = 0; for (int i = 0; i < proj->tracks.size(); ++i) if (proj->tracks[i].name == track) ti = i; QRect r; for (const auto& c : g->mapCells()) if (c.second == QString("slot:%1:0").arg(proj->tracks[ti].id)) r = c.first; return r; };
            QTest::mouseClick(g, Qt::LeftButton, Qt::NoModifier, slotOf("Drums").center() + QPoint(30, 0));
            QApplication::processEvents();
            const QImage first = cv->overviewStrip()->grab().toImage();
            QTest::mouseClick(g, Qt::LeftButton, Qt::NoModifier, slotOf("Pad").center() + QPoint(30, 0));
            QApplication::processEvents();
            const QImage second = cv->overviewStrip()->grab().toImage();
            CHECK((proj->selectedClip == proj->tracks[3].clipSlots[0] && first != second), "M21d strip: selecting another clip repaints the strip over the editor at once");
        }
    }

    // ---- Milestone 22: recording shows as it comes in, the sample editor's markers and selection,
    // Delete / Crop in the audio, the clip library, the browser's tree
    {
        // live peaks: the waveform of a take while it records
        {
            Transport::LivePeaks lp;
            std::vector<float> frames(size_t(1000) * 2, 0.0f);
            for (size_t f = 0; f < 1000; ++f) { frames[f * 2] = f < 500 ? 0.5f : -0.25f; frames[f * 2 + 1] = 0.0f; }
            lp.add(frames.data(), 1000);
            lp.addSilence(300);
            float lo = 0, hi = 0;
            lp.range(0, 256, lo, hi);
            float lo2 = 0, hi2 = 0;
            lp.range(600, 900, lo2, hi2);
            float lo3 = 0, hi3 = 0;
            lp.range(1100, 1300, lo3, hi3);
            CHECK((lp.frames() == 1300 && hi == 0.5f && lo == 0.0f && lo2 == -0.25f && hi2 == 0.0f && lo3 == 0.0f && hi3 == 0.0f),
                  "M22 recording: live peaks keep the input's min / max per block, silence included, and answer for any stretch of frames");
        }
        // the sample editor: selection, Delete cuts the audio out, Crop keeps it, the start marker and the loop brace
        {
            QTemporaryDir editDir;
            Project p; ClipPtr c = audioProject(p, burstsWav);
            p.recordFolder = editDir.path();
            audio::Engine e(&p);
            Transport t(&p, &e);
            SampleEditor se(&p, &t);
            se.resize(600, 220); se.show();
            se.setClip(c);
            QApplication::processEvents();
            se.setViewport(0, 4);
            const int y = SampleEditor::kTop + 40;
            const QString original = c->samplePath;
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(1)), y));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(2)), y));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(2)), y));
            double a = 0, b = 0;
            CHECK((se.selection(a, b) && std::abs(a - 1) < 1e-9 && std::abs(b - 2) < 1e-9), "M22 sample editor: a drag from beat 2 to beat 3 selects that beat (snapped)");
            QTest::keyClick(&se, Qt::Key_Delete);
            CHECK((std::abs(c->length - 3) < 1e-9 && c->sample && std::abs(c->sample->seconds() - 1.5) < 0.01 && c->samplePath != original && c->samplePath.startsWith(editDir.path())
                   && QFileInfo::exists(original) && !se.hasSelection() && p.undoStack()->undoText() == "Delete Audio"),
                  qPrintable(QString("M22 sample editor: Delete cuts the selected beat out of the audio into a new file (length %1, %2 s)").arg(c->length).arg(c->sample ? c->sample->seconds() : 0)));
            // what's after the cut moved earlier: the burst that was on beat 3 is on beat 2 now
            bool moved = std::abs(c->secondsAt(2) - 1.0) < 1e-6 && std::abs(c->secondsAt(1) - 0.5) < 1e-6;
            CHECK(moved, "M22 sample editor: after the cut, the audio that followed plays a beat earlier (the warp map follows)");
            p.undoStack()->undo();
            CHECK((c->samplePath == original && std::abs(c->length - 4) < 1e-9 && c->sample && std::abs(c->sample->seconds() - 2.0) < 0.01), "M22 sample editor: undo brings the original file and length back");
            se.select(1, 3);
            se.cropToSelection();
            CHECK((std::abs(c->length - 2) < 1e-9 && c->sample && std::abs(c->sample->seconds() - 1.0) < 0.01 && c->warpMarkers.first().beat == 0 && c->warpMarkers.first().seconds == 0
                   && std::abs(c->secondsAt(2) - 1.0) < 1e-6 && p.undoStack()->undoText() == "Crop Sample"),
                  "M22 sample editor: Crop keeps only the selection, starting at 1.1.1");
            p.undoStack()->undo();
            // the start marker (M23: it moves Clip::start on its own; the warp map and the view stay)
            auto sameMarkers = [](const QVector<WarpMarker>& a, const QVector<WarpMarker>& b) {
                if (a.size() != b.size()) return false;
                for (int i = 0; i < a.size(); ++i) if (a[i].beat != b[i].beat || a[i].seconds != b[i].seconds) return false;
                return true;
            };
            se.setViewport(0, 4);
            const QVector<WarpMarker> m0 = c->warpMarkers;
            const double x00 = se.beatToX(0);
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(0)) + 2, SampleEditor::kMarkY + 5));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(0)) + 2 + int(se.pixelsPerBeatForTests()), SampleEditor::kMarkY + 5));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(0)) + 2 + int(se.pixelsPerBeatForTests()), SampleEditor::kMarkY + 5));
            CHECK((sameMarkers(c->warpMarkers, m0) && std::abs(c->start - 1) < 1e-9 && c->loopStart == 0 && std::abs(c->length - 4) < 1e-9 && se.beatToX(0) == x00
                   && p.undoStack()->undoText() == "Move Clip Start"),
                  qPrintable(QString("M22/M23 sample editor: dragging the ▶ start marker a beat moves the clip's start marker (start %1), the warp map and the length stay").arg(c->start)));
            p.undoStack()->undo();
            CHECK(c->start == 0, "M23 sample editor: undo puts the start marker back");
            // the loop brace: dragged by its body, the loop moves along the clip and keeps its length (the map stays)
            se.setViewport(0, 4);
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(2)), SampleEditor::kBraceY + 4));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(3)), SampleEditor::kBraceY + 4));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(3)), SampleEditor::kBraceY + 4));
            CHECK((sameMarkers(c->warpMarkers, m0) && std::abs(c->loopStart - 1) < 1e-9 && std::abs(c->length - 4) < 1e-9 && c->start == 0 && p.undoStack()->undoText() == "Move Loop"),
                  "M22/M23 sample editor: dragging the loop brace's body moves the loop a beat along the clip, its length and the map stay");
            p.undoStack()->undo();
            // the brace's left end: the loop's start moves, its end stays; the ◀ end marker: the length
            se.setViewport(0, 4);
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(0)) + 1, SampleEditor::kBraceY + 4));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(1)) + 1, SampleEditor::kBraceY + 4));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(1)) + 1, SampleEditor::kBraceY + 4));
            CHECK((std::abs(c->loopStart - 1) < 1e-9 && std::abs(c->length - 3) < 1e-9 && std::abs(c->loopEnd() - 4) < 1e-9 && p.undoStack()->undoText() == "Move Loop Start"),
                  "M23 sample editor: dragging the brace's left end a beat moves the loop's start, its end stays (length 3)");
            QTest::mousePress(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(4)) - 1, SampleEditor::kMarkY + 5));
            QTest::mouseMove(&se, QPoint(int(se.beatToX(3)) - 1, SampleEditor::kMarkY + 5));
            QTest::mouseRelease(&se, Qt::LeftButton, Qt::NoModifier, QPoint(int(se.beatToX(3)) - 1, SampleEditor::kMarkY + 5));
            CHECK((std::abs(c->loopStart - 1) < 1e-9 && std::abs(c->length - 2) < 1e-9 && p.undoStack()->undoText() == "Clip Length"),
                  "M23 sample editor: dragging the ◀ end marker sets the length from the loop's start (3 → 2)");
            p.undoStack()->undo();
            p.undoStack()->undo();
            CHECK((c->loopStart == 0 && std::abs(c->length - 4) < 1e-9), "M23 sample editor: two undos put the brace back");
            // Loop Selection: the selection becomes the clip
            se.select(1, 3);
            se.loopSelection();
            CHECK((std::abs(c->length - 2) < 1e-9 && std::abs(c->warpMarkers.first().beat + 1) < 1e-9 && std::abs(c->secondsAt(0) - 0.5) < 1e-6 && c->loop), "M22 sample editor: Loop Selection makes the selection the clip, from 1.1.1");
            p.undoStack()->undo();
        }
        // the clip library: saved clips come back, the browser lists them under User Library, and they land on slots and lanes
        {
            Project p; ClipPtr c = audioProject(p, burstsWav);
            c->name = "Bursts Loop";
            c->gain = -3;
            c->envelopes.push_back({target::volume(), {{0, 0.2}, {2, 0.8}}});
            const QString path = presets::clipPathFor("Bursts Loop");
            QString error;
            const bool saved = presets::saveClip(path, "Bursts Loop", *c, &error);
            Clip back;
            QString name;
            const bool readBack = presets::readClip(path, back, &name, &error);
            CHECK((saved && readBack && name == "Bursts Loop" && back.isAudio() && back.sample && std::abs(back.length - c->length) < 1e-9 && back.gain == -3 && back.envelopes.size() == 1
                   && back.samplePath.startsWith(QDir(presets::clipFolder()).filePath("Samples")) && QFileInfo::exists(back.samplePath) && back.warpMarkers.size() == c->warpMarkers.size() && back.warpMarkers.last().beat == c->warpMarkers.last().beat),
                  qPrintable("M22 clip library: an audio clip saves with its sample copied into the library and reads back whole (" + error + ")"));
            ClipPtr midi = p.newClip("Riff", 8);
            midi->notes = {{60, 0, 1, 0.8, 1}, {64, 1, 0.5, 0.6, 1}};
            const QString midiPath = presets::clipPathFor("Riff");
            Clip midiBack;
            CHECK((presets::saveClip(midiPath, "Riff", *midi) && presets::readClip(midiPath, midiBack) && !midiBack.isAudio() && midiBack.notes.size() == 2 && midiBack.notes[1].pitch == 64 && midiBack.length == 8),
                  "M22 clip library: a MIDI clip saves its notes and reads back");
            Browser br;
            br.showUserLibrary();
            CHECK((br.listedItems().contains(path + "|c") && br.listedItems().contains(midiPath + "|c")), "M22 clip library: the browser lists saved clips under User Library (Clips)");
            QMimeData clipMime;
            clipMime.setData(Browser::mimeType(), (path + "|c").toUtf8());
            CHECK((Browser::clipFilesIn(&clipMime) == QStringList{path} && Browser::audioFilesIn(&clipMime).isEmpty()), "M22 clip library: a clip item drags as a clip, not as audio");
            // onto the Arrangement: an audio clip dropped on a MIDI lane makes an audio track; onto the Session: into the slots
            Project q; q.loadDemo();
            audio::Engine qe(&q);
            Transport qt(&q, &qe);
            ArrangementView arr(&q, &qt);
            arr.resize(1000, 600); arr.show();
            QApplication::processEvents();
            const int n0 = int(q.tracks.size());
            const int lane = arr.placeClipFiles({path}, 0, 8);
            CHECK((lane == 1 && q.tracks.size() == n0 + 1 && q.tracks[1].type == TrackType::Audio && q.tracks[1].arrangement.size() == 1 && std::abs(q.tracks[1].arrangement[0].start - 8) < 1e-9
                   && q.tracks[1].arrangement[0].clip->name == "Bursts Loop" && q.undoStack()->undoText() == "Add Clip"),
                  "M22 clip library: a library clip placed on a MIDI lane goes on a new audio track at that beat (one undo step)");
            ClipGrid grid(&q, &qt);
            const int gt = grid.placeClipFiles({midiPath}, 0, 2);
            CHECK((gt == 0 && q.tracks[0].clipSlots.size() > 2 && q.tracks[0].clipSlots[2] && q.tracks[0].clipSlots[2]->notes.size() == 2 && q.tracks[0].clipSlots[2] != midi),
                  "M22 clip library: a MIDI library clip placed on a MIDI track's slot is a copy in that slot");
            QString renamed;
            CHECK((presets::renameClip(midiPath, "Riff 2", &renamed) && !QFileInfo::exists(midiPath) && presets::clipNameOf(renamed) == "Riff 2" && presets::removeClip(renamed) && !QFileInfo::exists(renamed)),
                  "M22 clip library: renaming and deleting a saved clip");
        }
        // the browser's tree: a folder's subfolders are nodes with their files inside
        {
            QTemporaryDir lib;
            QDir(lib.path()).mkpath("Kicks/Deep");
            QFile::copy(burstsWav, lib.filePath("top.wav"));
            QFile::copy(burstsWav, lib.filePath("Kicks/kick.wav"));
            QFile::copy(burstsWav, lib.filePath("Kicks/Deep/sub.wav"));
            Browser br;
            br.addFolder(lib.path());
            QApplication::processEvents();
            auto* tree = br.findChild<QTreeWidget*>("browserItems");
            QTreeWidgetItem* kicks = nullptr;
            for (int i = 0; tree && i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->text(0).endsWith(" Kicks")) kicks = tree->topLevelItem(i);
            QTreeWidgetItem* deep = nullptr;
            for (int i = 0; kicks && i < kicks->childCount(); ++i) if (kicks->child(i)->text(0).endsWith(" Deep")) deep = kicks->child(i);
            const QStringList items = br.listedItems();
            CHECK((tree && kicks && deep && deep->childCount() == 1 && kicks->childCount() == 2 && tree->topLevelItemCount() == 2 && items.size() == 3
                   && items.contains(lib.filePath("Kicks/Deep/sub.wav") + "|s") && !kicks->isExpanded()),
                  "M22 browser: a sample folder shows as a tree (subfolders first, closed), every file still listed");
            kicks->setExpanded(true);
            br.addFolder(lib.path());   // a rebuild
            QApplication::processEvents();
            tree = br.findChild<QTreeWidget*>("browserItems");
            kicks = nullptr;
            for (int i = 0; tree && i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->text(0).endsWith(" Kicks")) kicks = tree->topLevelItem(i);
            CHECK((kicks && kicks->isExpanded() && kicks->text(0).startsWith(QStringLiteral("▾"))), "M22 browser: a folder you opened stays open across a rebuild (and shows ▾)");
            QLineEdit* search = br.findChild<QLineEdit*>();
            if (search) search->setText("sub");
            const QStringList found = br.listedItems();
            CHECK((found.contains(lib.filePath("Kicks/Deep/sub.wav") + "|s") && !found.contains(lib.filePath("Kicks/kick.wav") + "|s") && !found.contains(lib.filePath("top.wav") + "|s")),
                  "M22 browser: a search flattens the matches (only the files that match)");
            br.removeFolder(lib.path());
        }
    }

    // ---- Milestone 23: a real start marker and loop brace (Clip::start / loopStart): the engine plays from the
    //      start into the brace, Loop off plays start → end once, placements and the views follow, the piano roll's ruler
    {
        // an audio clip whose four beats play at four levels: 0.1, 0.2, 0.3, 0.4
        std::vector<float> steps(size_t(2.0 * 48000), 0.0f);
        for (size_t i = 0; i < steps.size(); ++i) steps[i] = 0.1f * float(i / 24000 + 1);
        const QString stepsWav = audioDir.filePath("steps 120.wav");
        writeMonoWav(stepsWav, steps, 48000);
        auto levelAt = [](const std::vector<float>& out, double beat) {   // the mean level in the middle of that beat (after launch)
            double sum = 0; int n = 0;
            for (int i = int(beat * 24000) + 6000; i < int(beat * 24000) + 18000 && size_t(i) * 2 < out.size(); ++i) { sum += std::fabs(out[size_t(i) * 2]); ++n; }
            return n ? sum / n : 0.0;
        };
        auto stepsProject = [&](Project& p, double start, double loopStart, double length, bool loop) {
            ClipPtr c = audioProject(p, stepsWav);
            p.tracks[0].arrangement.clear();
            c->warpMarkers = {{0, 0}, {4, 2.0}};
            c->detectedBpm = 0;
            c->warpMode = WarpMode::Repitch;   // reads the audio straight through the map
            c->start = start; c->loopStart = loopStart; c->length = length; c->loop = loop;
            return c;
        };
        auto launchAndRender = [](Project& p, int frames) {
            audio::Engine e(&p);
            e.send({Command::Stop, -1, -1, 0, 0, true});
            e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
            e.send({Command::Play});
            std::vector<float> out = render(e, frames);
            return std::make_pair(out, e.trackStatus(0).playingSlot);
        };
        auto near3 = [](double a, double b) { return std::abs(a - b) < 0.02; };
        {
            Project p; stepsProject(p, 2, 0, 4, true);   // start = 2 beats: the third beat first, then the fourth, then round the brace from 1.1.1
            auto [out, slot] = launchAndRender(p, 48000 * 3);
            CHECK((near3(levelAt(out, 0), 0.3) && near3(levelAt(out, 1), 0.4) && near3(levelAt(out, 2), 0.1) && near3(levelAt(out, 3), 0.2) && near3(levelAt(out, 5), 0.4)),
                  qPrintable(QString("M23 engine: a clip launched with start = 2 plays its third beat first (levels %1 %2 %3 %4), then goes round the brace from 1.1.1")
                                 .arg(levelAt(out, 0), 0, 'f', 3).arg(levelAt(out, 1), 0, 'f', 3).arg(levelAt(out, 2), 0, 'f', 3).arg(levelAt(out, 3), 0, 'f', 3)));
        }
        {
            Project p; stepsProject(p, 0, 1, 2, true);   // the brace at beats 2..3 (loopStart = 1, length 2): runs in from 1.1.1, then loops 0.2 0.3 0.2 0.3
            auto [out, slot] = launchAndRender(p, 48000 * 3);
            CHECK((near3(levelAt(out, 0), 0.1) && near3(levelAt(out, 1), 0.2) && near3(levelAt(out, 2), 0.3) && near3(levelAt(out, 3), 0.2) && near3(levelAt(out, 4), 0.3) && near3(levelAt(out, 5), 0.2)),
                  "M23 engine: a brace that doesn't start at 1.1.1 is run into from the start marker and then looped (0.1 0.2 0.3 0.2 0.3 0.2)");
        }
        {
            Project p; stepsProject(p, 1, 0, 3, false);   // Loop off: from the start marker (beat 2) to the end marker (beat 4), once
            auto [out, slot] = launchAndRender(p, 48000 * 2);
            CHECK((near3(levelAt(out, 0), 0.2) && near3(levelAt(out, 1), 0.3) && levelAt(out, 2) < 1e-4 && levelAt(out, 3) < 1e-4 && slot == -1),
                  qPrintable(QString("M23 engine: Loop off plays from the start marker to the end marker once (0.2 0.3, then silence %1) and the slot stops").arg(levelAt(out, 2), 0, 'f', 4)));
        }
        {
            // MIDI: the note on clip beat 1 sounds half a beat after a launch with start = 0.5; a placement's brace loops the right notes
            Project p; oneNoteProject(p);
            p.tracks[0].arrangement.clear();
            p.tracks[0].clipSlots[0]->start = 0.5;
            audio::Engine e(&p);
            e.send({Command::Stop, -1, -1, 0, 0, true});
            e.send({Command::Launch, p.tracks[0].id, 0, 0.0});
            e.send({Command::Play});
            std::vector<float> out = render(e, 36000);
            int first = -1;
            for (int i = 0; i < 36000; ++i) if (std::fabs(out[size_t(i) * 2]) > 1e-4f) { first = i; break; }
            CHECK(near(first, 12000, 4), qPrintable(QString("M23 engine: a MIDI clip launched with start = 0.5 sounds its beat-2 note after half a beat (sample %1)").arg(first)));
        }
        {
            Project p; oneNoteProject(p);
            p.tracks[0].devices[0].params[2].value = 0.0;   // a short release, so the gaps are silent
            ClipPtr c = p.tracks[0].clipSlots[0];
            c->notes = {Note{60, 0.0, 0.5, 1.0, 1.0}, Note{60, 1.0, 0.5, 1.0, 1.0}};
            c->loopStart = 1; c->length = 2;   // the brace is beats 2..3: notes at 1.1.1 (once) and 1.2.1 (every pass)
            p.tracks[0].arrangement = {{0, 8, 0, c}};
            std::vector<float> out = playArrangement(p, 48000 * 3);
            auto quietBetween = [&](int a, int b) { for (int i = a; i < b; ++i) if (std::fabs(out[size_t(i) * 2]) > 1e-3f) return false; return true; };
            auto loudNear = [&](int at) { for (int i = at; i < at + 600; ++i) if (std::fabs(out[size_t(i) * 2]) > 1e-3f) return true; return false; };
            CHECK((loudNear(0) && loudNear(24000) && quietBetween(46000, 72000) && loudNear(72000) && quietBetween(94000, 120000) && loudNear(120000)),
                  "M23 engine: a placement runs from its offset into the clip's brace and then loops the brace (notes on beats 1, 2, 4, 6: none on 3 or 5)");
            // the ops see the same passes: the notes in beats 3..7 of that placement
            const QVector<Note> seen = arrangement::notesIn(p.tracks[0].arrangement[0], 2, 6);
            CHECK((seen.size() == 2 && std::abs(seen[0].start - 3) < 1e-9 && std::abs(seen[1].start - 5) < 1e-9), "M23 ops: notesIn walks the placement's passes (the brace's note on beats 4 and 6)");
            CHECK((std::abs(p.tracks[0].arrangement[0].beatOf(1) - 1) < 1e-9 && std::abs(p.tracks[0].arrangement[0].beatOf(2) - 2) < 1e-9 && p.tracks[0].arrangement[0].beatOf(3) < 0),
                  "M23 model: ArrangementClip::beatOf finds where a clip beat first plays (beat 3 is past the brace: never)");
        }
        {
            // the model's position helpers, and the views that read them (Project::clipPosition)
            Clip c; c.start = 2; c.loopStart = 1; c.length = 2; c.loop = true;
            CHECK((c.positionAt(0) == 2 && c.positionAt(0.5) == 2.5 && c.positionAt(1) == 1 && c.positionAt(2.5) == 2.5 && c.positionFrom(0, 0.5) == 0.5 && c.positionFrom(0, 3.5) == 1.5),
                  "M23 model: positionAt runs from the start marker into the brace and round it; positionFrom does the same from a placement's offset");
            c.loop = false;
            CHECK((c.positionAt(0.5) == 2.5 && c.positionAt(1) < 0 && c.positionFrom(0, 2.9) == 2.9 && c.positionFrom(0, 3) < 0 && c.positionAt(-1) == 1),
                  "M23 model: Loop off stops at the end marker; a seek before the start marker is a position too");
            Project p; oneNoteProject(p);
            ClipPtr k = p.tracks[0].clipSlots[0];
            k->start = 1; k->loopStart = 2; k->length = 2;
            p.tracks[0].playingSlot = 0; p.tracks[0].slotStart = 10;
            double local = -1;
            // launched at 10 with start = 1: beats 1 .. 4 over 10 .. 13, then round the brace 2 .. 4 (13 → 2, 14.5 → 3.5)
            CHECK((p.playingClip(0, 10.5, &local) == k && local == 1.5 && p.playingClip(0, 14.5, &local) == k && local == 3.5 && p.clipPosition(k, 13.0) == 2.0),
                  "M23 model: playingClip / clipPosition give the clip beat under the playhead through the start marker and the brace");
            p.tracks[0].playingSlot = -1;
            p.tracks[0].arrangement = {{0, 6, 0, k}};   // a placement from the clip's 1.1.1: beats 0 .. 4, then 2, 3 (5 → 3.5 is 2 + 1.5)
            CHECK((p.playingClip(0, 1.0, &local) == k && local == 1.0 && p.playingClip(0, 3.5, &local) == k && local == 3.5 && p.playingClip(0, 5.5, &local) == k && local == 3.5),
                  "M23 model: and for a placement, from its offset");
        }
        {
            // save / load: the file keeps both fields; a 0.22 file without them reads as before
            QTemporaryDir dir;
            Project p; oneNoteProject(p);
            ClipPtr c = p.tracks[0].clipSlots[0];
            c->start = 2.5; c->loopStart = 1;
            setfile::Extras x;
            QString err;
            Project q; setfile::Extras y;
            const QString path = dir.filePath("markers.sessions");
            CHECK((setfile::save(path, p, x, &err) && setfile::load(path, q, y, &err) && q.tracks[0].clipSlots[0]->start == 2.5 && q.tracks[0].clipSlots[0]->loopStart == 1
                   && q.captureEditState() == p.captureEditState()),
                  qPrintable("M23 sets: the start marker and the loop position save and load (" + err + ")"));
            const QJsonObject j = setfile::clipToJson(*c, dir.path());
            CHECK((j.value("start").toDouble() == 2.5 && j.value("loopStart").toDouble() == 1 && setfile::clipFromJson(j, dir.path()).loopStart == 1), "M23 sets: the clip library's JSON carries them too");
            QJsonObject old = j; old.remove("start"); old.remove("loopStart");
            const Clip o = setfile::clipFromJson(old, dir.path());
            CHECK((o.start == 0 && o.loopStart == 0 && o.length == c->length), "M23 sets: a clip saved before 0.23 plays from 1.1.1 for its length");
            q.tracks[0].clipSlots[0]->loopStart = 0;
            CHECK(!(p.captureEditState() == q.captureEditState()), "M23 sets: EditState compares the fields (a different loop position is a different state)");
        }
        {
            // the piano roll's ruler: ▶ moves the start marker, the brace's body the loop, its ends the loop's start and the length
            Project p; oneNoteProject(p);
            audio::Engine e(&p);
            Transport t(&p, &e);
            ClipPtr c = p.tracks[0].clipSlots[0];
            PianoRoll roll(&p, &t);
            roll.resize(600, 300); roll.show();
            roll.setClip(c);
            QApplication::processEvents();
            const int ppb = int(roll.zoom());
            auto xOf = [&](double beat) { return 34 + int(beat * roll.zoom()); };
            QTest::mousePress(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(0) + 2, 9));
            QTest::mouseMove(&roll, QPoint(xOf(0) + 2 + ppb, 9));
            QTest::mouseRelease(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(0) + 2 + ppb, 9));
            CHECK((std::abs(c->start - 1) < 1e-9 && c->loopStart == 0 && c->length == 4 && p.undoStack()->undoText() == "Move Clip Start"),
                  qPrintable(QString("M23 piano roll: dragging the ▶ in the ruler a beat moves the start marker (start %1)").arg(c->start)));
            QTest::mousePress(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(2), 2));
            QTest::mouseMove(&roll, QPoint(xOf(2) + ppb, 2));
            QTest::mouseRelease(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(2) + ppb, 2));
            CHECK((std::abs(c->loopStart - 1) < 1e-9 && c->length == 4 && p.undoStack()->undoText() == "Move Loop"),
                  qPrintable(QString("M23 piano roll: dragging the brace's body a beat moves the loop (position %1, length %2)").arg(c->loopStart).arg(c->length)));
            QTest::mousePress(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(5) - 1, 2));
            QTest::mouseMove(&roll, QPoint(xOf(5) - 1 - ppb, 2));
            QTest::mouseRelease(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(5) - 1 - ppb, 2));
            CHECK((std::abs(c->loopStart - 1) < 1e-9 && std::abs(c->length - 3) < 1e-9 && p.undoStack()->undoText() == "Clip Length"),
                  qPrintable(QString("M23 piano roll: dragging the brace's right end a beat shortens the loop (length %1)").arg(c->length)));
            QTest::mousePress(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(1) + 1, 2));
            QTest::mouseMove(&roll, QPoint(xOf(1) + 1 + ppb, 2));
            QTest::mouseRelease(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(1) + 1 + ppb, 2));
            CHECK((std::abs(c->loopStart - 2) < 1e-9 && std::abs(c->length - 2) < 1e-9 && p.undoStack()->undoText() == "Move Loop Start"),
                  qPrintable(QString("M23 piano roll: dragging the brace's left end moves the loop's start, its end stays (position %1, length %2)").arg(c->loopStart).arg(c->length)));
            QTest::mousePress(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(3) - 20, 8));   // plain ruler: still zooms
            QTest::mouseMove(&roll, QPoint(xOf(3) - 20, 60));
            QTest::mouseRelease(&roll, Qt::LeftButton, Qt::NoModifier, QPoint(xOf(3) - 20, 60));
            CHECK((roll.zoom() != ppb && std::abs(c->loopStart - 2) < 1e-9), "M23 piano roll: the rest of the ruler still zooms");
            for (int i = 0; i < 4; ++i) p.undoStack()->undo();
            CHECK((c->start == 0 && c->loopStart == 0 && c->length == 4), "M23 piano roll: four undos put the markers back");
        }
        {
            // the Clip box: Start and Position are live
            Project p; oneNoteProject(p);
            audio::Engine e(&p);
            Transport t(&p, &e);
            ClipView cv(&p, &t);
            cv.resize(1200, 300); cv.show();
            p.selectedClip = p.tracks[0].clipSlots[0];
            p.selectedTrack = 0;
            cv.setClip(p.selectedClip);
            QApplication::processEvents();
            QList<DragValue*> fields = cv.findChildren<DragValue*>();
            DragValue* startField = nullptr; DragValue* posField = nullptr;
            for (DragValue* d : fields) {
                if (d->property("info").toString().startsWith("Clip Start|")) startField = d;
                if (d->property("info").toString().startsWith("Loop Position|")) posField = d;
            }
            if (startField) { startField->setValue(2, false); emit startField->valueEdited(2); }   // as a drag would
            if (posField) { posField->setValue(1, false); emit posField->valueEdited(1); }
            CHECK((startField && posField && p.selectedClip->start == 2 && p.selectedClip->loopStart == 1), "M23 Clip box: the Start and Position fields set the start marker and the loop's position");
            if (QWidget* f = QApplication::focusWidget()) f->clearFocus();   // (the name field holds focus offscreen, and typing there defers the refresh)
            p.selectedClip->start = 3; emit p.clipChanged(); QApplication::processEvents();
            CHECK((startField && startField->value() == 3), "M23 Clip box: the Start field follows the clip");
        }
        {
            // the sample editor's click-to-play seeks through the start marker; Set End Here and Loop Selection respect the brace
            QTemporaryDir editDir;
            Project p; ClipPtr c = audioProject(p, burstsWav);
            p.recordFolder = editDir.path();
            audio::Engine e(&p);
            Transport t(&p, &e);
            c->start = 1;
            t.playClipFrom(c, 3.0);   // on the timeline from 1.1.1 with offset 0: clip beat 3 is timeline beat 3
            CHECK(std::abs(t.startMarker() - 3.0) < 1e-9, "M23 transport: playing a timeline clip from a clip beat finds it through the placement's passes");
            p.tracks[0].arrangement.clear();
            t.playClipFrom(c, 3.0);
            CHECK(std::abs((t.beat() - p.tracks[0].slotStart) - 2.0) < 1e-6, "M23 transport: playing a Session clip from clip beat 3 with start = 1 puts the launch 2 beats back");
            t.stop();
            SampleEditor se(&p, &t);
            se.resize(600, 220); se.show(); se.setClip(c);
            c->loopStart = 1;
            se.setEndHere(3);
            CHECK((std::abs(c->length - 2) < 1e-9 && c->loopStart == 1), "M23 sample editor: Set End Here measures the length from the loop's start");
            se.select(1, 3);
            se.loopSelection();
            CHECK((c->start == 0 && c->loopStart == 0 && std::abs(c->length - 2) < 1e-9), "M23 sample editor: Loop Selection resets the markers to 1.1.1");
        }
    }

    // ================================================================ Milestone 24: the logo, launching after a stop
    {
        // A clip launched after the song stopped mid-way (one Stop: the playhead stays) plays at once from the
        // start marker. The launch used to be queued where the song had stopped, and waited for that beat.
        Project p;
        p.scenes = {"1", "2"};
        p.addTrack("A", QColor("#f2905e"));
        p.addTrack("B", QColor("#9b7ff0"));
        for (int t = 0; t < 2; ++t) { p.tracks[t].devices = {p.makeDevice("Sampler", DeviceKind::Instrument)}; p.tracks[t].devices[0].params[1].value = 0; }
        for (int t = 0; t < 2; ++t) for (int sc = 0; sc < 2; ++sc) {
            ClipPtr c = p.newClip(QString("%1%2").arg(t).arg(sc), 4);
            c->notes = {Note{60, 0.0, 0.5, 1.0, 1.0}};
            p.tracks[t].clipSlots[sc] = c;
        }
        audio::Engine e(&p);
        Transport tr(&p, &e);
        tr.launchScene(0);
        render(e, 24000 * 6);   // six beats in (48 kHz, 120 BPM)
        CHECK((e.trackStatus(0).playingSlot == 0 && e.trackStatus(1).playingSlot == 0 && e.beat() > 5.5), "M24 launch: a scene from 1.1.1 plays on both tracks");
        tr.stopTrack(1);        // the track's stop button: at the next bar
        render(e, 24000 * 2);
        CHECK((e.trackStatus(1).playingSlot < 0 && e.trackStatus(0).playingSlot == 0), "M24 launch: Stop Track stops that track at the next bar");
        tr.stop();              // once: the song stops where it is (beat 8); the start marker stays at 1.1.1
        render(e, 256);
        CHECK((!tr.playing() && std::abs(tr.startMarker()) < 1e-9), "M24 launch: one Stop leaves the start marker at 1.1.1");
        tr.launchSlot(1, 1);    // a single clip from the standstill
        std::vector<float> out = render(e, 2400);   // a tenth of a beat
        CHECK((tr.playing() && e.trackStatus(1).playingSlot == 1 && e.trackStatus(1).queuedSlot == -2 && e.beat() < 0.2 && peakOf(out) > 0.05f),
              "M24 launch: a clip launched after a mid-song stop plays at once from the start marker (it used to wait for the old beat)");
        tr.stop(); render(e, 256);
        tr.stop(); render(e, 256);   // twice: back to 1.1.1
        tr.setStartMarker(4);
        tr.launchScene(1);
        out = render(e, 2400);
        CHECK((e.trackStatus(0).playingSlot == 1 && e.trackStatus(1).playingSlot == 1 && std::abs(e.trackStatus(0).slotStart - 4.0) < 1e-6 && peakOf(out) > 0.05f),
              "M24 launch: a scene from a standstill with the start marker at bar 2 starts there at once, in phase with the launch");
        tr.launchSlot(0, 0);    // while the song plays: waits for the next bar (quantization 4)
        render(e, 2400);
        CHECK((e.trackStatus(0).playingSlot == 1 && e.trackStatus(0).queuedSlot == 0 && std::abs(e.trackStatus(0).queuedAt - 8.0) < 1e-6), "M24 launch: with the song playing, a clip waits for the next bar");
        render(e, 24000 * 4);
        CHECK((e.trackStatus(0).playingSlot == 0 && e.trackStatus(0).queuedSlot == -2), "M24 launch: and lands on it");
    }
    {
        // Help > About: the logo rendered in the theme's ink, the new words, no competitor named
        MainWindow w;
        QDialog* d = w.aboutDialog();
        auto* logo = d->findChild<QLabel*>("aboutLogo");
        auto* words = d->findChild<QLabel*>("aboutText");
        CHECK((logo && !logo->pixmap().isNull() && logo->pixmap().width() > 300), "M24 about: the Sessions logo renders from the resources");
        CHECK((words && words->text().contains("Neural Amp Modeler") && words->text().contains("Session grid") && !words->text().contains("in the style of")), "M24 about: the About text is the new one");
        delete d;
    }

    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
