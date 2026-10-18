#pragma once
// DeviceView: the selected track's device chain, left to right, with a drop
// zone at the end. Each device has a title bar (activator, name, delete) over its own
// panel (knobs, buttons and displays: ui/devices/).
//  - Drop devices from the browser, your presets, or other device panels: an orange line shows
//    where they go (kinds keep their order: MIDI effects, the instrument, audio effects). Next
//    to a rack's panel they go into the chain it shows. Near the edges the chain scrolls.
//  - Drag a device by its title to move it: along the chain, into or out of a rack, onto
//    another track (Session column, mixer, Arrangement lane) or the empty space (a new track).
//    Hold Ctrl to copy it.
//  - Click a title to select the device; Ctrl+click adds or removes one, Shift+click selects
//    the ones in between. Delete, Ctrl+D, Ctrl+C / Ctrl+V, Ctrl+G (group into a rack) and
//    Ctrl+Shift+G (ungroup) act on the selection while the Device View has focus.
//  - Double-click a title to fold the device to a slim strip (and back).
//  - Right-click a title: Group / Ungroup, Add Chain, Fold, Save as Preset…, Duplicate, Delete.
// Automated knobs follow their envelopes during playback; turning one by hand overrides its
// automation. The Sampler shows its sample; drop an audio file on it to load one (dropped
// anywhere else on a MIDI track's chain, it goes into a Sampler too).

#include "core/DeviceSpecs.h"
#include "core/Model.h"
#include <QFrame>
#include <QTimer>
#include <functional>

class QHBoxLayout;
class QScrollArea;
class QLabel;

namespace sessions {

class Project;
class Transport;
struct Device;
class DeviceView;
namespace devui { class DeviceBody; }
namespace devicedrag { struct Payload; }

// One device: its title bar over its body (see ui/devices/DeviceBody.h); a folded device is
// just the title, standing on end.
class DevicePanel : public QFrame {
    Q_OBJECT
public:
    DevicePanel(Project* project, Transport* transport, int track, const Device& d, QWidget* parent,
                std::function<void(const QString&, int)> onSample);
    devui::DeviceBody* body() const { return m_body; }   // null when folded
    int  deviceId() const { return m_id; }
    bool folded() const { return m_folded; }
    void setSelected(bool on);
    bool selected() const { return m_selected; }
    QWidget* titleBar() const { return m_title; }
    void showMenu(const QPoint& globalPos);   // the title's right-click menu

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;   // follows the body's width (the Monolith's drawers)

private:
    DeviceView* view() const;
    Project* m_project;
    int m_track, m_id;
    DeviceType m_type;
    bool m_folded = false, m_selected = false;
    QWidget* m_title = nullptr;
    devui::DeviceBody* m_body = nullptr;
    std::function<void(const QString&, int)> m_onSample;
};

// A rack: its own panel (macros, chain list) and, next to it inside the rack's frame, the
// devices of the chain selected in the list, with a drop zone for more.
class RackPanel : public QFrame {
    Q_OBJECT
public:
    RackPanel(Project* project, Transport* transport, int track, const Device& rack, QWidget* parent,
              std::function<void(const QString&, int)> onSample);
    int rackId() const { return m_id; }
    int chain() const { return m_chain; }                 // the chain shown; -1: it has none yet
    DevicePanel* ownPanel() const { return m_own; }
    QList<DevicePanel*> members() const { return m_members; }   // the shown chain's devices, in order
    QWidget* chainDrop() const { return m_drop; }         // null when the rack is folded
private:
    int m_id, m_chain = -1;
    DevicePanel* m_own = nullptr;
    QList<DevicePanel*> m_members;
    QWidget* m_drop = nullptr;
};

class DeviceView : public QWidget {
    Q_OBJECT
public:
    DeviceView(Project* project, Transport* transport, QWidget* parent = nullptr);
    void rebuild();
    QList<DevicePanel*> panels() const;   // every device panel shown, racks' chains included
    QList<QPair<int, QWidget*>> blockWidgets() const;   // the chain's blocks in order: (device id, its panel; a rack's whole frame)

    // ---- selection (device ids, in chain order) and the commands on it
    QList<int> selection() const;
    void setSelection(const QList<int>& ids);
    void clickDevice(int id, Qt::KeyboardModifiers mods);   // a title clicked
    bool hasSelection() const { return !selection().isEmpty(); }
    void deleteSelection();
    void duplicateSelection();     // the copies go right after the last one selected
    void copySelection();
    void paste();                  // after the selection, else where the devices' kinds go
    bool canPaste() const;
    int  groupSelection();         // into a new rack; its id, or 0
    bool ungroupSelection();       // the selected rack (one chain at most)
    bool savePreset(const QList<int>& ids, const QString& name, QString* error = nullptr);   // into the User Library
    void askToSavePreset(const QList<int>& ids);   // asks for a name, then savePreset

    // ---- drag and drop (public for the tests)
    void dragDevice(int id);   // starts a drag of the selection (or this one) from its title; returns when it's dropped
    // Where a device dropped at `pos` (this widget's coordinates) goes, and where the line shows
    // (in the host's coordinates).
    Project::DevicePlace placeAt(const QPoint& pos, QRect* line = nullptr) const;

signals:
    void sampleDropped(const QString& path, int deviceId);   // deviceId -1: the track's Sampler, or a new one

protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void mousePressEvent(QMouseEvent*) override;   // on the empty space: nothing selected

private:
    void followAutomation();
    void tickPanels();
    void showSelection();
    void hideLine();
    int  trackIndex() const;

    Project*     m_project;
    Transport*   m_transport;
    QScrollArea* m_scroll;
    QWidget*     m_host;
    QHBoxLayout* m_chain;
    QLabel*      m_dropZone = nullptr;
    QWidget*     m_line = nullptr;    // where a dragged device would go
    QTimer       m_meters;            // device panels' meters and animations
    QTimer       m_autoScroll;        // a drag near either edge scrolls the chain
    int          m_scrollStep = 0;
    int          m_trackId = 0;       // whose chain is shown (the selection belongs to it)
    QList<int>   m_selected;
    int          m_anchor = 0;        // Shift+click selects from here
    bool         m_panelDrag = false; // a title is being dragged: rebuilds wait until it's dropped
    bool         m_rebuildPending = false;
};

} // namespace sessions
