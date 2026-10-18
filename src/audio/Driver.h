#pragma once
// Driver: pulls audio from the engine on its own thread and hands it to the
// system. PulseDriver talks to PulseAudio (or PipeWire's Pulse server, the
// default on Ubuntu / Zorin); JackDriver talks to PipeWire's (or JACK's) JACK
// interface: duplex, small buffers and the interface's own channels, for playing
// live through the app; NullDriver keeps real time without a device.

#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>

namespace sessions::audio {

using RenderFn = std::function<void(float* interleavedStereo, int frames)>;
// Duplex drivers hand the input of the same period along: both interleaved stereo.
using DuplexFn = std::function<void(const float* in, float* out, int frames)>;

struct DriverOptions {
    QString device;         // a PulseAudio sink name; empty = the system default
    int     bufferMs = 30;  // how much audio sits in the server's buffer (latency vs. safety)
    // "pulse" (the desktop's sound server) or "jack" (PipeWire's JACK interface: low latency,
    // the interface's own inputs and outputs)
    QString system = "pulse";
    int     frames = 128;        // jack: the buffer size (the period)
    QStringList outPorts;        // jack: where Main L and R go (port names; empty = the first physical outputs)
    QStringList inPorts;         // jack: what Input 1 and 2 read (empty = the first physical inputs)
};

struct OutputDevice { QString name, description; };
// The outputs the sound server offers (empty without a server, or built without PulseAudio).
QVector<OutputDevice> listOutputDevices();
// Its inputs (microphones, line-ins, audio interfaces), without the monitors of outputs.
QVector<OutputDevice> listInputDevices();

class Driver {
public:
    virtual ~Driver() = default;
    virtual bool    open(int sampleRate, const DriverOptions& options, QString* error) = 0;
    virtual void    start(RenderFn render) = 0;   // spawns the audio thread
    virtual void    stop() = 0;                   // joins it
    virtual double  latency() const { return 0; } // seconds from render to speaker
    virtual QString name() const = 0;
    // Duplex drivers (JACK): input and output in one callback, so monitoring adds no extra delay.
    virtual bool    duplex() const { return false; }
    virtual void    startDuplex(DuplexFn) {}
    virtual double  inputLatency() const { return 0; }   // seconds from the jack to the callback
    virtual QString problem() const { return {}; }      // something the user should fix (a dead input), else empty
};

std::unique_ptr<Driver> makePulseDriver();   // nullptr when built without PulseAudio
std::unique_ptr<Driver> makeJackDriver();    // nullptr when no libjack (PipeWire's or JACK's) is installed
std::unique_ptr<Driver> makeNullDriver();

// JACK ports (PipeWire names them "<device>:<channel>"): the physical ones to record from
// (capture) or to play to. Empty without a JACK server or library. `rate` gets the server's rate.
QStringList listJackPorts(bool capture, int* rate = nullptr);
bool        jackAvailable();   // a libjack was found

// Capture: reads the sound server's input on its own thread and hands stereo blocks to
// `push` (interleaved floats). Opened only while something needs the input (an armed or
// monitoring audio track), so the desktop's "microphone in use" light stays honest.
using PushFn = std::function<void(const float* interleavedStereo, int frames)>;
class InputDriver {
public:
    virtual ~InputDriver() = default;
    virtual bool    open(int sampleRate, const QString& device, QString* error) = 0;   // device "" = default
    virtual void    start(PushFn push) = 0;
    virtual void    stop() = 0;
    virtual double  latency() const { return 0; }   // seconds from the jack to `push`
    virtual QString name() const = 0;
};
std::unique_ptr<InputDriver> makePulseInput();   // nullptr when built without PulseAudio

} // namespace sessions::audio
