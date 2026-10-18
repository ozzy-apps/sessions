#pragma once
// EnvelopeEditor: the Clip View's editor for clip envelopes (the Envelopes box's "Show").
// One parameter at a time across the clip's length, in clip beats, so it loops with the
// clip. While the clip plays, its envelope sets the parameter (it overrides the track's
// own value and its Arrangement automation).
//  - click: a new breakpoint there (on the grid; Alt: free), and drag it
//  - drag a breakpoint: move it (it can't pass its neighbours)
//  - double-click a breakpoint, or select it and press Delete: removes it
// Everything is undoable; a drag is one step.

#include <QWidget>
#include <memory>

namespace sessions {

class Project;
class Transport;
struct Clip;
struct Envelope;
using ClipPtr = std::shared_ptr<Clip>;

class EnvelopeEditor : public QWidget {
    Q_OBJECT
public:
    EnvelopeEditor(Project* project, Transport* transport, QWidget* parent = nullptr);
    void    setClip(ClipPtr clip);
    void    setTarget(const QString& target);
    QString target() const { return m_target; }
    double  beatToX(double beat) const { return kLeft + beat * m_zoom; }
    double  xToBeat(double x) const { return (x - kLeft) / m_zoom; }
    double  valueToY(double v) const;
    double  yToValue(double y) const;
    int     pointAt(const QPointF& p) const;   // -1 if none
    int     selectedPoint() const { return m_sel; }
    static constexpr int kRulerH = 18, kLeft = 8;

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    void   fit();
    double snap(double beat, bool free) const;
    double grid() const;
    void   removePoint(int i);
    const Envelope* envelope() const;
    double playheadBeat() const;   // clip beat under the playhead, or -1

    Project*   m_project;
    Transport* m_transport;
    ClipPtr    m_clip;
    QString    m_target;
    double     m_zoom = 40;   // px per beat
    int        m_sel = -1;
    bool       m_dragging = false;
    bool       m_editOpen = false;
};

} // namespace sessions
