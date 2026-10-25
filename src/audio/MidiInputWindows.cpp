#include "MidiInput.h"
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>
#include <QSettings>
#include <QTimer>
#include <array>
#include <atomic>
#include <vector>
// The multimedia declarations require the Windows types first.
// clang-format off
#include <windows.h>
#include <mmsystem.h>
// clang-format on
namespace sessions::audio {
struct MidiInput::Windows {
    struct Event {
        QString source;
        DWORD data = 0;
        QByteArray sysex;
        HMIDIIN input = nullptr;
        MIDIHDR* header = nullptr;
    };
    struct Input {
        Windows* owner = nullptr;
        HMIDIIN handle = nullptr;
        QString name;
        std::atomic<bool> closing{false};
        std::array<MIDIHDR, 2> headers{};
        std::array<std::array<char, 512>, 2> data{};
        ~Input() {
            closing = true;
            if (handle) {
                midiInStop(handle);
                midiInReset(handle);
                for (auto& h : headers)
                    midiInUnprepareHeader(handle, &h, sizeof(h));
                midiInClose(handle);
            }
        }
    };
    QMutex mutex;
    QQueue<Event> events;
    std::vector<std::unique_ptr<Input>> inputs;
    QHash<QString, HMIDIOUT> outputs;
    QStringList names;
    ~Windows() {
        inputs.clear();
        for (auto out : outputs) {
            midiOutReset(out);
            midiOutClose(out);
        }
    }
    static void CALLBACK receive(HMIDIIN handle, UINT message, DWORD_PTR context, DWORD_PTR p1, DWORD_PTR) {
        auto* input = reinterpret_cast<Input*>(context);
        if (input->closing)
            return;
        if (message == MIM_DATA || message == MIM_LONGDATA) {
            Event event;
            event.source = input->name;
            if (message == MIM_DATA)
                event.data = DWORD(p1);
            else {
                auto* h = reinterpret_cast<MIDIHDR*>(p1);
                event.sysex = QByteArray(h->lpData, int(h->dwBytesRecorded));
                event.input = handle;
                event.header = h;
            }
            {
                QMutexLocker lock(&input->owner->mutex);
                if (input->owner->events.size() < 4096)
                    input->owner->events.enqueue(std::move(event));
            }
        }
    }
};
namespace {
QStringList deviceNames() {
    QStringList result;
    for (UINT i = 0; i < midiInGetNumDevs(); ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
            result.push_back(QString::fromWCharArray(caps.szPname));
        else
            result.push_back(QString("MIDI input %1").arg(i + 1));
    }
    return result;
}
} // namespace
MidiInput::MidiInput(QObject* parent) : QObject(parent), m_windows(std::make_unique<Windows>()) {
    m_status = "Windows MIDI";
    connectAll();
    auto* timer = new QTimer(this);
    timer->setInterval(5);
    connect(timer, &QTimer::timeout, this, &MidiInput::readEvents);
    timer->start();
    auto* scan = new QTimer(this);
    scan->setInterval(2000);
    connect(scan, &QTimer::timeout, this, [this] {
        if (deviceNames() != m_windows->names) {
            connectAll();
            emit portsChanged();
        }
    });
    scan->start();
}
MidiInput::~MidiInput() = default;
int MidiInput::clientId() const {
    return -1;
}
QVector<MidiInput::Port> MidiInput::ports() const {
    QVector<Port> result;
    for (int i = 0; i < m_windows->names.size(); ++i) {
        const auto& name = m_windows->names[i];
        bool connected = false;
        for (const auto& in : m_windows->inputs)
            connected |= in->name == name;
        result.push_back({QString("winmm:%1").arg(i), name, connected});
    }
    return result;
}
bool MidiInput::enabled(const QString& name) const {
    return !QSettings("Sessions", "Sessions").value("midi/off").toStringList().contains(name);
}
void MidiInput::setEnabled(const QString& name, bool on) {
    QSettings s("Sessions", "Sessions");
    auto off = s.value("midi/off").toStringList();
    off.removeAll(name);
    if (!on)
        off.push_back(name);
    s.setValue("midi/off", off);
    connectAll();
    emit portsChanged();
}
void MidiInput::connectAll() {
    m_windows->inputs.clear();
    {
        QMutexLocker lock(&m_windows->mutex);
        m_windows->events.clear();
    }
    for (auto out : m_windows->outputs) {
        midiOutReset(out);
        midiOutClose(out);
    }
    m_windows->outputs.clear();
    m_windows->names = deviceNames();
    for (int i = 0; i < m_windows->names.size(); ++i) {
        const auto& name = m_windows->names[i];
        if (!enabled(name))
            continue;
        auto input = std::make_unique<Windows::Input>();
        input->owner = m_windows.get();
        input->name = name;
        if (midiInOpen(&input->handle, UINT(i), reinterpret_cast<DWORD_PTR>(&Windows::receive),
                       reinterpret_cast<DWORD_PTR>(input.get()), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            input->handle = nullptr;
            continue;
        }
        for (size_t j = 0; j < input->headers.size(); ++j) {
            auto& h = input->headers[j];
            h.lpData = input->data[j].data();
            h.dwBufferLength = DWORD(input->data[j].size());
            if (midiInPrepareHeader(input->handle, &h, sizeof(h)) == MMSYSERR_NOERROR)
                midiInAddBuffer(input->handle, &h, sizeof(h));
        }
        midiInStart(input->handle);
        m_windows->inputs.push_back(std::move(input));
    }
}
void MidiInput::findWritable() {}
bool MidiInput::send(const QString& port, const QByteArray& message) {
    emit sent(port, message);
    if (message.isEmpty())
        return false;
    HMIDIOUT out = m_windows->outputs.value(port, nullptr);
    if (!out)
        for (UINT i = 0; i < midiOutGetNumDevs(); ++i) {
            MIDIOUTCAPSW caps{};
            midiOutGetDevCapsW(i, &caps, sizeof(caps));
            QString expected = port;
            expected.replace("MIDIIN", "MIDIOUT");
            const QString name = QString::fromWCharArray(caps.szPname);
            if (name != port && name != expected)
                continue;
            if (midiOutOpen(&out, i, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR)
                m_windows->outputs.insert(port, out);
            else
                out = nullptr;
            break;
        }
    if (!out) {
        m_sendError = "MIDI output is unavailable: " + port;
        return false;
    }
    MMRESULT result = MMSYSERR_ERROR;
    if (uchar(message.front()) == 0xf0 && uchar(message.back()) == 0xf7) {
        // Output buffers must survive until the driver finishes. A bounded wait is
        // on the UI thread, never the audio callback.
        QByteArray data = message;
        MIDIHDR header{};
        header.lpData = data.data();
        header.dwBufferLength = DWORD(data.size());
        result = midiOutPrepareHeader(out, &header, sizeof(header));
        if (result == MMSYSERR_NOERROR) {
            result = midiOutLongMsg(out, &header, sizeof(header));
            for (int i = 0; i < 100 && result == MMSYSERR_NOERROR && !(header.dwFlags & MHDR_DONE); ++i)
                Sleep(1);
            if (!(header.dwFlags & MHDR_DONE)) {
                midiOutReset(out);
                result = MMSYSERR_ERROR;
            }
            midiOutUnprepareHeader(out, &header, sizeof(header));
        }
    } else if (message.size() >= 2 && message.size() <= 3 && uchar(message[0]) >= 0x80 && uchar(message[0]) < 0xf0) {
        DWORD packed = uchar(message[0]) | (DWORD(uchar(message[1])) << 8);
        if (message.size() == 3)
            packed |= DWORD(uchar(message[2])) << 16;
        result = midiOutShortMsg(out, packed);
    }
    if (result != MMSYSERR_NOERROR)
        m_sendError = "Couldn't send MIDI feedback to " + port;
    return result == MMSYSERR_NOERROR;
}
void MidiInput::readEvents() {
    QQueue<Windows::Event> events;
    {
        QMutexLocker lock(&m_windows->mutex);
        events.swap(m_windows->events);
    }
    while (!events.isEmpty()) {
        const auto e = events.dequeue();
        if (e.header)
            midiInAddBuffer(e.input, e.header, sizeof(MIDIHDR));
        if (!enabled(e.source))
            continue;
        if (!e.sysex.isEmpty())
            injectSysEx(e.sysex, e.source);
        else
            inject(int(e.data & 255), int((e.data >> 8) & 127), int((e.data >> 16) & 127), e.source);
    }
}
void MidiInput::inject(int status, int d1, int d2, const QString& source) {
    m_sourceName = source;
    emit activity();
    const int type = status & 0xf0, channel = status & 15;
    if (type == 0x90 && d2 > 0)
        emit noteOn(channel, d1, d2);
    else if (type == 0x80 || type == 0x90)
        emit noteOff(channel, d1);
    else if (type == 0xb0)
        emit controlChange(channel, d1, d2);
    else if (type == 0xe0)
        emit pitchBend(channel, (d1 & 127) | ((d2 & 127) << 7));
    else if (type == 0xd0)
        emit channelPressure(channel, d1 & 127);
}
void MidiInput::injectSysEx(const QByteArray& m, const QString& source) {
    if (m.size() != 6 || uchar(m[0]) != 0xf0 || uchar(m[1]) != 0x7f || uchar(m[2]) > 127 || uchar(m[3]) != 6 ||
        uchar(m[4]) > 127 || uchar(m[5]) != 0xf7)
        return;
    emit activity();
    emit machineControl(source, uchar(m[4]));
}
} // namespace sessions::audio
