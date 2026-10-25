#include "NektarPads.h"
#include "ControlSurface.h"
#include "Model.h"
#include "Transport.h"
#include <QDateTime>
#include <QMap>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace sessions {
namespace {
constexpr int notes[8] = {60, 62, 64, 65, 67, 69, 71, 72};
QByteArray packet(std::initializer_list<int> data) {
    QByteArray result;
    for (int b : data)
        result.append(char(b));
    return result;
}
// LX+ live-control SysEx, not the stored-preset/memory-dump format. The
// payload (including its checksum) sums to zero modulo 128.
QByteArray sysex(const QByteArray& payload) {
    QByteArray out = QByteArray::fromHex("f00001777f");
    out += payload;
    int sum = 0;
    for (unsigned char b : payload)
        sum += b;
    out.append(char((-sum) & 127));
    out.append(char(0xf7));
    return out;
}
} // namespace

NektarPads::NektarPads(Project* project, Transport* transport, QObject* parent)
    : QObject(parent), m_project(project), m_transport(transport) {
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, [this] { refreshLights(); });
    m_settle.setSingleShot(true);
    m_settle.setInterval(1000);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        m_initialized = false;
        refreshLights(true);
    });
    reloadSettings();
}

void NektarPads::reloadSettings() {
    QSettings s("Sessions", "Sessions");
    const bool enabled =
        s.value("midi/controller", "auto").toString() != "off" && s.value("midi/nektarFeedback", true).toBool();
    const QString preferred = s.value("midi/controllerInput").toString();
    const Mode mode = Mode(std::clamp(s.value("midi/nektarPadMode", 0).toInt(), 0, 2));
    if (!enabled || preferred != m_preferred)
        restoreDevice();
    m_enabled = enabled;
    m_preferred = preferred;
    m_clipBank = std::clamp(s.value("midi/nektarClipBank", 0).toInt(), 0, 7);
    m_sceneBank = std::clamp(s.value("midi/nektarSceneBank", 0).toInt(), 0, 7);
    if (mode != m_mode)
        releaseHeld();
    m_mode = mode;
    updatePorts();
    m_shownMode = -1;
    refreshLights(true);
}

void NektarPads::setSender(std::function<bool(const QString&, const QByteArray&)> sender) {
    m_send = std::move(sender);
    m_initialized = false;
    refreshLights(true);
}

void NektarPads::setPorts(const QVector<Port>& ports) {
    m_ports = ports;
    updatePorts();
}

void NektarPads::updatePorts() {
    QString controls, output;
    if (m_enabled) {
        QMap<int, QMap<int, Port>> clients;
        for (const Port& p : m_ports) {
            if (!p.enabled || !ControlSurface::isNektarImpact(p.name))
                continue;
            const QStringList parts = p.id.split(':');
            bool clientOk = false, portOk = false;
            const int client = parts.value(0).toInt(&clientOk), port = parts.value(1).toInt(&portOk);
            if (clientOk && portOk && parts.size() == 2)
                clients[client][port] = p;
        }
        for (const auto& ports : clients) {
            if (ports.size() < 2)
                continue; // separate keys/output and DAW control input required
            bool selected = m_preferred.isEmpty();
            for (const Port& p : ports)
                selected |= p.name == m_preferred;
            if (!selected)
                continue;
            output = ports.first().name;
            controls = ports.last().name;
            break;
        }
    }
    if (controls == m_controls && output == m_output) {
        if (connected() && !m_timer.isActive())
            m_timer.start();
        return;
    }
    restoreDevice();
    m_controls = controls;
    m_output = output;
    m_initialized = false;
    m_shownMode = -1;
    m_retryAt = 0;
    if (connected()) {
        m_timer.start();
        refreshLights(true);
        m_settle.start();
    }
}

int NektarPads::padNote(int pad) {
    return pad >= 0 && pad < 8 ? notes[pad] : -1;
}

QByteArray NektarPads::paletteMessage(const std::array<int, 8>& palettes) {
    QByteArray payload = packet({1, 6, 0});
    for (int i = 0; i < 8; ++i)
        payload += packet({0, i + 1, 1, std::clamp(palettes[size_t(i)], 0, 15)});
    return sysex(payload);
}

QByteArray NektarPads::padModeMessage(bool launcher) {
    return sysex(packet({1, 9, 4, 1, 0, 1, launcher ? 3 : 2}));
}

QByteArray NektarPads::usbModeMessage(int mode) {
    return sysex(packet({1, 5, 0, 1, 15, 1, std::clamp(mode, 0, 2)}));
}

bool NektarPads::send(const QByteArray& data) {
    return m_send && !m_output.isEmpty() && m_send(m_output, data);
}

bool NektarPads::initialize() {
    if (m_initialized)
        return true;
    m_touched = true;
    if (!send(usbModeMessage(2)) || !send(padModeMessage(m_mode != Mode::Notes)))
        return false;
    m_initialized = true;
    m_shownMode = -1;
    return true;
}

void NektarPads::releaseHeld() {
    for (Held& held : m_held) {
        for (int id : held.tracks) {
            const int track = m_project->indexOfTrack(id);
            if (track >= 0 && held.scene >= 0)
                m_transport->releaseSlot(track, held.scene);
        }
        held = {};
    }
    m_shift = false;
    std::fill(std::begin(m_navigationDown), std::end(m_navigationDown), false);
    std::fill(std::begin(m_pressed), std::end(m_pressed), false);
    std::fill(std::begin(m_buttonDown), std::end(m_buttonDown), false);
    std::fill(std::begin(m_wasMode), std::end(m_wasMode), false);
    std::fill(std::begin(m_bankUsed), std::end(m_bankUsed), false);
}

void NektarPads::restoreDevice() {
    releaseHeld();
    m_timer.stop();
    m_settle.stop();
    if (m_touched) {
        send(packet({0xbf, 117, 0}));
        send(packet({0xbf, 118, 0}));
        send(padModeMessage(false));
        send(usbModeMessage(0));
    }
    m_touched = m_initialized = false;
    m_shownMode = -1;
}

void NektarPads::shutdown() {
    restoreDevice();
    m_enabled = false;
}

void NektarPads::setMode(Mode mode) {
    if (mode == m_mode)
        return;
    releaseHeld();
    m_mode = mode;
    QSettings("Sessions", "Sessions").setValue("midi/nektarPadMode", int(mode));
    m_shownMode = -1;
    refreshLights(true);
    emit message(
        "Nektar pads",
        mode == Mode::Notes
            ? "Notes"
            : QString(mode == Mode::Clips ? "Clips %1–%2" : "Scenes %1–%2").arg(bank() * 8 + 1).arg(bank() * 8 + 8));
}

void NektarPads::selectBank(int bank) {
    if (m_mode == Mode::Scenes)
        m_sceneBank = bank;
    else
        m_clipBank = bank;
    QSettings("Sessions", "Sessions")
        .setValue(m_mode == Mode::Scenes ? "midi/nektarSceneBank" : "midi/nektarClipBank", bank);
    m_shownMode = -1;
    refreshLights(true);
    emit message("Nektar pads",
                 QString(m_mode == Mode::Scenes ? "Scenes %1–%2" : "Clips %1–%2").arg(bank * 8 + 1).arg(bank * 8 + 8));
}

bool NektarPads::controlChange(const QString& source, int channel, int cc, int value) {
    if (!connected() || source != m_controls || channel != 15 || value < 0 || value > 127)
        return false;
    if (cc == 108) {
        m_shift = value > 0;
        return true;
    }
    if (cc == 117 || cc == 118) {
        const int button = cc - 117;
        const Mode requested = button == 0 ? Mode::Clips : Mode::Scenes;
        const bool down = value > 0;
        if (down && !m_buttonDown[button]) {
            const bool same = m_mode == requested;
            if (!same)
                setMode(requested);
            m_buttonDown[button] = true;
            m_wasMode[button] = same;
            m_bankUsed[button] = false;
        } else if (!down && m_buttonDown[button]) {
            const bool back = m_mode == requested && m_wasMode[button] && !m_bankUsed[button];
            m_buttonDown[button] = false;
            if (back)
                setMode(Mode::Notes);
        }
        refreshLights(true);
        return true;
    }
    if ((cc == 109 || cc == 110) && !m_shift && m_mode != Mode::Notes) {
        // Only the press acts; firmware sends both 1/0 and 127/0 button forms.
        const int button = cc - 109;
        const bool pressed = value > 0 && !m_navigationDown[button];
        m_navigationDown[button] = value > 0;
        if (pressed && !m_project->tracks.isEmpty()) {
            const int selected = std::clamp(m_project->selectedTrack + (cc == 109 ? -1 : 1), 0,
                                            std::max(0, int(m_project->tracks.size()) - 1));
            emit trackChosen(selected);
            m_project->selectedTrack = selected;
            refreshLights(true);
        }
        return true;
    }
    if (cc == 115 && value > 0)
        setMode(Mode::Notes); // Preset restores performance pads
    return false;
}

bool NektarPads::takesNote(const QString& source, int, int note) const {
    return connected() && source == m_controls &&
           std::find(std::begin(notes), std::end(notes), note) != std::end(notes);
}

bool NektarPads::note(const QString& source, int channel, int pitch, int velocity, bool on) {
    if (!takesNote(source, channel, pitch))
        return false;
    const int pad = int(std::find(std::begin(notes), std::end(notes), pitch) - std::begin(notes));
    if (!on || velocity <= 0) {
        m_pressed[pad] = false;
        const Held held = m_held[size_t(pad)];
        m_held[size_t(pad)] = {};
        for (int id : held.tracks) {
            const int track = m_project->indexOfTrack(id);
            if (track >= 0 && held.scene >= 0)
                m_transport->releaseSlot(track, held.scene);
        }
        return true;
    }
    if (m_pressed[pad])
        return true;
    m_pressed[pad] = true;
    if (m_mode == Mode::Notes)
        return true; // original pad notes arrive on the keyboard port
    const int button = m_mode == Mode::Scenes ? 1 : 0;
    if (m_buttonDown[button]) {
        m_bankUsed[button] = true;
        selectBank(pad);
        return true;
    }
    const int scene = bank() * 8 + pad;
    if (scene >= m_project->scenes.size())
        return true;
    if (m_mode == Mode::Scenes) {
        m_transport->launchScene(scene);
        return true;
    }
    const int selected = m_project->selectedTrack;
    if (selected < 0 || selected >= m_project->tracks.size())
        return true;
    if (m_shift) {
        emit clipChosen(selected, scene);
        return true;
    }
    auto launch = [&](int index) {
        const Track& t = m_project->tracks[index];
        if (t.clipSlots.value(scene))
            m_transport->launchSlot(index, scene);
        else if (t.armed)
            m_transport->recordSlot(index, scene);
        else
            m_transport->stopTrack(index);
    };
    Held held;
    held.scene = scene;
    if (m_project->tracks[selected].isGroup()) {
        for (int member : m_project->membersOf(selected)) {
            held.tracks.push_back(m_project->tracks[member].id);
            launch(member);
        }
    } else {
        held.tracks.push_back(m_project->tracks[selected].id);
        launch(selected);
    }
    m_held[size_t(pad)] = std::move(held);
    refreshLights();
    return true;
}

std::array<NektarPads::Light, 8> NektarPads::lights(qint64 milliseconds) const {
    std::array<Light, 8> result{};
    const int button = m_mode == Mode::Scenes ? 1 : 0;
    const bool selecting = m_buttonDown[button];
    const bool blink = (milliseconds / 180) % 2 != 0;
    const bool sceneBlink = std::fmod(m_transport->beat(), 1.0) < .5;
    for (int pad = 0; pad < 8; ++pad) {
        if (selecting) {
            const bool available = pad * 8 < m_project->scenes.size();
            result[size_t(pad)] = {available ? 15 : 12, pad == bank()};
            continue;
        }
        const int scene = bank() * 8 + pad;
        if (scene >= m_project->scenes.size())
            continue;
        bool content = false, playing = false, queued = false, recording = false;
        auto include = [&](int index) {
            const Track& t = m_project->tracks[index];
            content |= bool(t.clipSlots.value(scene));
            playing |= m_transport->playing() && t.playingSlot == scene;
            queued |= t.queuedSlot == scene || (t.playingSlot == scene && t.queuedSlot == -1);
            recording |= m_transport->isRecordingSlot(index, scene);
        };
        if (m_mode == Mode::Scenes) {
            for (int i = 0; i < m_project->tracks.size(); ++i)
                if (!m_project->tracks[i].isGroup())
                    include(i);
        } else if (m_mode == Mode::Clips && m_project->selectedTrack >= 0 &&
                   m_project->selectedTrack < m_project->tracks.size()) {
            const int index = m_project->selectedTrack;
            if (m_project->tracks[index].isGroup())
                for (int member : m_project->membersOf(index))
                    include(member);
            else
                include(index);
        }
        // LX+ palette IDs encode the off/on colour pair: yellow/green=5,
        // off/green=3, yellow/red=8, off/red=6, yellow/orange=15.
        result[size_t(pad)].palette = recording ? (content ? 8 : 6) : (content ? 5 : 3);
        result[size_t(pad)].on = recording || (queued ? blink : playing && (m_mode == Mode::Clips || sceneBlink));
    }
    return result;
}

void NektarPads::refreshLights(bool all, qint64 milliseconds) {
    if (!connected() || !m_send)
        return;
    const qint64 now = milliseconds < 0 ? QDateTime::currentMSecsSinceEpoch() : milliseconds;
    if (now < m_retryAt)
        return;
    if (!initialize()) {
        m_retryAt = now + 1000;
        return;
    }
    bool failed = false;
    const bool reset = all || m_shownMode != int(m_mode);
    if (reset) {
        if (!send(padModeMessage(m_mode != Mode::Notes)) ||
            !send(packet({0xbf, 117, m_mode == Mode::Clips ? 127 : 0})) ||
            !send(packet({0xbf, 118, m_mode == Mode::Scenes ? 127 : 0})))
            failed = true;
        else
            m_shownMode = int(m_mode);
    }
    if (m_mode != Mode::Notes && !failed) {
        const auto wanted = lights(now);
        bool palettesChanged = reset;
        for (int i = 0; i < 8; ++i)
            palettesChanged |= wanted[size_t(i)].palette != m_shown[size_t(i)].palette;
        std::array<int, 8> palettes{};
        for (int i = 0; i < 8; ++i)
            palettes[size_t(i)] = wanted[size_t(i)].palette;
        if (palettesChanged && !send(paletteMessage(palettes)))
            failed = true;
        if (!failed)
            for (int i = 0; i < 8; ++i) {
                const Light light = wanted[size_t(i)];
                if (all || palettesChanged || light.on != m_shown[size_t(i)].on) {
                    if (!send(packet({0x9f, notes[i], light.on ? 1 : 0}))) {
                        failed = true;
                        break;
                    }
                }
            }
        if (!failed)
            m_shown = wanted;
    }
    if (failed) {
        m_shownMode = -1;
        m_initialized = false;
        m_retryAt = now + 1000;
    } else
        m_retryAt = 0;
}
} // namespace sessions
