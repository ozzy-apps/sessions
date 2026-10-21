#pragma once
// Sessions — MIDI Map and Key Map: controls you play from a MIDI controller or the
// computer keyboard. A mapping ties a source (a controller's CC, a note, a key) to a target:
//
//   track:<trackId>:volume | :pan | :send:<k> | :active | :solo | :arm
//   device:<trackId>:<deviceId>:<param> | device:<trackId>:<deviceId>:on
//   main:volume | main:cue | tempo
//   slot:<trackId>:<scene> | scene:<n> | stop:<trackId> | stopall
//   transport:play | transport:stop | transport:record | transport:loop | transport:metronome
//
// Targets name tracks and devices by id, so mappings survive moving things around and are
// saved with the set. Continuous targets follow a CC's value; buttons, launches and stops
// fire on a note-on, or when a CC rises past 64 (a note-off releases a Gate clip).

#include <QObject>
#include <QHash>
#include <QString>
#include <QVector>

namespace sessions {

class Project;
class Transport;

struct Mapping {
    enum Source { MidiCC, MidiNote, Key };
    Source  source = MidiCC;
    int     channel = 0;   // MIDI channel 0..15 (unused for keys)
    int     number = 0;    // CC or note number, or a Qt::Key
    QString target;
    // A relative (endless) knob: values 1 .. 63 turn up, 127 .. 65 down (two's complement), and
    // move the value by 1/127 a step instead of setting it. Learned from how the knob sends.
    bool    relative = false;
    bool sameSource(const Mapping& o) const { return source == o.source && number == o.number && (source == Key || channel == o.channel); }
    bool operator==(const Mapping& o) const { return sameSource(o) && target == o.target; }
};

namespace mapping {
bool    isContinuous(const QString& target);   // a knob / fader (else a button or a launch)
QString sourceText(const Mapping& m);           // "CC 21", "C3", "Q" (channel added when not 1)
QString targetName(const Project& p, const QString& target);   // "Bass · Volume", "Scene 3"
}

class Mapper : public QObject {
    Q_OBJECT
public:
    Mapper(Project* project, Transport* transport, QObject* parent = nullptr);

    // Incoming messages: true when a mapping (or learning) took them, so they don't also play notes.
    // `relative`: the controller is known to send this CC from an endless knob (APC Key 25 mk2).
    bool controlChange(int channel, int cc, int value, bool relative = false);
    bool note(int channel, int pitch, bool on, int velocity);
    bool key(int qtKey, bool press);

    // Map modes: the selected target learns the next source of that kind.
    enum class Mode { Off, Midi, Key };
    void    setMode(Mode m);
    Mode    mode() const { return m_mode; }
    void    select(const QString& target) { m_selected = target; emit changed(); }
    QString selected() const { return m_selected; }
    void    removeMappings(const QString& target);   // Delete in map mode
    QVector<Mapping> mappingsFor(const QString& target) const;

    // Sets a target the way its control would (one undo step per gesture).
    void setValue(const QString& target, double value01);
    void trigger(const QString& target, bool on);
    double value(const QString& target) const;       // a continuous target's value now, 0..1
    // What learning heard: a run of small steps (1 .. 15 up, 113 .. 127 down) that repeats or turns back.
    static bool looksRelative(const QVector<int>& values);

signals:
    void changed();   // a mapping was added or removed, the selection or the mode changed

private:
    void learn(const Mapping& m);
    int  trackIndex(int id) const;

    Project*   m_project;
    Transport* m_transport;
    Mode       m_mode = Mode::Off;
    QString    m_selected;
    QHash<int, int> m_lastCC;   // (channel << 8 | cc) -> last value, to see a button's rising edge
    int          m_learnKey = -1;   // the CC being learned (channel << 8 | cc) and the values it sent
    QString      m_learnTarget;
    QVector<int> m_learnValues;
};

} // namespace sessions
