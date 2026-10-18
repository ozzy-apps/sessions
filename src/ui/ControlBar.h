#pragma once
// ControlBar, laid out: Follow, tap tempo, tempo, time signature,
// metronome, launch quantization | position, transport, arrangement record, loop start,
// loop switch, loop length, draw mode, computer MIDI keyboard, Re-Enable Automation |
// CPU / audio status, view switches.

#include <QWidget>
#include <QElapsedTimer>
#include <QVector>

class QToolButton;
class QComboBox;
class QLabel;
class QWidget;

namespace sessions {

class Project;
class Transport;
class DragValue;

class ControlBar : public QWidget {
    Q_OBJECT
public:
    ControlBar(Project* project, Transport* transport, QWidget* parent = nullptr);

    bool drawMode() const;
    void setSessionViewActive(bool session);
    bool midiKeyboard() const;
    void setMidiKeyboard(bool on);
    void setMidiKeyboardInfo(const QString& text);   // octave / velocity, shown on hover

signals:
    void viewRequested(bool session);     // true = Session, false = Arrangement
    void drawModeChanged(bool on);
    void midiKeyboardToggled(bool on);
    void followToggled(bool on);          // the Arrangement scrolls with the playhead
    void keyMapToggled(bool on);          // KEY: Key Map mode
    void midiMapToggled(bool on);         // MIDI: MIDI Map mode

public slots:
    void toggleDrawMode();
    void setMapButtons(bool keyMap, bool midiMap);   // show the map mode that's on
    void flashMidiIn();                              // the MIDI light: a message came in

private:
    void refresh();

    Project*     m_project;
    Transport*   m_transport;
    DragValue*   m_tempo;
    QLabel*      m_position;
    QToolButton* m_play;
    QToolButton* m_stop;
    QToolButton* m_rec;
    void refreshIcons();   // the SVG icons (assets/controlbar), re-set when the theme changes
    QToolButton* m_loop;
    QToolButton* m_punchIn;
    QToolButton* m_punchOut;
    DragValue*   m_loopStart;
    DragValue*   m_loopLength;
    QToolButton* m_follow;
    QToolButton* m_keyMap;
    QToolButton* m_midiMap;
    QWidget*     m_midiLight;   // the MIDI-in lamp (MidiLight in ControlBar.cpp)
    qint64       m_midiLitUntil = 0;
    QToolButton* m_metro;
    QToolButton* m_draw;
    QToolButton* m_keys;
    QToolButton* m_reEnable;
    QToolButton* m_session;
    QToolButton* m_arrange;
    QComboBox*   m_quant;
    QLabel*      m_cpu;
    int          m_cpuTicks = 0;
    QVector<qint64> m_taps;
    QElapsedTimer m_tapClock;
};

} // namespace sessions
