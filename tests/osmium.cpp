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
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTest>
#include <QUndoStack>
#include <cstdio>
#include <cmath>
using namespace sessions;
namespace mo = sessions::monolith;
static int failures = 0, checks = 0;
#define CHECK(c, m) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL: %s\n", m); } else std::printf("ok: %s\n", m); } while(0)
static void drag(QWidget* w, QPoint from, QPoint to, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QTest::mousePress(w, Qt::LeftButton, mods, from);
    QMouseEvent move(QEvent::MouseMove, to, w->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, mods);
    QApplication::sendEvent(w, &move);
    QTest::mouseRelease(w, Qt::LeftButton, mods, to);
}
static std::vector<float> render(std::vector<float> params, const QVector<ModRoute>& routes, const QVector<CurvePoint>& curve, DeviceType type = DeviceType::Osmium) {
    auto proc = audio::createProcessor(type, 48000); audio::Context ctx; ctx.tempo = 120; ctx.beatsPerSample = 2.0 / 48000;
    CurvePoint points[mo::kCurves * osmium::kCurvePoints]{}; int counts[mo::kCurves]{}; ctx.curveStride = osmium::kCurvePoints;
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
    CHECK(paramCount(DeviceType::Monolith) == 170 && paramCount(DeviceType::Osmium) == osmium::kParams && osmium::kParams == 191,
          "Monolith keeps its layout; Osmium appends table banks, warp shapes and per-oscillator folders after its original 182 parameters");
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
    deviceView.resize(1240, 720); deviceView.show(); QApplication::processEvents();
    auto* undoAction = p.undoStack()->createUndoAction(&deviceView); undoAction->setShortcut(QKeySequence::Undo); deviceView.addAction(undoAction);
    auto* redoAction = p.undoStack()->createRedoAction(&deviceView); redoAction->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)); deviceView.addAction(redoAction);
    auto* open = panel.findChild<QPushButton*>("osmiumOpenEditor"); CHECK(open, "Osmium is accessible from its rack panel");
    open->click(); QApplication::processEvents();
    auto* editor = panel.findChild<devui::DeviceBody*>("osmiumEditor"); auto* dialog = &deviceView;
    CHECK(editor && dialog->isVisible() && deviceView.editorExpanded() && !editor->isWindow() && editor->width() >= 1060, "The integrated editor expands inside DeviceView with a full modulation workspace");
    auto* knob = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::fparam(0, mo::FCutoff)));
    bool usableSizes = true;
    for (auto* k : editor->findChildren<QWidget*>()) if (k->objectName().startsWith("osmiumKnob_")) usableSizes &= k->property("dialDiameter").toInt() >= 52;
    CHECK(usableSizes && knob->property("dialDiameter").toInt() >= 64,
          "Osmium uses compact readable dials while retaining larger filter cutoff controls");
    CHECK(editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::macro(0)))->property("dialDiameter").toInt() == 84,
          "Macros have larger 84px dials and 54px faces");
    const double base = current().params[mo::fparam(0, mo::FCutoff)].value, amount = current().mods[0].amount;
    int undo = p.undoStack()->index();
    drag(knob, QPoint(knob->width() / 2, 15), QPoint(knob->width() / 2 + 30, -15));
    CHECK(current().mods[0].amount > amount + 0.3 && current().params[mo::fparam(0, mo::FCutoff)].value == base, "Dragging the outer ring changes modulation range while preserving the base value");
    CHECK(p.undoStack()->index() == undo + 1, "An entire modulation drag is one undo step");
    p.undoStack()->undo(); CHECK(std::abs(current().mods[0].amount - amount) < 1e-6, "Undo restores the modulation amount");
    drag(knob, QPoint(knob->width() / 2, 39), QPoint(knob->width() / 2, 19));
    CHECK(current().params[mo::fparam(0, mo::FCutoff)].value > base && current().mods[0].amount == amount, "Dragging the knob centre changes the base while preserving range");
    p.undoStack()->undo();
    const int cy = 16 + knob->property("dialDiameter").toInt() / 2, cx = knob->width() / 2;
    const int br = knob->property("baseRadius").toInt(), rr = knob->property("rangeRadius").toInt();
    drag(knob, QPoint(cx, cy - br), QPoint(cx, cy - br - 20));
    CHECK(current().params[mo::fparam(0, mo::FCutoff)].value > base && current().mods[0].amount == amount,
          "The entire inner ring edits the base, without stealing modulation gestures");
    p.undoStack()->undo(); undo = p.undoStack()->index();
    drag(knob, QPoint(cx, cy - rr + 5), QPoint(cx + 30, cy - rr - 30));
    drag(knob, QPoint(cx, 5), QPoint(cx + 30, -20));
    CHECK(p.undoStack()->index() == undo && current().mods[0].amount == amount && current().params[mo::fparam(0, mo::FCutoff)].value == base,
          "The ring gap and label cannot accidentally change the base or modulation");
    auto custom = theme::current(); custom.accent = QColor("#d957e7"); theme::apply(custom);
    const auto themedKnob = knob->grab().toImage().convertToFormat(QImage::Format_RGB32);
    int accentPixels = 0;
    for (int y = 14; y < cy + rr + 3; ++y) for (int x = 0; x < themedKnob.width(); ++x) if (themedKnob.pixelColor(x, y).rgb() == custom.accent.rgb()) ++accentPixels;
    CHECK(accentPixels > 12, "Assigned modulation rings follow a custom theme accent");
    auto* unassigned = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::oparam(0, mo::Intensity)));
    const auto emptyKnob = unassigned->grab().toImage(); const int uc = unassigned->width() / 2;
    const auto trackColor = emptyKnob.pixelColor(uc, 17), gapColor = emptyKnob.pixelColor(uc, 22);
    CHECK(trackColor != gapColor && trackColor.red() > gapColor.red(), "An unassigned knob retains a faded accent track separated by a clear gap");
    theme::apply(theme::Kind::Dark);
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
          && editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcCurve2))->isChecked(), "Dropping a source assigns and selects its accent range");
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
    CHECK(current().curves[0].size() == 33 && current().curves[0].first().y < 0.01 && current().curves[0].last().y > 0.99,
          "Draw mode fills skipped grid positions during a fast drag");
    CHECK(p.undoStack()->index() == undo + 1, "Freehand drawing is one undo step");
    p.undoStack()->undo(); CHECK(current().curves[0] == beforeDraw, "Undo restores the curve before freehand drawing"); draw->click();
    auto activate = [](QComboBox* box, int i) { box->setCurrentIndex(i); QMetaObject::invokeMethod(box, "activated", Q_ARG(int, i)); QApplication::processEvents(); };
    auto* grid = editor->findChild<QComboBox*>("osmiumCurveGrid_0");
    activate(grid, grid->findData(256)); draw->click(); undo = p.undoStack()->index();
    drag(curve, QPoint(14, curve->height() - 16), QPoint(curve->width() - 14, 12));
    const auto dense = current().curves[0];
    CHECK(dense.size() == 257 && std::abs(dense[128].y - 0.5) < 0.001 && dense.last().x == 1,
          "1/256 drawing creates 257 time-sorted nodes and interpolates skipped positions");
    CHECK(p.undoStack()->index() == undo + 1, "A dense freehand stroke remains a single undo step");
    Device denseSaved = current(); denseSaved.curves[1] = dense;
    CHECK(setfile::deviceFromJson(setfile::deviceToJson(denseSaved)).curves == denseSaved.curves,
          "Both 257-node Osmium curves survive saving without truncation");
    denseSaved.name = "Monolith";
    CHECK(setfile::deviceFromJson(setfile::deviceToJson(denseSaved)).curves[0].size() == mo::kCurvePoints,
          "Legacy Monolith retains its original 32-node limit");
    p.undoStack()->undo(); CHECK(current().curves[0] == beforeDraw, "Undo restores every original sparse node after dense drawing"); draw->click();
    activate(shape, 0); const auto triangle = current().curves[0];
    auto curvePos = [&](double x, double y) { return QPoint(qRound(14 + (curve->width() - 28) * x), qRound(curve->height() - 16 - (curve->height() - 28) * y)); };
    QTest::mouseClick(curve, Qt::LeftButton, Qt::NoModifier, curvePos(0.5, 1));
    auto* xField = editor->findChild<DragValue*>("osmiumCurveX_0"); auto* yField = editor->findChild<DragValue*>("osmiumCurveY_0");
    auto* bendField = editor->findChild<DragValue*>("osmiumCurveBend_0");
    auto typeValue = [](DragValue* field, const QString& value) { QTest::mouseDClick(field, Qt::LeftButton); auto* edit = field->findChild<QLineEdit*>(); if (edit) { edit->selectAll(); QTest::keyClicks(edit, value); QTest::keyClick(edit, Qt::Key_Return); } };
    undo = p.undoStack()->index(); typeValue(xField, "37.125"); typeValue(yField, "62.5"); typeValue(bendField, "-43.75");
    CHECK(std::abs(current().curves[0][1].x - 0.37125) < 1e-8 && current().curves[0][1].y == 0.625 && current().curves[0][1].bend == -0.4375,
          "Exact X, Y and outgoing bend fields edit the selected node independently of grid snap");
    CHECK(p.undoStack()->index() == undo + 3, "Each typed node value is one undo step");
    p.undoStack()->undo(); p.undoStack()->undo(); p.undoStack()->undo();
    CHECK(current().curves[0] == triangle, "Undo restores numeric node edits without changing other nodes");
    QTest::keyClick(curve, Qt::Key_Down, Qt::ShiftModifier);
    CHECK(std::abs(current().curves[0][1].y - 0.999) < 1e-8, "Shift-arrow nudges selected node levels at fine resolution"); p.undoStack()->undo();
    QTest::keyClick(curve, Qt::Key_Right);
    CHECK(std::abs(current().curves[0][1].x - (0.5 + 1.0 / 256)) < 1e-8, "Arrow keys nudge time by the selected fine grid"); p.undoStack()->undo();
    QTest::mouseDClick(curve, Qt::LeftButton, Qt::NoModifier, curvePos(0.25, 0.4));
    const auto grouped = current().curves[0];
    QTest::mouseClick(curve, Qt::LeftButton, Qt::ShiftModifier, curvePos(0.5, 1));
    drag(curve, curvePos(0.25, 0.4), curvePos(0.375, 0.3));
    CHECK(std::abs((current().curves[0][2].x - current().curves[0][1].x) - (grouped[2].x - grouped[1].x)) < 1e-8
          && current().curves[0][1].x > grouped[1].x && current().curves[0].first() == grouped.first() && current().curves[0].last() == grouped.last(),
          "Shift-selected nodes move together while preserving spacing and unselected endpoints");
    p.undoStack()->undo(); CHECK(current().curves[0] == grouped, "A group move undoes as one gesture");
    QTest::keyClick(curve, Qt::Key_Delete);
    CHECK(current().curves[0].size() == 2 && current().curves[0].first().x == 0 && current().curves[0].last().x == 1,
          "Delete removes selected interior nodes together and retains endpoints"); p.undoStack()->undo();
    auto* zoom = editor->findChild<QComboBox*>("osmiumCurveZoom_0"); auto* curveScroll = editor->findChild<QScrollBar*>("osmiumCurveScroll_0");
    activate(zoom, zoom->findData(8)); curveScroll->setValue(7000);
    const auto beforeZoomEdit = current().curves[0]; drag(curve, curvePos(1, 0), curvePos(0.6, 0.2));
    CHECK(current().curves[0].last().x == 1 && current().curves[0].last().y > 0.15 && current().curves[0].first() == beforeZoomEdit.first(),
          "Zoom and pan address the far end of the saved curve and endpoints keep their time"); p.undoStack()->undo(); activate(zoom, 0);
    auto* yGrid = editor->findChild<QComboBox*>("osmiumCurveYGrid_0"); activate(yGrid, yGrid->findData(8));
    drag(curve, curvePos(0.25, 0.4), curvePos(0.27, 0.64));
    CHECK(std::abs(current().curves[0][1].y - 0.625) < 1e-8, "Level snapping supports precise horizontal bands independently of time snapping");
    p.undoStack()->undo(); activate(yGrid, 0);
    QTest::keyClick(curve, Qt::Key_Escape);
    drag(curve, curvePos(0.15, 1.02), curvePos(0.6, 0.2));
    CHECK(!xField->isEnabled() && !yField->isEnabled() && editor->findChild<QLabel*>("osmiumCurveSelection_0")->text().startsWith("2 selected"),
          "Marquee selection picks multiple nodes without ambiguous numeric editing");
    QTest::mouseClick(curve, Qt::LeftButton, Qt::NoModifier, curvePos(0.25, 0.4));
    drag(curve, curvePos(0.25, 0.4), curvePos(0.375, 0.3), Qt::ControlModifier);
    CHECK(current().curves[0][1].x > grouped[1].x && current().curves[0][1].y == grouped[1].y,
          "Ctrl-drag locks node motion to the dominant axis"); p.undoStack()->undo();
    QTest::keyClick(curve, Qt::Key_A, Qt::ControlModifier); QTest::keyClick(curve, Qt::Key_Right);
    CHECK(current().curves[0].first() == grouped.first() && current().curves[0].last() == grouped.last()
          && current().curves[0][1].x > grouped[1].x && current().curves[0][2].x < 1,
          "Select-all movement preserves fixed endpoint times and sorted interior nodes"); p.undoStack()->undo();
    QTest::keyClick(curve, Qt::Key_Escape);
    const auto offsetStart = curvePos(0.25, 0.4) + QPoint(5, 5);
    drag(curve, offsetStart, offsetStart + QPoint(2, 2), Qt::AltModifier);
    CHECK(std::abs(current().curves[0][1].x - grouped[1].x - 2.0 / (curve->width() - 28)) < 1e-8
          && std::abs(current().curves[0][1].y - grouped[1].y + 2.0 / (curve->height() - 28)) < 1e-8,
          "Grabbing a node off-center preserves the grab offset and avoids a position jump"); p.undoStack()->undo();
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
    // The real Engine snapshot must carry the full second curve, with the larger stride.
    {
        Project audioProject; audioProject.scenes = {"1"}; audioProject.addTrack("Dense curve", theme::accent);
        audioProject.tracks[0].devices = {audioProject.makeDevice("Osmium", DeviceKind::Instrument)};
        auto& d = audioProject.tracks[0].devices[0];
        for (int o = 0; o < 3; ++o) d.params[mo::oparam(o, mo::Level)].value = 0;
        d.params[mo::SubLevel].value = 0; d.params[mo::NoiseLevel].value = 0;
        d.params[mo::eparam(0, mo::A)].value = 0; d.params[mo::eparam(0, mo::S)].value = 1;
        d.params[mo::cparam(1, mo::CRetrig)].value = 1; d.params[mo::cparam(1, mo::CSmooth)].value = 0;
        d.mods = {{mo::SrcCurve2, mo::oparam(0, mo::Level), 0.8}};
        d.curves[0] = {{0, 0}, {1, 0}}; d.curves[1].clear();
        for (int i = 0; i <= 256; ++i) d.curves[1].push_back({i / 256.0, i < 64 ? 0.0 : 1.0});
        audio::Engine denseEngine(&audioProject); denseEngine.send({audio::Command::Play});
        denseEngine.send({audio::Command::NoteOn, audioProject.tracks[0].id, -1, 48, 0.8});
        std::vector<float> out(48000); float block[512 * 2]{};
        for (int offset = 0; offset < 48000; offset += 512) { const int n = std::min(512, 48000 - offset); denseEngine.process(block, n); for (int i = 0; i < n; ++i) out[offset + i] = block[i * 2]; }
        CHECK(energy(out, 1000, 8000) < 1e-6 && energy(out, 30000, 42000) > 0.002,
              "Engine renders nodes beyond the old 32-point limit and uses the correct second-curve stride");
    }
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
    bool completeBank = true;
    for (const auto& preset : osmium::factoryPresets()) completeBank &= patches->findData(QString::fromUtf8(preset.name)) >= 0;
    CHECK(patches->isEditable() && patches->count() == int(osmium::factoryPresets().size()) + 7 && completeBank,
          "The searchable selector contains all factory presets and the five original starting sounds");
    patches->completer()->setCompletionPrefix("dubstep");
    CHECK(patches->completer()->completionCount() == 10, "Searching Dubstep lists the ten Dubstep wobble basses");
    patches->completer()->setCompletionPrefix("dnb bass");
    CHECK(patches->completer()->completionCount() == 10, "Searching DnB Bass lists the ten DnB wobble basses");
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
        dialog->resize(1920, 850); QApplication::processEvents();
        factoryCapture("Osmium Dubstep - Gravedigger", mo::SrcCurve1, "osmium-bass-phrase.png");
        factoryCapture("Osmium Wobble - Undertaker", mo::SrcEnv2, "osmium-large-envelope.png");
        pageButton("Sound")->click(); QApplication::processEvents(); editor->grab().save(dir + "/osmium-large-sound.png");
        pageButton("Voice && mixer")->click(); QApplication::processEvents(); editor->grab().save(dir + "/osmium-large-voice.png");
        curveSource->click(); activate(grid, grid->findData(128)); draw->click();
        const auto level = [](double t) { return 0.5 + 0.39 * std::sin(6.283185307179586 * (3 * t + 3 * t * t)) * (0.6 + 0.4 * std::sin(3.141592653589793 * t)); };
        QTest::mousePress(curve, Qt::LeftButton, Qt::NoModifier, curvePos(0, level(0)));
        for (int i = 1; i <= 128; ++i) { const double t = i / 128.0; const auto pt = curvePos(t, level(t));
            QMouseEvent move(QEvent::MouseMove, pt, curve->mapToGlobal(pt), Qt::NoButton, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(curve, &move); }
        QTest::mouseRelease(curve, Qt::LeftButton, Qt::NoModifier, curvePos(1, level(1))); draw->click();
        QTest::mouseClick(curve, Qt::LeftButton, Qt::NoModifier, curvePos(0.5, level(0.5)));
        QApplication::processEvents(); editor->grab().save(dir + "/osmium-large-curve.png");
        activate(zoom, zoom->findData(4)); curveScroll->setValue(1500); QApplication::processEvents(); editor->grab().save(dir + "/osmium-large-curve-zoom.png");
        theme::apply(theme::Kind::Light); QApplication::processEvents(); editor->grab().save(dir + "/osmium-large-curve-light.png"); theme::apply(theme::Kind::Dark);
        activate(zoom, 0); dialog->resize(1060, 620); QApplication::processEvents(); editor->grab().save(dir + "/osmium-small-curve.png");
    }
    { EditScope edit(&p, "Delete Osmium"); p.tracks[0].devices.clear(); }
    emit p.structureChanged(); QTest::qWait(70);
    CHECK(!deviceView.editorExpanded() && (!sameEditor || !sameEditor->isVisible()), "Deleting the instrument closes its integrated editor without closing Sessions");
    dialog->close(); std::printf("%d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
