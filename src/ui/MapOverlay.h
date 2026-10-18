#pragma once
// MapOverlay: what MIDI Map and Key Map modes look like. Laid over the whole window
// without taking clicks: every control you can map is tinted (blue in MIDI Map, orange in
// Key Map), the selected one is outlined, and mapped ones show what drives them ("CC 21",
// "Q"). Controls declare themselves with a "mapTarget" property (see Mapping.h); the Session
// grid's slots, scenes and stop buttons come from ClipGrid::mapCells().

#include <QWidget>
#include <QTimer>

namespace sessions {

class Project;
class Mapper;
class ClipGrid;

class MapOverlay : public QWidget {
    Q_OBJECT
public:
    MapOverlay(Project* project, Mapper* mapper, ClipGrid* grid, QWidget* parent);
    struct Target { QRect rect; QString target; };
    QVector<Target> targets() const;   // in this widget's coordinates

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override { m_refresh.start(); }
    void hideEvent(QHideEvent*) override { m_refresh.stop(); }
    bool eventFilter(QObject* obj, QEvent* ev) override;   // follow the parent's size

private:
    Project*  m_project;
    Mapper*   m_mapper;
    ClipGrid* m_grid;
    QTimer    m_refresh;   // views scroll and switch underneath
};

} // namespace sessions
