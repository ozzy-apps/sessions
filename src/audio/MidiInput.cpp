#include <algorithm>
#include "MidiInput.h"
#include <QSettings>
#include <QSocketNotifier>
#include <vector>
#ifdef SESSIONS_HAVE_ALSA
#include <alsa/asoundlib.h>
#include <poll.h>
#endif

namespace sessions::audio {

namespace {
QStringList offPorts() { return QSettings("Sessions", "Sessions").value("midi/off").toStringList(); }
}

MidiInput::MidiInput(QObject* parent) : QObject(parent) {
#ifdef SESSIONS_HAVE_ALSA
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0) {
        m_status = "The ALSA sequencer isn't available (is the snd-seq module loaded?).";
        return;
    }
    m_seq = seq;
    snd_seq_set_client_name(seq, "Sessions");
    m_port = snd_seq_create_simple_port(seq, "MIDI In", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (m_port < 0) {
        snd_seq_close(seq);
        m_seq = nullptr;
        m_status = "Couldn't create a MIDI input port.";
        return;
    }
    snd_seq_connect_from(seq, m_port, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);   // devices coming and going
    const int n = snd_seq_poll_descriptors_count(seq, POLLIN);
    std::vector<pollfd> fds(size_t(std::max(0, n)));
    snd_seq_poll_descriptors(seq, fds.data(), unsigned(fds.size()), POLLIN);
    for (const pollfd& fd : fds) {
        auto* notifier = new QSocketNotifier(fd.fd, QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, &MidiInput::readEvents);
    }
    m_status = QString("Sessions: MIDI In (%1:%2)").arg(snd_seq_client_id(seq)).arg(m_port);
    connectAll();
#else
    m_status = "Sessions was built without ALSA (sudo apt install libasound2-dev, then rebuild).";
#endif
}

MidiInput::~MidiInput() {
#ifdef SESSIONS_HAVE_ALSA
    if (m_seq) snd_seq_close(m_seq);
#endif
}

int MidiInput::clientId() const {
#ifdef SESSIONS_HAVE_ALSA
    return m_seq ? snd_seq_client_id(m_seq) : -1;
#else
    return -1;
#endif
}

QVector<MidiInput::Port> MidiInput::ports() const {
    QVector<Port> out;
#ifdef SESSIONS_HAVE_ALSA
    if (!m_seq) return out;
    snd_seq_client_info_t* client;
    snd_seq_port_info_t* port;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    const int self = snd_seq_client_id(m_seq);
    while (snd_seq_query_next_client(m_seq, client) >= 0) {
        const int c = snd_seq_client_info_get_client(client);
        if (c == SND_SEQ_CLIENT_SYSTEM || c == self) continue;
        snd_seq_port_info_set_client(port, c);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(m_seq, port) >= 0) {
            const unsigned caps = snd_seq_port_info_get_capability(port);
            const unsigned need = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((caps & need) != need || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;
            if (!(snd_seq_port_info_get_type(port) & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
            Port p;
            p.id = QString("%1:%2").arg(c).arg(snd_seq_port_info_get_port(port));
            p.name = QString::fromUtf8(snd_seq_client_info_get_name(client)) + ": " + QString::fromUtf8(snd_seq_port_info_get_name(port));
            p.connected = m_connected.contains(p.id);
            out.push_back(p);
        }
    }
#endif
    return out;
}

bool MidiInput::enabled(const QString& portName) const { return !offPorts().contains(portName); }

void MidiInput::setEnabled(const QString& portName, bool on) {
    QStringList off = offPorts();
    off.removeAll(portName);
    if (!on) off << portName;
    QSettings("Sessions", "Sessions").setValue("midi/off", off);
#ifdef SESSIONS_HAVE_ALSA
    if (!m_seq) return;
    for (const Port& p : ports()) {
        if (p.name != portName) continue;
        const QStringList cp = p.id.split(':');
        if (on && !p.connected && snd_seq_connect_from(m_seq, m_port, cp[0].toInt(), cp[1].toInt()) >= 0) m_connected.insert(p.id);
        if (!on && p.connected) { snd_seq_disconnect_from(m_seq, m_port, cp[0].toInt(), cp[1].toInt()); m_connected.remove(p.id); }
    }
    emit portsChanged();
#endif
}

void MidiInput::connectAll() {
#ifdef SESSIONS_HAVE_ALSA
    m_sourceNames.clear();
    for (const Port& p : ports()) {
        m_sourceNames.insert(p.id, p.name);
        if (p.connected || !enabled(p.name)) continue;
        const QStringList cp = p.id.split(':');
        if (snd_seq_connect_from(m_seq, m_port, cp[0].toInt(), cp[1].toInt()) >= 0) m_connected.insert(p.id);
    }
#endif
}

void MidiInput::inject(int status, int d1, int d2, const QString& sourceName) {
    m_sourceName = sourceName;
    const int type = status & 0xF0, channel = status & 0x0F;
    emit activity();
    if (type == 0x90 && d2 > 0) emit noteOn(channel, d1, d2);
    else if (type == 0x80 || type == 0x90) emit noteOff(channel, d1);
    else if (type == 0xB0) emit controlChange(channel, d1, d2);
    else if (type == 0xE0) emit pitchBend(channel, (d1 & 0x7F) | ((d2 & 0x7F) << 7));
    else if (type == 0xD0) emit channelPressure(channel, d1 & 0x7F);
}

void MidiInput::injectSysEx(const QByteArray& message, const QString& sourceName) {
    // Only a complete, parameter-free universal realtime MMC command is accepted.
    // Manufacturer SysEx, malformed packets and all bulk data remain uninterpreted.
    if (message.size() != 6 || uchar(message[0]) != 0xf0 || uchar(message[1]) != 0x7f
        || uchar(message[2]) > 0x7f || uchar(message[3]) != 0x06
        || uchar(message[4]) > 0x7f || uchar(message[5]) != 0xf7) return;
    emit activity();
    emit machineControl(sourceName, uchar(message[4]));
}

void MidiInput::readEvents() {
#ifdef SESSIONS_HAVE_ALSA
    bool portsMoved = false;
    snd_seq_event_t* ev = nullptr;
    while (snd_seq_event_input(m_seq, &ev) >= 0 && ev) {
        const QString source = m_sourceNames.value(QString("%1:%2").arg(ev->source.client).arg(ev->source.port));
        switch (ev->type) {
        case SND_SEQ_EVENT_NOTEON:
            inject(ev->data.note.velocity ? 0x90 | ev->data.note.channel : 0x80 | ev->data.note.channel, ev->data.note.note, ev->data.note.velocity, source);
            break;
        case SND_SEQ_EVENT_NOTEOFF:
            inject(0x80 | ev->data.note.channel, ev->data.note.note, 0, source);
            break;
        case SND_SEQ_EVENT_CONTROLLER:
            inject(0xB0 | ev->data.control.channel, int(ev->data.control.param), int(ev->data.control.value), source);
            break;
        case SND_SEQ_EVENT_PITCHBEND: {
            const int v = std::clamp(int(ev->data.control.value) + 8192, 0, 16383);
            inject(0xE0 | ev->data.control.channel, v & 0x7F, v >> 7, source);
            break;
        }
        case SND_SEQ_EVENT_CHANPRESS:
            inject(0xD0 | ev->data.control.channel, int(ev->data.control.value), 0, source);
            break;
        case SND_SEQ_EVENT_SYSEX:
            if (ev->data.ext.ptr && ev->data.ext.len == 6)
                injectSysEx(QByteArray(static_cast<const char*>(ev->data.ext.ptr), 6), source);
            break;
        case SND_SEQ_EVENT_PORT_START:
        case SND_SEQ_EVENT_PORT_EXIT:
        case SND_SEQ_EVENT_PORT_CHANGE:
        case SND_SEQ_EVENT_CLIENT_CHANGE:
        case SND_SEQ_EVENT_CLIENT_EXIT:
            portsMoved = true;
            connectAll(); // refresh source identity before the next queued controller message
            break;
        case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
            m_connected.remove(QString("%1:%2").arg(ev->data.connect.sender.client).arg(ev->data.connect.sender.port));
            portsMoved = true;
            break;
        default: break;
        }
        if (snd_seq_event_input_pending(m_seq, 0) <= 0) break;
    }
    if (portsMoved) {
        QSet<QString> alive;
        for (const Port& p : ports()) alive.insert(p.id);
        m_connected.intersect(alive);
        connectAll();   // a new keyboard: connect it
        emit portsChanged();
    }
#endif
}

} // namespace sessions::audio
