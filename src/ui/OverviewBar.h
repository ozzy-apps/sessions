#pragma once
// OverviewBar: the strip along the bottom of the window, a status bar.
//  - On the left, what's selected (the track and the clip) in a line of text.
//  - The clip overview: the selected clip in miniature (its notes, or its waveform), with
//    the playhead running through it while it plays and an outline around the part the
//    Clip View's editor shows. Drag it sideways to scroll the editor, up or down to zoom it
//    (Preferences picks which way zooms in); click it for the Clip View. The same widget sits
//    over the Clip View (MainWindow's detail header), where a click centres the editor there.
//  - The device chain overview: the selected track's devices in miniature, in chain order
//    (each one a scaled-down picture of its panel in the Device View; a rack is one picture),
//    dimmed when switched off. Drag it sideways to scroll along a long chain; click a device
//    for the Device View with it selected.
// Both repaint on their own while the window is shown (a slow timer) and whenever the
// project changes.

#include "core/Model.h"
#include <QWidget>
#include <QTimer>
#include <QPixmap>
#include <QHash>
#include <functional>

class QLabel;

namespace sessions {

class Project;
class Transport;

class ClipOverview : public QWidget {
    Q_OBJECT
public:
    ClipOverview(Project* project, Transport* transport, QWidget* parent);
    // The editor's viewport (first and last clip beat on screen) to outline and drag; false when nothing's shown.
    void setViewport(std::function<bool(double&, double&)> get, std::function<void(double, double)> set);
    double beatAt(double x) const;   // the clip beat under x
signals:
    void clicked();                  // a click that didn't drag
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    double spanBeats() const;        // what the strip spans: the clip's length (at least a beat)
    Project*   m_project;
    Transport* m_transport;
    std::function<bool(double&, double&)> m_getView;
    std::function<void(double, double)>   m_setView;
    QPointF m_pressPos;
    double  m_first0 = 0, m_last0 = 0, m_pressBeat = 0;
    bool    m_dragging = false, m_moved = false;
};

class ChainOverview : public QWidget {
    Q_OBJECT
public:
    ChainOverview(Project* project, QWidget* parent);
    int deviceAt(const QPoint& pos) const;   // the block under pos: its device id, or 0
    QList<QPair<QRect, int>> blocks() const; // each block's rect (as drawn, scrolled) and device id (for tests)
    void setSelectionProvider(std::function<QList<int>()> fn) { m_selected = std::move(fn); }   // the Device View's selection, outlined
    // The Device View's panels as pictures (device id, the panel grabbed): the strip shows them scaled down.
    void setThumbProvider(std::function<QList<QPair<int, QPixmap>>()> fn) { m_thumbs = std::move(fn); }
    void refreshThumbs();                    // grabs the panels again (the bar's timer does it now and then)
    bool hasThumbs() const { return !m_pictures.isEmpty(); }
    int  scrollOffset() const { return m_scroll; }
signals:
    void clicked(int deviceId);              // 0: the empty part of the strip
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;            // tooltips: the device's name
private:
    int  contentWidth() const;
    Project* m_project;
    std::function<QList<int>()> m_selected;
    std::function<QList<QPair<int, QPixmap>>()> m_thumbs;
    QList<QPair<int, QPixmap>> m_pictures;   // in chain order, scaled to the strip
    int     m_scroll = 0, m_scroll0 = 0;
    QPointF m_pressPos;
    bool    m_dragging = false, m_moved = false;
};

class OverviewBar : public QWidget {
    Q_OBJECT
public:
    OverviewBar(Project* project, Transport* transport, QWidget* parent = nullptr);
    ClipOverview*  clipOverview() const { return m_clip; }
    ChainOverview* chainOverview() const { return m_chain; }
    void refresh();   // the status text and a repaint
    static constexpr int kBarHeight = 40;
private:
    Project*       m_project;
    QLabel*        m_status;
    ClipOverview*  m_clip;
    ChainOverview* m_chain;
    QTimer         m_timer;
    QString        m_key;   // the selection the status text shows
    int            m_ticks = 0;
};

} // namespace sessions
