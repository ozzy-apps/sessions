// Osmium's expanded editor: usable drawing areas, direct table browsing and compact controls.
#include "audio/Engine.h"
#include "audio/Wavetables.h"
#include "core/DeviceSpecs.h"
#include "core/OsmiumPresets.h"
#include "core/SetFile.h"
#include <QCompleter>
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/DeviceView.h"
#include "ui/devices/DeviceBody.h"
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace mo = sessions::monolith;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* text) { ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", text); } }
void settle() { QApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); }
void drag(QWidget* widget, QPoint start, QPoint end, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::mousePress(widget, Qt::LeftButton, modifiers, start);
    QMouseEvent move(QEvent::MouseMove, end, widget->mapToGlobal(end), Qt::NoButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(widget, &move); QTest::mouseRelease(widget, Qt::LeftButton, modifiers, end); settle();
}
QPushButton* page(QWidget* editor, const QString& text) {
    for (auto* button : editor->findChildren<QPushButton*>()) if (button->text() == text) return button;
    return nullptr;
}
bool horizontalOverflow(QWidget* editor) {
    for (auto* area : editor->findChildren<QScrollArea*>()) if (area->isVisible() && area->horizontalScrollBar()->maximum() > 0) return true;
    return false;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); QTemporaryDir preferences;
    QSettings::setDefaultFormat(QSettings::IniFormat); QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    QCoreApplication::setOrganizationName("SessionsOsmiumUiTest"); QCoreApplication::setApplicationName("SessionsOsmiumUiTest");
    theme::apply(theme::Kind::Dark);
    Project project; project.scenes = {"1"}; project.addTrack("Osmium", theme::accent);
    project.tracks[0].devices = {project.makeDevice("Osmium", DeviceKind::Instrument)};
    audio::Engine engine(&project); Transport transport(&project, &engine); DeviceView view(&project, &transport);
    view.resize(2000, 940); view.show(); settle();
    auto current = [&]() -> Device& { return project.tracks[0].devices[0]; };
    auto compactPicker = [&] { return view.findChild<QComboBox*>("osmiumCompactPatch"); };
    auto compactArrow = [&](bool next) { return view.findChild<QPushButton*>(next ? "osmiumNextPreset" : "osmiumPreviousPreset"); };
    auto chooseCompact = [&](int index) { auto* combo = compactPicker(); combo->setCurrentIndex(index); QMetaObject::invokeMethod(combo, "activated", Q_ARG(int, index)); settle(); };
    auto* compact = view.findChild<devui::DeviceBody*>("osmiumCompact");
    check(compact && compactPicker() && compactArrow(false) && compactArrow(true) && compactPicker()->isEditable()
        && compactPicker()->count() == int(osmium::factoryPresets().size()) + 7, "The compact panel has a searchable complete preset selector and previous/next controls");
    if (!compact || !compactPicker()) return 1;
    compactPicker()->completer()->setCompletionPrefix("sLoW rUpTuRe");
    check(compactPicker()->completer()->completionCount() == 1, "Compact search finds a partial preset name without case sensitivity");
    const auto initialDevice = setfile::deviceToJson(current()); const int initialUndo = project.undoStack()->index(); const int originalId = current().id;
    const auto& newPatch = osmium::factoryPresets()[88];
    chooseCompact(compactPicker()->findData(QString::fromUtf8(newPatch.name)));
    check(current().name == newPatch.name && current().id == originalId && current().mods == newPatch.mods && current().curves == newPatch.curves
        && project.undoStack()->index() == initialUndo + 1, "Compact selection loads the complete sound in one undo step and preserves identity");
    project.undoStack()->undo(); settle();
    check(setfile::deviceToJson(current()) == initialDevice && compactPicker()->currentIndex() == 0, "Undo restores the exact previous device and compact selection");
    project.undoStack()->redo(); settle();
    check(compactPicker()->currentData().toString() == current().name, "Redo restores compact selection alongside the sound");
    const int beforeNext = project.undoStack()->index(); compactArrow(true)->click(); settle();
    check(current().name == osmium::factoryPresets()[89].name && project.undoStack()->index() == beforeNext + 1, "Next loads the adjacent preset in one undo step");
    compactArrow(false)->click(); settle();
    check(current().name == newPatch.name, "Previous returns to the preceding factory preset");
    chooseCompact(4); compactArrow(true)->click(); settle();
    check(current().name == osmium::factoryPresets().front().name, "Navigation skips the separator after the starting patches");
    const auto& firstWobble = osmium::factoryPresets()[112];
    chooseCompact(compactPicker()->findData(QString::fromUtf8(osmium::factoryPresets()[111].name)));
    compactArrow(true)->click(); settle();
    check(current().name == firstWobble.name && current().id == originalId && current().curves == firstWobble.curves,
          "Next skips the wobble separator and loads the complete dense phrase without changing identity");
    compactArrow(false)->click(); settle();
    check(current().name == osmium::factoryPresets()[111].name, "Previous skips the wobble separator in reverse");
    compactPicker()->completer()->setCompletionPrefix("dubstep");
    check(compactPicker()->completer()->completionCount() == 10, "Compact search includes all ten new Dubstep sounds");
    chooseCompact(0); compactArrow(false)->click(); settle();
    check(current().name == osmium::factoryPresets().back().name, "Previous wraps from Init to the last preset");
    compactArrow(true)->click(); settle();
    check(current().name == "Osmium" && compactPicker()->currentIndex() == 0, "Next wraps from the last preset to Init");
    auto* combo = compactPicker(); combo->showPopup(); QTest::keyClick(combo->view(), Qt::Key_End); QTest::keyClick(combo->view(), Qt::Key_Return); settle();
    check(current().name == osmium::factoryPresets().back().name, "Keyboard selection from the compact popup loads the chosen sound");
    chooseCompact(compactPicker()->findData(QString::fromUtf8(newPatch.name)));
    compact = view.findChild<devui::DeviceBody*>("osmiumCompact");
    check(compact->width() == 502 && compact->height() == devui::kBodyHeight && compactPicker()->width() >= 260
        && compact->rect().contains(compactPicker()->geometry()) && compact->rect().contains(compactArrow(true)->geometry()), "Preset controls fit the existing compact panel without increasing its footprint");
    if (const QString path = qEnvironmentVariable("SESSIONS_OSMIUM_UI_SHOTS"); !path.isEmpty()) {
        QDir().mkpath(path); compact->parentWidget()->grab().save(path + "/osmium-compact-dark.png");
        compactPicker()->showPopup(); settle(); compactPicker()->view()->window()->grab().save(path + "/osmium-compact-presets.png"); compactPicker()->hidePopup();
        theme::apply(theme::Kind::Light); settle(); view.findChild<QWidget*>("osmiumCompact")->parentWidget()->grab().save(path + "/osmium-compact-light.png");
        theme::apply(theme::Kind::Dark); settle();
    }
    auto* expand = view.findChild<QPushButton*>("osmiumOpenEditor"); check(expand, "Osmium retains its rack expansion control");
    if (!expand) return 1; expand->click(); settle();
    auto* editor = view.findChild<devui::DeviceBody*>("osmiumEditor"); check(editor && view.editorExpanded(), "The enhanced editor remains embedded in Device View");
    if (!editor) return 1;
    auto* expandedPicker = editor->findChild<QComboBox*>("osmiumPatch");
    check(expandedPicker && expandedPicker->currentData().toString() == current().name, "Expanding retains the compact preset selection");
    const int firstFactory = expandedPicker->findData(QString::fromUtf8(osmium::factoryPresets().front().name));
    expandedPicker->setCurrentIndex(firstFactory); QMetaObject::invokeMethod(expandedPicker, "activated", Q_ARG(int, firstFactory)); settle();
    check(compactPicker()->currentData().toString() == current().name && view.editorExpanded(), "Expanded loading synchronizes the compact selector and keeps the editor open");
    project.undoStack()->undo(); settle();
    check(expandedPicker->currentData().toString() == newPatch.name && compactPicker()->currentData().toString() == newPatch.name, "Undo synchronizes both preset selectors");
    expandedPicker->setCurrentIndex(0); QMetaObject::invokeMethod(expandedPicker, "activated", Q_ARG(int, 0)); settle();
    auto* sound = page(editor, "Sound"); auto* effects = page(editor, "Effects"); auto* voice = page(editor, "Voice && mixer");
    const auto dropAndUndo = [&](QWidget* control, int param) {
        if (!control || !control->acceptDrops() || control->property("mapTarget").toString() != editor->target(param)) return false;
        const auto previous = current().mods; const int undoIndex = project.undoStack()->index();
        QMimeData mime; mime.setData("application/x-sessions-osmium-source", QByteArray::number(current().id) + ':' + QByteArray::number(mo::SrcCurve2));
        QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(control, &enter);
        QDropEvent drop(QPointF(10, 10), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(control, &drop); settle();
        const bool assigned = enter.isAccepted() && drop.isAccepted() && std::any_of(current().mods.begin(), current().mods.end(), [=](const ModRoute& route) { return route.param == param && route.source == mo::SrcCurve2; })
            && project.undoStack()->index() == undoIndex + 1;
        if (project.undoStack()->index() > undoIndex) project.undoStack()->undo(); settle();
        const bool restored = current().mods == previous && project.undoStack()->index() == undoIndex;
        sound->click(); settle(); return assigned && restored;
    };
    auto* soundGrid = editor->findChild<QWidget*>("osmiumSoundPage");
    check(sound && effects && voice && soundGrid && soundGrid->property("columns").toInt() == 5, "A wide editor gives all five sound sections one row");
    auto* wave = editor->findChild<QWidget*>("osmiumWave_0"); auto* filter = editor->findChild<QWidget*>("osmiumFilter_0");
    check(wave && filter && wave->width() >= 260 && wave->height() >= 220 && filter->height() >= 220,
        "Waveform and filter displays use the available height instead of narrow strips");
    auto* position = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::oparam(0, mo::Position)));
    auto* fold = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(osmium::fold(0)));
    auto* cutoff = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::fparam(0, mo::FCutoff)));
    auto* macro = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::macro(0)));
    check(position && cutoff && fold && macro && position->property("dialDiameter").toInt() == 64
        && cutoff->property("dialDiameter").toInt() == 64 && fold->property("dialDiameter").toInt() == 52 && macro->property("dialDiameter").toInt() == 84,
        "Major scan/cutoff controls and macros stay large while secondary controls become compact");
    check(fold && fold->property("rangeRadius").toDouble() - fold->property("baseRadius").toDouble() >= 8,
        "Compact controls preserve the separate base and modulation hit areas");
    check(!horizontalOverflow(editor), "The wide sound layout has no horizontal overflow");

    const double initialPosition = current().params[mo::oparam(0, mo::Position)].value;
    const int undo = project.undoStack()->index();
    drag(wave, QPoint(wave->width() / 3, wave->height() / 2), QPoint(2 * wave->width() / 3, wave->height() / 2));
    check(current().params[mo::oparam(0, mo::Position)].value > initialPosition + 0.25 && project.undoStack()->index() == undo + 1,
        "Dragging the large display scans the actual oscillator in one undo step");
    project.undoStack()->undo(); settle();
    check(std::abs(current().params[mo::oparam(0, mo::Position)].value - initialPosition) < 1e-8, "Undo restores the display scan position");
    drag(wave, QPoint(wave->width() / 3, wave->height() / 2), QPoint(2 * wave->width() / 3, wave->height() / 2), Qt::ShiftModifier);
    check(current().params[mo::oparam(0, mo::Position)].value > initialPosition
        && current().params[mo::oparam(0, mo::Position)].value < initialPosition + 0.05, "Shift gives fine wavetable scanning");
    project.undoStack()->undo(); settle();
    const auto tableImage = wave->grab().toImage();
    QTest::mouseClick(wave, Qt::LeftButton, Qt::NoModifier, QPoint(65, 14)); settle();
    check(wave->property("displayMode").toInt() == 1 && wave->grab().toImage() != tableImage, "Wave switches to the current warped single-cycle display");
    QTest::mouseClick(wave, Qt::LeftButton, Qt::NoModifier, QPoint(107, 14)); settle();
    check(wave->property("displayMode").toInt() == 2 && wave->grab().toImage() != tableImage, "Harm switches to actual harmonic magnitudes");
    QTest::mouseClick(wave, Qt::LeftButton, Qt::NoModifier, QPoint(24, 14)); settle();
    check(wave->property("displayMode").toInt() == 0, "Table restores the layered frame display");

    auto* picker = editor->findChild<QPushButton*>("osmiumTablePicker_0"); auto* tree = editor->findChild<QTreeWidget*>("osmiumTableTree_0");
    auto* search = editor->findChild<QLineEdit*>("osmiumTableSearch_0");
    int tables = 0; if (tree) for (int family = 0; family < tree->topLevelItemCount(); ++family) tables += tree->topLevelItem(family)->childCount();
    check(picker && tree && search && tables == audio::wavetableCount() && tables == 48 && tree->topLevelItemCount() == 8,
        "The table browser contains all 48 tables in eight musical families");
    check(dropAndUndo(picker, mo::oparam(0, mo::Table)), "The legacy table picker retains its MIDI map target and accepts undoable modulation drops");
    picker->click(); settle(); QTest::keyClicks(search, "reese"); settle();
    int visible = 0;
    for (int family = 0; family < tree->topLevelItemCount(); ++family) {
        auto* group = tree->topLevelItem(family); if (group->isHidden()) continue;
        for (int row = 0; row < group->childCount(); ++row) if (!group->child(row)->isHidden()) ++visible;
    }
    check(visible == 1, "Search finds a table by partial name across families");
    const double oldLegacy = current().params[mo::oparam(0, mo::Table)].value;
    const int tableUndo = project.undoStack()->index(); QTest::keyClick(search, Qt::Key_Return); settle();
    check(osmium::tableIndex(current().params[mo::oparam(0, mo::Table)].value, current().params[osmium::tableExtension(0)].value) == 38
        && current().params[mo::oparam(0, mo::Table)].value == oldLegacy && project.undoStack()->index() == tableUndo + 1,
        "Choosing a new table preserves the old selector and is one undo step");
    check(picker->property("tableIndex").toInt() == 38 && picker->text() == "Reese Core", "The table button follows the selected extended bank entry");
    check(dropAndUndo(picker, osmium::tableExtension(0)), "An extended table maps and receives modulation through its bank selector");
    project.undoStack()->undo(); settle();
    check(current().params[osmium::tableExtension(0)].value == 0 && picker->property("tableIndex").toInt() < 8, "Undo returns the browser and sound to the legacy bank");
    picker->click(); settle(); QTest::keyClicks(search, "Organic"); settle();
    visible = 0; for (int family = 0; family < tree->topLevelItemCount(); ++family) {
        auto* group = tree->topLevelItem(family); if (group->isHidden()) continue;
        for (int row = 0; row < group->childCount(); ++row) if (!group->child(row)->isHidden()) ++visible;
    }
    check(visible == 4, "Search also filters by musical family");
    editor->findChild<QMenu*>("osmiumTableMenu_0")->hide(); settle();
    auto* warp = editor->findChild<QComboBox*>("osmiumWarpMode_0");
    check(warp && warp->count() == 8, "Each oscillator exposes eight warp modes including Cross FM");
    check(dropAndUndo(warp, mo::oparam(0, mo::Mode)), "The legacy warp choice retains its MIDI map target and undoable modulation assignment");
    warp->setCurrentIndex(7); QMetaObject::invokeMethod(warp, "activated", Q_ARG(int, 7)); settle();
    check(osmium::warpMode(current().params[mo::oparam(0, mo::Mode)].value, current().params[osmium::warpExtension(0)].value) == 7,
        "Cross FM selection reaches the new oscillator parameter");
    check(dropAndUndo(warp, osmium::warpExtension(0)), "An extended warp maps and receives modulation through its extension selector");
    warp->setCurrentIndex(1); QMetaObject::invokeMethod(warp, "activated", Q_ARG(int, 1)); settle();
    check(current().params[osmium::warpExtension(0)].value == 0 && choiceIndex(specOf(DeviceType::Osmium)->params[mo::oparam(0, mo::Mode)], current().params[mo::oparam(0, mo::Mode)].value) == 1,
        "Choosing an original warp clears the extension and retains its original normalized mapping");
    const auto rangeBefore = current().mods; const double foldBefore = current().params[osmium::fold(0)].value;
    const int foldCentre = 16 + fold->property("dialDiameter").toInt() / 2;
    drag(fold, QPoint(fold->width() / 2, foldCentre), QPoint(fold->width() / 2, foldCentre - 15));
    check(current().params[osmium::fold(0)].value > foldBefore && current().mods == rangeBefore, "The compact Fold centre edits audio amount without grabbing modulation");
    project.undoStack()->undo(); settle();
    const double cutoffBefore = current().params[mo::fparam(0, mo::FCutoff)].value;
    drag(filter, QPoint(filter->width() / 3, filter->height() / 2), QPoint(filter->width() * 3 / 4, filter->height() / 3));
    check(current().params[mo::fparam(0, mo::FCutoff)].value > cutoffBefore, "The larger response graph still edits cutoff and resonance");
    project.undoStack()->undo(); settle();
    const auto mappedControl = [&](int param) -> QWidget* {
        for (QWidget* widget : editor->findChildren<QWidget*>()) if (widget->property("mapTarget").toString() == editor->target(param)) return widget;
        return nullptr;
    };
    check(dropAndUndo(mappedControl(mo::FRouting), mo::FRouting), "The filter Routing field preserves MIDI mapping and undoable modulation drops");
    check(dropAndUndo(mappedControl(mo::Feedback), mo::Feedback), "The filter Feedback field preserves MIDI mapping and undoable modulation drops");

    voice->click(); settle(); auto* detune = editor->findChild<QWidget*>(QString("osmiumKnob_%1").arg(mo::Detune));
    check(detune && detune->isVisible() && detune->property("dialDiameter").toInt() == 52 && !horizontalOverflow(editor),
        "Voice groups use compact controls and fit without horizontal scrolling");
    drag(detune, QPoint(detune->width() / 2, 42), QPoint(detune->width() / 2, 32));
    check(voice->isChecked() && detune->isVisible(), "A compact voice edit keeps its editor page open"); project.undoStack()->undo(); settle();
    auto* field = editor->findChild<QWidget*>("osmiumUnisonField");
    const double spreadBefore = current().params[mo::Spread].value, detuneBefore = current().params[mo::Detune].value;
    const int fieldUndo = project.undoStack()->index();
    check(field && field->isVisible() && field->height() >= 132, "The voice page uses remaining space for a stereo and detune field");
    drag(field, field->rect().center(), field->rect().center() + QPoint(20, -15));
    check(current().params[mo::Spread].value > spreadBefore && current().params[mo::Detune].value > detuneBefore
        && project.undoStack()->index() == fieldUndo + 1 && voice->isChecked(), "Dragging the unison field shapes spread and detune in one undo step");
    project.undoStack()->undo(); settle();
    check(std::abs(current().params[mo::Spread].value - spreadBefore) < 1e-8 && std::abs(current().params[mo::Detune].value - detuneBefore) < 1e-8,
        "Undo restores both unison field parameters");
    effects->click(); settle(); bool effectsVisible = true;
    for (int section = 0; section < 4; ++section) { auto* plot = editor->findChild<QWidget*>(QString("osmiumFxPlot_%1").arg(section)); effectsVisible &= plot && plot->isVisible() && plot->height() >= 100; }
    check(effectsVisible && !horizontalOverflow(editor), "All four effects have dedicated visual displays and fit a wide editor");
    const auto darkEffect = editor->findChild<QWidget*>("osmiumFxPlot_3")->grab().toImage();
    theme::apply(theme::Kind::Light); settle();
    check(effects->isChecked() && editor->findChild<QWidget*>("osmiumFxPlot_3")->grab().toImage() != darkEffect, "Theme switching recolors the visual displays and preserves the page");
    theme::apply(theme::Kind::Dark); sound->click(); settle();
    view.resize(1320, 800); settle();
    check(soundGrid->property("columns").toInt() == 3 && !horizontalOverflow(editor), "A medium editor wraps complete filter cards beneath three oscillators");
    view.resize(1060, 620); settle();
    check(soundGrid->property("columns").toInt() == 2 && !horizontalOverflow(editor) && wave->width() >= 260 && wave->height() >= 132,
        "The minimum editor keeps usable displays and scrolls vertically");
    auto* curveSource = editor->findChild<QPushButton*>(QString("osmiumSource_%1").arg(mo::SrcCurve1)); curveSource->click(); settle();
    check(page(editor, "Modulators")->isChecked() && editor->findChild<QWidget*>("osmiumCurve_0")->isVisible(), "The new sound layout leaves the full curve editor available");

    if (const QString path = qEnvironmentVariable("SESSIONS_OSMIUM_UI_SHOTS"); !path.isEmpty()) {
        QDir().mkpath(path); view.resize(2000, 940);
        const int gestureIndex = expandedPicker->findData(QString::fromUtf8(newPatch.name));
        expandedPicker->setCurrentIndex(gestureIndex); QMetaObject::invokeMethod(expandedPicker, "activated", Q_ARG(int, gestureIndex)); settle();
        curveSource->click(); settle();
        editor->grab().save(path + "/osmium-gesture-curve-dark.png");
        sound->click(); settle();
        editor->grab().save(path + "/osmium-sound-dark.png");
        picker->click(); settle(); tree->scrollToTop(); settle(); editor->grab().save(path + "/osmium-table-browser-background.png");
        editor->findChild<QMenu*>("osmiumTableMenu_0")->grab().save(path + "/osmium-table-browser.png");
        editor->findChild<QMenu*>("osmiumTableMenu_0")->hide();
        for (int mode = 1; mode < 3; ++mode) { QTest::mouseClick(wave, Qt::LeftButton, Qt::NoModifier, QPoint(24 + mode * 41, 14)); settle(); editor->grab().save(path + QString("/osmium-display-%1-dark.png").arg(mode)); }
        QTest::mouseClick(wave, Qt::LeftButton, Qt::NoModifier, QPoint(24, 14));
        voice->click(); settle(); editor->grab().save(path + "/osmium-voice-dark.png"); effects->click(); settle(); editor->grab().save(path + "/osmium-effects-dark.png");
        theme::apply(theme::Kind::Light); sound->click(); settle(); editor->grab().save(path + "/osmium-sound-light.png"); effects->click(); settle(); editor->grab().save(path + "/osmium-effects-light.png");
        theme::apply(theme::Kind::Dark); sound->click(); view.resize(1320, 800); settle(); editor->grab().save(path + "/osmium-sound-medium.png");
        view.resize(1060, 620); settle(); editor->grab().save(path + "/osmium-sound-small.png");
    }
    std::printf("osmium_ui: %d checks, %d failures\n", checks, failures); return failures ? 1 : 0;
}
