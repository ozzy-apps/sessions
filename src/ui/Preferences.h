#pragma once
// Preferences (Edit > Preferences…, Ctrl+,). Everything is applied as you change it and kept
// in QSettings:
//  - Audio: the audio system ("audio/system": "jack" = PipeWire / JACK, low latency, your interface's
//    own channels; "pulse" = the desktop sound server). JACK: buffer ("audio/frames"), which ports
//    Main L / R play to ("audio/out1", "audio/out2") and Input 1 / 2 read ("audio/in1", "audio/in2";
//    "" = the first physical ones, "-" = none). Pulse: output device and buffer ("audio/device",
//    "audio/bufferMs"), the input device for recording ("audio/input")
//  - MIDI: which input ports play Sessions ("midi/off" lists the ones switched off)
//  - Record: the count-in before recordings ("record/countIn": 0, 1, 2 or 4 bars)
//  - Look: interface zoom ("ui/zoom", applies on restart); which way a drag zooms in the overview
//    and the rulers ("ui/zoomDragUp": true = dragging up zooms in; off, dragging down does)
//  - Startup: open the demo, an empty set, or the last set ("startup/open": demo / empty / last)

#include "audio/Driver.h"
#include <QDialog>
#include <functional>

class QComboBox;
class QLabel;
class QListWidget;
class QWidget;

namespace sessions {

namespace audio { class Engine; class MidiInput; }

class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    PreferencesDialog(audio::Engine* engine, audio::MidiInput* midi, std::function<void()> restartForZoom, QWidget* parent = nullptr);
    static int bufferMs();          // the saved buffer size (30 by default)
    static QString outputDevice();  // the saved output ("" = the system default)
    static QString inputDevice();   // the saved input ("" = the system default)
    static int     countInBars();   // bars of click before a recording ("record/countIn", 0 = none)
    static bool    zoomDragUp();    // "ui/zoomDragUp" (cached): dragging up zooms in instead of down
    static void    setZoomDragUp(bool up);
    // The zoom factor a vertical drag of `dyPixels` (down is positive) means, honouring zoomDragUp():
    // what the overview strips and the rulers multiply their pixels-per-beat by.
    static double  dragZoom(double dyPixels);
    static audio::DriverOptions savedAudio();   // everything above for Engine::setOutput

private:
    void applyAudio();
    void refreshAudioStatus();
    void fillJackPorts();
    void showSystem();
    void refreshMidiPorts();

    audio::Engine*    m_engine;
    audio::MidiInput* m_midi;
    QComboBox*   m_system;
    QWidget*     m_jackBox;
    QWidget*     m_pulseBox;
    QComboBox*   m_frames;
    QComboBox*   m_jackPorts[4];   // out L, out R, in 1, in 2
    QComboBox*   m_device;
    QComboBox*   m_inputDevice;
    QLabel*      m_inputStatus;
    QComboBox*   m_buffer;
    QLabel*      m_audioStatus;
    QListWidget* m_ports;
    QLabel*      m_midiStatus;
};

} // namespace sessions
