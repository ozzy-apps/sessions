#pragma once
// ArrangementView: the timeline. Custom painted, laid out.
//  - top: the overview (the whole song; the outline is what's on screen: drag it sideways
//    to scroll, up or down to zoom (Preferences picks which way zooms in), click to go
//    there), locator strip (double-click adds, drag moves, click jumps), loop brace (drag
//    to move, drag its ends to resize), beat ruler (drag up/down zooms, sideways scrolls,
//    click moves the start marker)
//  - bottom: the time ruler (minutes:seconds at the set's tempo)
//  - lanes: click/drag empty space, or a clip's body, for a time selection (Shift extends;
//    inside a clip it selects that part of it), drag a clip by its name bar to move it
//    (across tracks too; Ctrl: a copy), drag a clip's right edge to lengthen, drag a
//    selected part of a clip to move that part on its own (Ctrl: a copy of it, the clip
//    stays whole), double-click to add a clip; Ctrl+C / Ctrl+X copy or cut the selection
//    (a part of a clip becomes a clip of its own that still holds the whole sample: extend
//    it and the rest is there), Ctrl+V pastes at the insert marker; Ctrl+E split, Ctrl+J
//    consolidate, Ctrl+L loop the selection
//  - automation mode (A): each lane shows one parameter's envelope; click adds a
//    breakpoint, drag moves it, double-click deletes it
//  - track headers on the right: the name on a block of the track's colour (drag it to
//    reorder; right-click for the track menu), then the mixer: activator / solo / arm
//    (returns: Pre / Post), volume and pan fields, send fields. Drag a field up or down
//    (Shift: fine), double-click it to reset.
//  - return tracks as slim lanes under the tracks (no clips), then the Main lane (main volume)
//  - audio clips show their waveform, and their fades (0.33): hover a clip and a square sits at
//    each top corner; drag it sideways for a fade-in / fade-out of that length (the grid; Alt:
//    free), then drag the round handle on the curve up or down for its shape; right-click →
//    Reset Fades. Fades belong to the placement (a split keeps the fade-in left, the fade-out right)
//  - drop audio files (browser, file manager) on an audio
//    lane to place them at the snapped beat, anywhere else for a new audio track
//  - devices (browser items, presets, device panels dragged by their title) dropped on a
//    track's lane or header go on that track; on the Main lane or below it, onto a new track
//  - a Session clip dragged in (Tab while dragging it in the Session View) follows the mouse;
//    dropping it on a lane of its kind places a copy at the snapped beat (Alt: free)
//  - wheel scrolls, Ctrl+wheel zooms around the cursor

#include <QWidget>
#include <QHash>
#include <QVector>
#include <QPair>
#include <memory>
#include "core/Transport.h"

class QToolButton;

namespace sessions {

class Project;
class Transport;
struct Track;
struct Clip;
struct ArrangementClip;

class ArrangementView : public QWidget {
    Q_OBJECT
public:
    int    laneTop(int i) const;      // a lane's top in widget pixels (tests, the device-drop line)
    int    laneHeight(int i) const;
    ArrangementView(Project* project, Transport* transport, QWidget* parent = nullptr);

    // Edit commands (Edit menu / shortcuts).
    void deleteSelection();      // clip, breakpoint, or the material in the time selection
    void duplicateSelection();   // clip, or the material in the time selection
    void zoomToFit();
    void fitSong();   // a new set: fit the song and keep fitting until you zoom or scroll
    void jumpTo(double beat);   // start marker there; a playing song jumps too
    void setFollow(bool on) { m_follow = on; }   // scroll a page on when the playhead leaves the view
    void loopSelection();        // Ctrl+L
    void splitAtSelection();     // Ctrl+E
    void consolidateSelection(); // Ctrl+J
    void resetFades();           // the selected clip's fades go (the context menu)
    void insertTime();
    void deleteTime();
    void duplicateTime();
    void addLocator();           // at the insert marker, else the playhead
    void renameSelectedTrack();
    void renameSelectedClip();   // in place, on the clip's name bar
    void setAutomationMode(bool on);
    bool automationMode() const { return m_autoMode; }
    bool hasTimeSelection() const { return m_timeSel.active && m_timeSel.end > m_timeSel.start; }
    // A value field in a track header (lane = trackCount() for the Main lane's volume).
    struct Field { enum Kind { None, Volume, Pan, Send, MainVolume } kind = None; int lane = -1, send = -1; };
    QRect  fieldRect(const Field& f) const;   // empty when it doesn't fit the lane
    double firstVisibleBeat() const { return m_offset; }
    double pixelsPerBeat() const { return m_zoom; }
    std::shared_ptr<Clip> selectedClip() const;
    void pasteClip(const std::shared_ptr<Clip>& clip);   // at the insert marker on the selected track
    void selectTrack(int lane);
    void selectTime(double start, double end, int lane0, int lane1);   // a time selection, as if dragged

    // The time selection's material as pieces (0.21): a part of a clip is a clip of its own
    // (a copy of the whole clip, placed at the part's offset), so it can be pasted, and extended
    // to show the rest. Lanes and starts are relative to the selection's first lane and start.
    struct ClipPiece { int lane = 0; double start = 0, length = 0, offset = 0; std::shared_ptr<Clip> clip; };
    QVector<ClipPiece> copyRange() const;          // empty without a time selection (or with nothing in it)
    void cutRange();                               // removes the time selection's material (copy it first)
    void pastePieces(const QVector<ClipPiece>& pieces);   // at the insert marker, from the selected track down

    // A Session clip dragged in: MainWindow::showSession hands it over when you press Tab while
    // dragging a clip in the Session View. The view follows the mouse (it grabs it; the button is
    // still down) and the release places a copy of the clip at the snapped beat (Alt: free) on a
    // lane of the clip's kind, one undo step; anywhere else nothing happens. endClipDrag() takes
    // it back without placing (Tab again returns it to the Session View; Escape drops it).
    void    beginClipDrag(const std::shared_ptr<Clip>& clip, int fromTrack, int fromScene);
    bool    clipDragActive() const { return m_clipDrag != nullptr; }
    std::shared_ptr<Clip> endClipDrag(int* fromTrack = nullptr, int* fromScene = nullptr);
    int     clipDropLane(const QPointF& pos, const std::shared_ptr<Clip>& clip) const;   // the lane it would land on, -1 for none

    // Clips from the library (.sessionsclip files, 0.22) placed from `beat` on lane `lane` (a lane of
    // another kind, or -1: a new track of the clip's kind after it). One undo step; the track's index.
    int    placeClipFiles(const QStringList& paths, int lane, double beat);

    QRect  laneRect(int lane) const;   // lanes: tracks first, then returns
    double xToBeat(double x) const { return m_offset + x / m_zoom; }
    double beatToX(double b) const { return (b - m_offset) * m_zoom; }

signals:
    void trackSelected(int track);
    void clipSelected(int track, ArrangementClip* clip);
    // The context menu asked for a window-wide command (0.22): "cut", "copy", "paste", "saveClip" (the
    // selected clip into the User Library), "showSample" (the selected clip's file in the file manager).
    void commandRequested(const QString& command);
    void openDeviceView(int track);
    void automationModeChanged(bool on);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    // The fade handles of an audio placement on a lane (0.33): the squares at the top of the
    // body where each fade ends (at the corner with no fade), the circles half-way along each curve.
    struct FadeHandles { QPointF inSq, outSq, inCircle, outCircle; };
    FadeHandles fadeHandles(const ArrangementClip& a, int lane) const;
    bool   hovered(const ArrangementClip& a) const;   // the clip under the mouse (its handles show)
    int    laneWidth() const { return width() - m_headerW; }
    int    trackAt(int y) const;      // lane index (tracks then returns), -1 outside
    ArrangementClip* clipAt(int track, double beat);
    ArrangementClip* validSel();      // m_sel, if it still points into the model
    void   drawClip(QPainter& p, const Track& t, const ArrangementClip& a, int y, int h, double alpha);
    void   drawTake(QPainter& p, const Transport::Take& take);   // a recording in progress: solid, with its waveform / notes so far
    void   drawEnvelope(QPainter& p, int lane);
    void   fitLanes();
    double grid() const;              // snap step for the current zoom
    double snap(double beat, bool fine) const;
    QString autoTarget(int lane) const;
    double valueToY(int lane, double v) const;
    double yToValue(int lane, double y) const;
    int    pointAt(int lane, const QPointF& pos) const;   // breakpoint index under the mouse, or -1
    int    locatorAt(double x) const;
    void   chooseTarget(int lane, const QPoint& globalPos);
    QRect  headerRect(int lane) const;
    // header parts (lane = trackCount() is the Main lane)
    QRect  nameRect(int lane) const;
    QRect  mixerRect(int lane) const;
    QRect  actRect(int lane) const;
    QRect  soloRect(int lane) const;
    QRect  armRect(int lane) const;
    QRect  prePostRect(int lane) const;
    QRect  chooserRect(int lane) const;
    int    mainLaneTop() const { return laneTop(laneCount()); }
    int    laneCount() const;
    Field  fieldAt(const QPoint& p) const;
    double fieldValue(const Field& f) const;  // what it shows: the envelope's value while automated
    void   setFieldValue(const Field& f, double v);
    QString fieldText(const Field& f) const;
    double overviewEnd() const;               // beats the overview spans
    void   centerOn(double beat);

    Project*   m_project;
    Transport* m_transport;
    QToolButton* m_backToArr;
    QToolButton* m_autoButton;

    double m_zoom = 12.0;    // pixels per beat
    double m_offset = 0.0;   // first visible beat
    double m_offset0 = 0.0, m_overK = 1.0;   // Drag::Overview: the offset at the press, the overview's pixels per beat
    bool   m_userZoomed = false;   // until you zoom or scroll, the view keeps fitting the song
    bool   m_follow = true;
    bool   m_lastOverridden = false;   // whether any track was overridden at the last tick (a change repaints)
    int    m_dropLane = -2;        // a file drag hovers this lane (-1: below the tracks), -2: none
    int    m_deviceLane = -2;      // a device drag hovers this lane (-1: a new track), -2: none
    int    m_deviceAfter = -1;     // with m_deviceLane -1: the new track goes right after this lane (an instrument over an audio track with clips)
    int    deviceLaneAt(int y) const;   // where a device dropped at y goes: a lane, -1 a new track, -2 nowhere
    double m_dropBeat = 0;
    static constexpr int kOverH = 22, kLocY = kOverH, kLocH = 14, kBraceY = kLocY + kLocH, kBraceH = 6, kRulerH = 16;
    static constexpr int kTopH = kBraceY + kBraceH + kRulerH, kTimeH = 16, kNameW = 112;
    int    m_trackH = 56, m_returnH = 40, m_headerW = 236;
    bool   m_autoMode = false;
    QHash<int, QString> m_autoTargets;   // track id -> parameter shown in automation mode

    // selection
    int    m_selTrack = 0;
    ArrangementClip* m_sel = nullptr;
    struct TimeSel { bool active = false; double start = 0, end = 0; int lane0 = 0, lane1 = 0; } m_timeSel;
    struct PointSel { int lane = -1; int index = -1; } m_selPoint;
    int    m_selLocator = -1;

    // interaction state
    enum class Drag { None, Ruler, Move, Resize, ResizeLeft, Time, Point, Header, Locator, LoopMove, LoopStart, LoopEnd, Overview, Field, MoveRange,
                      FadeIn, FadeOut, FadeInCurve, FadeOutCurve };
    int     m_hoverLane = -1, m_hoverIndex = -1;   // the clip under the mouse (lane, index in its arrangement)
    double  m_fieldWrap = 0;   // Drag::Field: pixels the endless drag has warped the pointer by (added to the drag's distance)
    // Drag::ResizeLeft: the clip's left edge: the start moves, the audio stays put (the offset follows; a
    // looping clip wraps, a one-shot stops at its audio's start)
    double  m_clipOffset0 = 0;
    void    settleOverlaps(int lane, ArrangementClip*& placed);   // what's under a moved / resized clip is cut away: what's placed is what plays
    // Drag::MoveRange: the selected part of the clips, split out (or copied, with Ctrl) and moved together
    QVector<QPair<int, int>> m_rangeDrag;   // (lane, index into its arrangement) of each piece
    QVector<double> m_rangeStart0;          // each piece's start at the press
    double  m_rangeSel0 = 0, m_rangeSel1 = 0;   // the time selection at the press
    bool    m_copyDrag = false;             // Ctrl: the drag moves a copy (an unmoved copy is taken back on release)
    int     m_copyOrig = -1;                // Drag::Move with a copy: the original's index in its lane
    void    beginRangeDrag(bool copy);
    Field   m_field;            // Drag::Field: the header field being dragged
    double  m_fieldV0 = 0;
    Drag    m_drag = Drag::None;
    QPointF m_pressPos, m_mousePos;
    double  m_pressBeat = 0, m_zoom0 = 12, m_start0 = 0, m_len0 = 0;
    double  m_anchorBeat = 0;
    int     m_anchorLane = 0;
    bool    m_moved = false;
    bool    m_editOpen = false;   // a drag is recording an undo step
    int     m_dragTrack = -1;
    double  m_loop0 = 0, m_loopLen0 = 0;

    // a Session clip dragged in (see beginClipDrag)
    std::shared_ptr<Clip> m_clipDrag;
    int     m_clipDragTrack = -1, m_clipDragScene = -1;   // the slot it came from
    bool    m_clipDragFine = false;                       // Alt held at the last move
    void    placeDraggedClip(const std::shared_ptr<Clip>& clip, const QPointF& pos, bool fine);
};

} // namespace sessions
