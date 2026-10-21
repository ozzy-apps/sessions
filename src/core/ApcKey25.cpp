#include "ApcKey25.h"
#include "Mapping.h"
#include "Model.h"
#include "Transport.h"
#include <QColor>
#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <cmath>

namespace sessions {

namespace {
// The mk2's pad palette (velocity -> colour): the bright entries, for the nearest match to a clip's colour.
struct Swatch { int velocity; int r, g, b; };
const Swatch kPalette[] = {
    {3, 255, 255, 255}, {4, 255, 76, 76}, {5, 255, 0, 0}, {8, 255, 189, 108}, {9, 255, 84, 0}, {12, 255, 255, 76},
    {13, 255, 255, 0}, {16, 136, 255, 76}, {17, 84, 255, 0}, {20, 76, 255, 76}, {21, 0, 255, 0}, {25, 0, 255, 25},
    {28, 76, 255, 136}, {29, 0, 255, 85}, {32, 76, 255, 183}, {33, 0, 255, 153}, {36, 76, 195, 255}, {37, 0, 169, 255},
    {40, 76, 136, 255}, {41, 0, 85, 255}, {44, 76, 76, 255}, {45, 0, 0, 255}, {48, 135, 76, 255}, {49, 84, 0, 255},
    {52, 255, 76, 255}, {53, 255, 0, 255}, {56, 255, 76, 135}, {57, 255, 0, 84}, {60, 255, 21, 0}, {61, 153, 53, 0},
    {62, 121, 81, 0}, {63, 67, 100, 0}, {96, 255, 127, 0}, {109, 255, 225, 38}, {113, 220, 255, 107}, {114, 128, 255, 189},
    {115, 154, 153, 255}, {116, 142, 102, 255},
};
constexpr int kGreen = 21, kRed = 5;
// mk2 brightness / behaviour channels used here (solid levels; blinking and pulsing are drawn by us, in time).
constexpr int kFull = 6, kBright = 3, kHalf = 2, kDim = 1;
// The 2014 model's pad colours.
constexpr int kClassicGreen = 1, kClassicRed = 3, kClassicAmber = 5;

int nearestSwatch(QColor c) {
    if (!c.isValid()) return kPalette[0].velocity;
    float h, s, v, a;
    c.getHsvF(&h, &s, &v, &a);
    if (s < 0.18f) return 3;                     // greys: white
    c = QColor::fromHsvF(h < 0 ? 0 : h, std::max(s, 0.55f), 1.0f);   // lights are bright: match the hue
    int best = kPalette[0].velocity; long bestD = -1;
    for (const Swatch& sw : kPalette) {
        const long dr = c.red() - sw.r, dg = c.green() - sw.g, db = c.blue() - sw.b;
        const long d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (bestD < 0 || d < bestD) { bestD = d; best = sw.velocity; }
    }
    return best;
}

const QRegularExpression& keysName() {
    static const QRegularExpression re("(\\bKeys?|\\bMIDI\\s*1|\\bPort\\s*1|\\bK)\\s*$", QRegularExpression::CaseInsensitiveOption);
    return re;
}
const QRegularExpression& controlsName() {
    static const QRegularExpression re("(\\bContr\\w*|\\bMIDI\\s*2|\\bPort\\s*2|\\bC)\\s*$", QRegularExpression::CaseInsensitiveOption);
    return re;
}

const ApcKey25::Buttons kClassicModes[] = {ApcKey25::Buttons::ClipStop, ApcKey25::Buttons::Solo, ApcKey25::Buttons::Arm, ApcKey25::Buttons::Mute, ApcKey25::Buttons::Select};
const ApcKey25::Buttons kMk2Modes[] = {ApcKey25::Buttons::ClipStop, ApcKey25::Buttons::Solo, ApcKey25::Buttons::Mute, ApcKey25::Buttons::Arm, ApcKey25::Buttons::Select};

QString buttonsName(ApcKey25::Buttons b) {
    switch (b) {
    case ApcKey25::Buttons::ClipStop: return "Clip Stop";
    case ApcKey25::Buttons::Solo: return "Solo";
    case ApcKey25::Buttons::Arm: return "Rec Arm";
    case ApcKey25::Buttons::Mute: return "Mute";
    case ApcKey25::Buttons::Select: return "Select";
    }
    return {};
}
}

ApcKey25::ApcKey25(Project* project, Transport* transport, Mapper* mapper, QObject* parent)
    : QObject(parent), m_project(project), m_transport(transport), m_mapper(mapper) {
    QSettings s("Sessions", "Sessions");
    m_knobs = Knobs(std::clamp(s.value("midi/apcKnobs", int(Knobs::Device)).toInt(), 0, 3));
    m_started = QDateTime::currentMSecsSinceEpoch();
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, [this] { refreshLights(); });
}

ApcKey25::~ApcKey25() = default;

ApcKey25::Model ApcKey25::modelOf(const QString& portName) {
    static const QRegularExpression apc("\\bAPC\\s*Key\\s*25\\b", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression mk2("\\bmk\\s*(?:2|ii)\\b", QRegularExpression::CaseInsensitiveOption);
    if (!apc.match(portName).hasMatch()) return Model::None;
    return mk2.match(portName).hasMatch() ? Model::Mk2 : Model::Classic;
}

void ApcKey25::setEnabled(bool on) {
    if (on == m_enabled) return;
    if (!on) darken();
    m_enabled = on;
    m_shift = false;
    m_held.clear();
    if (connected()) { m_timer.start(); refreshLights(true); } else m_timer.stop();
    emit boxChanged();
}

void ApcKey25::setPorts(const QVector<Port>& ports) {
    const QString before = m_controls;
    m_controls.clear();
    m_keys.clear();
    m_model = Model::None;
    // An APC's ports, by client, in port order.
    QMap<int, QMap<int, QString>> clients;
    for (const Port& p : ports) {
        if (modelOf(p.name) == Model::None) continue;
        const QStringList id = p.id.split(':');
        clients[id.value(0).toInt()][id.value(1).toInt()] = p.name;
    }
    for (auto c = clients.cbegin(); c != clients.cend() && m_controls.isEmpty(); ++c) {
        const QList<QString> names = c->values();
        QString controls;
        if (names.size() == 1) controls = names[0];   // the 2014 model: one port
        else {
            for (const QString& n : names) if (controlsName().match(n).hasMatch()) controls = n;
            if (controls.isEmpty()) for (const QString& n : names) if (!keysName().match(n).hasMatch() && n != names.first()) { controls = n; break; }
            if (controls.isEmpty()) controls = names.value(1);
            for (const QString& n : names) if (n != controls) m_keys.insert(n);
        }
        m_controls = controls;
        m_model = modelOf(controls);
        if (m_model == Model::Classic && names.size() > 1) m_model = Model::Mk2;   // two ports: the mk2, whatever it's called
    }
    if (m_controls != before) {
        m_lit.clear();
        m_shift = false;
        m_held.clear();
        if (connected()) { m_timer.start(); refreshLights(true); } else m_timer.stop();
        emit boxChanged();
    }
}

ApcKey25::Role ApcKey25::role(const QString& source) const {
    if (!m_enabled || source.isEmpty()) return Role::None;
    if (source == m_controls) return Role::Controls;
    if (m_keys.contains(source)) return Role::Keys;
    const Model m = modelOf(source);   // not (yet) among the ports: by its name
    if (m == Model::None) return Role::None;
    if (m == Model::Mk2 && keysName().match(source).hasMatch()) return Role::Keys;
    return Role::Controls;
}

bool ApcKey25::isRelativeKnob(const QString& source, int channel, int cc) const {
    if (role(source) != Role::Controls || channel != 0 || cc < kKnob || cc >= kKnob + kColumns) return false;
    const Model m = source == m_controls ? m_model : modelOf(source);
    return m == Model::Mk2;
}

// ---- the box -------------------------------------------------------------

QVector<int> ApcKey25::visibleTracks() const {
    QVector<int> out;
    for (int i = 0; i < m_project->tracks.size(); ++i) if (!m_project->hiddenByFold(i)) out.push_back(i);
    return out;
}

int ApcKey25::firstScene() const { return std::clamp(m_y, 0, std::max(0, int(m_project->scenes.size()) - kRows)); }

QVector<int> ApcKey25::boxTracks() const {
    const QVector<int> shown = visibleTracks();
    QVector<int> out(kColumns, -1);
    const int x = std::clamp(m_x, 0, std::max(0, int(shown.size()) - kColumns));
    for (int k = 0; k < kColumns; ++k) out[k] = shown.value(x + k, -1);
    return out;
}

bool ApcKey25::canMove(int tracks, int scenes) const {
    const int maxX = std::max(0, int(visibleTracks().size()) - kColumns), maxY = std::max(0, int(m_project->scenes.size()) - kRows);
    const int x = std::clamp(m_x, 0, maxX), y = std::clamp(m_y, 0, maxY);
    return std::clamp(x + tracks, 0, maxX) != x || std::clamp(y + scenes, 0, maxY) != y;
}

void ApcKey25::moveBox(int tracks, int scenes) {
    const int maxX = std::max(0, int(visibleTracks().size()) - kColumns), maxY = std::max(0, int(m_project->scenes.size()) - kRows);
    const int x = std::clamp(std::clamp(m_x, 0, maxX) + tracks, 0, maxX), y = std::clamp(std::clamp(m_y, 0, maxY) + scenes, 0, maxY);
    if (x == m_x && y == m_y) return;
    m_x = x; m_y = y;
    m_held.clear();
    emit boxChanged();
    refreshLights();
}

// ---- input ---------------------------------------------------------------

namespace {
bool controlNote(int channel, int pitch) {
    return channel == 0 && ((pitch >= 0 && pitch < ApcKey25::kColumns * ApcKey25::kRows)
                            || (pitch >= ApcKey25::kTrackButton && pitch < ApcKey25::kTrackButton + ApcKey25::kColumns)
                            || pitch == ApcKey25::kStopAll || (pitch >= ApcKey25::kSceneButton && pitch < ApcKey25::kSceneButton + ApcKey25::kRows)
                            || pitch == ApcKey25::kPlay || pitch == ApcKey25::kRecord || pitch == ApcKey25::kShift);
}
}

bool ApcKey25::takesNote(const QString& source, int channel, int pitch) const {
    if (role(source) != Role::Controls) return false;
    const bool mk2Port = source == m_controls ? m_model == Model::Mk2 : modelOf(source) == Model::Mk2;
    return mk2Port || controlNote(channel, pitch);   // the mk2's controls port plays no notes; the 2014 model's other notes do
}

bool ApcKey25::note(const QString& source, int channel, int pitch, int velocity, bool on) {
    if (!takesNote(source, channel, pitch)) return false;
    if (!controlNote(channel, pitch)) return true;
    on = on && velocity > 0;
    if (pitch == kShift) { m_shift = on; refreshLights(); return true; }
    if (pitch < kColumns * kRows) pad(pitch % kColumns, kRows - 1 - pitch / kColumns, on);
    else if (pitch >= kTrackButton && pitch < kTrackButton + kColumns) trackButton(pitch - kTrackButton, on);
    else if (!on) return true;
    else if (pitch == kStopAll) m_transport->stopAll();
    else if (pitch >= kSceneButton && pitch < kSceneButton + kRows) sceneButton(pitch - kSceneButton);
    else if (pitch == kPlay) m_transport->togglePlay();
    else if (pitch == kRecord) m_transport->setArrangementRecord(!m_transport->arrangementRecord());
    refreshLights();
    return true;
}

bool ApcKey25::controlChange(const QString& source, int channel, int cc, int value) {
    if (role(source) != Role::Controls || value < 0 || value > 127) return false;
    const bool mk2Port = source == m_controls ? m_model == Model::Mk2 : modelOf(source) == Model::Mk2;
    if (channel != 0 || cc < kKnob || cc >= kKnob + kColumns) return mk2Port;
    knob(cc - kKnob, value, mk2Port);
    return true;
}

void ApcKey25::pad(int column, int row, bool on) {
    const int note = (kRows - 1 - row) * kColumns + column;
    if (!on) {
        if (m_held.contains(note)) { const int v = m_held.take(note); m_transport->releaseSlot(v >> 12, v & 0xFFF); }
        return;
    }
    const int t = boxTracks().value(column, -1), scene = firstScene() + row;
    if (t < 0 || scene < 0 || scene >= m_project->scenes.size()) return;
    const Track& track = m_project->tracks[t];
    const bool filled = scene < track.clipSlots.size() && track.clipSlots[scene];
    if (m_shift) { if (filled) emit clipChosen(t, scene); return; }
    if (track.isGroup()) {   // like the Session View's group slot: its tracks' clips in that scene, the others stop
        for (int m : m_project->membersOf(t)) {
            const Track& member = m_project->tracks[m];
            if (scene < member.clipSlots.size() && member.clipSlots[scene]) m_transport->launchSlot(m, scene);
            else m_transport->stopTrack(m);
        }
        return;
    }
    if (filled) { m_transport->launchSlot(t, scene); m_held.insert(note, t << 12 | scene); }
    else if (track.armed) m_transport->recordSlot(t, scene);
    else m_transport->stopTrack(t);
}

void ApcKey25::trackButton(int i, bool on) {
    if (!on) return;
    if (m_shift) {
        if (i < 4) {
            static const int dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
            moveBox(dx[i], dy[i]);
            return;
        }
        const Knobs k = Knobs(i - 4);
        if (k == Knobs::Send && m_knobs == Knobs::Send) {   // again: the next send
            int sends = 0;
            for (const Track& t : m_project->tracks) sends = std::max(sends, int(t.sends.size()));
            m_sendIndex = sends > 0 ? (m_sendIndex + 1) % sends : 0;
        }
        m_knobs = k;
        m_pickup.clear();
        QSettings("Sessions", "Sessions").setValue("midi/apcKnobs", int(k));
        static const char* names[] = {"Volume", "Pan", "Send", "Device"};
        emit message("APC Key 25 knobs: " + QString(names[int(k)]) + (k == Knobs::Send ? " " + QString(QChar('A' + m_sendIndex)) : QString()),
                     k == Knobs::Device ? "The knobs turn the selected device's Macro 1–8, else its first eight parameters."
                                        : "The knobs turn the eight tracks in the box.");
        return;
    }
    const int t = boxTracks().value(i, -1);
    if (t < 0) return;
    const Track& track = m_project->tracks[t];
    const QString base = QString("track:%1:").arg(track.id);
    switch (m_buttons) {
    case Buttons::ClipStop:
        if (track.isGroup()) for (int m : m_project->membersOf(t)) m_transport->stopTrack(m);
        else m_transport->stopTrack(t);
        break;
    case Buttons::Solo: m_mapper->trigger(base + "solo", true); break;
    case Buttons::Arm: if (!track.isGroup()) m_mapper->trigger(base + "arm", true); break;
    case Buttons::Mute: m_mapper->trigger(base + "active", true); break;
    case Buttons::Select: emit trackChosen(t); break;
    }
}

void ApcKey25::sceneButton(int i) {
    if (m_shift) {
        m_buttons = (m_model == Model::Classic ? kClassicModes : kMk2Modes)[i];
        emit message("APC Key 25 track buttons: " + buttonsName(m_buttons), "The buttons under the pads act on the eight tracks in the box.");
        return;
    }
    const int scene = firstScene() + i;
    if (scene >= m_project->scenes.size()) return;
    m_transport->launchScene(scene);
    emit sceneLaunched(scene);
}

QVector<QString> ApcKey25::knobTargets() const {
    QVector<QString> out(kColumns);
    if (m_knobs == Knobs::Device) {
        const int ti = m_project->selectedTrack;
        const Track* t = m_project->trackAt(ti);
        if (!t || t->devices.isEmpty()) return out;
        const int wanted = m_deviceChoice ? m_deviceChoice() : 0;
        const Device* d = nullptr;
        for (const Device& dev : t->devices) if (dev.id == wanted) d = &dev;
        if (!d) for (const Device& dev : t->devices) if (!d && dev.kind == DeviceKind::Instrument && dev.rack == 0) d = &dev;
        if (!d) d = &t->devices.first();
        bool macros = false;
        for (int k = 0; k < kColumns; ++k)
            for (int p = 0; p < d->params.size(); ++p)
                if (d->params[p].name.compare(QString("Macro %1").arg(k + 1), Qt::CaseInsensitive) == 0) {
                    out[k] = QString("device:%1:%2:%3").arg(t->id).arg(d->id).arg(p);
                    macros = true;
                    break;
                }
        if (!macros) for (int k = 0; k < std::min(kColumns, int(d->params.size())); ++k) out[k] = QString("device:%1:%2:%3").arg(t->id).arg(d->id).arg(k);
        return out;
    }
    const QVector<int> cols = boxTracks();
    for (int k = 0; k < kColumns; ++k) {
        if (cols[k] < 0) continue;
        const Track& t = m_project->tracks[cols[k]];
        const QString base = QString("track:%1:").arg(t.id);
        if (m_knobs == Knobs::Volume) out[k] = base + "volume";
        else if (m_knobs == Knobs::Pan) out[k] = base + "pan";
        else if (m_sendIndex < t.sends.size()) out[k] = base + QString("send:%1").arg(m_sendIndex);
    }
    return out;
}

double ApcKey25::valueOf(const QString& target) const { return m_mapper->value(target); }

void ApcKey25::knob(int k, int value, bool relative) {
    const QString target = knobTargets().value(k);
    if (target.isEmpty()) return;
    const double current = valueOf(target);
    if (relative) {
        const int delta = value < 64 ? value : value - 128;   // two's complement: 1 .. 63 up, 127 .. 65 down
        m_mapper->setValue(target, current + delta * (m_shift ? 0.25 : 1.0) / 127.0);
        return;
    }
    // The 2014 model's knobs are absolute: with Pickup, a knob takes over where the value is.
    Pickup& state = m_pickup[target + QString(":%1").arg(k)];
    if (state.applied >= 0 && std::abs(current - state.applied) > 1e-6) { state.caught = false; state.previous = -1; }   // moved elsewhere since
    const int wanted = int(std::lround(current * 127));
    const bool pickup = QSettings("Sessions", "Sessions").value("midi/controllerPickup", true).toBool();
    if (!pickup || std::abs(value - wanted) <= 1 || (state.previous >= 0 && (state.previous - wanted) * (value - wanted) <= 0)) state.caught = true;
    state.previous = value;
    state.applied = current;
    if (state.caught) { m_mapper->setValue(target, value / 127.0); state.applied = valueOf(target); }
}

// ---- lights --------------------------------------------------------------

double ApcKey25::phase() const {
    if (m_transport->playing()) return m_transport->beat();
    return (QDateTime::currentMSecsSinceEpoch() - m_started) / 500.0;   // a steady 120 BPM while stopped
}

int ApcKey25::padColour(int t, int scene, int* channel) const {
    *channel = 0;
    if (t < 0 || scene < 0 || scene >= m_project->scenes.size()) return 0;
    const Track& track = m_project->tracks[t];
    const bool mk2 = m_model == Model::Mk2;
    const double ph = phase();
    const bool blinkOn = std::fmod(ph * 2, 1.0) < 0.5, pulseHigh = std::fmod(ph, 1.0) < 0.5;
    bool clip = false, playing = false, queued = false, recording = false;
    QColor colour;
    if (track.isGroup()) {
        for (int m : m_project->membersOf(t)) {
            const Track& member = m_project->tracks[m];
            clip |= scene < member.clipSlots.size() && member.clipSlots[scene];
            playing |= member.playingSlot == scene;
            queued |= member.queuedSlot == scene;
        }
        colour = track.color;
    } else {
        const ClipPtr c = scene < track.clipSlots.size() ? track.clipSlots[scene] : nullptr;
        clip = bool(c);
        playing = track.playingSlot == scene;
        queued = track.queuedSlot == scene;
        recording = m_transport->isRecordingSlot(t, scene);
        if (c) colour = m_project->clipColor(c);
    }
    if (recording) { *channel = mk2 ? (pulseHigh ? kFull : kHalf) : 0; return mk2 ? kRed : kClassicRed; }
    if (!clip) {
        if (mk2 && track.armed && !track.isGroup()) { *channel = kDim; return kRed; }   // an empty pad here records
        return 0;
    }
    if (queued) { if (!blinkOn) return 0; *channel = mk2 ? kFull : 0; return mk2 ? kGreen : kClassicGreen; }
    if (playing) { *channel = mk2 ? (pulseHigh ? kFull : kBright) : 0; return mk2 ? kGreen : kClassicGreen; }
    *channel = mk2 ? (track.isGroup() ? kHalf : kFull) : 0;
    return mk2 ? nearestSwatch(colour) : kClassicAmber;
}

QHash<int, QPair<int, int>> ApcKey25::lights() const {
    QHash<int, QPair<int, int>> out;
    if (!connected()) return out;
    const QVector<int> cols = boxTracks();
    const int y = firstScene();
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kColumns; ++col) {
            int channel = 0;
            const int velocity = padColour(cols[col], y + row, &channel);
            out.insert((kRows - 1 - row) * kColumns + col, {velocity ? channel : 0, velocity});
        }
    const bool blinkOn = std::fmod(phase() * 2, 1.0) < 0.5;
    auto lit = [](bool on) { return QPair<int, int>{0, on ? 1 : 0}; };
    const auto& modes = m_model == Model::Classic ? kClassicModes : kMk2Modes;
    for (int i = 0; i < kColumns; ++i) {
        bool on = false;
        if (m_shift) {
            static const int dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
            on = i < 4 ? canMove(dx[i], dy[i]) : int(m_knobs) == i - 4;
        } else if (cols[i] >= 0) {
            const Track& t = m_project->tracks[cols[i]];
            switch (m_buttons) {
            case Buttons::ClipStop: {
                bool playing = t.playingSlot >= 0, stopping = t.queuedSlot == -1;
                if (t.isGroup()) for (int m : m_project->membersOf(cols[i])) { playing |= m_project->tracks[m].playingSlot >= 0; stopping |= m_project->tracks[m].queuedSlot == -1; }
                on = stopping ? blinkOn : playing;
                break;
            }
            case Buttons::Solo: on = t.solo; break;
            case Buttons::Arm: on = t.armed; break;
            case Buttons::Mute: on = t.active; break;
            case Buttons::Select: on = m_project->selectedTrack == cols[i]; break;
            }
        }
        out.insert(kTrackButton + i, lit(on));
    }
    for (int i = 0; i < kRows; ++i) {
        bool on = false;
        if (m_shift) on = modes[i] == m_buttons;
        else {
            const int scene = y + i;
            bool any = false, queued = false;
            for (int c : cols) {
                if (c < 0) continue;
                const QList<int> members = m_project->tracks[c].isGroup() ? m_project->membersOf(c) : QList<int>{c};
                for (int m : members) {
                    const Track& t = m_project->tracks[m];
                    any |= scene < t.clipSlots.size() && t.clipSlots[scene];
                    queued |= t.queuedSlot == scene;
                }
            }
            on = queued ? blinkOn : any;
        }
        out.insert(kSceneButton + i, lit(on));
    }
    out.insert(kPlay, lit(m_transport->playing()));
    out.insert(kRecord, lit(m_transport->arrangementRecord() || m_transport->sessionRecording()));
    return out;
}

void ApcKey25::send(int status, int d1, int d2) {
    if (!m_send || m_controls.isEmpty()) return;
    QByteArray m(3, 0);
    m[0] = char(status); m[1] = char(d1 & 0x7F); m[2] = char(d2 & 0x7F);
    m_send(m_controls, m);
}

void ApcKey25::refreshLights(bool all) {
    if (!connected()) return;
    const auto want = lights();
    for (auto it = want.cbegin(); it != want.cend(); ++it) {
        const auto was = m_lit.constFind(it.key());
        if (!all && was != m_lit.constEnd() && *was == it.value()) continue;
        send(0x90 | it.value().first, it.key(), it.value().second);
        m_lit.insert(it.key(), it.value());
    }
}

void ApcKey25::darken() {
    if (m_controls.isEmpty()) return;
    for (auto it = m_lit.cbegin(); it != m_lit.cend(); ++it) if (it.value().second) send(0x90, it.key(), 0);
    m_lit.clear();
}

}
