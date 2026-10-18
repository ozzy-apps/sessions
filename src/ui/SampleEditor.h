#pragma once
// SampleEditor: the Clip View's editor for audio clips, after the classic sample display. From the
// top: a beat ruler, the loop brace, the start / end markers, a strip for the warp markers and
// the transients, then the waveform, drawn through the clip's warp map so it lines up with the
// beats it plays on.
//  - the ruler: drag up / down to zoom (Preferences picks which way zooms in), sideways to
//    scroll. The wheel scrolls, Ctrl+wheel zooms around the pointer. The view stays between
//    1.1.1 (or the audio before it) and the audio's end.
//  - the loop brace (the clip's colour): the loop, from Clip::loopStart for the length (0.23,
//    the classic way). Drag its body to move the loop along the clip (its length stays), its left end
//    for the loop's start (the end stays), its right end for the length. Faded with Loop off.
//  - the start marker (▶ at Clip::start) and the end marker (◀ at the brace's end): drag ▶ to
//    begin playing elsewhere (before the loop the clip runs into it; with Loop off it plays from
//    ▶ to ◀ once), drag ◀ for the length. The warp map stays put: Set 1.1.1 Here is the command
//    that moves the grid on the audio.
//  - the marker strip: warp markers (yellow) pin a moment of the audio to a beat; the
//    transients Sessions found show as grey ticks. Drag a marker (or a transient: it becomes
//    a marker) sideways and the audio on either side stretches to follow it; double-click
//    the strip or the waveform to add a marker there (on the grid; Alt: free); double-click
//    a marker, or select it and press Delete, to remove it (two always stay).
//  - the waveform: drag across it to select a stretch (0.22; snapped, Alt: free; Shift+click
//    extends): Delete cuts it out of the audio (what follows moves earlier), Crop keeps only it,
//    Ctrl+L makes it the loop. A click plays from there (a clip on the timeline moves the
//    playhead, a Session clip is launched and set there). Shift+drag slides the audio under
//    the grid (every marker moves, snapped; Alt: free); the middle button drags the view.
//    A Session slot being recorded shows its waveform as it comes in.
//  - right-click anywhere for the selection commands, the warp commands (Set 1.1.1 Here,
//    Warp From Here, …), Set End Here, Reverse, Save Clip to User Library, Show in File Manager.
// Everything is undoable; a drag is one step. Delete and Crop write a new file (core/SampleEdit).

#include <QWidget>
#include <memory>

namespace sessions {

class Project;
class Transport;
struct Clip;
using ClipPtr = std::shared_ptr<Clip>;

class SampleEditor : public QWidget {
    Q_OBJECT
public:
    SampleEditor(Project* project, Transport* transport, QWidget* parent = nullptr);
    void   setClip(ClipPtr clip);
    ClipPtr clip() const { return m_clip; }
    double beatToX(double beat) const { return kLeft + (beat - m_offset) * m_zoom; }
    double xToBeat(double x) const { return (x - kLeft) / m_zoom + m_offset; }
    double pixelsPerBeatForTests() const { return m_zoom; }
    int    markerAt(const QPoint& p) const;      // -1 if none
    int    transientAt(const QPoint& p) const;   // index into the sample's transients under the strip, -1 if none
    int    selectedMarker() const { return m_sel; }
    bool   viewport(double& first, double& last) const;   // the beats on screen
    void   setViewport(double first, double last);
    void   extent(double& first, double& last) const;     // what there is to see: from 1.1.1 (or the audio before it) to the audio's end
    // The time selection (clip beats), if any.
    bool   hasSelection() const { return m_rangeOn && m_rangeB - m_rangeA > 1e-9; }
    bool   selection(double& start, double& end) const;
    void   select(double start, double end);
    void   selectAll();                              // the whole clip (the start marker or the loop, whichever is first, to the end marker)
    // Edits (each one undo step). deleteSelection: the selected stretch is cut out of the audio
    // (a new file), else the selected warp marker goes. cropToSelection keeps only the stretch.
    // loopSelection makes the stretch the clip: it starts at 1.1.1 and is the length.
    void   deleteSelection();
    void   cropToSelection();
    void   loopSelection();
    // The warp commands, at clip beat `beat`; each one undo step.
    void   setStartHere(double beat);                 // the audio under `beat` becomes 1.1.1
    void   setEndHere(double beat);                   // the clip's length
    void   warpFromHere(double beat, double bpm, bool startHere);   // from `beat` on, a straight tempo; startHere: and that's 1.1.1
    void   addMarkerAt(double beat);
    static constexpr int kRulerH = 16, kBraceY = kRulerH, kBraceH = 8, kMarkY = kBraceY + kBraceH, kMarkH = 10;
    static constexpr int kStripY = kMarkY + kMarkH, kStripH = 12, kLeft = 8, kTop = kStripY + kStripH;

signals:
    void selectionChanged();
    // The context menu asked for a window-wide command (0.22): "saveClip", "showSample".
    void commandRequested(const QString& command);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void   fit();                      // the whole sample (and the loop) fills the width
    void   clampView();                // the view stays within extent(), zoomed in no further than 1/64 beat per 8 px
    double snap(double beat, bool free) const;
    void   removeMarker(int i);
    int    insertMarker(double beat, double seconds);   // into the sorted list; its index
    double playheadBeat() const;       // clip beat under the playhead, or -1
    QString editFolder() const;

    Project*   m_project;
    Transport* m_transport;
    ClipPtr    m_clip;
    double     m_zoom = 40, m_offset = 0;   // px per beat, first beat shown
    bool       m_userZoomed = false;        // until you zoom, the sample fills the width (and refits on resize)
    int        m_sel = -1;
    int        m_hoverTransient = -1;
    enum class Drag { None, Marker, LoopEnd, LoopStart, Start, LoopMove, Ruler, Scroll, Slide, Select } m_drag = Drag::None;
    QPointF    m_pressPos;
    double     m_pressBeat = 0, m_zoom0 = 40, m_offset0 = 0, m_len0 = 0, m_start0 = 0, m_loopStart0 = 0;
    QVector<double> m_slide0;          // Drag::Slide: the markers' beats at the press
    bool       m_moved = false;        // Drag::Scroll / Select: a press that didn't move is a click: play from there
    bool       m_editOpen = false;
    bool       m_rangeOn = false;      // the time selection
    double     m_rangeA = 0, m_rangeB = 0, m_rangeAnchor = 0;
};

} // namespace sessions
