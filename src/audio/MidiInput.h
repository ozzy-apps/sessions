#pragma once
// MidiInput: MIDI from keyboards and controllers (and other programs) through the ALSA
// sequencer, which PipeWire serves too. Sessions opens one input port, "Sessions: MIDI In",
// and connects every readable MIDI port to it (devices plugged in later included), except
// the ones you switch off in Preferences (QSettings "midi/off", by port name).
//
// Events are read on the UI thread through a socket notifier on the sequencer and come out
// as signals. Without ALSA at build time this compiles to an input that's never available.
//
// It also sends: "Sessions: MIDI Out" delivers messages to a device's port by name, for control
// surfaces' lights (the APC Key 25's pads). The first message to a port connects MIDI Out to it:
// the kernel only opens a hardware port's output while something is connected, and drops
// messages sent to it otherwise. Nothing else is routed there.

#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <QHash>
#include <QByteArray>
#include <memory>

struct _snd_seq;

namespace sessions::audio {

class MidiInput : public QObject {
    Q_OBJECT
public:
    explicit MidiInput(QObject* parent = nullptr);
    ~MidiInput() override;

    bool    available() const {
#ifdef Q_OS_WIN
        return m_windows != nullptr;
#else
        return m_seq != nullptr;
#endif
    }
    QString status() const { return m_status; }   // why it isn't available, or the port's address

    struct Port { QString id, name; bool connected = false; };   // id "client:port"
    QVector<Port> ports() const;                  // readable MIDI ports, except ours and the system's
    bool    enabled(const QString& portName) const;
    void    setEnabled(const QString& portName, bool on);   // connects / disconnects now, remembered
    int     clientId() const;                     // our port's address, for tools and tests
    int     portId() const { return m_port; }

    // The same signals as from a device, for code that doesn't come through ALSA.
    void    inject(int status, int data1, int data2, const QString& sourceName = {});
    void    injectSysEx(const QByteArray& message, const QString& sourceName = {});
    QString sourceName() const { return m_sourceName; } // source of the signal currently being delivered

    // One MIDI message (note, CC or a complete SysEx) to the writable port with this name
    // (as ports() names it). False when it couldn't be delivered; sent() is emitted either way.
    bool    send(const QString& portName, const QByteArray& message);
    QString sendError() const { return m_sendError; }   // why the last send() failed

signals:
    void noteOn(int channel, int pitch, int velocity);   // velocity 1..127
    void noteOff(int channel, int pitch);
    void controlChange(int channel, int controller, int value);
    void pitchBend(int channel, int value);                 // 0..16383, 8192 = centre
    void channelPressure(int channel, int value);           // 0..127 (aftertouch)
    void activity();                                     // any message (the control bar's light)
    void portsChanged();                                 // a device came or went
    void machineControl(const QString& sourceName, int command);
    void sent(const QString& portName, const QByteArray& message);   // every message send() was given (tests)

private:
#ifdef Q_OS_WIN
    struct Windows;
    std::unique_ptr<Windows> m_windows;
#endif
    void readEvents();
    void connectAll();

    _snd_seq*     m_seq = nullptr;
    int           m_port = -1;
    int           m_outPort = -1;   // "MIDI Out" (when the sequencer opened for output too)
    QHash<QString, QString> m_writable;   // port name -> "client:port", ports we can send to
    QSet<QString> m_outConnected;         // "client:port" ids MIDI Out is connected to
    QString       m_sendError;
    void    findWritable();
    QString       m_status;
    QSet<QString> m_connected;   // port ids we're subscribed to
    QHash<QString, QString> m_sourceNames;
    QString m_sourceName;
};

} // namespace sessions::audio
