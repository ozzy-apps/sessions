// JackDriver: the JACK API, as PipeWire serves it (pipewire-jack) or a JACK server does, loaded
// at run time so building needs no JACK headers. Duplex: one callback gets the inputs and
// fills the outputs of the same period, so playing through the app (a guitar into the amp)
// costs two periods plus the interface's own conversion. The buffer size is asked for with
// PIPEWIRE_LATENCY (PipeWire) and jack_set_buffer_size (JACK).

#include "Driver.h"
#include <QByteArray>
#include <QFile>
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <dlfcn.h>
#include <thread>
#include <vector>

namespace sessions::audio {

namespace {

using jack_nframes_t = uint32_t;
struct jack_client_t;
struct jack_port_t;
struct jack_latency_range_t { jack_nframes_t min, max; };
using ProcessCallback = int (*)(jack_nframes_t, void*);
using ShutdownCallback = void (*)(void*);
constexpr unsigned long kIsInput = 0x1, kIsOutput = 0x2, kIsPhysical = 0x4;
constexpr int kNoStartServer = 0x01;
constexpr int kCaptureLatency = 0, kPlaybackLatency = 1;
const char* const kAudioType = "32 bit float mono audio";

struct JackApi {
    void* lib = nullptr;
    jack_client_t* (*client_open)(const char*, int, int*, ...) = nullptr;
    int (*client_close)(jack_client_t*) = nullptr;
    jack_nframes_t (*get_sample_rate)(jack_client_t*) = nullptr;
    jack_nframes_t (*get_buffer_size)(jack_client_t*) = nullptr;
    int (*set_buffer_size)(jack_client_t*, jack_nframes_t) = nullptr;
    int (*set_process_callback)(jack_client_t*, ProcessCallback, void*) = nullptr;
    void (*on_shutdown)(jack_client_t*, ShutdownCallback, void*) = nullptr;
    jack_port_t* (*port_register)(jack_client_t*, const char*, const char*, unsigned long, unsigned long) = nullptr;
    void* (*port_get_buffer)(jack_port_t*, jack_nframes_t) = nullptr;
    int (*activate)(jack_client_t*) = nullptr;
    int (*deactivate)(jack_client_t*) = nullptr;
    const char** (*get_ports)(jack_client_t*, const char*, const char*, unsigned long) = nullptr;
    void (*free_)(void*) = nullptr;
    int (*connect)(jack_client_t*, const char*, const char*) = nullptr;
    int (*disconnect)(jack_client_t*, const char*, const char*) = nullptr;
    const char* (*port_name)(const jack_port_t*) = nullptr;
    void (*port_get_latency_range)(jack_port_t*, int, jack_latency_range_t*) = nullptr;
};

// PipeWire's libjack first (it needs no JACK server), then a JACK installation's.
const JackApi* api() {
    static const JackApi* loaded = [] () -> const JackApi* {
        static JackApi a;
        const char* candidates[] = {
            "/usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/libjack.so.0", "/usr/lib/aarch64-linux-gnu/pipewire-0.3/jack/libjack.so.0",
            "/usr/lib64/pipewire-0.3/jack/libjack.so.0", "/usr/lib/pipewire-0.3/jack/libjack.so.0", "libjack.so.0",
        };
        for (const char* path : candidates) {
            if (path[0] == '/' && !QFile::exists(path)) continue;
            if ((a.lib = dlopen(path, RTLD_NOW | RTLD_LOCAL))) break;
        }
        if (!a.lib) return nullptr;
        bool ok = true;
        auto sym = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(a.lib, name));
            ok &= fn != nullptr;
        };
        sym(a.client_open, "jack_client_open");
        sym(a.client_close, "jack_client_close");
        sym(a.get_sample_rate, "jack_get_sample_rate");
        sym(a.get_buffer_size, "jack_get_buffer_size");
        sym(a.set_buffer_size, "jack_set_buffer_size");
        sym(a.set_process_callback, "jack_set_process_callback");
        sym(a.on_shutdown, "jack_on_shutdown");
        sym(a.port_register, "jack_port_register");
        sym(a.port_get_buffer, "jack_port_get_buffer");
        sym(a.activate, "jack_activate");
        sym(a.deactivate, "jack_deactivate");
        sym(a.get_ports, "jack_get_ports");
        sym(a.free_, "jack_free");
        sym(a.connect, "jack_connect");
        sym(a.disconnect, "jack_disconnect");
        sym(a.port_name, "jack_port_name");
        sym(a.port_get_latency_range, "jack_port_get_latency_range");
        return ok ? &a : nullptr;
    }();
    return loaded;
}

// The physical ports, without the monitors PipeWire adds for its outputs; analog ones first
// (a USB interface on the wrong profile shows a "Digital (IEC958)" input with nothing behind it).
QStringList physicalPorts(const JackApi* j, jack_client_t* c, bool capture) {
    QStringList analog, digital;
    const char** ports = j->get_ports(c, nullptr, kAudioType, kIsPhysical | (capture ? kIsOutput : kIsInput));
    if (!ports) return {};
    for (int i = 0; ports[i]; ++i) {
        const QString name = QString::fromUtf8(ports[i]);
        if (name.contains(":monitor_")) continue;
        (name.contains("IEC958") || name.contains("HDMI") ? digital : analog) << name;
    }
    j->free_(ports);
    return analog + digital;
}

// The device a port belongs to: "UMC202HD 192k Analog Stereo:playback_FL" -> "UMC202HD 192k".
QString deviceOf(const QString& port) {
    QString client = port.section(':', 0, 0);
    for (const char* cut : {" Analog", " Digital", " Pro", " Multichannel", " Surround", " Mono", " Stereo"}) {
        const int at = client.indexOf(QLatin1String(cut));
        if (at > 0) client = client.left(at);
    }
    return client.trimmed();
}

constexpr int kMaxFrames = 8192;

class JackDriver : public Driver {
public:
    ~JackDriver() override {
        stop();
        if (m_client) api()->client_close(m_client);
    }
    bool open(int sampleRate, const DriverOptions& options, QString* error) override {
        const JackApi* j = api();
        if (!j) { if (error) *error = "no JACK library (install pipewire-jack)"; return false; }
        m_options = options;
        const int frames = std::clamp(options.frames, 16, 4096);
        qputenv("PIPEWIRE_LATENCY", QByteArray::number(frames) + "/" + QByteArray::number(sampleRate));
        if (!qEnvironmentVariableIsSet("PIPEWIRE_PROPS")) qputenv("PIPEWIRE_PROPS", "{ application.icon-name = sessions }");
        int status = 0;
        m_client = j->client_open("Sessions", kNoStartServer, &status);
        if (!m_client) { if (error) *error = "no JACK server (PipeWire's JACK interface isn't running)"; return false; }
        m_sr = int(j->get_sample_rate(m_client));
        if (m_sr != sampleRate) {
            if (error) *error = QString("the server runs at %1 Hz; Sessions needs %2 Hz").arg(m_sr).arg(sampleRate);
            j->client_close(m_client);
            m_client = nullptr;
            return false;
        }
        if (int(j->get_buffer_size(m_client)) != frames) j->set_buffer_size(m_client, jack_nframes_t(frames));
        const char* outNames[2] = {"Main L", "Main R"};
        const char* inNames[2] = {"Input 1", "Input 2"};
        for (int c = 0; c < 2; ++c) {
            m_out[c] = j->port_register(m_client, outNames[c], kAudioType, kIsOutput, 0);
            m_in[c] = j->port_register(m_client, inNames[c], kAudioType, kIsInput, 0);
            if (!m_out[c] || !m_in[c]) { if (error) *error = "couldn't make the ports"; return false; }
        }
        m_inBuf.assign(size_t(kMaxFrames) * 2, 0.0f);
        m_outBuf.assign(size_t(kMaxFrames) * 2, 0.0f);
        j->set_process_callback(m_client, &JackDriver::onProcess, this);
        j->on_shutdown(m_client, [](void* self) { static_cast<JackDriver*>(self)->m_dead.store(true); }, this);
        return true;
    }
    void start(RenderFn render) override {
        startDuplex([render](const float*, float* out, int n) { render(out, n); });
    }
    void startDuplex(DuplexFn fn) override {
        const JackApi* j = api();
        if (!m_client || !j) return;
        m_fn = std::move(fn);
        if (j->activate(m_client) != 0) return;
        m_active = true;
        // connect: the chosen ports, else the first physical ones. Outputs first; then each input,
        // checking the graph keeps running: an input with nothing behind it (an interface on a
        // profile its hardware doesn't have) stalls everything, so it's disconnected again.
        const QStringList playback = physicalPorts(j, m_client, false);
        QStringList capture = physicalPorts(j, m_client, true);
        QString outDevice;
        for (int c = 0; c < 2; ++c) {
            const QString to = m_options.outPorts.value(c).isEmpty() ? playback.value(c) : m_options.outPorts.value(c);
            if (!to.isEmpty() && to != "-") j->connect(m_client, j->port_name(m_out[c]), to.toUtf8().constData());
            if (outDevice.isEmpty() && !to.isEmpty() && to != "-") outDevice = deviceOf(to);
        }
        // automatic inputs: the same device as the output (your interface), not the first one found
        QStringList same;
        for (const QString& p : capture) if (deviceOf(p) == outDevice) same << p;
        if (!same.isEmpty()) capture = same + capture;
        running(400);
        for (int c = 0; c < 2; ++c) {
            const QString from = m_options.inPorts.value(c).isEmpty() ? capture.value(c) : m_options.inPorts.value(c);
            if (from.isEmpty() || from == "-") continue;
            const QByteArray src = from.toUtf8();
            if (j->connect(m_client, src.constData(), j->port_name(m_in[c])) != 0) continue;
            if (!running(500)) {
                j->disconnect(m_client, src.constData(), j->port_name(m_in[c]));
                m_problem = QString("Input %1 (%2) gives no audio and stopped the sound, so it was disconnected. "
                                    "If it's your interface, set its profile to Analog Stereo Duplex or Pro Audio "
                                    "(in a terminal: wpctl status, then wpctl set-profile <device number> <profile number>), "
                                    "then pick its input here again.").arg(c + 1).arg(from);
                running(500);
            }
        }
    }
    void stop() override {
        if (m_active && m_client) api()->deactivate(m_client);
        m_active = false;
    }
    bool duplex() const override { return true; }
    QString problem() const override { return m_problem; }
    double latency() const override { return portLatency(m_out[0], kPlaybackLatency) + period(); }
    double inputLatency() const override { return portLatency(m_in[0], kCaptureLatency); }
    QString name() const override {
        const JackApi* j = api();
        const int frames = m_client && j ? int(j->get_buffer_size(m_client)) : m_options.frames;
        return QString("PipeWire / JACK, %1 kHz, %2 frames").arg(m_sr / 1000.0).arg(frames);
    }

private:
    // Whether the callbacks keep coming (waits up to `ms` for a few more).
    bool running(int ms) {
        const int start = m_calls.load(std::memory_order_relaxed);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < until) {
            if (m_calls.load(std::memory_order_relaxed) - start >= 3) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
    double period() const {
        const JackApi* j = api();
        return m_client && j && m_sr > 0 ? double(j->get_buffer_size(m_client)) / m_sr : 0.0;
    }
    double portLatency(jack_port_t* port, int mode) const {
        const JackApi* j = api();
        if (!m_client || !j || !port || m_sr <= 0) return 0;
        jack_latency_range_t r{0, 0};
        j->port_get_latency_range(port, mode, &r);
        return double(r.max) / m_sr;
    }
    static int onProcess(jack_nframes_t nframes, void* arg) {
        auto* self = static_cast<JackDriver*>(arg);
        self->m_calls.fetch_add(1, std::memory_order_relaxed);
        const JackApi* j = api();
        const int n = std::min(int(nframes), kMaxFrames);
        const float* in[2] = {static_cast<const float*>(j->port_get_buffer(self->m_in[0], nframes)),
                              static_cast<const float*>(j->port_get_buffer(self->m_in[1], nframes))};
        float* out[2] = {static_cast<float*>(j->port_get_buffer(self->m_out[0], nframes)),
                         static_cast<float*>(j->port_get_buffer(self->m_out[1], nframes))};
        float* ib = self->m_inBuf.data();
        float* ob = self->m_outBuf.data();
        for (int i = 0; i < n; ++i) { ib[2 * i] = in[0] ? in[0][i] : 0.0f; ib[2 * i + 1] = in[1] ? in[1][i] : 0.0f; }
        if (self->m_fn) self->m_fn(ib, ob, n);
        else std::fill(ob, ob + 2 * n, 0.0f);
        for (int i = 0; i < n; ++i) { if (out[0]) out[0][i] = ob[2 * i]; if (out[1]) out[1][i] = ob[2 * i + 1]; }
        for (int c = 0; c < 2; ++c) if (out[c] && int(nframes) > n) std::fill(out[c] + n, out[c] + nframes, 0.0f);
        return 0;
    }

    jack_client_t* m_client = nullptr;
    jack_port_t* m_in[2] = {nullptr, nullptr};
    jack_port_t* m_out[2] = {nullptr, nullptr};
    DriverOptions m_options;
    DuplexFn m_fn;
    std::vector<float> m_inBuf, m_outBuf;
    int m_sr = 48000;
    bool m_active = false;
    std::atomic<bool> m_dead{false};
    std::atomic<int> m_calls{0};
    QString m_problem;
};

} // namespace

bool jackAvailable() { return api() != nullptr; }

QStringList listJackPorts(bool capture, int* rate) {
    const JackApi* j = api();
    if (!j) return {};
    int status = 0;
    jack_client_t* c = j->client_open("Sessions (ports)", kNoStartServer, &status);
    if (!c) return {};
    if (rate) *rate = int(j->get_sample_rate(c));
    const QStringList out = physicalPorts(j, c, capture);
    j->client_close(c);
    return out;
}

std::unique_ptr<Driver> makeJackDriver() { return api() ? std::make_unique<JackDriver>() : nullptr; }

} // namespace sessions::audio
