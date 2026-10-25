// Windows uses Qt's WASAPI-backed shared audio. Device callbacks only copy or
// render into preallocated buffers; all device setup stays on the UI thread.
#include "Driver.h"
#include <QAudioDevice>
#include <QAudioSink>
#include <QAudioSource>
#include <QIODevice>
#include <QMediaDevices>
#include <algorithm>
#include <array>
#include <cstring>
namespace sessions::audio {
namespace {
QAudioDevice device(bool input, const QString& id) {
    const auto devices = input ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs();
    if (!id.isEmpty())
        for (const auto& d : devices)
            if (QString::fromLatin1(d.id().toBase64()) == id)
                return d;
    return id.isEmpty() ? (input ? QMediaDevices::defaultAudioInput() : QMediaDevices::defaultAudioOutput())
                        : QAudioDevice();
}
QAudioFormat format(int rate, int channels = 2) {
    QAudioFormat f;
    f.setSampleRate(rate);
    f.setChannelCount(channels);
    f.setSampleFormat(QAudioFormat::Float);
    return f;
}
class OutputStream : public QIODevice {
  public:
    RenderFn render;
    qint64 readData(char* data, qint64 size) override {
        const qint64 frames = size / (2 * sizeof(float));
        for (qint64 at = 0; at < frames;) {
            const int n = int(std::min<qint64>(256, frames - at));
            render(m_buffer.data(), n);
            std::memcpy(data + at * 2 * sizeof(float), m_buffer.data(), size_t(n) * 2 * sizeof(float));
            at += n;
        }
        return frames * 2 * sizeof(float);
    }
    qint64 writeData(const char*, qint64) override {
        return -1;
    }
    bool isSequential() const override {
        return true;
    }
    qint64 bytesAvailable() const override {
        return 4096 + QIODevice::bytesAvailable();
    }

  private:
    std::array<float, 512> m_buffer{};
};
class InputStream : public QIODevice {
  public:
    PushFn push;
    int channels = 2;
    qint64 readData(char*, qint64) override {
        return -1;
    }
    qint64 writeData(const char* data, qint64 size) override {
        const int stride = channels * int(sizeof(float));
        // Preserve a partial frame if a backend splits a write at a byte boundary.
        for (qint64 at = 0; at < size; ++at) {
            m_frame[size_t(m_used++)] = data[at];
            if (m_used != stride)
                continue;
            float left = 0, right = 0;
            std::memcpy(&left, m_frame.data(), sizeof(float));
            if (channels == 2)
                std::memcpy(&right, m_frame.data() + sizeof(float), sizeof(float));
            else
                right = left;
            m_buffer[size_t(m_frames) * 2] = left;
            m_buffer[size_t(m_frames) * 2 + 1] = right;
            m_used = 0;
            if (++m_frames == 256) {
                push(m_buffer.data(), m_frames);
                m_frames = 0;
            }
        }
        if (m_frames) {
            push(m_buffer.data(), m_frames);
            m_frames = 0;
        }
        return size;
    }

  private:
    std::array<char, 8> m_frame{};
    std::array<float, 512> m_buffer{};
    int m_used = 0, m_frames = 0;
};
class QtOutput : public Driver {
  public:
    ~QtOutput() override {
        stop();
    }
    bool open(int rate, const DriverOptions& options, QString* error) override {
        const auto d = device(false, options.device);
        const auto f = format(rate);
        if (d.isNull() || !d.isFormatSupported(f)) {
            if (error)
                *error = "No output supporting stereo 32-bit float at the session sample rate.";
            return false;
        }
        m_sink = std::make_unique<QAudioSink>(d, f);
        m_bufferMs = std::clamp(options.bufferMs, 10, 500);
        m_sink->setBufferSize(rate * 2 * int(sizeof(float)) * m_bufferMs / 1000);
        return true;
    }
    void start(RenderFn render) override {
        m_stream.render = std::move(render);
        m_stream.open(QIODevice::ReadOnly);
        m_sink->start(&m_stream);
    }
    void stop() override {
        if (m_sink)
            m_sink->stop();
        m_stream.close();
    }
    double latency() const override {
        return m_bufferMs / 1000.0;
    }
    QString name() const override {
        return "Windows audio (WASAPI shared)";
    }
    QString problem() const override {
        return m_sink && m_sink->error() != QAudio::NoError ? "Windows audio output is unavailable." : QString();
    }

  private:
    OutputStream m_stream;
    std::unique_ptr<QAudioSink> m_sink;
    int m_bufferMs = 30;
};
class QtInput : public InputDriver {
  public:
    ~QtInput() override {
        stop();
    }
    bool open(int rate, const QString& id, QString* error) override {
        const auto d = device(true, id);
        auto f = format(rate);
        if (!d.isNull() && !d.isFormatSupported(f))
            f = format(rate, 1);
        if (d.isNull() || !d.isFormatSupported(f)) {
            if (error)
                *error = "No input supporting 32-bit float at the session sample rate. Check Windows microphone "
                         "permissions.";
            return false;
        }
        m_stream.channels = f.channelCount();
        m_source = std::make_unique<QAudioSource>(d, f);
        m_source->setBufferSize(rate * f.bytesPerFrame() / 100);
        return true;
    }
    void start(PushFn push) override {
        m_stream.push = std::move(push);
        m_stream.open(QIODevice::WriteOnly);
        m_source->start(&m_stream);
    }
    void stop() override {
        if (m_source)
            m_source->stop();
        m_stream.close();
    }
    double latency() const override {
        return .01;
    }
    QString name() const override {
        return "Windows audio input";
    }

  private:
    InputStream m_stream;
    std::unique_ptr<QAudioSource> m_source;
};
} // namespace
std::unique_ptr<Driver> makeQtAudioDriver() {
    return std::make_unique<QtOutput>();
}
std::unique_ptr<InputDriver> makeQtAudioInput() {
    return std::make_unique<QtInput>();
}
QVector<OutputDevice> listQtAudioDevices(bool input) {
    QVector<OutputDevice> result;
    for (const auto& d : input ? QMediaDevices::audioInputs() : QMediaDevices::audioOutputs())
        result.push_back({QString::fromLatin1(d.id().toBase64()), d.description()});
    return result;
}
} // namespace sessions::audio
