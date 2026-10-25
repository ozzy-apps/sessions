#include "Driver.h"
#include <QtGlobal>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#ifdef SESSIONS_HAVE_PULSE
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#endif

namespace sessions::audio {

namespace {

constexpr int kBlock = 256;   // frames per render call (5.3 ms at 48 kHz)

class NullDriver : public Driver {
public:
    ~NullDriver() override { stop(); }
    bool open(int sampleRate, const DriverOptions&, QString*) override { m_sr = sampleRate; return true; }
    void start(RenderFn render) override {
        m_running = true;
        m_thread = std::thread([this, render] {
            std::vector<float> buf(kBlock * 2);
            const auto period = std::chrono::nanoseconds(int64_t(1e9 * kBlock / m_sr));
            auto next = std::chrono::steady_clock::now();
            while (m_running.load(std::memory_order_relaxed)) {
                render(buf.data(), kBlock);
                next += period;
                std::this_thread::sleep_until(next);
                // After a stall (suspend, debugger) don't race to catch up.
                if (std::chrono::steady_clock::now() - next > std::chrono::milliseconds(200)) next = std::chrono::steady_clock::now();
            }
        });
    }
    void stop() override {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }
    QString name() const override { return "Silent clock"; }

private:
    int m_sr = 48000;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

#ifdef SESSIONS_HAVE_PULSE
class PulseDriver : public Driver {
public:
    ~PulseDriver() override {
        stop();
        if (m_pa) pa_simple_free(m_pa);
    }
    bool open(int sampleRate, const DriverOptions& options, QString* error) override {
        if (!qEnvironmentVariableIsSet("PULSE_PROP_application.icon_name"))
            qputenv("PULSE_PROP_application.icon_name", "sessions");   // icon in the system sound settings
        pa_sample_spec ss;
        ss.format = PA_SAMPLE_FLOAT32LE;
        ss.rate = uint32_t(sampleRate);
        ss.channels = 2;
        pa_buffer_attr attr;
        attr.maxlength = uint32_t(-1);
        attr.tlength = uint32_t(pa_usec_to_bytes(pa_usec_t(std::clamp(options.bufferMs, 5, 500)) * 1000, &ss));
        attr.prebuf = uint32_t(-1);
        attr.minreq = uint32_t(pa_usec_to_bytes(5000, &ss));
        attr.fragsize = uint32_t(-1);
        int err = 0;
        const QByteArray device = options.device.toUtf8();
        m_pa = pa_simple_new(nullptr, "Sessions", PA_STREAM_PLAYBACK, device.isEmpty() ? nullptr : device.constData(), "Playback", &ss, nullptr, &attr, &err);
        if (!m_pa) {
            if (error) *error = QString::fromUtf8(pa_strerror(err));
            return false;
        }
        m_sr = sampleRate;
        return true;
    }
    void start(RenderFn render) override {
        m_running = true;
        m_thread = std::thread([this, render] {
            std::vector<float> buf(kBlock * 2);
            int err = 0, n = 0;
            while (m_running.load(std::memory_order_relaxed)) {
                render(buf.data(), kBlock);
                // Blocks until the server has room, which is what paces the engine.
                if (pa_simple_write(m_pa, buf.data(), buf.size() * sizeof(float), &err) < 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if ((++n & 31) == 0) {
                    pa_usec_t l = pa_simple_get_latency(m_pa, &err);
                    if (l != pa_usec_t(-1)) m_latency.store(double(l) / 1e6, std::memory_order_relaxed);
                }
            }
        });
    }
    void stop() override {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }
    double  latency() const override { return m_latency.load(std::memory_order_relaxed); }
    QString name() const override { return QString("PulseAudio / PipeWire, %1 kHz").arg(m_sr / 1000.0); }

private:
    pa_simple* m_pa = nullptr;
    int m_sr = 48000;
    std::atomic<bool> m_running{false};
    std::atomic<double> m_latency{0.03};
    std::thread m_thread;
};

class PulseInput : public InputDriver {
public:
    ~PulseInput() override {
        stop();
        if (m_pa) pa_simple_free(m_pa);
    }
    bool open(int sampleRate, const QString& device, QString* error) override {
        pa_sample_spec ss;
        ss.format = PA_SAMPLE_FLOAT32LE;
        ss.rate = uint32_t(sampleRate);
        ss.channels = 2;   // mono sources are upmixed by the server
        pa_buffer_attr attr;
        attr.maxlength = uint32_t(-1);
        attr.tlength = uint32_t(-1);
        attr.prebuf = uint32_t(-1);
        attr.minreq = uint32_t(-1);
        attr.fragsize = uint32_t(pa_usec_to_bytes(5000, &ss));   // ~5 ms: low latency for monitoring
        int err = 0;
        const QByteArray dev = device.toUtf8();
        m_pa = pa_simple_new(nullptr, "Sessions", PA_STREAM_RECORD, dev.isEmpty() ? nullptr : dev.constData(), "Recording", &ss, nullptr, &attr, &err);
        if (!m_pa) {
            if (error) *error = QString::fromUtf8(pa_strerror(err));
            return false;
        }
        m_sr = sampleRate;
        return true;
    }
    void start(PushFn push) override {
        m_running = true;
        m_thread = std::thread([this, push] {
            std::vector<float> buf(kBlock * 2);
            int err = 0, n = 0;
            while (m_running.load(std::memory_order_relaxed)) {
                if (pa_simple_read(m_pa, buf.data(), buf.size() * sizeof(float), &err) < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                push(buf.data(), kBlock);
                if ((++n & 31) == 0) {
                    pa_usec_t l = pa_simple_get_latency(m_pa, &err);
                    if (l != pa_usec_t(-1)) m_latency.store(double(l) / 1e6, std::memory_order_relaxed);
                }
            }
        });
    }
    void stop() override {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
    }
    double  latency() const override { return m_latency.load(std::memory_order_relaxed); }
    QString name() const override { return QString("PulseAudio / PipeWire input, %1 kHz").arg(m_sr / 1000.0); }

private:
    pa_simple* m_pa = nullptr;
    int m_sr = 48000;
    std::atomic<bool> m_running{false};
    std::atomic<double> m_latency{0.005};
    std::thread m_thread;
};
#endif

#ifdef SESSIONS_HAVE_PULSE
// Runs one introspection query on a throwaway connection, given at most two seconds.
template <class Query>
void queryServer(Query&& query) {
    pa_mainloop* ml = pa_mainloop_new();
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "Sessions");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto pump = [&] { pa_mainloop_iterate(ml, 0, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
    if (ctx && pa_context_connect(ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
        pa_context_state_t st;
        while ((st = pa_context_get_state(ctx)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(st) && std::chrono::steady_clock::now() < deadline) pump();
        if (st == PA_CONTEXT_READY) {
            pa_operation* op = query(ctx);
            while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING && std::chrono::steady_clock::now() < deadline) pump();
            if (op) pa_operation_unref(op);
        }
        pa_context_disconnect(ctx);
    }
    if (ctx) pa_context_unref(ctx);
    pa_mainloop_free(ml);
}
#endif

} // namespace

QVector<OutputDevice> listInputDevices() {
#ifdef Q_OS_WIN
    return listQtAudioDevices(true);
#endif
    QVector<OutputDevice> out;
#ifdef SESSIONS_HAVE_PULSE
    queryServer([&out](pa_context* ctx) {
        return pa_context_get_source_info_list(ctx, [](pa_context*, const pa_source_info* info, int eol, void* data) {
            if (eol > 0 || !info || info->monitor_of_sink != PA_INVALID_INDEX) return;   // skip the outputs' monitors
            static_cast<QVector<OutputDevice>*>(data)->push_back({QString::fromUtf8(info->name), QString::fromUtf8(info->description)});
        }, &out);
    });
#endif
    return out;
}

std::unique_ptr<InputDriver> makePulseInput() {
#ifdef Q_OS_WIN
    return makeQtAudioInput();
#elif defined(SESSIONS_HAVE_PULSE)
    return std::make_unique<PulseInput>();
#else
    return nullptr;
#endif
}

QVector<OutputDevice> listOutputDevices() {
#ifdef Q_OS_WIN
    return listQtAudioDevices(false);
#endif
    QVector<OutputDevice> out;
#ifdef SESSIONS_HAVE_PULSE
    // A throwaway connection with its own main loop, given at most two seconds.
    pa_mainloop* ml = pa_mainloop_new();
    pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "Sessions");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    auto pump = [&] { pa_mainloop_iterate(ml, 0, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
    if (ctx && pa_context_connect(ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0) {
        pa_context_state_t st;
        while ((st = pa_context_get_state(ctx)) != PA_CONTEXT_READY && PA_CONTEXT_IS_GOOD(st) && std::chrono::steady_clock::now() < deadline) pump();
        if (st == PA_CONTEXT_READY) {
            pa_operation* op = pa_context_get_sink_info_list(ctx, [](pa_context*, const pa_sink_info* info, int eol, void* data) {
                if (eol > 0 || !info) return;
                static_cast<QVector<OutputDevice>*>(data)->push_back({QString::fromUtf8(info->name), QString::fromUtf8(info->description)});
            }, &out);
            while (op && pa_operation_get_state(op) == PA_OPERATION_RUNNING && std::chrono::steady_clock::now() < deadline) pump();
            if (op) pa_operation_unref(op);
        }
        pa_context_disconnect(ctx);
    }
    if (ctx) pa_context_unref(ctx);
    pa_mainloop_free(ml);
#endif
    return out;
}

std::unique_ptr<Driver> makePulseDriver() {
#ifdef Q_OS_WIN
    return makeQtAudioDriver();
#elif defined(SESSIONS_HAVE_PULSE)
    return std::make_unique<PulseDriver>();
#else
    return nullptr;
#endif
}

std::unique_ptr<Driver> makeNullDriver() { return std::make_unique<NullDriver>(); }

} // namespace sessions::audio
