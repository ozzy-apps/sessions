#pragma once
// PianoRoll: the MIDI note editor with a velocity lane.
//  - double-click empty space adds a note (grid length), double-click a note deletes it
//  - Draw Mode: single click adds / deletes, drag paints
//  - drag notes to move, drag a note's right edge to lengthen, drag empty space to box-select
//  - Shift+click toggles selection; Alt (or Cmd) while dragging ignores the grid
//  - arrows move selected notes (Shift+Up/Down = octave, Shift+Left/Right = length)
//  - ruler: drag up/down to zoom, sideways to scroll; wheel scrolls pitch, Ctrl+wheel zooms.
//    It carries the neutral loop brace (on top: drag its body to move the loop, its
//    ends for the loop's start and length) and the ▶ start / ◀ end markers (0.23; Model.h Clip::
//    start / loopStart): drag ▶ to begin playing elsewhere, ◀ for the length.
//  - velocity lane: drag stems; selected notes move together
//  - keyboard: click a key to select all notes of that pitch
//  - scale (from the clip's Notes tab): in-scale rows are lit, the root marked;
//    Fold shows only the rows in the scale, or only rows that hold notes

#include <QWidget>
#include <QSet>
#include <QRectF>

namespace sessions {

class Project;
class Transport;
struct Clip;
struct Note;
using ClipPtr = std::shared_ptr<Clip>;

class PianoRoll : public QWidget {
    Q_OBJECT
public:
    PianoRoll(Project* project, Transport* transport, QWidget* parent = nullptr);

    void   setClip(ClipPtr clip);
    void   setDrawMode(bool on) { m_draw = on; update(); }
    double gridSize() const { return m_grid; }
    double zoom() const { return m_zoom; }   // pixels per beat
    void   setGridSize(double g);
    void   deleteSelected();
    void   duplicateSelected();
    void   selectAll();
    void   nudge(int dPitch, double dBeat, bool length);
    QList<int> selectedNotes() const;          // indices into the clip's notes, sorted
    void   setFold(bool on);
    bool   fold() const { return m_fold; }
    void   refreshRows();                       // after the clip's scale changed
    // What's on screen, in clip beats (the clip overviews outline it and drag it): first and last beat.
    bool   viewport(double& first, double& last) const;
    void   setViewport(double first, double last);
    void   setFollow(bool on);          // independent from Arrangement follow
    bool   follow() const { return m_follow; }
    void   clampView();   // the view never runs off before 1.1.1 or far past the clip's end
    static constexpr int kRulerH = 16;   // the ruler's height: the brace in its top 5 px, the markers below

signals:
    void notesChanged();
    void gridChanged(double);
    void commandRequested(const QString& command);   // the context menu: "saveClip", "cut", "copy", "paste" (0.22)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;

private:
    struct Hit { enum Area { Ruler, Keys, Grid, Velocity } area = Grid; double beat = 0; int pitch = 0; int noteIndex = -1; bool edge = false; };
    Hit    hitTest(const QPointF& p) const;
    enum class RulerHit { None, Start, LoopStart, LoopEnd, LoopBody };   // what's under a press on the ruler (the markers and the brace)
    RulerHit rulerHit(const QPointF& p) const;
    double clipExtent() const;          // beats from 1.1.1 to the end marker (or past the start marker)
    int    rows() const;                        // visible rows
    void   rebuildRows();
    int    rowOfPitch(int pitch) const;          // index into m_rowPitches, -1 if folded away
    int    pitchAtRow(int visibleRow) const;     // -1 below the last row
    double xToBeat(double x) const { return m_offset + (x - m_keyW) / m_zoom; }
    double beatToX(double b) const { return m_keyW + (b - m_offset) * m_zoom; }
    double pitchToY(int p) const;
    double snap(double b, bool fine) const;
    void   addNote(int pitch, double beat);
    void   changed() { emit notesChanged(); update(); }
    int    ownerTrack() const;          // the track playing this clip (for colour, playhead, preview)
    void   preview(int pitch, double velocity = 0.8);
    void   openEdit(const QString& label);   // undo step spanning a mouse drag
    void   closeEdit();
    void   followPlayhead();

    Project*   m_project;
    Transport* m_transport;
    ClipPtr    m_clip;
    bool       m_draw = false;
    double     m_grid = 0.25;
    double     m_zoom = 44, m_offset = 0;
    QVector<int> m_rowPitches;   // pitch of each row, top to bottom
    int        m_topRow = 0;     // first visible row
    bool       m_fold = false;
    bool       m_needCenter = false;
    void       centerOnNotes();
    void       fitToClip();
    bool       m_userZoomed = false;   // per clip: until you zoom, the loop fills the width
    bool       m_follow = true;
    const int  m_rowH = 10, m_keyW = 34, m_rulerH = kRulerH, m_velH = 44;

    QSet<int>  m_selected;    // note indices
    enum class Drag { None, Ruler, Move, Resize, Box, Velocity, Paint, Start, LoopMove, LoopStart, LoopEnd };
    Drag       m_drag = Drag::None;
    QPointF    m_pressPos;
    double     m_pressBeat = 0, m_zoom0 = 44;
    double     m_start0 = 0, m_loopStart0 = 0, m_len0 = 4;   // the markers at the press (Drag::Start / Loop*)
    int        m_pressPitch = 0, m_lastPaintPitch = -1;
    double     m_lastPaintBeat = -1;
    struct Orig { int pitch; double start, length, vel; };
    QHash<int, Orig> m_orig;
    QRectF     m_box;
    int        m_velAnchor = -1;
    bool       m_editOpen = false;
    int        m_lastPreviewPitch = -1;
};

} // namespace sessions
