#pragma once
// ApcKey25: the Akai APC Key 25 (2014) and APC Key 25 mk2 as a clip launcher, laid out the
// way they work with Ableton Live. A box of 8 tracks × 5 scenes on the Session View
// (drawn there) is what the pads play:
//
//  - Pads: a clip launches (honouring its launch mode; letting go releases a Gate clip), an
//    empty pad stops its track, or records into it when the track is armed. A group's pad
//    launches its tracks' clips in that scene. Shift + pad shows the clip without launching it.
//  - Scene buttons launch the box's scenes; Stop All Clips stops every clip.
//  - Track buttons (under the pads) do what's printed under the scene buttons: Clip Stop (to
//    begin with), Solo, Rec Arm, Mute (lit: the track plays) or Select. Hold Shift and press a
//    scene button to choose.
//  - Shift + track buttons 1–4 move the box up / down / left / right; 5–8 choose what the knobs
//    turn: Volume, Pan, Send (press again for the next send) or Device: Macro 1–8 of the
//    selected device (Osmium's macros, a rack's), else its first eight parameters.
//  - Play starts and stops; Rec switches Arrangement Record.
//  - Knobs: the mk2's are endless (relative, two's complement; Shift turns finely), the 2014
//    model's absolute, taking over at the current value when Pickup is on.
//
// Lights: pads show the clips (the mk2 in their colours, the 2014 model amber), green when
// playing (the mk2 pulses with the beat), blinking green while queued, red while recording;
// an armed track's empty pads glow dim red (mk2). Track and scene buttons light for what they
// would change; while Shift is held they show the box's directions and the modes.
//
// Ports: the mk2 has two, the keys (with sustain) on the first and everything else on the
// second, so its keys play as before and nothing from the second plays notes. The 2014 model
// has one: channel 1's pad and button notes are taken, other channels play. Messages go out
// through `sender` to the same port. User MIDI maps get first refusal (MainWindow).

#include <QObject>
#include <QHash>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <functional>

namespace sessions {
class Project;
class Transport;
class Mapper;

class ApcKey25 : public QObject {
    Q_OBJECT
public:
    enum class Model { None, Classic, Mk2 };
    enum class Role { None, Keys, Controls };
    enum class Knobs { Volume, Pan, Send, Device };
    enum class Buttons { ClipStop, Solo, Arm, Mute, Select };
    struct Port { QString id, name; };   // id "client:port"
    static constexpr int kColumns = 8, kRows = 5;
    // Notes on channel 1 of the controls port.
    static constexpr int kTrackButton = 64, kStopAll = 81, kSceneButton = 82, kPlay = 91, kRecord = 93, kShift = 98, kKnob = 48;

    ApcKey25(Project* project, Transport* transport, Mapper* mapper, QObject* parent = nullptr);
    ~ApcKey25() override;

    static Model modelOf(const QString& portName);
    void setEnabled(bool on);                        // the control surface profile isn't Off
    bool enabled() const { return m_enabled; }
    void setPorts(const QVector<Port>& ports);       // the MIDI inputs (ports change): finds the APC
    void setSender(std::function<void(const QString& port, const QByteArray& message)> sender) { m_send = std::move(sender); }
    void setDeviceChoice(std::function<int()> choice) { m_deviceChoice = std::move(choice); }   // the Device View's selected device id
    Role role(const QString& source) const;
    Model model() const { return m_model; }
    bool connected() const { return m_enabled && !m_controls.isEmpty(); }
    QString controlsPort() const { return m_controls; }

    // From MIDI input: true when the message was the APC's (it doesn't play notes then).
    bool takesNote(const QString& source, int channel, int pitch) const;   // a pad or button (whatever the map mode)
    bool note(const QString& source, int channel, int pitch, int velocity, bool on);
    bool controlChange(const QString& source, int channel, int cc, int value);
    bool isRelativeKnob(const QString& source, int channel, int cc) const;   // the mk2's knobs, for MIDI Map

    // The box: the first track (an index into visibleTracks()) and the first scene.
    int  boxTrack() const { return m_x; }
    int  boxScene() const { return m_y; }
    int  firstScene() const;                         // the box's first scene, kept within the set's scenes
    void moveBox(int tracks, int scenes);
    QVector<int> visibleTracks() const;              // regular and group tracks shown in the Session (folds respected)
    QVector<int> boxTracks() const;                  // track indexes under the 8 columns (-1: none)
    Knobs   knobs() const { return m_knobs; }
    Buttons buttons() const { return m_buttons; }
    int     sendIndex() const { return m_sendIndex; }
    bool    shift() const { return m_shift; }
    QVector<QString> knobTargets() const;            // what the 8 knobs turn now (Mapper targets, "" for none)

    // What the lights show now: note -> (channel, velocity) on the controls port.
    QHash<int, QPair<int, int>> lights() const;
    void refreshLights(bool all = false);            // sends what changed (all: everything)
    void darken();                                   // every light off (leaving, profile Off)

signals:
    void boxChanged();
    void clipChosen(int track, int scene);           // Shift + pad: show it
    void trackChosen(int track);                     // Select mode
    void sceneLaunched(int scene);
    void message(const QString& title, const QString& text);   // a mode changed (the info line)

private:
    void pad(int column, int row, bool on);
    void trackButton(int i, bool on);
    void sceneButton(int i);
    void knob(int k, int value, bool relative);
    double phase() const;                            // beats: the transport's, else a 120 BPM clock
    int  padColour(int track, int scene, int* channel) const;
    void send(int status, int d1, int d2);
    double valueOf(const QString& target) const;   // a knob target's value now, 0..1
    bool canMove(int tracks, int scenes) const;

    Project*   m_project;
    Transport* m_transport;
    Mapper*    m_mapper;
    bool       m_enabled = true, m_shift = false;
    Model      m_model = Model::None;
    QString    m_controls;          // the controls port's name ("" = no APC)
    QSet<QString> m_keys;           // the mk2's keys ports
    int        m_x = 0, m_y = 0;
    Knobs      m_knobs = Knobs::Device;
    Buttons    m_buttons = Buttons::ClipStop;
    int        m_sendIndex = 0;     // Send mode: which send (A = 0)
    QHash<int, int> m_held;         // pad note -> (track << 12 | scene) it launched, for its release
    struct Pickup { int previous = -1; bool caught = false; double applied = -1; };
    QHash<QString, Pickup> m_pickup;
    QHash<int, QPair<int, int>> m_lit;   // what the device shows (as last sent)
    QTimer     m_timer;
    qint64     m_started = 0;
    std::function<void(const QString&, const QByteArray&)> m_send;
    std::function<int()> m_deviceChoice;
};
}
