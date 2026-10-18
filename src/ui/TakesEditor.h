#pragma once
// TakesEditor: the Clip View's Takes view for a loop-recorded audio clip (after FORM and
// the classic take lanes). A beat ruler on top, then the comp lane (what the clip plays: each
// stretch drawn from the take it uses, in that take's colour) and one lane per take, the
// newest first, each showing its own audio, bright where the comp uses it.
//  - drag across a take lane: that stretch of the clip plays this take (snapped to the grid;
//    Alt: free). Short crossfades hide the joins.
//  - click a lane: one grid step from this take
//  - double-click a lane (or right-click → Use Whole Take): the whole clip from this take
//  - right-click → Newest Take Everywhere: clears the comp
//  - wheel: scrolls the lanes when they don't all fit
// Nothing is destroyed: the takes' files stay as recorded; the comp only says which plays where.

#include <QWidget>
#include <memory>

namespace sessions {

class Project;
class Transport;
struct Clip;
using ClipPtr = std::shared_ptr<Clip>;

class TakesEditor : public QWidget {
    Q_OBJECT
public:
    TakesEditor(Project* project, Transport* transport, QWidget* parent = nullptr);
    void    setClip(ClipPtr clip);
    ClipPtr clip() const { return m_clip; }
    double  beatToX(double beat) const { return kLeft + (beat - m_offset) * m_zoom; }
    double  xToBeat(double x) const { return (x - kLeft) / m_zoom + m_offset; }
    int     laneCount() const;                 // the comp lane and the takes
    QRect   laneRect(int lane) const;          // lane 0 = comp, then the takes newest first; empty when scrolled out
    int     laneAt(const QPoint& p) const;     // -1 none
    int     takeOfLane(int lane) const;        // the take a lane shows, -1 for the comp lane
    int     laneOfTake(int take) const;
    static QColor takeColor(const QColor& track, int take);   // each take in its own shade of the track colour
    static constexpr int kRulerH = 18, kLeft = 72, kLaneH = 44, kCompH = 56;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void   fit();                              // the whole clip fills the width
    double grid() const;
    double snap(double beat) const;
    void   useTake(double start, double end, int take, const QString& label);
    void   drawTake(QPainter& p, int take, const QRect& lane, double fromBeat, double toBeat, const QColor& color, bool bright) const;
    double playheadBeat() const;               // clip beat under the playhead, or -1

    Project*   m_project;
    Transport* m_transport;
    ClipPtr    m_clip;
    double     m_zoom = 40, m_offset = 0;      // px per beat, first beat shown
    int        m_firstLane = 0;                // scroll: the first take lane shown (the comp lane always shows)
    int        m_dragTake = -1;                // a drag in progress on this take's lane
    double     m_dragA = 0, m_dragB = 0;
    bool       m_free = false;
};

} // namespace sessions
