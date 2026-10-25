#include "ControlSurface.h"
#include "NektarPads.h"
#include "Mapping.h"
#include "Model.h"
#include "Transport.h"
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace sessions {
ControlSurface::ControlSurface(Project* project, Transport* transport, Mapper* mapper, QObject* parent)
    : QObject(parent), m_pads(new NektarPads(project, transport, this)), m_project(project), m_transport(transport), m_mapper(mapper) { reloadSettings(); }

void ControlSurface::reloadSettings() {
    QSettings s("Sessions", "Sessions");
    m_enabled = s.value("midi/controller", "auto").toString() != "off";
    m_source = s.value("midi/controllerInput").toString();
    m_bank = std::clamp(s.value("midi/controllerBank", 0).toInt(), 0, 56);
    m_pickup = s.value("midi/controllerPickup", true).toBool();
    m_controls.clear();
    m_buttons.clear();
    m_lastDevice = 0;
    m_pads->reloadSettings();
}

bool ControlSurface::isNektarImpact(const QString& source) {
    static const QRegularExpression name("\\b(?:Impact\\s*)?LX\\s*(?:49|61)\\s*(?:\\+|Plus\\b)", QRegularExpression::CaseInsensitiveOption);
    return name.match(source).hasMatch();
}

bool ControlSurface::accepts(const QString& source) const {
    return m_enabled && !source.isEmpty() && (m_source.isEmpty() ? isNektarImpact(source) : source == m_source);
}

bool ControlSurface::continuous(const QString& source, int channel, int cc, const QString& target, double current, double value) {
    Pickup& state = m_controls[source + QString(":%1:%2:").arg(channel).arg(cc) + target];
    // UI edits, automation, undo and a new set must all re-arm pickup.
    if (state.applied >= 0 && std::abs(current - state.applied) > 1e-6) { state.caught = false; state.previous = -1; }
    const int position = int(std::lround(value * 127)), wanted = int(std::lround(current * 127));
    if (!m_pickup || std::abs(position - wanted) <= 1
        || (state.previous >= 0 && (state.previous - wanted) * (position - wanted) <= 0)) state.caught = true;
    state.previous = position;
    state.applied = current;
    if (state.caught) { m_mapper->setValue(target, value); state.applied = value; }
    return true; // an uncaught fader must not leak into the instrument
}

bool ControlSurface::controlChange(const QString& source, int channel, int cc, int value) {
    if (m_pads->controlChange(source, channel, cc, value)) return true;
    if (!accepts(source) || channel < 0 || channel > 15 || value < 0 || value > 127) return false;
    // The dedicated read-only Mixer/Instrument presets use fixed channel 16.
    // Assignments follow Nektar's Ardour/Mixbus map, without a firmware handshake.
    if (channel == 15) {
        if (cc == 20) return continuous(source, channel, cc, "main:volume", m_project->mainVolume, value / 127.0);
        if ((cc >= 12 && cc <= 19) || (cc >= 30 && cc <= 37)) {
            const int index = m_bank + cc - (cc < 30 ? 12 : 30);
            if (index >= m_project->tracks.size()) return true;
            const Track& track = m_project->tracks[index];
            const bool pan = cc >= 30;
            return continuous(source, channel, cc, QString("track:%1:").arg(track.id) + (pan ? "pan" : "volume"),
                              pan ? (track.pan + 1) / 2 : track.volume, value / 127.0);
        }
        if (cc >= 56 && cc <= 63) {
            const Track* track = m_project->trackAt(m_project->selectedTrack);
            if (!track || track->devices.isEmpty()) return true;
            const int chosen = m_deviceChoice ? m_deviceChoice() : 0;
            const Device* device = nullptr;
            for (const Device& d : track->devices) if (d.id == chosen) device = &d;
            if (!device) for (const Device& d : track->devices)
                if (d.kind == DeviceKind::Instrument) { device = &d; break; }
            if (!device) device = &track->devices.first();
            if (device->id != m_lastDevice) { m_controls.clear(); m_lastDevice = device->id; }
            int param = cc - 56;
            // Racks expose their labelled Macro knobs before their other parameters.
            for (int p = 0; p < device->params.size(); ++p)
                if (device->params[p].name.compare(QString("Macro %1").arg(cc - 55), Qt::CaseInsensitive) == 0) { param = p; break; }
            if (param < device->params.size())
                continuous(source, channel, cc, QString("device:%1:%2:%3").arg(track->id).arg(device->id).arg(param),
                           device->params[param].value, value / 127.0);
            return true;
        }
        if ((cc >= 21 && cc <= 29) || (cc >= 102 && cc <= 107) || cc == 109 || cc == 110) {
            const QString key = source + QString(":%1").arg(cc);
            const bool down = value > 0, pressed = down && !m_buttons.value(key);
            m_buttons[key] = down;
            if (!pressed) return true;
            if (cc == 109 || cc == 110) {
                m_bank = std::clamp(m_bank + (cc == 109 ? -8 : 8), 0, 56);
                QSettings("Sessions", "Sessions").setValue("midi/controllerBank", m_bank);
                m_controls.clear();
            } else if (cc >= 21 && cc <= 28) {
                const int index = m_bank + cc - 21;
                if (index < m_project->tracks.size()) {
                    const Track& t = m_project->tracks[index];
                    m_mapper->trigger(QString("track:%1:active").arg(t.id), true);
                }
            } else if (cc == 29) {
                if (const Track* t = m_project->trackAt(m_project->selectedTrack))
                    m_mapper->trigger(QString("track:%1:arm").arg(t->id), true);
            } else if (cc == 102) m_mapper->trigger("transport:loop", true);
            else if (cc == 103) machineControl(source, 5);
            else if (cc == 104) machineControl(source, 4);
            else if (cc == 105) machineControl(source, 1);
            else if (cc == 106) machineControl(source, 2);
            else if (cc == 107) m_transport->setArrangementRecord(!m_transport->arrangementRecord());
            return true;
        }
    }
    if (channel == 15 && cc == 7) return continuous(source, channel, cc, "main:volume", m_project->mainVolume, value / 127.0);
    if (channel > 7 || (cc != 7 && cc != 10 && cc != 12)) return false;
    const int index = m_bank + channel;
    if (index >= m_project->tracks.size()) return true;
    Track& track = m_project->tracks[index];
    const QString base = QString("track:%1:").arg(track.id);
    if (cc == 7) return continuous(source, channel, cc, base + "volume", track.volume, value / 127.0);
    if (cc == 10) return continuous(source, channel, cc, base + "pan", (track.pan + 1) / 2, value / 127.0);
    const bool active = value < 64; // factory mute buttons alternate 127 and 0
    if (track.active != active) {
        EditScope edit(m_project, active ? "Unmute Track" : "Mute Track");
        track.active = active;
        emit m_project->mixerChanged();
    }
    return true;
}

bool ControlSurface::machineControl(const QString& source, int command) {
    if (!accepts(source)) return false;
    switch (command) {
    case 0x01: case 0x09: // Stop / Pause: repeated commands do not rewind the song
        if (m_transport->playing()) m_transport->stop();
        return true;
    case 0x02: case 0x03: // Play / Deferred Play
        if (!m_transport->playing()) m_transport->continuePlay();
        return true;
    case 0x04: m_transport->setBeat(m_transport->beat() + 4); return true;
    case 0x05: m_transport->setBeat(std::max(0.0, m_transport->beat() - 4)); return true;
    case 0x06:
        m_transport->setArrangementRecord(true);
        if (!m_transport->playing()) m_transport->continuePlay();
        return true;
    case 0x07: m_transport->setArrangementRecord(false); return true;
    case 0x08: m_transport->setArrangementRecord(true); return true;
    default: return false;
    }
}
}
