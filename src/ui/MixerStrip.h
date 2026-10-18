#pragma once
// MixerStrip: one track's channel strip, laid out Session mixer:
//  - I/O: on audio tracks, where the input comes from (inputs 1/2, 1 or 2) and the
//    Monitor switch (In / Auto / Off); other strips keep the space so everything lines up
//  - Sends: one knob per return, zig-zagging in two columns under a "Sends" label
//    (return tracks leave the space empty, so every volume section lines up)
//  - volume section: on the left the volume field, pan, the activator (track number),
//    Solo and Arm; on the right the meter with the fader marker and its dB scale.
// MainStrip: the Main column's strip. Its Sends area holds each return's Pre / Post
// switch; then the volume field, Cue volume and the main meter / fader.
//
// Every move is an undo step (drags merge into one). Automated controls follow
// their envelope during playback; moving one by hand overrides its automation
// until Re-Enable Automation.

#include <QWidget>
#include <QVector>
#include <algorithm>
#include <functional>

class QToolButton;
class QComboBox;

namespace sessions {

class Project;
class Transport;
struct Track;
class Knob;
class LevelFader;
class DragValue;

class MixerStrip : public QWidget {
    Q_OBJECT
public:
    MixerStrip(Project* project, Transport* transport, int trackIndex, QWidget* parent = nullptr);
    void syncFromModel();
    int trackIndex() const { return m_index; }
    static int sendsHeight(int returns);   // the Sends area, the same in every strip
    static constexpr int kIoHeight = 40;   // the I/O area at the top of every strip

protected:
    void paintEvent(QPaintEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;   // the track's menu (0.22)

private:
    Track* track() const;
    void   followAutomation();

    Project*   m_project;
    Transport* m_transport;
    int        m_index;
    bool       m_isReturn;
    QVector<Knob*> m_sends;
    Knob*      m_pan;
    LevelFader* m_level;
    DragValue* m_volText;
    QToolButton* m_act;
    QToolButton* m_solo = nullptr;
    QToolButton* m_arm = nullptr;
    bool         m_wasDriven = false;     // automation or a clip envelope set a control on the last tick
    QComboBox*   m_input = nullptr;        // audio tracks
    QToolButton* m_monitor[3] = {};       // In, Auto, Off
};

class MainStrip : public QWidget {
    Q_OBJECT
public:
    MainStrip(Project* project, Transport* transport, QWidget* parent = nullptr);
    void syncFromModel();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    Project* m_project;
    QVector<QToolButton*> m_prePost;   // one per return
    Knob*    m_cue;
    LevelFader* m_level;
    DragValue* m_volText;
};

} // namespace sessions
