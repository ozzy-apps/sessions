#include "Mapping.h"
#include "Import.h"
#include "Model.h"
#include "Transport.h"
#include "Scales.h"
#include <QKeySequence>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace mapping {

bool isContinuous(const QString& target) {
    const QStringList p = target.split(':');
    if (p.value(0) == "track") return p.value(2) == "volume" || p.value(2) == "pan" || p.value(2) == "send";
    if (p.value(0) == "device") return p.value(3) != "on" && p.value(3) != "next" && p.value(3) != "prev" && p.value(3) != "irnext" && p.value(3) != "irprev";
    return target == "main:volume" || target == "main:cue" || target == "tempo";
}

QString sourceText(const Mapping& m) {
    const QString ch = m.channel ? QString(" ch%1").arg(m.channel + 1) : QString();
    switch (m.source) {
    case Mapping::MidiCC:   return QString("CC %1").arg(m.number) + ch;
    case Mapping::MidiNote: return QString("%1%2").arg(noteName(m.number)).arg(m.number / 12 - 2) + ch;
    case Mapping::Key:      return QKeySequence(m.number).toString(QKeySequence::NativeText);
    }
    return {};
}

QString targetName(const Project& p, const QString& target) {
    const QStringList s = target.split(':');
    auto track = [&p](int id) { const int i = p.indexOfTrack(id); return i >= 0 ? p.trackAt(i)->name : QString("(deleted track)"); };
    if (s.value(0) == "track") {
        const QString what = s.value(2);
        const QString part = what == "volume" ? "Volume" : what == "pan" ? "Pan" : what == "send" ? "Send " + QString(QChar('A' + s.value(3).toInt()))
                           : what == "active" ? "Activator" : what == "solo" ? "Solo" : "Arm";
        return track(s.value(1).toInt()) + " · " + part;
    }
    if (s.value(0) == "device") {
        const int ti = p.indexOfTrack(s.value(1).toInt());
        const Track* t = p.trackAt(ti);
        if (t) for (const Device& d : t->devices)
            if (d.id == s.value(2).toInt())
                return d.name + " · " + (s.value(3) == "on" ? QString("On / Off") : s.value(3) == "next" ? QString("Next Capture")
                                       : s.value(3) == "prev" ? QString("Previous Capture") : s.value(3) == "irnext" ? QString("Next IR")
                                       : s.value(3) == "irprev" ? QString("Previous IR") : d.params.value(s.value(3).toInt()).name);
        return "(deleted device)";
    }
    if (s.value(0) == "slot") return track(s.value(1).toInt()) + QString(" · Clip Slot %1").arg(s.value(2).toInt() + 1);
    if (s.value(0) == "scene") return QString("Scene %1").arg(s.value(1).toInt() + 1);
    if (s.value(0) == "stop") return track(s.value(1).toInt()) + " · Stop";
    if (target == "stopall") return "Stop All Clips";
    if (target == "main:volume") return "Main Volume";
    if (target == "main:cue") return "Cue Volume";
    if (target == "tempo") return "Tempo";
    if (s.value(0) == "transport") { QString w = s.value(1); w[0] = w[0].toUpper(); return w; }
    return target;
}

} // namespace mapping

Mapper::Mapper(Project* project, Transport* transport, QObject* parent)
    : QObject(parent), m_project(project), m_transport(transport) {}

int Mapper::trackIndex(int id) const { return m_project->indexOfTrack(id); }

void Mapper::setMode(Mode m) {
    m_mode = m;
    if (m == Mode::Off) m_selected.clear();
    emit changed();
}

QVector<Mapping> Mapper::mappingsFor(const QString& target) const {
    QVector<Mapping> out;
    for (const Mapping& m : m_project->mappings) if (m.target == target) out.push_back(m);
    return out;
}

void Mapper::removeMappings(const QString& target) {
    auto& ms = m_project->mappings;
    ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const Mapping& m) { return m.target == target; }), ms.end());
    emit changed();
}

// One source drives one target, and a control has at most one MIDI and one key mapping.
void Mapper::learn(const Mapping& m) {
    auto& ms = m_project->mappings;
    const bool isKey = m.source == Mapping::Key;
    ms.erase(std::remove_if(ms.begin(), ms.end(), [&](const Mapping& o) {
        return o.sameSource(m) || (o.target == m.target && (o.source == Mapping::Key) == isKey);
    }), ms.end());
    ms.push_back(m);
    emit changed();
}

bool Mapper::looksRelative(const QVector<int>& values) {
    if (values.size() < 2) return false;
    bool repeats = false, up = false, down = false;
    for (int i = 0; i < values.size(); ++i) {
        const int v = values[i];
        if (v >= 1 && v <= 15) up = true;
        else if (v >= 113 && v <= 127) down = true;
        else return false;   // an absolute knob passes through the middle
        if (i > 0 && values[i - 1] == v) repeats = true;   // an absolute knob never sends the same value twice running
    }
    return repeats || (up && down);
}

bool Mapper::controlChange(int channel, int cc, int ccValue, bool relative) {
    const int key = channel << 8 | cc;
    if (m_mode == Mode::Midi && !m_selected.isEmpty()) {
        if (key != m_learnKey || m_selected != m_learnTarget) { m_learnKey = key; m_learnTarget = m_selected; m_learnValues.clear(); }
        m_learnValues.push_back(ccValue);
        Mapping m{Mapping::MidiCC, channel, cc, m_selected};
        m.relative = mapping::isContinuous(m_selected) && (relative || looksRelative(m_learnValues));
        learn(m);
        return true;
    }
    const int last = m_lastCC.value(key, 0);
    m_lastCC.insert(key, ccValue);
    bool handled = false;
    const QVector<Mapping> ms = m_project->mappings;
    for (const Mapping& m : ms) {
        if (m.source != Mapping::MidiCC || m.channel != channel || m.number != cc) continue;
        handled = true;
        if (mapping::isContinuous(m.target) && (m.relative || relative)) {   // an endless knob steps the value (also a map learned as absolute)
            const int delta = ccValue < 64 ? ccValue : ccValue - 128;
            setValue(m.target, value(m.target) + delta / 127.0);
        } else if (mapping::isContinuous(m.target)) setValue(m.target, ccValue / 127.0);
        else if (ccValue >= 64 && last < 64) trigger(m.target, true);
        else if (ccValue < 64 && last >= 64) trigger(m.target, false);
    }
    return handled;
}

bool Mapper::note(int channel, int pitch, bool on, int velocity) {
    if (m_mode == Mode::Midi && !m_selected.isEmpty()) {
        if (on) learn({Mapping::MidiNote, channel, pitch, m_selected});
        return true;
    }
    bool handled = false;
    const QVector<Mapping> ms = m_project->mappings;
    for (const Mapping& m : ms) {
        if (m.source != Mapping::MidiNote || m.channel != channel || m.number != pitch) continue;
        handled = true;
        if (mapping::isContinuous(m.target)) { if (on) setValue(m.target, velocity / 127.0); }   // a pad sets it by how hard you hit
        else trigger(m.target, on);
    }
    return handled;
}

bool Mapper::key(int qtKey, bool press) {
    if (m_mode == Mode::Key) {
        if (press && !m_selected.isEmpty()) { learn({Mapping::Key, 0, qtKey, m_selected}); return true; }
        return false;
    }
    bool handled = false;
    const QVector<Mapping> ms = m_project->mappings;
    for (const Mapping& m : ms) {
        if (m.source != Mapping::Key || m.number != qtKey || mapping::isContinuous(m.target)) continue;
        handled = true;
        trigger(m.target, press);
    }
    return handled;
}

void Mapper::setValue(const QString& tgt, double v) {
    v = std::clamp(v, 0.0, 1.0);
    const QStringList p = tgt.split(':');
    if (p.value(0) == "track") {
        const int i = trackIndex(p.value(1).toInt());
        Track* t = m_project->trackAt(i);
        if (!t) return;
        const QString what = p.value(2);
        if (what == "volume") {
            EditScope e(m_project, "Volume", mixerMergeKey(t->id, 1));
            t->volume = v;
            m_project->touchTarget(i, target::volume());
        } else if (what == "pan") {
            EditScope e(m_project, "Pan", mixerMergeKey(t->id, 2));
            t->pan = v * 2 - 1;
            m_project->touchTarget(i, target::pan());
        } else if (what == "send") {
            const int k = p.value(3).toInt();
            if (k >= t->sends.size()) return;
            EditScope e(m_project, "Send " + QString(QChar('A' + k)), mixerMergeKey(t->id, 10 + k));
            t->sends[k] = v;
            m_project->touchTarget(i, target::send(k));
        } else {
            return;
        }
        emit m_project->mixerChanged();
    } else if (p.value(0) == "device") {
        const int i = trackIndex(p.value(1).toInt());
        Track* t = m_project->trackAt(i);
        if (!t) return;
        const int id = p.value(2).toInt(), param = p.value(3).toInt();
        for (int j = 0; j < t->devices.size(); ++j) {
            if (t->devices.at(j).id != id || param >= t->devices.at(j).params.size()) continue;
            // Index after the snapshot: a reference taken before it would write into the snapshot too.
            EditScope e(m_project, "Change " + t->devices.at(j).params[param].name, deviceMergeKey(id, param));
            t->devices[j].params[param].value = v;
            m_project->touchTarget(i, target::device(id, param));
            emit m_project->deviceChanged();
        }
    } else if (tgt == "main:volume") {
        EditScope e(m_project, "Main Volume", mixerMergeKey(0, 1));
        m_project->mainVolume = v;
        emit m_project->mixerChanged();
    } else if (tgt == "main:cue") {
        m_project->cueVolume = v;   // like the knob: not undoable
        emit m_project->mixerChanged();
    } else if (tgt == "tempo") {
        EditScope e(m_project, "Tempo", mixerMergeKey(0, 99));
        m_project->setTempo(60 + std::round(v * 127));   // the CC's 128 steps: 60 .. 187 BPM
    }
}

double Mapper::value(const QString& tgt) const {
    const QStringList p = tgt.split(':');
    if (p.value(0) == "track" || p.value(0) == "device") {
        const Track* t = m_project->trackAt(trackIndex(p.value(1).toInt()));
        if (!t) return 0;
        if (p.value(0) == "device") {
            for (const Device& d : t->devices) if (d.id == p.value(2).toInt()) return d.params.value(p.value(3).toInt()).value;
            return 0;
        }
        if (p.value(2) == "volume") return t->volume;
        if (p.value(2) == "pan") return (t->pan + 1) / 2;
        if (p.value(2) == "send") return t->sends.value(p.value(3).toInt());
        return 0;
    }
    if (tgt == "main:volume") return m_project->mainVolume;
    if (tgt == "main:cue") return m_project->cueVolume;
    if (tgt == "tempo") return std::clamp((m_project->tempo - 60) / 127.0, 0.0, 1.0);
    return 0;
}

void Mapper::trigger(const QString& tgt, bool on) {
    const QStringList p = tgt.split(':');
    const QString kind = p.value(0);
    if (kind == "slot") {
        const int i = trackIndex(p.value(1).toInt());
        if (i < 0 || i >= m_project->tracks.size()) return;
        const int scene = p.value(2).toInt();
        const bool filled = scene < m_project->tracks[i].clipSlots.size() && m_project->tracks[i].clipSlots[scene];
        if (on) { if (filled) m_transport->launchSlot(i, scene); else m_transport->stopTrack(i); }
        else if (filled) m_transport->releaseSlot(i, scene);   // Gate / Repeat clips
        return;
    }
    if (!on) return;   // everything else acts on press
    if (kind == "track") {
        const int i = trackIndex(p.value(1).toInt());
        Track* t = m_project->trackAt(i);
        if (!t) return;
        const QString what = p.value(2);
        if (what == "active") {
            EditScope e(m_project, t->active ? "Track Off" : "Track On");
            t->active = !t->active;
        } else if (what == "solo" && i < m_project->tracks.size()) {
            t->solo = !t->solo;   // solo isn't undoable
        } else if (what == "arm" && i < m_project->tracks.size()) {
            t->armed = !t->armed;
            emit m_project->structureChanged();
        } else {
            return;
        }
        emit m_project->mixerChanged();
    } else if (kind == "device" && (p.value(3) == "irnext" || p.value(3) == "irprev")) {   // the next / previous IR in the library
        const int i = trackIndex(p.value(1).toInt());
        EditScope e(m_project, p.value(3) == "irnext" ? "Next IR" : "Previous IR");
        import::stepIr(*m_project, i, p.value(2).toInt(), p.value(3) == "irnext" ? 1 : -1);
    } else if (kind == "device" && (p.value(3) == "next" || p.value(3) == "prev")) {   // NAM: the next / previous capture in the library
        const int i = trackIndex(p.value(1).toInt());
        EditScope e(m_project, p.value(3) == "next" ? "Next Capture" : "Previous Capture");
        import::stepCapture(*m_project, i, p.value(2).toInt(), p.value(3) == "next" ? 1 : -1);
    } else if (kind == "device") {
        Track* t = m_project->trackAt(trackIndex(p.value(1).toInt()));
        if (!t) return;
        for (int j = 0; j < t->devices.size(); ++j) {
            if (t->devices.at(j).id != p.value(2).toInt()) continue;
            EditScope e(m_project, t->devices.at(j).enabled ? "Device Off" : "Device On");
            t->devices[j].enabled = !t->devices.at(j).enabled;
            emit m_project->deviceChanged();
            emit m_project->structureChanged();
        }
    } else if (kind == "scene") {
        m_transport->launchScene(p.value(1).toInt());
    } else if (kind == "stop") {
        const int i = trackIndex(p.value(1).toInt());
        if (i >= 0 && i < m_project->tracks.size()) m_transport->stopTrack(i);
    } else if (tgt == "stopall") {
        m_transport->stopAll();
    } else if (tgt == "transport:play") {
        m_transport->togglePlay();
    } else if (tgt == "transport:stop") {
        m_transport->stop();
    } else if (tgt == "transport:record") {
        m_transport->setArrangementRecord(!m_transport->arrangementRecord());
    } else if (tgt == "transport:loop") {
        m_transport->setLoopEnabled(!m_transport->loopEnabled());
    } else if (tgt == "transport:metronome") {
        m_transport->setMetronome(!m_transport->metronome());
    }
}

} // namespace sessions
