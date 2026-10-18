#pragma once
// SessionView: the clip grid (custom painted) on top, mixer strips below, with a divider
// you can drag between them (remembered in QSettings "session/gridHeight"). Clip slots keep
// one size at any window size (columns 110 px unless resized, rows 20 px); the grid fills its
// height with empty cells, scrolls its scenes when they don't fit, and a bigger window gives
// the mixer the extra height. The bottom row of each track: ■ stops it; a playing clip
// shows its pass number, a clock that fills once per loop, and the loop's length in beats.
// Columns are tracks, then return tracks, then the Main / scene column; rows are scenes.
//
// ClipGrid interactions:
//  - ▶ launches (mouse up matters for Gate / Repeat clips); ■ in an empty slot stops the track
//  - ● in an empty slot of an armed track records a new clip from the computer keyboard
//  - drag a clip to another slot to move it, hold Ctrl to copy; press Tab while dragging and the
//    Arrangement takes the clip (MainWindow hands it over): drop it on a lane there (Tab again: back here)
//  - drag a track title sideways to reorder tracks; drag a scene (not its ▶) up or down to move it
//  - devices (browser items, presets, device panels dragged by their title) dropped on a
//    track's column or its mixer strip go on that track; on the free space, onto a new track
//  - drag the divider at a column's right edge to resize it; double-click it for the default width
//  - the returns and the Main column stay at the right edge; the space between them and the
//    tracks takes dropped samples (a new audio track)
//  - right-click a title, scene or clip for its menu; Ctrl+R renames the selection
//  - double-click an empty slot: new clip; a title: Device View; a scene name: rename

#include <QWidget>
#include <QVector>
#include <memory>

class QSplitter;
class QMimeData;

namespace sessions {

class Project;
class Transport;
class MixerStrip;
class MainStrip;
struct Clip;
struct Track;

class ClipGrid : public QWidget {
    Q_OBJECT
public:
    enum class Sel { Slot, Track, Scene };

    ClipGrid(Project* project, Transport* transport, QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    static constexpr int kDefaultColumnWidth = 110, kMinColumnWidth = 72, kMaxColumnWidth = 480;
    int  columnWidth(int col) const;    // tracks and returns (combined index); the Main column after them
    int  columnLeft(int col) const;     // x of a column: returns and Main sit at the right edge
    int  contentWidth() const;          // every column, without the free space before the returns
    int  freeSpace() const;             // the gap between the last track and the returns
    void setRowHeight(int h) { m_rowH = h; updateGeometry(); update(); }
    int  rowHeight() const { return m_rowH; }
    static constexpr int kRowHeight = 20;   // clip slots keep this height at any window size
    int  fixedHeight(int rowH) const;   // the height that shows every scene
    int  visibleRows() const;           // scene rows that fit between the header and the stop row
    int  firstVisibleRow() const { return m_firstRow; }
    void scrollToRow(int row);          // the first scene shown (clamped)
    void ensureVisible(int row);
    static constexpr int kHeaderRow = -1, kStopRow = -2;   // slotRect rows besides the scenes
    // The status shown under a track: its playing clip's pass (1, 2, … for a looping clip, 0
    // otherwise), how far round the clock is (0..1) and the loop's length in beats.
    struct TrackStatus { bool playing = false; int pass = 0; double fraction = 0, length = 0; };
    TrackStatus trackStatus(int track) const;
    double loopFraction(const Track& t, const Clip& clip) const;   // 0 .. 1 through the loop, from the beat you hear
    // MIDI / Key Map: the visible slots, scene launchers and stop buttons, and their targets.
    QVector<QPair<QRect, QString>> mapCells() const;
    QString mapTargetAt(const QPoint& p) const;

    Sel selectionKind() const { return m_selKind; }
    int selectedTrack() const { return m_selTrack; }
    int selectedScene() const { return m_selScene; }
    void selectTrack(int track);        // from elsewhere (Arrangement header clicks)

    // Edit commands, acting on the selection (slot, track title or scene).
    void deleteSelection();
    void duplicateSelection();
    void renameSelection();
    void insertScene();
    void captureScene();
    std::shared_ptr<Clip> selectedClip() const;
    void pasteClip(const std::shared_ptr<Clip>& clip);   // copies `clip` into the selected slot
    // Clips from the library (.sessionsclip files, 0.22) into a track's slots from `scene` down (a
    // track of another kind, or -1: a new track of the clip's kind). One undo step; the track's index.
    int  placeClipFiles(const QStringList& paths, int track, int scene);

    // A clip drag can leave the grid: Tab while dragging a clip switches to the Arrangement, which
    // takes the clip (MainWindow::showSession). takeClipDrag ends the drag here and says what was
    // dragged; resumeClipDrag (Tab again) carries on as if the mouse had never left.
    bool clipDragActive() const { return m_dragging && m_drag == Drag::Clip; }
    std::shared_ptr<Clip> takeClipDrag(int* track = nullptr, int* scene = nullptr);   // nullptr when nothing's dragged
    void resumeClipDrag(int track, int scene);

    // Device drags (DeviceDrag.h) over column `col` (trackCount(): Main, -1: the free space),
    // from the grid or the mixer strips under it: highlights where they'd go; false when they can't.
    bool deviceDragOver(const QMimeData* mime, int col);
    bool deviceDrop(const QMimeData* mime, int col, bool copy);   // false when nothing happened
    void clearDeviceDrop();
    int  columnAt(int x) const;         // -1 outside (or in the free space); trackCount() = the Main column

signals:
    void trackSelected(int track);
    void clipSelected(int track, int scene);      // a clip was clicked (open in Clip View)
    void openDeviceView(int track);
    void columnsResized();
    // The context menu asked for a window-wide command (0.22): "cut", "copy", "paste", "saveClip", "showSample".
    void commandRequested(const QString& command);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void leaveEvent(QEvent*) override;
    // Audio files (browser samples, the file manager): onto an audio track's slots, or a new audio track.
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void wheelEvent(QWheelEvent*) override;   // scrolls the scenes when they don't all fit
    void resizeEvent(QResizeEvent*) override;

private:
    // Empty: a filler cell below the last scene (track = column, scene = the row it would be)
    struct Hit { enum What { None, Header, Slot, SceneSlot, Stop, StopAll, MainHeader, ReturnBody, Empty } what = None; int track = -1; int scene = -1; bool launchButton = false; };
    int   stopTop() const;
    int   rowTop(int row) const;
    Hit   hitTest(const QPoint& p) const;
    int   dividerAt(const QPoint& p) const;   // a track / return column whose right edge is under p, or -1
    void  setColumnWidth(int col, int w);
    QRect slotRect(int col, int row) const;
    void  createClip(int track, int scene);
    void  addScenesThrough(int row);     // empty scenes until `row` exists (filler cells)
    void  select(Sel kind, int track, int scene);

    Project*   m_project;
    Transport* m_transport;
    int m_rowH = 20, m_headerH = 20, m_stopH = 18, m_mainW = 120, m_gap = 1;
    int m_firstRow = 0;   // first scene shown
    static constexpr int kSceneNumberW = 18;
    Sel m_selKind = Sel::Slot;
    int m_selTrack = 0, m_selScene = 0;
    Hit m_hover;

public:   // (declared before the drag helpers above can use it)
    enum class Drag { None, Clip, Track, Resize, Scene };
private:
    // mouse gestures
    Drag   m_drag = Drag::None;
    bool   m_dragging = false;      // moved past the drag threshold
    QPoint m_pressPos, m_mousePos;
    int    m_dragTrack = -1, m_dragScene = -1;
    int    m_resizeWidth0 = 0;           // Drag::Resize: the column's width when you grabbed its divider
    bool   m_dropHere = false;           // a file drag hovers the free space
    int    m_dropTrack = -1, m_dropScene = -1;   // a file drag hovers this audio track slot
    int    m_dropColumn = -1;            // a device drag hovers this track's column
    int    m_dropAfter = -1;             // a device drag hovers a track that can't take it: a new track right after it
    bool   m_sceneClick = false;         // pressed on a scene's name: it launches on release unless dragged
    int    rowAt(int y) const;
    int    m_launchTrack = -1, m_launchScene = -1;   // slot held down (for Gate / Repeat release)
};

class SessionView : public QWidget {
    Q_OBJECT
public:
    SessionView(Project* project, Transport* transport, QWidget* parent = nullptr);
    ClipGrid* grid() const { return m_grid; }
    void rebuildStrips();

signals:
    void trackSelected(int track);
    void clipSelected(int track, int scene);
    void openDeviceView(int track);
    void columnsResized();   // a column was dragged wider or narrower (saved with the set)

protected:
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject* obj, QEvent* ev) override;   // the strips host resized: place the strips

private:
    void layoutColumns();
    void placeStrips();   // each strip under its column (strips are positioned by hand, not by a layout)

    Project*   m_project;
    Transport* m_transport;
    ClipGrid*  m_grid;
    QSplitter* m_split;
    int        m_gridHeight = 0;   // where you left the divider (0: fit the scenes)
    QWidget*   m_stripsHost;
    QVector<MixerStrip*> m_strips;
    QVector<int> m_stripIds;     // track ids the strips were built for
    MainStrip* m_main = nullptr;
};

} // namespace sessions
