// Osmium integration: real Qt gestures, undo, set/preset data and rendered audio.
#include "core/DeviceSpecs.h"
#include "core/ModulationCurve.h"
#include "core/OsmiumPresets.h"
#include "core/SetFile.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "audio/Engine.h"
#include "audio/Devices.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include "widgets/DragValue.h"
#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QAction>
#include <QDir>
#include <QJsonArray>
#include <QMouseEvent>
#include <QMimeData>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTest>
#include <QUndoStack>
#include <cstdio>
#include <cmath>
using namespace sessions;
namespace mo = sessions::monolith;
static int failures = 0, checks = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL: %s\n", m); } else std::printf("ok: %s\n", m); } while(0)
static void drag(QWidget* w, QPoint from, QPoint to) {
    QTest::mousePress(w, Qt::LeftButton, Qt::NoModifier, from);
    QMouseEvent move(QEvent::MouseMove, to, w->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &move);
    QTest::mouseRelease(w, Qt::LeftButton, Qt::NoModifier, to);
}
static std::vector<float> render(std::vector<float> params, const QVector<ModRoute>& routes, const QVector<CurvePoint>& curve, DeviceType type = DeviceType::Osmium) {
    auto proc = audio::createProcessor(type, 48000); audio::Context ctx; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000;
    CurvePoint points[mo::kCurves * mo::kCurvePoints]{}; int counts[mo::kCurves]{};
    for (int i = 0; i < curve.size(); ++i) points[i] = curve[i]; counts[0] = int(curve.size());
    ctx.mods = routes.constData(); ctx.modCount = int(routes.size()); ctx.curves = points; ctx.curveCount = counts;
    std::vector<float> out(48000); float l[audio::kMaxBlock], r[audio::kMaxBlock];
    for (int offset = 0; offset < 48000; offset += audio::kMaxBlock) {
        const int n = std::min(audio::kMaxBlock, 48000 - offset); std::fill(l, l + n, 0); std::fill(r, r + n, 0);
        audio::EventList ev; if (offset == 0) ev.add({0, 48, 0.8f, 36000}); ctx.beat = offset * ctx.beatsPerSample;
        proc->render(ev, l, r, n, params.data(), ctx); std::copy(l, l + n, out.begin() + offset);
    }
    return out;
}
static double energy(const std::vector<float>& out, int a, int b) { double sum = 0; for (int i = a; i < b; ++i) sum += out[size_t(i)] * out[size_t(i)]; return std::sqrt(sum / (b - a)); }
int main(int argc, char** argv) {
    QApplication app(argc, argv); theme::apply(theme::Kind::Dark);
    CHECK(paramCount(DeviceType::Monolith) == 170 && paramCount(DeviceType::Osmium) == 182, "Monolith keeps its layout; Osmium has 12 independent envelope bends");
    Project p; p.scenes = {"1"}; p.addTrack("Osmium", theme::osmiumGreen); p.tracks[0].devices = {p.makeDevice("Osmium", DeviceKind::Instrument)};
    const int id = p.tracks[0].devices[0].id;
    auto current = [&]() -> Device& { return p.tracks[0].devices[0]; };
    CHECK(current().curves.size() == 2 && current().mods.size() == 1, "New Osmium has editable curves and an audible filter envelope assignment");
    Device saved = current(); saved.curves[0] = {{0, 0, -0.6, 1}, {0.5, 1, 0.4, 2}, {1, 0.3}};
    saved.params[osmium::bend(2, 1)].value = 0.83; saved.mods.push_back({mo::SrcCurve1, mo::oparam(0, mo::Position), -0.37});
    const auto restored = setfile::deviceFromJson(setfile::deviceToJson(saved));
    CHECK(restored.curves == saved.curves && restored.mods == saved.mods && restored.params[osmium::bend(2, 1)].value == 0.83, "Curvature, hold/smooth modes, envelope bends and signed ranges survive persistence");
    const auto legacy = setfile::deviceFromJson(QJsonObject{{"name", "Monolith"}, {"curves", QJsonArray{QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 1}}}}});
    CHECK(legacy.curves[0][0].bend == 0 && legacy.curves[0][0].shape == 0, "Legacy two-coordinate Monolith curves stay linear");
    audio::Engine engine(&p); Transport transport(&p, &engine);
    DeviceView deviceView(&p, &transport); auto& panel = deviceView;
    deviceView.resize(1240, 600); deviceView.show(); QApplication::processEvents();
    auto* undoAction = p.undoStack()->createUndoAction(&deviceView); undoAction->setShortcut(QKeySequence::Undo); deviceView.addAction(undoAction);
    auto* redoAction = p.undoStack()->createRedoAction(&deviceView); redoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)); deviceView.addAction(redoAction);
    auto* open = panel.findChild<QPushButton*>("osmiumOpenEditor"); CHECK(open, "Osmium is accessible from its rack panel");
    open->click(); QApplication::processEvents();
    auto* editor = panel.findChild<devui::DeviceBody*>("osmiumEditor"); auto* dialog = &deviceView;
    CHECK(editor && dialog->isVisible() && deviceView.editorExpanded() && !editor->isWindow() && editor->width() >= 1060, "The integrated editor expands inside DeviceView with a full modulation workspace");
    auto* knob = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::fparam(0, mo::FCutoff)));
    const double base = current().params[mo::fparam(0, mo::FCutoff)].value, amount = current().mods[0].amount;
    int undo = p.undoStack()->index();
    drag(knob, QPoint(knob->width() / 2, 15), QPoint(knob->width() / 2 + 30, -15));
    CHECK(current().mods[0].amount > amount + 0.3 && current().params[mo::fparam(0, mo::FCutoff)].value == base, "Dragging the green ring changes modulation range while preserving the base value");
    CHECK(p.undoStack()->index() == undo + 1, "An entire modulation drag is one undo step");
    p.undoStack()->undo(); CHECK(std::abs(current().mods[0].amount - amount) < 1e-6, "Undo restores the modulation amount");
    drag(knob, QPoint(knob->width() / 2, 39), QPoint(knob->width() / 2, 19));
    CHECK(current().params[mo::fparam(0, mo::FCutoff)].value > base && current().mods[0].amount == amount, "Dragging the knob centre changes the base while preserving range");
    p.undoStack()->undo();
    auto* lfo = editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcLfo1)); lfo->click();
    QTest::mouseClick(knob, Qt::LeftButton, Qt::NoModifier, QPoint(knob->width() / 2, knob->height() - 8));
    CHECK(current().mods.size() == 2 && current().mods[1].source == mo::SrcLfo1, "Clicking the source slot adds a second modulator on the same knob");
    drag(knob, QPoint(knob->width() / 2, 15), QPoint(knob->width() / 2 - 80, 60));
    CHECK(current().mods[1].amount < 0, "Range gestures pass through zero and invert bipolar modulation");
    auto* curveSource = editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcCurve1)); curveSource->click();
    auto* shape = editor->findChild<QComboBox*>("osmiumCurveShape_0"); shape->setCurrentIndex(0); QMetaObject::invokeMethod(shape, "activated", Q_ARG(int, 0)); QApplication::processEvents();
    auto* curve = editor->findChild<QWidget*>("osmiumCurve_0"); const auto before = current().curves[0];
    const QPoint mid(int(14 + (curve->width() - 28) * 0.25), int(curve->height() / 2 - 2));
    drag(curve, mid, mid + QPoint(0, 32));
    CHECK(current().curves[0][0].bend > 0.2 && current().curves[0][1] == before[1], "Dragging a curve diamond bends a segment without moving nodes");
    p.undoStack()->undo(); CHECK(current().curves[0] == before, "Undo restores a complete curve gesture");
    const int oldCount = int(current().curves[0].size());
    QTest::mouseDClick(curve, Qt::LeftButton, Qt::NoModifier, QPoint(int(14 + (curve->width() - 28) * 0.75), 30));
    CHECK(current().curves[0].size() == oldCount + 1, "Double-click adds a snapped curve node");
    auto* envSource = editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcEnv2)); envSource->click();
    auto* env = editor->findChild<QWidget*>("osmiumEnvelope_1");
    const double ap = editor->pos(mo::eparam(1, mo::A)), dp = editor->pos(mo::eparam(1, mo::D));
    const double x1 = 14 + (env->width() - 28) * (0.08 + 0.2 * ap), x2 = x1 + (env->width() - 28) * (0.08 + 0.2 * dp);
    const QPoint diamond(int((x1 + x2) / 2), int((12 + env->height() - 16) / 2));
    drag(env, diamond, diamond + QPoint(0, -30));
    CHECK(current().params[osmium::bend(1, 1)].value > 0.6, "The decay diamond edits its own envelope bend");
    // Modulation drops are scoped to one instrument; choosing a new source updates the ring.
    const int routeCount = int(current().mods.size());
    QMimeData foreign; foreign.setData("application/x-sessions-osmium-source", QByteArray::number(id + 99) + ":25");
    QDragEnterEvent wrong(QPoint(20, 20), Qt::CopyAction, &foreign, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(knob, &wrong);
    CHECK(!wrong.isAccepted() && current().mods.size() == routeCount, "A source dragged from a different Osmium cannot accidentally create a route");
    QMimeData local; local.setData("application/x-sessions-osmium-source", QByteArray::number(id) + ':' + QByteArray::number(mo::SrcCurve2));
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &local, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(knob, &enter);
    QDropEvent drop(QPoint(20, 20), Qt::CopyAction, &local, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(knob, &drop);
    CHECK(drop.isAccepted() && current().mods.size() == routeCount + 1 && current().mods.last().source == mo::SrcCurve2
          && editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcCurve2))->isChecked(), "Dropping a source assigns and selects its green range");
    auto* amountField = editor->findChild<DragValue*>("osmiumAmount");
    QTest::mouseDClick(amountField, Qt::LeftButton); auto* input = amountField->findChild<QLineEdit*>();
    CHECK(input, "The range field opens an exact numeric editor");
    if (input) { input->selectAll(); QTest::keyClicks(input, "-42.5"); QTest::keyClick(input, Qt::Key_Return); }
    CHECK(std::abs(current().mods.last().amount + 0.425) < 1e-6, "Typing a signed percentage sets the corresponding audio modulation amount");
    auto* pitchField = static_cast<QWidget*>(nullptr);
    for (auto* w : editor->findChildren<DragValue*>()) if (w->property("mapTarget").toString() == editor->target(mo::oparam(0, mo::Pitch))) pitchField = w;
    QDragEnterEvent pitchEnter(QPoint(10, 8), Qt::CopyAction, &local, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(pitchField, &pitchEnter);
    QDropEvent pitchDrop(QPoint(10, 8), Qt::CopyAction, &local, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(pitchField, &pitchDrop);
    CHECK(pitchDrop.isAccepted() && current().mods.last().param == mo::oparam(0, mo::Pitch), "Pitch fields accept modulation and expose their range in the inspector");
    curveSource->click(); auto* draw = static_cast<QPushButton*>(nullptr);
    for (auto* b : editor->findChildren<QPushButton*>()) if (b->text() == "Draw" && b->isVisible()) draw = b;
    const auto beforeDraw = current().curves[0]; draw->click(); undo = p.undoStack()->index();
    drag(curve, QPoint(14, curve->height() - 16), QPoint(curve->width() - 14, 12));
    CHECK(current().curves[0].size() == 17 && current().curves[0].first().y < 0.01 && current().curves[0].last().y > 0.99,
          "Draw mode fills skipped grid positions during a fast drag");
    CHECK(p.undoStack()->index() == undo + 1, "Freehand drawing is one undo step");
    p.undoStack()->undo(); CHECK(current().curves[0] == beforeDraw, "Undo restores the curve before freehand drawing"); draw->click();
    // A knob's range source is focus, not a request to navigate to its editor.
    auto pageButton = [&](const QString& label) -> QPushButton* { for (auto* b : editor->findChildren<QPushButton*>()) if (b->text() == label) return b; return nullptr; };
    pageButton("Voice && mixer")->click();
    auto* detune = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::Detune));
    QTest::mouseClick(detune, Qt::LeftButton, Qt::NoModifier, QPoint(detune->width() / 2, detune->height() - 8));
    drag(detune, QPoint(detune->width() / 2, 31), QPoint(detune->width() / 2, 21));
    CHECK(pageButton("Voice && mixer")->isChecked() && detune->isVisible(), "Editing Detune's base keeps Voice & mixer open");
    envSource->click(); pageButton("Voice && mixer")->click();
    drag(detune, QPoint(detune->width() / 2, 15), QPoint(detune->width() / 2 + 15, 0));
    CHECK(pageButton("Voice && mixer")->isChecked() && curveSource->isChecked(), "Dragging an existing Curve range selects its source without leaving Voice & mixer");
    pageButton("Effects")->click();
    auto* effect = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::fx(mo::ChorusDepth)));
    drag(effect, QPoint(effect->width() / 2, 15), QPoint(effect->width() / 2 + 15, 0));
    CHECK(pageButton("Effects")->isChecked() && effect->isVisible(), "Creating and dragging a modulation range keeps Effects open");
    theme::apply(theme::Kind::Light); CHECK(pageButton("Effects")->isChecked(), "A theme refresh preserves the selected editor page"); theme::apply(theme::Kind::Dark);
    curveSource->click(); CHECK(pageButton("Modulators")->isChecked() && curve->isVisible(), "Explicitly clicking a source still opens its modulation editor");
    std::vector<float> params(audio::kMaxParams, 0); for (int i = 0; i < current().params.size(); ++i) params[size_t(i)] = float(current().params[i].value);
    params[mo::OscRestart] = 1; params[mo::eparam(0, mo::A)] = float(editor->posFor(mo::eparam(0, mo::A), 0.5)); params[mo::ecurve(0)] = 0.5;
    params[osmium::bend(0, 0)] = 0; const auto fast = render(params, {}, {});
    params[osmium::bend(0, 0)] = 1; const auto slow = render(params, {}, {});
    CHECK(energy(fast, 3000, 10000) > energy(slow, 3000, 10000) * 8, "Envelope bends change the rendered attack, not just the graph");
    params[mo::eparam(0, mo::A)] = 0; params[mo::eparam(0, mo::S)] = 1; params[mo::oparam(0, mo::Level)] = 0; params[mo::oparam(1, mo::Level)] = 0;
    params[mo::cparam(0, mo::CRetrig)] = 1; params[mo::cparam(0, mo::CSmooth)] = 0;
    const QVector<ModRoute> routes{{mo::SrcCurve1, mo::oparam(0, mo::Level), 0.8}};
    const auto rise = render(params, routes, {{0, 0, -0.8}, {1, 1}}), fall = render(params, routes, {{0, 0, 0.8}, {1, 1}});
    CHECK(energy(rise, 2000, 11000) > energy(fall, 2000, 11000) * 3, "Per-segment curve bend reaches the actual modulation audio path");
    params[mo::eparam(1, mo::A)] = 0; params[mo::eparam(1, mo::D)] = float(editor->posFor(mo::eparam(1, mo::D), 0.25)); params[mo::eparam(1, mo::S)] = 0;
    params[osmium::bend(1, 1)] = 0.5; params[mo::ecurve(1)] = 0;
    const QVector<ModRoute> envRoute{{mo::SrcEnv2, mo::oparam(0, mo::Level), 0.8}};
    const auto highCurve = render(params, envRoute, {}); params[mo::ecurve(1)] = 1; const auto lowCurve = render(params, envRoute, {});
    CHECK(energy(highCurve, 3000, 10000) > energy(lowCurve, 3000, 10000) * 2, "An envelope's level curve shapes its modulation source as well as its display");
    CHECK(std::all_of(fast.begin(), fast.end(), [](float f) { return std::isfinite(f) && std::abs(f) < 4; }), "Osmium renders finite, bounded audio");
    // Exercise the real DeviceView rebuild path, rather than just a standalone panel.
    QPointer<QWidget> sameWindow = dialog; QPointer<devui::DeviceBody> sameEditor = editor;
    editor->edit(mo::FDrive, 0.41); p.undoStack()->undo(); QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(sameWindow && sameEditor && sameWindow->isVisible(), "The integrated editor survives undo and deferred deletion of its rack panel");
    const auto size = dialog->size(); deviceView.rebuild(); QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(sameWindow && sameEditor && dialog->size() == size, "Rebuilding a device panel reuses the editor and preserves its pane size");
    const double beforeShortcut = current().params[mo::FDrive].value; editor->edit(mo::FDrive, 0.42);
    dialog->activateWindow(); QTest::qWait(20); QTest::keyClick(dialog, Qt::Key_Z, Qt::ControlModifier); QApplication::processEvents();
    CHECK(std::abs(current().params[mo::FDrive].value - beforeShortcut) < 1e-6, "Ctrl+Z in the integrated editor undoes the instrument edit");
    QTest::keyClick(dialog, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); QApplication::processEvents();
    CHECK(std::abs(current().params[mo::FDrive].value - 0.42) < 1e-6, "Ctrl+Shift+Z in the integrated editor restores the instrument edit");
    auto* patches = editor->findChild<QComboBox*>("osmiumPatch");
    CHECK(patches->isEditable() && patches->count() == 46, "The searchable selector contains all 40 DnB presets and the original five entries");
    patches->completer()->setCompletionPrefix("seismic");
    CHECK(patches->completer()->completionCount() == 1, "Preset search matches a name within its category without case sensitivity");
    const QString oldName = current().name; const auto oldCurves = current().curves; const int oldId = current().id;
    const auto* factory = osmium::factoryPreset("Osmium Wobble - Seismic");
    const int nativeItem = patches->findData(QString::fromUtf8(factory->name)); undo = p.undoStack()->index();
    patches->setCurrentIndex(nativeItem); QMetaObject::invokeMethod(patches, "activated", Q_ARG(int, nativeItem)); QApplication::processEvents();
    CHECK(current().id == oldId && current().name == factory->name && current().mods == factory->mods && current().curves == factory->curves,
          "Loading a factory patch preserves the instrument's identity and installs its name, routes and curves");
    CHECK(p.undoStack()->index() == undo + 1 && sameWindow && sameWindow->isVisible(), "A complete preset load is one undo step and keeps the editor open");
    p.undoStack()->undo(); QApplication::processEvents();
    CHECK(current().name == oldName && current().curves == oldCurves, "Undo restores the previous sound and curves after loading a factory patch");
    p.undoStack()->redo(); QApplication::processEvents();
    CHECK(patches->currentData().toString() == current().name, "The patch selector follows redo and the persisted factory sound name");
    if (argc > 1) {
        const QString dir = QString::fromLocal8Bit(argv[1]); QDir().mkpath(dir);
        auto* patch = editor->findChild<QComboBox*>("osmiumPatch"); patch->setCurrentIndex(1); QMetaObject::invokeMethod(patch, "activated", Q_ARG(int, 1)); QApplication::processEvents();
        QTest::mouseClick(knob, Qt::LeftButton, Qt::NoModifier, QPoint(knob->width() / 2, 41));
        envSource->click();
        for (auto* b : editor->findChildren<QPushButton*>()) if (b->text() == "Assign") b->click();
        editor->edit(mo::eparam(1, mo::A), editor->posFor(mo::eparam(1, mo::A), 0.08)); editor->edit(mo::eparam(1, mo::S), 0.3);
        QApplication::processEvents(); editor->grab().save(dir + "/osmium-envelope.png");
        curveSource->click(); for (auto* b : editor->findChildren<QPushButton*>()) if (b->text() == "Assign") b->click();
        QApplication::processEvents(); editor->grab().save(dir + "/osmium-curve.png");
        lfo->click(); QApplication::processEvents(); editor->grab().save(dir + "/osmium-lfo.png");
        editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcStep1))->click(); QApplication::processEvents(); editor->grab().save(dir + "/osmium-step.png");
        for (auto* b : editor->findChildren<QPushButton*>()) if (b->text() == "Voice && mixer") b->click();
        QApplication::processEvents(); editor->grab().save(dir + "/osmium-voice.png");
        for (auto* b : editor->findChildren<QPushButton*>()) if (b->text() == "Effects") b->click();
        QApplication::processEvents(); editor->grab().save(dir + "/osmium-effects.png"); lfo->click();
        theme::apply(theme::Kind::Light); QApplication::processEvents(); editor->grab().save(dir + "/osmium-light.png"); theme::apply(theme::Kind::Dark);
        panel.grab().save(dir + "/osmium-rack.png");
        auto factoryCapture = [&](const QString& name, int source, const QString& filename) {
            const int at = patches->findData(name); patches->setCurrentIndex(at); QMetaObject::invokeMethod(patches, "activated", Q_ARG(int, at));
            editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(source))->click();
            QTest::mouseClick(knob, Qt::LeftButton, Qt::NoModifier, QPoint(knob->width() / 2, 41));
            QApplication::processEvents(); editor->grab().save(dir + "/" + filename);
        };
        factoryCapture("Osmium Pad - Aurora", mo::SrcEnv2, "osmium-dnb-pad.png");
        factoryCapture("Osmium Wobble - Undertaker", mo::SrcCurve1, "osmium-dnb-wobble.png");
    }
    { EditScope edit(&p, "Delete Osmium"); p.tracks[0].devices.clear(); }
    emit p.structureChanged(); QTest::qWait(70);
    CHECK(!deviceView.editorExpanded() && (!sameEditor || !sameEditor->isVisible()), "Deleting the instrument closes its integrated editor without closing Sessions");
    dialog->close(); std::printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
