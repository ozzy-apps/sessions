#include "ControlSurface.h"
#include "Mapping.h"
#include "Model.h"
#include "Transport.h"
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace sessions {
ControlSurface::ControlSurface(Project* project, Transport* transport, Mapper* mapper, QObject* parent)
    : QObject(parent), m_project(project), m_transport(transport), m_mapper(mapper) { reloadSettings(); }

void ControlSurface::reloadSettings() {
    QSettings s("Sessions", "Sessions");
    m_enabled = s.value("midi/controller", "auto").toString() != "off";
    m_source = s.value("midi/controllerInput").toString();
    m_bank = std::clamp(s.value("midi/controllerBank", 0).toInt(), 0, 56);
    m_pickup = s.value("midi/controllerPickup", true).toBool();
    m_controls.clear();
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
    if (!accepts(source) || channel < 0 || channel > 15 || value < 0 || value > 127) return false;
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
