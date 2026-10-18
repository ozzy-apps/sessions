#include "audio/Engine.h"
#include "core/Model.h"
#include "core/Theme.h"
#include "core/Transport.h"
#include "ui/ArrangementView.h"
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include <QUndoStack>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
void drag(QWidget& widget, QPointF from, QPointF to, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const auto global = [&widget](QPointF p) { return QPointF(widget.mapToGlobal(p.toPoint())); };
    QMouseEvent press(QEvent::MouseButtonPress, from, global(from), Qt::LeftButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(&widget, &press);
    QMouseEvent move(QEvent::MouseMove, to, global(to), Qt::NoButton, Qt::LeftButton, modifiers);
    QApplication::sendEvent(&widget, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, global(to), Qt::LeftButton, Qt::NoButton, modifiers);
    QApplication::sendEvent(&widget, &release);
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
}

int main(int argc, char** argv) {
    QTemporaryDir settings;
    qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
    qputenv("XDG_DATA_HOME", settings.path().toUtf8());
    QApplication app(argc, argv);
    theme::apply(theme::Kind::Dark);
    Project project;
    project.scenes = {"1"};
    project.addTrack("Drums", QColor("#789bc4"));
    project.addTrack("Bass", QColor("#9aad72"));
    if (project.returns.isEmpty()) { project.insertReturn(); project.insertReturn(); }
    audio::Engine engine(&project);
    Transport transport(&project, &engine);
    ArrangementView view(&project, &transport);
    view.resize(1100, 680);
    view.show();
    QApplication::processEvents();
    auto* scroll = view.findChild<QScrollBar*>("arrangementTrackScroll");
    auto* follow = view.findChild<QToolButton*>("arrangementFollowButton");
    check(scroll && follow, "Arrangement exposes its Follow toggle and track scrollbar");
    if (!scroll || !follow) return 1;
    const int regularCount = int(project.tracks.size());
    const int main = project.trackCount();
    check(view.laneTop(main) + view.laneHeight(main) == view.height() - 16,
          "Master stays at the bottom above the time ruler");
    check(view.laneTop(regularCount) > view.laneTop(regularCount - 1) + view.laneHeight(regularCount - 1) + 40,
          "few tracks leave empty space before the pinned returns");
    check(scroll->maximum() == 0 && scroll->isHidden(), "short arrangements need no vertical scrollbar");
    QSignalSpy selected(&view, &ArrangementView::trackSelected);
    const int gapY = (view.laneTop(regularCount - 1) + view.laneHeight(regularCount - 1) + view.laneTop(regularCount)) / 2;
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, gapY));
    check(selected.isEmpty(), "the empty space does not select a return track");

    QSignalSpy followChanges(&view, &ArrangementView::followChanged);
    QTest::mouseClick(follow, Qt::LeftButton);
    check(!view.follow() && !follow->isChecked() && followChanges.size() == 1,
          "the local Follow toggle changes the view and emits one state update");
    transport.setStartMarker(160);
    transport.play();
    const double fixedOffset = view.firstVisibleBeat();
    emit transport.tick();
    check(near(view.firstVisibleBeat(), fixedOffset), "Follow off keeps a distant playing position from scrolling the timeline");
    view.setFollow(true);
    emit transport.tick();
    check(follow->isChecked() && view.firstVisibleBeat() > fixedOffset,
          "Follow on pages to a distant playing position without changing zoom");
    const double heldOffset = view.firstVisibleBeat();
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, 45));
    transport.setBeat(300);
    emit transport.tick();
    check(near(view.firstVisibleBeat(), heldOffset), "Follow holds the view while a ruler gesture is active");
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, 45));
    const QPointF wheelPos(300, 100);
    QWheelEvent horizontalWheel(wheelPos, view.mapToGlobal(wheelPos.toPoint()), QPoint(), QPoint(-120, 0),
                                Qt::NoButton, Qt::NoModifier, Qt::ScrollUpdate, false);
    QApplication::sendEvent(&view, &horizontalWheel);
    const double manuallyPanned = view.firstVisibleBeat();
    transport.setBeat(400);
    emit transport.tick();
    check(near(view.firstVisibleBeat(), manuallyPanned), "Follow waits for a manual wheel gesture to finish");
    transport.stop();
    view.fitSong();

    const int markerY = view.laneTop(0) - 5;
    const int braceY = view.laneTop(0) - 14;
    transport.setStartMarker(2);
    transport.setLoop(4, 12);
    drag(view, QPointF(view.beatToX(2) + 2, markerY), QPointF(view.beatToX(6), markerY));
    check(near(transport.startMarker(), 6), "the right-facing start marker drags independently of the loop brace");
    drag(view, QPointF(view.beatToX(16) - 2, markerY), QPointF(view.beatToX(20), markerY));
    check(near(transport.loopStart(), 4) && near(transport.loopLength(), 16), "the left-facing end marker resizes the loop end");
    drag(view, QPointF(view.beatToX(4), braceY), QPointF(view.beatToX(8), braceY));
    check(near(transport.loopStart(), 8) && near(transport.loopLength(), 12), "the loop-start brace handle keeps the end fixed");
    drag(view, QPointF(view.beatToX(12), braceY), QPointF(view.beatToX(16), braceY));
    check(near(transport.loopStart(), 12) && near(transport.loopLength(), 12), "the loop brace body moves both boundaries together");
    const QImage neutralMarkers = view.grab().toImage().copy(QRect(0, braceY - 4, 600, 18));
    const QColor oldTrackColor = project.tracks[0].color;
    project.tracks[0].color = QColor("#ff00ac");
    emit project.mixerChanged();
    const QImage recoloredMarkers = view.grab().toImage().copy(QRect(0, braceY - 4, 600, 18));
    check(neutralMarkers == recoloredMarkers, "start, end and loop markers retain neutral colors when track colors change");
    project.tracks[0].color = oldTrackColor;
    const auto neutralFlag = [&] {
        return view.grab().toImage().pixelColor(int(view.beatToX(6)) + 2, markerY);
    };
    const QColor darkFlag = neutralFlag();
    check(darkFlag == theme::text, "dark-theme start flags use the neutral text color");
    theme::apply(theme::Kind::Light);
    check(neutralFlag() == theme::text, "light-theme start flags use contrasting neutral ink");
    theme::apply(theme::Kind::Dark);

    const ClipPtr clip = project.newClip("Group content", 24);
    project.tracks[0].arrangement = {{4, 24, 0, clip}};
    const int group = project.groupTracks({0, 1});
    view.selectTrack(group);
    project.undoStack()->clear();
    const int groupY = view.laneTop(group), groupH = view.laneHeight(group);
    drag(view, QPointF(view.beatToX(4) + 4, groupY + 17), QPointF(view.beatToX(8), groupY + 17));
    check(near(project.tracks[group].groupFadeIn, 4), "group fade-in square creates a bus fade over member content");
    check(project.undoStack()->count() == 1, "a group fade drag is one undo step");
    project.undoStack()->undo();
    check(near(project.tracks[group].groupFadeIn, 0), "undo restores the group fade without changing members");
    project.undoStack()->redo();
    drag(view, QPointF(view.beatToX(28) - 5, groupY + 17), QPointF(view.beatToX(24), groupY + 17));
    check(near(project.tracks[group].groupFadeOut, 4), "group fade-out square shares the group's aggregate ending");
    const QPointF curve(view.beatToX(6), groupY + groupH - 2 - 0.5 * (groupH - 15));
    drag(view, curve, QPointF(curve.x(), groupY + 25));
    check(project.tracks[group].groupFadeInCurve > 0.7, "the group fade curve node changes its shape");
    view.resetFades();
    check(near(project.tracks[group].groupFadeIn, 0) && near(project.tracks[group].groupFadeOut, 0)
              && near(project.tracks[group].groupFadeInCurve, 0.5), "Reset Group Fades clears lengths and shapes");
    project.setFolded(group, true);
    check(view.laneHeight(group + 1) == 0 && view.laneHeight(group) > 0, "a folded group retains its bus lane and fade handles");
    const QString capture = qEnvironmentVariable("SESSIONS_ARRANGEMENT_CAPTURE");
    if (!capture.isEmpty()) view.grab().save(capture);

    for (int i = 0; i < 24; ++i) project.addTrack(QString("Track %1").arg(i + 3), QColor("#6e819e"));
    QApplication::processEvents();
    const int returnTop = view.laneTop(int(project.tracks.size()));
    const int masterTop = view.laneTop(project.trackCount());
    const int firstTop = view.laneTop(0);
    check(scroll->maximum() > 0 && !scroll->isHidden(), "many tracks expose a vertical scrollbar");
    scroll->setValue(scroll->maximum());
    check(view.laneTop(0) < firstTop && view.laneTop(int(project.tracks.size()) - 1) + view.laneHeight(int(project.tracks.size()) - 1) <= returnTop,
          "vertical scrolling reaches the final regular track");
    check(view.laneTop(int(project.tracks.size())) == returnTop && view.laneTop(project.trackCount()) == masterTop,
          "returns and Master stay fixed while ordinary tracks scroll");
    selected.clear();
    const int last = int(project.tracks.size()) - 1;
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, view.laneTop(last) + 30));
    check(!selected.isEmpty() && selected.last()[0].toInt() == last, "hit testing selects the correct scrolled lane");
    view.selectTrack(0);
    check(view.laneTop(0) >= 70, "selecting an offscreen track reveals it without moving the pinned buses");

    while (project.returns.size() < 8) project.insertReturn();
    view.resize(1100, 330);
    QApplication::processEvents();
    auto* returnScroll = view.findChild<QScrollBar*>("arrangementReturnScroll");
    const int shortMaster = project.trackCount();
    const int shortMasterTop = view.laneTop(shortMaster);
    check(shortMasterTop + view.laneHeight(shortMaster) == view.height() - 16 && shortMasterTop >= 70,
          "eight returns in a short view keep Master fully visible at the bottom");
    check(returnScroll && returnScroll->maximum() > 0 && !returnScroll->isHidden(),
          "overflowing returns expose their own scrollbar");
    if (returnScroll) {
        returnScroll->setValue(returnScroll->maximum());
        const int finalReturn = project.trackCount() - 1;
        check(view.laneTop(finalReturn) + view.laneHeight(finalReturn) <= shortMasterTop,
              "return scrolling reaches the final return above Master");
        selected.clear();
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(100, view.laneTop(finalReturn) + 18));
        check(!selected.isEmpty() && selected.last()[0].toInt() == finalReturn,
              "hit testing identifies a scrolled return without selecting a regular track");
        check(view.laneTop(shortMaster) == shortMasterTop, "scrolling returns never moves Master");
    }
    view.setAutomationMode(true);
    check(view.laneTop(shortMaster) + view.laneHeight(shortMaster) == view.height() - 16,
          "taller automation headers also keep Master pinned in a short view");
    view.resize(1100, 140);
    QApplication::processEvents();
    check(view.laneTop(shortMaster) >= 70 && view.laneTop(shortMaster) + view.laneHeight(shortMaster) == view.height() - 16,
          "a very short viewport reserves its remaining body for visible Master controls");
    std::printf("Arrangement workflow: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
