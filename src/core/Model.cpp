#include "Model.h"
#include "Tempo.h"
#include <functional>
#include "DeviceSpecs.h"
#include "OsmiumPresets.h"
#include "audio/Builtin.h"
#include <QUndoStack>
#include <QUndoCommand>
#include <QDateTime>
#include <QFileInfo>
#include <algorithm>

namespace sessions {

// ---- equality (undo skips edits that changed nothing) -----------------------

static bool operator==(const Note& a, const Note& b) {
    return a.pitch == b.pitch && a.start == b.start && a.length == b.length && a.velocity == b.velocity && a.chance == b.chance;
}
static bool operator==(const WarpMarker& a, const WarpMarker& b) { return a.beat == b.beat && a.seconds == b.seconds; }
static bool operator==(const AutoPoint& a, const AutoPoint& b) { return a.beat == b.beat && a.value == b.value; }
static bool operator==(const Envelope& a, const Envelope& b) { return a.target == b.target && a.points == b.points; }
static bool operator==(const Take& a, const Take& b) { return a.samplePath == b.samplePath && a.name == b.name && a.startBeat == b.startBeat; }
static bool operator==(const CompRegion& a, const CompRegion& b) { return a.start == b.start && a.end == b.end && a.take == b.take; }
static bool operator==(const Clip& a, const Clip& b) {
    return a.id == b.id && a.name == b.name && a.color == b.color && a.length == b.length && a.loop == b.loop && a.notes == b.notes
        && a.start == b.start && a.loopStart == b.loopStart && a.takes == b.takes && a.comp == b.comp
        && a.launchMode == b.launchMode && a.legato == b.legato && a.quantization == b.quantization
        && a.followEnabled == b.followEnabled && a.followA == b.followA && a.followB == b.followB
        && a.followChanceA == b.followChanceA && a.followTime == b.followTime
        && a.scaleOn == b.scaleOn && a.scaleRoot == b.scaleRoot && a.scale == b.scale
        && a.samplePath == b.samplePath && a.warpMode == b.warpMode && a.warpMarkers == b.warpMarkers
        && a.gain == b.gain && a.transpose == b.transpose && a.detune == b.detune && a.reverse == b.reverse
        && a.grainMs == b.grainMs && a.flux == b.flux && a.formants == b.formants
        && a.warp == b.warp && a.grain == b.grain && a.beatsTail == b.beatsTail && a.envelopes == b.envelopes && a.detectedBpm == b.detectedBpm;
}
static bool operator==(const DeviceParam& a, const DeviceParam& b) { return a.name == b.name && a.value == b.value; }
static bool operator==(const DrumPad& a, const DrumPad& b) { return a.name == b.name && a.samplePath == b.samplePath; }
static bool operator==(const MacroMap& a, const MacroMap& b) {
    return a.macro == b.macro && a.deviceId == b.deviceId && a.param == b.param && a.lo == b.lo && a.hi == b.hi;
}
static bool operator==(const Device& a, const Device& b) {
    return a.id == b.id && a.name == b.name && a.kind == b.kind && a.enabled == b.enabled && a.params == b.params
        && a.samplePath == b.samplePath && a.modelPath == b.modelPath && a.pads == b.pads && a.maps == b.maps && a.mods == b.mods
        && a.chains == b.chains && a.zones == b.zones && a.rack == b.rack && a.chain == b.chain && a.folded == b.folded
        && a.sidechainTrack == b.sidechainTrack && a.curves == b.curves && a.sampleBpm == b.sampleBpm && a.slices == b.slices;
}
static bool operator==(const Locator& a, const Locator& b) { return a.beat == b.beat && a.name == b.name; }
static bool operator==(const EditState::Placed& a, const EditState::Placed& b) {
    return a.start == b.start && a.length == b.length && a.offset == b.offset && a.clip == b.clip
        && a.fadeIn == b.fadeIn && a.fadeOut == b.fadeOut && a.fadeInCurve == b.fadeInCurve && a.fadeOutCurve == b.fadeOutCurve;
}
static bool operator==(const EditState::TrackState& a, const EditState::TrackState& b) {
    return a.id == b.id && a.name == b.name && a.color == b.color && a.volume == b.volume && a.pan == b.pan
        && a.sends == b.sends && a.active == b.active && a.preFader == b.preFader && a.slotClips == b.slotClips
        && a.arrangement == b.arrangement && a.devices == b.devices && a.automation == b.automation && a.type == b.type
        && a.monitor == b.monitor && a.inputChannel == b.inputChannel && a.groupId == b.groupId
        && a.groupFadeIn == b.groupFadeIn && a.groupFadeOut == b.groupFadeOut
        && a.groupFadeInCurve == b.groupFadeInCurve && a.groupFadeOutCurve == b.groupFadeOutCurve;
}
bool operator==(const EditState& a, const EditState& b) {
    return a.clips == b.clips && a.tracks == b.tracks && a.returns == b.returns && a.master.devices == b.master.devices && a.master.automation == b.master.automation && a.scenes == b.scenes
        && a.tempo == b.tempo && a.mainVolume == b.mainVolume && a.locators == b.locators;
}

// ---- envelopes ----------------------------------------------------------------

double Envelope::valueAt(double beat) const {
    if (points.isEmpty()) return 0;
    if (beat <= points.first().beat) return points.first().value;
    if (beat >= points.last().beat) return points.last().value;
    auto it = std::upper_bound(points.begin(), points.end(), beat, [](double b, const AutoPoint& p) { return b < p.beat; });
    const AutoPoint& b = *it;
    const AutoPoint& a = *(it - 1);
    const double span = b.beat - a.beat;
    return span > 1e-9 ? a.value + (b.value - a.value) * (beat - a.beat) / span : b.value;
}

// ---- audio clips ----------------------------------------------------------------

double Clip::secondsAt(double beat) const { return warpSecondsAt(warpMarkers.constData(), int(warpMarkers.size()), beat); }

double Clip::positionFrom(double from, double t) const {
    const double pos = clipPositionAt(from, loopStart, length, loop, t);
    return pos < 0 || (!loop && pos >= loopEnd()) ? -1.0 : pos;
}
double Clip::positionAt(double t) const { return positionFrom(start, t); }

double ArrangementClip::beatOf(double clipBeat) const {
    if (!clip) return -1;
    double found = -1;
    forClipPasses(offset, clip->loopStart, clip->length, clip->loop, 0, length, [&](double at, double from, double to) {
        if (found < 0 && clipBeat >= from && clipBeat < to && at + (clipBeat - from) < length) found = start + at + (clipBeat - from);
    });
    return found;
}

double Clip::beatAt(double seconds) const {
    const auto& m = warpMarkers;
    if (m.size() < 2) return (seconds - (m.isEmpty() ? 0.0 : m[0].seconds)) * 2 + (m.isEmpty() ? 0.0 : m[0].beat);
    int i = 0;
    while (i < m.size() - 2 && m[i + 1].seconds <= seconds) ++i;
    const double span = m[i + 1].seconds - m[i].seconds;
    return span > 1e-12 ? m[i].beat + (seconds - m[i].seconds) * (m[i + 1].beat - m[i].beat) / span : m[i].beat;
}

double Clip::sampleBpm() const {
    if (warpMarkers.size() < 2) return 120;
    const double secs = warpMarkers.last().seconds - warpMarkers.first().seconds;
    return secs > 1e-9 ? (warpMarkers.last().beat - warpMarkers.first().beat) * 60.0 / secs : 120;
}

// ---- takes and comping ----------------------------------------------------------

int Clip::takeAt(double beat) const {
    if (takes.isEmpty()) return -1;
    for (const CompRegion& r : comp)
        if (beat >= r.start && beat < r.end && r.take >= 0 && r.take < takes.size()) return r.take;
    return int(takes.size()) - 1;
}

void Clip::useTake(double start, double end, int take) {
    start = std::clamp(start, 0.0, length);
    end = std::clamp(end, 0.0, length);
    if (take < 0 || take >= takes.size() || end - start < 1e-9) return;
    QVector<CompRegion> next;
    for (const CompRegion& r : comp) {   // cut what's there around the new region
        if (r.end <= start + 1e-9 || r.start >= end - 1e-9) { next.push_back(r); continue; }
        if (r.start < start - 1e-9) next.push_back({r.start, start, r.take});
        if (r.end > end + 1e-9) next.push_back({end, r.end, r.take});
    }
    next.push_back({start, end, take});
    std::sort(next.begin(), next.end(), [](const CompRegion& a, const CompRegion& b) { return a.start < b.start; });
    comp.clear();
    for (const CompRegion& r : next) {   // join neighbours that play the same take
        if (!comp.isEmpty() && comp.last().take == r.take && std::abs(comp.last().end - r.start) < 1e-9) comp.last().end = r.end;
        else comp.push_back(r);
    }
}

void Clip::useWholeTake(int take) {
    comp.clear();
    if (take >= 0 && take < takes.size() - 1) comp.push_back({0, length, take});
}

double Clip::takeSeconds(int take, double beat) const {
    if (take < 0 || take >= takes.size()) return -1;
    return secondsAt(beat) - secondsAt(takes[take].startBeat);
}

const Envelope* Track::envelope(const QString& t) const {
    for (const Envelope& e : automation) if (e.target == t) return &e;
    return nullptr;
}

Envelope* Track::envelope(const QString& t) {
    for (Envelope& e : automation) if (e.target == t) return &e;
    return nullptr;
}

const Envelope* Clip::envelope(const QString& t) const {
    for (const Envelope& e : envelopes) if (e.target == t) return &e;
    return nullptr;
}

Envelope* Clip::envelope(const QString& t) {
    for (Envelope& e : envelopes) if (e.target == t) return &e;
    return nullptr;
}

// ---- undo command -----------------------------------------------------------

namespace {

class EditCommand : public QUndoCommand {
public:
    EditCommand(Project* p, const QString& label, quintptr key, EditState before, EditState after)
        : QUndoCommand(label), m_p(p), m_key(key), m_before(std::move(before)), m_after(std::move(after)),
          m_time(QDateTime::currentMSecsSinceEpoch()) {}

    void undo() override { m_p->restoreEditState(m_before); }
    void redo() override {
        if (m_first) { m_first = false; return; }   // the edit itself already happened
        m_p->restoreEditState(m_after);
    }
    int id() const override { return m_key ? 0x5e55 : -1; }
    bool mergeWith(const QUndoCommand* other) override {
        auto* o = static_cast<const EditCommand*>(other);
        if (o->m_key != m_key) return false;
        if (!isRecordMergeKey(m_key) && o->m_time - m_time > 1000) return false;
        m_after = o->m_after;
        m_time = o->m_time;
        if (m_before == m_after) setObsolete(true);
        return true;
    }

private:
    Project*  m_p;
    quintptr  m_key;
    EditState m_before, m_after;
    qint64    m_time;
    bool      m_first = true;
};

} // namespace

// ---- project ----------------------------------------------------------------

Project::Project(QObject* parent) : QObject(parent), m_undo(new QUndoStack(this)) {
    m_undo->setUndoLimit(200);
    master.id = kMasterId;
    master.type = TrackType::Audio;
    master.name = "Master";
    master.color = QColor("#9aa7b3");
}

QColor Project::clipColor(const ClipPtr& clip) const {
    if (clip && clip->color.isValid()) return clip->color;
    const Track* t = trackAt(trackOfClip(clip));
    return t ? t->color : QColor("#9aa7b3");
}

Project::~Project() = default;

const QVector<QColor>& Project::palette() {
    // The grid: fourteen hues across, five rows down (vivid, pastel, soft, deep, muted), then greys.
    // The first row starts with the colours the demo set has always used, so sets look as before.
    static const QVector<QColor> p = [] {
        QVector<QColor> out;
        const double hues[kPaletteColumns] = {20, 42, 56, 80, 110, 150, 175, 192, 210, 232, 258, 285, 320, 345};
        const struct { double s, l; } rows[5] = {{0.86, 0.62}, {0.88, 0.75}, {0.45, 0.66}, {0.75, 0.42}, {0.30, 0.46}};
        for (const auto& r : rows)
            for (double h : hues) out.push_back(QColor(QColor::fromHslF(h / 360.0, r.s, r.l).name()));   // (through the name: 8 bits a channel, as sets save them)
        for (double l : {0.92, 0.78, 0.64, 0.5, 0.38, 0.26}) out.push_back(QColor(QColor::fromHslF(0.58, 0.06, l).name()));
        return out;
    }();
    return p;
}

Track* Project::trackAt(int i) {
    if (i < 0) return nullptr;
    if (i < tracks.size()) return &tracks[i];
    if (i < trackCount()) return &returns[i - tracks.size()];
    if (i == trackCount()) return &master;
    return nullptr;
}

const Track* Project::trackAt(int i) const { return const_cast<Project*>(this)->trackAt(i); }

int Project::indexOfTrack(int id) const {
    for (int i = 0; i < trackCount(); ++i) if (trackAt(i)->id == id) return i;
    return -1;
}

int Project::trackOfClip(const ClipPtr& clip) const {
    if (!clip) return -1;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks[i].clipSlots.contains(clip)) return i;
        for (const auto& a : tracks[i].arrangement) if (a.clip == clip) return i;
    }
    return -1;
}

ClipPtr Project::newClip(const QString& name, double length) {
    auto c = std::make_shared<Clip>();
    c->id = m_nextClipId++;
    c->name = name;
    c->length = length;
    m_clipRegistry.insert(c->id, c);
    return c;
}

// The file's source beat grid is independent of the project's playback tempo.
// Warp then maps those source beats onto the project's clock.
ClipPtr Project::newAudioClip(const QString& path, const SamplePtr& sample) {
    const double secs = sample && sample->seconds() > 0 ? sample->seconds() : 1.0;
    const tempo::Sync s = sample ? tempo::syncTo(*sample, tempo, 0, path) : tempo::Sync{secs * tempo / 60.0, false, 0};
    ClipPtr c = newClip(QFileInfo(path).completeBaseName(), s.beats);
    c->samplePath = QFileInfo(path).absoluteFilePath();
    c->sample = sample;
    c->loop = s.loop;
    c->warp = s.detected > 0;
    c->detectedBpm = s.detected;
    c->beatsTail = 1;   // preserve attacks while filling stretched drum tails; old sets retain Gate
    c->warpMarkers = {{0, 0}, {s.beats, secs}};
    return c;
}

QVector<ClipPtr> Project::audioClips() const {
    QVector<ClipPtr> out;
    QSet<int> seen;
    auto take = [&](const ClipPtr& c) { if (c && c->isAudio() && !seen.contains(c->id)) { seen.insert(c->id); out.push_back(c); } };
    for (const Track& t : tracks) {
        for (const ClipPtr& c : t.clipSlots) take(c);
        for (const ArrangementClip& a : t.arrangement) take(a.clip);
    }
    return out;
}

bool Project::syncClipToTempo(const ClipPtr& clip) {
    if (!clip || !clip->isAudio() || !clip->sample || !clip->takes.isEmpty()) return false;
    const tempo::Sync s = tempo::syncTo(*clip->sample, tempo, clip->detectedBpm, clip->samplePath);
    EditScope edit(this, "Sync to Tempo");
    const double secs = clip->sample->seconds();
    const double before = clip->length;
    clip->detectedBpm = s.detected;
    clip->loop = s.loop;
    clip->warp = s.detected > 0;
    clip->warpMarkers = {{0, 0}, {s.beats, secs}};
    clip->length = s.beats;
    if (before > 0 && std::fabs(s.beats / before - 1) > 1e-9) {   // placements keep showing the same audio
        const double f = s.beats / before;
        clip->start *= f;
        clip->loopStart *= f;
        for (Envelope& e : clip->envelopes) for (AutoPoint& pt : e.points) pt.beat *= f;
        for (Track& t : tracks) for (ArrangementClip& a : t.arrangement) if (a.clip == clip) { a.length *= f; a.offset *= f; }
    }
    emit clipChanged();
    emit structureChanged();
    return true;
}

ClipPtr Project::copyClip(const ClipPtr& src) {
    if (!src) return nullptr;
    auto c = std::make_shared<Clip>(*src);
    c->id = m_nextClipId++;
    m_clipRegistry.insert(c->id, c);
    return c;
}

Device Project::makeDevice(const QString& name, DeviceKind kind) {
    Device d;
    d.id = m_nextDeviceId++;
    d.name = name;
    d.kind = kind;
    if (const DeviceSpec* s = deviceSpec(name)) {
        d.kind = s->kind;
        for (const ParamSpec& p : s->params) d.params.push_back({p.name, p.def});
        if (s->type == DeviceType::DrumRack) d.pads = audio::defaultDrumPads();   // a ready kit: the 808's sounds
        if (s->type == DeviceType::Monolith) for (const ModRoute& r : presetMods(name)) d.mods.push_back(r);   // a Sound's routes
        if (s->type == DeviceType::Osmium) {
            d.mods = {{monolith::SrcEnv2, monolith::fparam(0, monolith::FCutoff), 0.28}};
            d.curves = {{{0, 0}, {0.25, 1, -0.35, 1}, {0.5, 0.15}, {0.75, 0.7, 0.3, 1}, {1, 0}},
                        {{0, 1, 0.35}, {0.5, 0, -0.35}, {1, 1}}};
            if (const auto* patch = osmium::factoryPreset(name)) osmium::applyPreset(d, *patch);
        }
    } else {
        for (const char* p : {"Amount", "Tone", "Mix", "Output"}) d.params.push_back({p, 0.5});
    }
    return d;
}

// ---- racks ------------------------------------------------------------------

bool isRack(const Device& d) { return isRackType(deviceType(d.name)); }
bool isDrumRack(const Device& d) { return deviceType(d.name) == DeviceType::DrumRack; }
bool holdsChains(const Device& d) { return isRack(d) || (isDrumRack(d) && d.rack == 0); }
int  chainCapacity(const Device& d) { return isDrumRack(d) ? drumrack::kPads : racks::kChains; }

Zone Zone::sane() const {
    Zone z;
    z.lo = std::clamp(lo, 0, 127);
    z.hi = std::clamp(hi, z.lo, 127);
    z.fadeLo = std::clamp(fadeLo, 0, z.hi - z.lo);
    z.fadeHi = std::clamp(fadeHi, 0, z.hi - z.lo - z.fadeLo);
    return z;
}

int deviceBlockEnd(const QVector<Device>& devices, int index) {
    if (index < 0 || index >= devices.size()) return index + 1;
    int end = index + 1;
    if (holdsChains(devices[index]))
        while (end < devices.size() && devices[end].rack == devices[index].id) ++end;
    return end;
}

namespace {
int kindRank(DeviceKind k) { return k == DeviceKind::MidiEffect ? 0 : k == DeviceKind::Instrument ? 1 : 2; }
void pruneMacroMaps(QVector<Device>& devs);

// A rack's chain knobs back to the spec's defaults (Volume 0 dB / Transpose 0, centred, on, not soloed).
void resetChainKnobs(Device& rack, int chain) {
    const DeviceSpec* spec = specOf(deviceType(rack.name));
    if (!spec || !isRack(rack)) return;
    for (int k = 0; k < racks::kPerChain; ++k) {
        const int i = racks::param(chain, k);
        if (i < rack.params.size() && i < int(spec->params.size())) rack.params[i].value = spec->params[size_t(i)].def;
    }
}

// A new chain at the end of a rack: named, default knobs and zones.
int appendChain(Device& rack, const QString& name) {
    const int c = int(rack.chains.size());
    rack.chains.push_back(name);
    rack.zones.resize(rack.chains.size());
    rack.zones[c] = ChainZones{};
    resetChainKnobs(rack, c);
    return c;
}
}

void normalizeRacks(QVector<Device>& devices) {
    QSet<int> rackIds;   // racks always sit on the track itself
    for (Device& d : devices) if (isRack(d)) { d.rack = 0; d.chain = 0; rackIds.insert(d.id); }
    QSet<int> drumIds;   // Drum Racks on the track itself hold their pads' chains; one in a rack's chain holds none
    for (Device& d : devices)
        if (isDrumRack(d) && !(d.rack && rackIds.contains(d.rack))) { d.rack = 0; d.chain = 0; drumIds.insert(d.id); }
    QVector<Device> top;
    QHash<int, QVector<Device>> inside;
    for (Device& d : devices) {
        const bool member = d.rack != 0 && d.rack != d.id && deviceType(d.name) != DeviceType::Macros
                         && (rackIds.contains(d.rack) || (drumIds.contains(d.rack) && !isDrumRack(d)));
        if (member) inside[d.rack].push_back(d);
        else { d.rack = 0; d.chain = 0; top.push_back(d); }
    }
    QVector<Device> out;
    for (Device& d : top) {
        if (!isRack(d)) d.zones.clear();
        out.push_back(d);
        if (!holdsChains(d)) continue;
        const int rackIndex = int(out.size()) - 1;
        const DeviceType type = deviceType(d.name);
        const int capacity = chainCapacity(d);
        QVector<Device> members = inside.value(d.id);
        if (type != DeviceType::DrumRack) {
            Device& rack = out[rackIndex];
            while (rack.chains.size() > capacity) rack.chains.removeLast();
            for (Device& m : members) {
                m.chain = std::clamp(m.chain, 0, capacity - 1);
                while (rack.chains.size() <= m.chain) rack.chains.push_back(QString("Chain %1").arg(rack.chains.size() + 1));
            }
            rack.zones.resize(rack.chains.size());
            for (ChainZones& z : rack.zones) { z.key = z.key.sane(); z.vel = z.vel.sane(); z.select = z.select.sane(); }
        } else {
            members.erase(std::remove_if(members.begin(), members.end(), [](const Device& m) { return m.chain < 0 || m.chain >= drumrack::kPads; }), members.end());
        }
        // chain by chain; in each, MIDI effects, the instrument, audio effects
        std::stable_sort(members.begin(), members.end(), [](const Device& a, const Device& b) {
            return a.chain != b.chain ? a.chain < b.chain : kindRank(a.kind) < kindRank(b.kind);
        });
        for (const Device& m : members) {
            if (type == DeviceType::AudioEffectRack && m.kind != DeviceKind::AudioEffect) continue;
            if (type == DeviceType::MidiEffectRack && m.kind != DeviceKind::MidiEffect) continue;
            out.push_back(m);
        }
    }
    devices = out;
}

bool Project::chainAccepts(const Device& rack, const Device& d) {
    const DeviceType t = deviceType(d.name);
    if (isRackType(t) || t == DeviceType::Macros || rackRecipe(d.name)) return false;
    switch (deviceType(rack.name)) {
    case DeviceType::InstrumentRack: return true;
    case DeviceType::DrumRack:       return t != DeviceType::DrumRack;   // a pad: anything but another Drum Rack
    case DeviceType::AudioEffectRack: return d.kind == DeviceKind::AudioEffect;
    case DeviceType::MidiEffectRack: return d.kind == DeviceKind::MidiEffect;
    default: return false;
    }
}

QVector<Device> Project::makeDeviceBlock(const QString& name, DeviceKind kind) {
    const RackRecipe* r = rackRecipe(name);
    if (!r) return {makeDevice(name, kind)};
    Device rack = makeDevice(name, kind);   // the rack, under the recipe's name
    QVector<Device> members;
    QVector<QVector<int>> ids;   // device ids per chain, for the macro maps
    const bool midiRack = deviceType(name) == DeviceType::MidiEffectRack;
    for (int c = 0; c < int(r->chains.size()) && c < racks::kChains; ++c) {
        const RackRecipe::Chain& ch = r->chains[size_t(c)];
        appendChain(rack, ch.name);
        rack.zones[c].key = Zone{ch.keyLo, ch.keyHi, 0, 0}.sane();
        if (midiRack) {
            rack.params[racks::param(c, racks::Transpose)].value = std::clamp((ch.transpose + 48) / 96.0, 0.0, 1.0);
            rack.params[racks::param(c, racks::VelocityScale)].value = std::clamp(ch.velocity / 2.0, 0.0, 1.0);
        } else {
            rack.params[racks::param(c, racks::Volume)].value = std::clamp(0.75 * std::pow(10.0, ch.volumeDb / 40.0), 0.0, 1.0);
        }
        ids.push_back({});
        for (const RackRecipe::Dev& dev : ch.devices) {
            Device m = makeDevice(dev.name, DeviceKind::AudioEffect);
            for (const auto& [i, v] : dev.params) if (i < m.params.size()) m.params[i].value = v;
            m.rack = rack.id;
            m.chain = c;
            ids.last().push_back(m.id);
            members.push_back(m);
        }
    }
    for (const RackRecipe::Map& m : r->maps) {
        const int target = m.device < 0 ? rack.id : ids.value(m.chain).value(m.device, 0);
        if (target) rack.maps.push_back({m.macro, target, m.param, m.lo, m.hi});
    }
    return QVector<Device>{rack} + members;
}

bool Project::addDeviceToChain(int trackIndex, int rackId, int chain, const QString& name) {
    Device d = makeDevice(name, DeviceKind::AudioEffect);
    return insertDevices(trackIndex, {{d}}, DevicePlace{0, rackId, chain}, "Add " + name);
}

int Project::addChain(int trackIndex, int rackId) {
    Track* t = trackAt(trackIndex);
    if (!t) return -1;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != rackId || !isRack(t->devices.at(i))) continue;
        if (t->devices.at(i).chains.size() >= racks::kChains) return -1;
        EditScope edit(this, "Add Chain");
        Device& rack = trackAt(trackIndex)->devices[i];
        const int c = appendChain(rack, QString("Chain %1").arg(rack.chains.size() + 1));
        emit structureChanged();
        return c;
    }
    return -1;
}

namespace {
// Moves a rack's chain knobs: chain c's knobs go to chain to(c) (-1: they're dropped). Their values
// (chains nothing moves into get the defaults), the track's automation and its clips' envelopes on
// them, and macro mappings onto them follow. Call inside an EditScope.
void remapChainKnobs(Project& p, int trackIndex, int rackId, const std::function<int(int)>& to) {
    Track* t = p.trackAt(trackIndex);
    if (!t) return;
    auto mapParam = [&](int param) {   // the knob's new index, or -1
        if (param < racks::kBase || param >= racks::ChainSelect) return param;
        const int c = (param - racks::kBase) / racks::kPerChain, k = (param - racks::kBase) % racks::kPerChain;
        const int d = to(c);
        return d < 0 ? -1 : racks::param(d, k);
    };
    for (Device& rack : t->devices) {
        if (rack.id != rackId) continue;
        const QVector<DeviceParam> old = rack.params;
        for (int c = 0; c < racks::kChains; ++c) resetChainKnobs(rack, c);
        for (int i = racks::kBase; i < racks::ChainSelect && i < old.size(); ++i)
            if (const int j = mapParam(i); j >= 0 && j < rack.params.size()) rack.params[j].value = old[i].value;
    }
    const QString prefix = QString("device:%1:").arg(rackId);
    auto remapEnvelopes = [&](QVector<Envelope>& envs) {
        QVector<Envelope> out;
        for (Envelope e : envs) {
            if (e.target.startsWith(prefix)) {
                const int j = mapParam(e.target.mid(prefix.size()).toInt());
                if (j < 0) continue;
                e.target = target::device(rackId, j);
            }
            out.push_back(e);
        }
        if (!(out.size() == envs.size() && std::equal(out.begin(), out.end(), envs.begin(), [](const Envelope& a, const Envelope& b) { return a.target == b.target; })))
            envs = out;
    };
    remapEnvelopes(t->automation);
    QSet<QString> overridden;
    for (const QString& target : t->overriddenTargets) {
        if (!target.startsWith(prefix)) { overridden.insert(target); continue; }
        if (const int j = mapParam(target.mid(prefix.size()).toInt()); j >= 0) overridden.insert(target::device(rackId, j));
    }
    t->overriddenTargets = overridden;
    QSet<Clip*> clips;
    for (const ClipPtr& c : t->clipSlots) if (c) clips.insert(c.get());
    for (const ArrangementClip& a : t->arrangement) if (a.clip) clips.insert(a.clip.get());
    for (Clip* c : clips) remapEnvelopes(c->envelopes);
    for (Device& d : t->devices) {
        if (d.maps.isEmpty()) continue;
        QVector<MacroMap> maps;
        for (MacroMap m : d.maps) {
            if (m.deviceId == rackId) {
                m.param = mapParam(m.param);
                if (m.param < 0) continue;
            }
            maps.push_back(m);
        }
        if (maps.size() != d.maps.size() || !std::equal(maps.begin(), maps.end(), d.maps.begin(), [](const MacroMap& a, const MacroMap& b) { return a.param == b.param; }))
            d.maps = maps;
    }
}
}

void Project::deleteChain(int trackIndex, int rackId, int chain) {
    Track* t = trackAt(trackIndex);
    if (!t) return;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != rackId || !holdsChains(t->devices.at(i))) continue;
        if (isDrumRack(t->devices.at(i))) {   // a pad: its devices go, the pad and its sample stay
            if (chain < 0 || chain >= drumrack::kPads) return;
            const int end = deviceBlockEnd(t->devices, i);
            bool any = false;
            for (int j = i + 1; j < end; ++j) any |= t->devices.at(j).chain == chain;
            if (!any) return;
            EditScope edit(this, "Clear Pad Devices");
            t = trackAt(trackIndex);
            for (int j = end - 1; j > i; --j) if (t->devices.at(j).chain == chain) t->devices.removeAt(j);
            pruneMacroMaps(t->devices);
            emit structureChanged();
            return;
        }
        if (chain < 0 || chain >= t->devices.at(i).chains.size()) return;
        EditScope edit(this, "Delete Chain");
        t = trackAt(trackIndex);
        const int end = deviceBlockEnd(t->devices, i);
        QSet<int> gone;
        for (int j = end - 1; j > i; --j) {
            if (t->devices.at(j).chain == chain) { gone.insert(t->devices.at(j).id); t->devices.removeAt(j); }
            else if (t->devices.at(j).chain > chain) t->devices[j].chain -= 1;
        }
        remapChainKnobs(*this, trackIndex, rackId, [chain](int c) { return c == chain ? -1 : c > chain ? c - 1 : c; });   // later chains move up
        t = trackAt(trackIndex);
        Device& rack = t->devices[i];
        rack.chains.removeAt(chain);
        if (chain < rack.zones.size()) rack.zones.removeAt(chain);
        rack.zones.resize(rack.chains.size());
        // the deleted devices' automation and envelopes go with them
        auto dropGone = [&](QVector<Envelope>& envs) {
            if (std::none_of(envs.begin(), envs.end(), [&](const Envelope& e) { const QStringList p = e.target.split(':'); return p.size() == 3 && gone.contains(p[1].toInt()); })) return;
            envs.erase(std::remove_if(envs.begin(), envs.end(), [&](const Envelope& e) { const QStringList p = e.target.split(':'); return p.size() == 3 && gone.contains(p[1].toInt()); }), envs.end());
        };
        dropGone(t->automation);
        for (const ClipPtr& c : t->clipSlots) if (c) dropGone(c->envelopes);
        for (const ArrangementClip& a : t->arrangement) if (a.clip) dropGone(a.clip->envelopes);
        pruneMacroMaps(t->devices);
        emit structureChanged();
        return;
    }
}

bool Project::moveChain(int trackIndex, int rackId, int from, int to) {
    Track* t = trackAt(trackIndex);
    if (!t) return false;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != rackId || !isRack(t->devices.at(i))) continue;
        const int n = int(t->devices.at(i).chains.size());
        to = std::clamp(to, 0, n - 1);
        if (from < 0 || from >= n || from == to) return false;
        // where each chain goes: `from` to `to`, the ones between shift by one
        auto dest = [from, to](int c) {
            if (c == from) return to;
            if (from < to && c > from && c <= to) return c - 1;
            if (to < from && c >= to && c < from) return c + 1;
            return c;
        };
        EditScope edit(this, "Move Chain");
        t = trackAt(trackIndex);
        const int end = deviceBlockEnd(t->devices, i);
        for (int j = i + 1; j < end; ++j) t->devices[j].chain = dest(t->devices.at(j).chain);
        std::stable_sort(t->devices.begin() + i + 1, t->devices.begin() + end, [](const Device& a, const Device& b) { return a.chain < b.chain; });
        Device& rack = t->devices[i];
        rack.zones.resize(n);
        const QStringList names = rack.chains;
        const QVector<ChainZones> zones = rack.zones;
        for (int c = 0; c < n; ++c) { rack.chains[dest(c)] = names[c]; rack.zones[dest(c)] = zones[c]; }
        remapChainKnobs(*this, trackIndex, rackId, dest);
        emit structureChanged();
        return true;
    }
    return false;
}

bool Project::setChainZone(int trackIndex, int rackId, int chain, int which, const Zone& zone, quintptr mergeKey) {
    Track* t = trackAt(trackIndex);
    if (!t || which < 0 || which > ChainZones::Select) return false;
    for (int i = 0; i < t->devices.size(); ++i) {
        const Device& rack = t->devices.at(i);
        if (rack.id != rackId || !isRack(rack) || chain < 0 || chain >= rack.chains.size()) continue;
        const Zone z = zone.sane();
        if (chain < rack.zones.size() && rack.zones.at(chain)[which] == z) return false;
        static const char* const names[] = {"Key Zone", "Velocity Zone", "Chain Zone"};
        EditScope edit(this, QString("Change ") + names[which], mergeKey);
        Device& r = trackAt(trackIndex)->devices[i];
        r.zones.resize(r.chains.size());
        r.zones[chain][which] = z;
        emit deviceChanged();
        return true;
    }
    return false;
}

void Project::renameChain(int trackIndex, int rackId, int chain, const QString& name) {
    Track* t = trackAt(trackIndex);
    if (!t || name.trimmed().isEmpty()) return;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != rackId || chain < 0 || chain >= t->devices.at(i).chains.size()) continue;
        EditScope edit(this, "Rename Chain");
        trackAt(trackIndex)->devices[i].chains[chain] = name.trimmed();
        emit structureChanged();
        return;
    }
}

bool Project::deleteDevice(int trackIndex, int deviceId) {
    Track* t = trackAt(trackIndex);
    if (!t) return false;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != deviceId) continue;
        EditScope edit(this, "Delete " + t->devices.at(i).name);
        t = trackAt(trackIndex);
        const int end = deviceBlockEnd(t->devices, i);
        t->devices.remove(i, end - i);
        emit structureChanged();
        return true;
    }
    return false;
}

int Project::groupIntoRack(int trackIndex, int deviceId) { return groupIntoRack(trackIndex, QList<int>{deviceId}); }

int Project::groupIntoRack(int trackIndex, const QList<int>& deviceIds) {
    const Track* t = trackAt(trackIndex);
    if (!t) return 0;
    QVector<Device> devs = t->devices;
    QList<int> picked;   // indexes, in chain order
    bool instrument = false, midi = false, audio = false;
    for (int i = 0; i < devs.size(); ++i) {
        const Device& d = devs.at(i);
        if (!deviceIds.contains(d.id)) continue;
        if (d.rack != 0 || isRack(d) || deviceType(d.name) == DeviceType::Macros) return 0;
        if (isDrumRack(d) && deviceBlockEnd(devs, i) > i + 1) return 0;   // its pads hold devices: it stays on the track
        picked << i;
        instrument |= d.kind == DeviceKind::Instrument;
        midi |= d.kind == DeviceKind::MidiEffect;
        audio |= d.kind == DeviceKind::AudioEffect;
    }
    if (picked.isEmpty() || (midi && audio && !instrument)) return 0;   // MIDI and audio effects need an instrument between them
    const QString type = instrument ? "Instrument Rack" : midi ? "MIDI Effect Rack" : "Audio Effect Rack";
    Device rack = makeDevice(type, instrument ? DeviceKind::Instrument : midi ? DeviceKind::MidiEffect : DeviceKind::AudioEffect);
    appendChain(rack, devs.at(picked.first()).name);
    QVector<Device> members;
    for (int i : picked) {
        Device m = devs.at(i);
        m.rack = rack.id;
        m.chain = 0;
        members.push_back(m);
    }
    const int at = picked.first();
    for (int k = int(picked.size()) - 1; k >= 0; --k) devs.removeAt(picked[k]);
    devs.insert(at, rack);
    for (int j = 0; j < members.size(); ++j) devs.insert(at + 1 + j, members[j]);
    EditScope edit(this, "Group into Rack");
    trackAt(trackIndex)->devices = devs;   // after the snapshot
    emit structureChanged();
    return rack.id;
}

bool Project::ungroupRack(int trackIndex, int rackId) {
    Track* t = trackAt(trackIndex);
    if (!t) return false;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != rackId || !isRack(t->devices.at(i))) continue;
        if (t->devices.at(i).chains.size() > 1) return false;
        EditScope edit(this, "Ungroup Rack");
        t = trackAt(trackIndex);
        const int end = deviceBlockEnd(t->devices, i);
        for (int j = i + 1; j < end; ++j) { t->devices[j].rack = 0; t->devices[j].chain = 0; }
        t->devices.removeAt(i);
        emit structureChanged();
        return true;
    }
    return false;
}

bool Project::acceptsDevice(int track, DeviceKind kind) const {
    const Track* t = trackAt(track);
    if (!t) return false;
    return kind == DeviceKind::AudioEffect || (!isReturn(track) && t->type == TrackType::Midi);
}

// ---- sidechains ------------------------------------------------------------------

bool Project::keyReaches(int fromId, int toId) const {
    if (fromId == toId) return true;
    // follow keys forward: every track holding a device keyed from `fromId` comes after it
    QVector<int> stack{fromId};
    QSet<int> seen{fromId};
    while (!stack.isEmpty()) {
        const int cur = stack.takeLast();
        for (int i = 0; i < trackCount(); ++i) {
            const Track& t = *trackAt(i);
            if (seen.contains(t.id)) continue;
            bool keyed = false;
            for (const Device& d : t.devices) keyed |= d.sidechainTrack == cur;
            if (!keyed) continue;
            if (t.id == toId) return true;
            seen.insert(t.id);
            stack.push_back(t.id);
        }
    }
    return false;
}

bool Project::canKeyFrom(int trackIndex, int deviceId, int sourceId, QString* why) const {
    auto fail = [why](const QString& w) { if (why) *why = w; return false; };
    const Track* t = trackAt(trackIndex);
    if (!t) return fail("There's no track there.");
    const Device* dev = nullptr;
    for (const Device& d : t->devices) if (d.id == deviceId) dev = &d;
    if (!dev) return fail("That device isn't there any more.");
    if (!sidechain::supports(deviceType(dev->name))) return fail(dev->name + " has no sidechain input.");
    if (sourceId == 0) return true;
    if (sourceId == t->id) return fail("A device can't be keyed from its own track.");
    const int si = indexOfTrack(sourceId);
    if (si < 0 || si >= tracks.size()) return fail("Keys come from regular tracks (not returns).");
    if (tracks[si].isGroup()) return fail("Keys come from regular tracks (not group tracks).");
    // the source must not (through other keys) depend on this track: that would be a feedback loop
    if (keyReaches(t->id, sourceId)) return fail(QString("%1 is already keyed from this track (directly or through another): that would loop.").arg(tracks[si].name));
    return true;
}

bool Project::setKeySource(int trackIndex, int deviceId, int sourceId) {
    if (!canKeyFrom(trackIndex, deviceId, sourceId)) return false;
    const Track* t = trackAt(trackIndex);
    for (const Device& d : t->devices) if (d.id == deviceId && d.sidechainTrack == sourceId) return true;   // nothing to do
    QString name = "Internal";
    if (const int si = indexOfTrack(sourceId); sourceId && si >= 0) name = tracks[si].name;
    {
        EditScope edit(this, "Sidechain from " + name);
        Track* tt = trackAt(trackIndex);   // after the snapshot (Qt containers share data until written)
        for (Device& d : tt->devices) if (d.id == deviceId) d.sidechainTrack = sourceId;
    }
    emit deviceChanged();
    return true;
}

bool Project::addDeviceToSelected(const QString& name, DeviceKind kind) {
    if (!trackAt(selectedTrack)) return false;
    // the device, or a rack and its chains' devices, where its kind goes (an instrument replaces the track's)
    return insertDevices(selectedTrack, {makeDeviceBlock(name, kind)}, DevicePlace{}, "Add " + name);
}

// ---- putting devices somewhere, moving and copying them ----------------------

namespace {

// Where something of rank `r` may go among the units in devs[from, to) (blocks at the top level,
// or one chain's devices; step(i) is the index after the unit at i): after the last unit ranked
// lower, before the first ranked higher.
template <typename Step>
void rankRange(const QVector<Device>& devs, int from, int to, int r, Step step, int& lo, int& hi) {
    lo = from;
    hi = to;
    bool higher = false;
    for (int i = from; i < to; i = step(i)) {
        const int k = kindRank(devs.at(i).kind);
        if (k < r) lo = step(i);
        if (k > r && !higher) { hi = i; higher = true; }
    }
}

// Racks keep only the macro mappings to themselves and their chains' devices; Macros devices only
// those to devices on their track.
void pruneMacroMaps(QVector<Device>& devs) {
    QHash<int, int> rackOf;   // device id -> its rack (0: top level)
    for (const Device& d : devs) rackOf.insert(d.id, d.rack);
    for (int i = 0; i < devs.size(); ++i) {
        const Device& c = devs.at(i);
        if (c.maps.isEmpty()) continue;
        const bool rack = isRack(c);
        const int id = c.id;
        auto gone = [&](const MacroMap& m) {
            if (!rackOf.contains(m.deviceId)) return true;
            return rack && m.deviceId != id && rackOf.value(m.deviceId) != id;
        };
        if (std::none_of(c.maps.begin(), c.maps.end(), gone)) continue;
        QVector<MacroMap>& maps = devs[i].maps;
        maps.erase(std::remove_if(maps.begin(), maps.end(), gone), maps.end());
    }
}

// Places `blocks` in `devs` (one track's devices) at `at`; false, with `why`, when one can't go
// there (`devs` may be half done then: callers work on a copy). `midiTrack`: instruments and MIDI
// effects may go on the track itself.
bool placeBlocks(QVector<Device>& devs, const Project::DeviceBlocks& blocks, const Project::DevicePlace& at,
                 bool midiTrack, QList<int>* placed, QString* why) {
    auto fail = [why](const QString& w) { if (why) *why = w; return false; };
    if (blocks.isEmpty() || std::any_of(blocks.begin(), blocks.end(), [](const QVector<Device>& b) { return b.isEmpty(); }))
        return fail("Nothing to put there.");
    const int instruments = int(std::count_if(blocks.begin(), blocks.end(), [](const QVector<Device>& b) { return b.first().kind == DeviceKind::Instrument; }));
    if (instruments > 1) return fail("A track (or a rack's chain) plays one instrument.");
    QList<int> ids;

    if (at.rackId == 0) {   // the track's own chain
        for (const QVector<Device>& b : blocks)
            if (!midiTrack && b.first().kind != DeviceKind::AudioEffect)
                return fail(b.first().kind == DeviceKind::Instrument ? "Instruments go on MIDI tracks: drop it on one, or on the empty space for a new one."
                                                                     : "MIDI effects go on MIDI tracks: drop it on one, or on the empty space for a new one.");
        auto step = [&devs](int i) { return deviceBlockEnd(devs, i); };
        int anchor = -1;   // the index to insert at; -1: each block's natural place
        if (at.beforeId == 0) anchor = int(devs.size());
        else if (at.beforeId > 0) {
            anchor = int(devs.size());
            for (int i = 0; i < devs.size() && anchor == devs.size(); i = step(i))
                for (int j = i; j < step(i); ++j) if (devs.at(j).id == at.beforeId) anchor = i;   // a chain device: its rack's block
        }
        for (QVector<Device> b : blocks) {
            b[0].rack = 0;
            b[0].chain = 0;
            const int r = kindRank(b.first().kind);
            if (r == 1)   // one instrument per track: a new one replaces it
                for (int i = 0; i < devs.size(); i = step(i)) {
                    if (devs.at(i).kind != DeviceKind::Instrument) continue;
                    const int n = step(i) - i;
                    devs.remove(i, n);
                    if (anchor > i) anchor = std::max(i, anchor - n);
                    break;
                }
            int lo = 0, hi = 0;
            rankRange(devs, 0, int(devs.size()), r, step, lo, hi);
            const int pos = std::max(lo, std::min(anchor < 0 ? hi : anchor, hi));
            for (int j = 0; j < b.size(); ++j) devs.insert(pos + j, b[j]);
            ids << b.first().id;
            if (anchor >= 0) anchor = pos + int(b.size());
        }
        if (placed) *placed = ids;
        return true;
    }

    // a rack's chain, or a Drum Rack's pad
    int ri = -1;
    for (int i = 0; i < devs.size(); ++i) if (devs.at(i).id == at.rackId && holdsChains(devs.at(i)) && devs.at(i).rack == 0) ri = i;
    if (ri < 0) return fail("That rack isn't there any more.");
    const DeviceType rackType = deviceType(devs.at(ri).name);
    for (const QVector<Device>& b : blocks) {
        if (b.size() == 1 && Project::chainAccepts(devs.at(ri), b.first())) continue;
        if (isDrumRack(b.first()))
            return fail(rackType == DeviceType::DrumRack ? "A Drum Rack doesn't go on another Drum Rack's pad."
                                                         : "A Drum Rack whose pads hold devices doesn't go inside a rack: clear its pads' devices first.");
        if (isRack(b.first()) || rackRecipe(b.first().name)) return fail("Racks don't go inside racks.");
        if (deviceType(b.first().name) == DeviceType::Macros) return fail("Macros stay on the track: a rack has its own macro knobs.");
        if (rackType == DeviceType::MidiEffectRack) return fail("A MIDI Effect Rack holds MIDI effects.");
        return fail("An Audio Effect Rack holds audio effects.");
    }
    int chain = at.chain;
    if (rackType == DeviceType::DrumRack) {
        if (chain < 0 || chain >= drumrack::kPads) return fail("Drop it on one of the Drum Rack's pads.");
    } else if (chain < 0 || chain >= devs.at(ri).chains.size()) {
        if (devs.at(ri).chains.size() >= racks::kChains) return fail(QString("A rack holds %1 chains.").arg(racks::kChains));
        chain = appendChain(devs[ri], blocks.first().first().name);   // a new chain, named after what starts it
    }
    int pref = -1;   // where the next one goes; -1: the chain's end
    bool first = true;
    for (const QVector<Device>& b : blocks) {
        Device d = b.first();
        d.rack = at.rackId;
        d.chain = chain;
        const int end = deviceBlockEnd(devs, ri);
        int cs = ri + 1;
        while (cs < end && devs.at(cs).chain < chain) ++cs;
        int ce = cs;
        while (ce < end && devs.at(ce).chain == chain) ++ce;
        if (first && at.beforeId > 0)
            for (int j = cs; j < ce; ++j) if (devs.at(j).id == at.beforeId) pref = j;
        first = false;
        const int r = kindRank(d.kind);
        if (r == 1)
            for (int j = cs; j < ce; ++j)
                if (devs.at(j).kind == DeviceKind::Instrument) {
                    devs.removeAt(j);
                    --ce;
                    if (pref > j) --pref;
                    break;
                }
        int lo = 0, hi = 0;
        rankRange(devs, cs, ce, r, [](int i) { return i + 1; }, lo, hi);
        const int pos = std::max(lo, std::min(pref < 0 ? ce : pref, hi));
        devs.insert(pos, d);
        ids << d.id;
        if (pref >= 0) pref = pos + 1;
    }
    if (placed) *placed = ids;
    return true;
}

} // namespace

bool Project::canPlace(int to, const DeviceBlocks& blocks, const DevicePlace& at, QString* why) const {
    const Track* t = trackAt(to);
    if (!t) { if (why) *why = "There's no track there."; return false; }
    QVector<Device> devs = t->devices;
    return placeBlocks(devs, blocks, at, !isReturn(to) && t->type == TrackType::Midi, nullptr, why);
}

bool Project::insertDevices(int to, const DeviceBlocks& blocks, const DevicePlace& at, const QString& label, QList<int>* placed, QString* why) {
    const Track* t = trackAt(to);
    if (!t) { if (why) *why = "There's no track there."; return false; }
    QVector<Device> devs = t->devices;   // a copy: nothing changes unless all of it fits
    QList<int> ids;
    if (!placeBlocks(devs, blocks, at, !isReturn(to) && t->type == TrackType::Midi, &ids, why)) return false;
    pruneMacroMaps(devs);
    EditScope edit(this, label);
    trackAt(to)->devices = devs;   // after the snapshot
    emit structureChanged();
    if (placed) *placed = ids;
    return true;
}

Project::DeviceBlocks Project::deviceBlocks(int track, const QList<int>& ids) const {
    DeviceBlocks out;
    const Track* t = trackAt(track);
    if (!t) return out;
    const QSet<int> want(ids.begin(), ids.end());
    const QVector<Device>& devs = t->devices;
    for (int i = 0; i < devs.size(); ++i) {
        const Device& d = devs.at(i);
        if (!want.contains(d.id) || (d.rack && want.contains(d.rack))) continue;   // inside a rack that's going too: goes with it
        if (holdsChains(d)) {
            const int end = deviceBlockEnd(devs, i);
            out.push_back(devs.mid(i, end - i));
            i = end - 1;
        } else {
            Device c = d;   // out of its rack (where it lands decides)
            c.rack = 0;
            c.chain = 0;
            out.push_back({c});
        }
    }
    return out;
}

Project::DeviceBlocks Project::freshCopies(const DeviceBlocks& blocks) {
    QHash<int, int> ids;
    for (const QVector<Device>& b : blocks) for (const Device& d : b) ids.insert(d.id, m_nextDeviceId++);
    DeviceBlocks out = blocks;
    for (QVector<Device>& b : out)
        for (Device& d : b) {
            d.id = ids.value(d.id);
            if (d.rack) d.rack = ids.value(d.rack, 0);
            for (MacroMap& m : d.maps) m.deviceId = ids.value(m.deviceId, m.deviceId);   // the copies' knobs, else the same as before
        }
    return out;
}

bool Project::moveDevices(int from, const QList<int>& ids, int to, const DevicePlace& at, bool copy, QList<int>* placed, QString* why) {
    auto fail = [why](const QString& w) { if (why) *why = w; return false; };
    const Track* src = trackAt(from);
    const Track* dst = trackAt(to);
    if (!src || !dst) return fail("There's no track there.");
    DeviceBlocks blocks = deviceBlocks(from, ids);
    if (blocks.isEmpty()) return fail("Those devices aren't there any more.");
    QSet<int> moving;   // every device going, racks' chains included
    for (const QVector<Device>& b : blocks) for (const Device& d : b) moving.insert(d.id);
    if (at.rackId && moving.contains(at.rackId)) return fail("A rack can't go inside itself.");
    if (copy && from == to && at.rackId == 0
        && std::any_of(blocks.begin(), blocks.end(), [](const QVector<Device>& b) { return b.first().kind == DeviceKind::Instrument; }))
        return fail("A track plays one instrument: drop the copy on another track, or into a rack's chain.");
    if (from != to)   // a sidechained device can't land on the track that keys it, or make a loop
        for (const QVector<Device>& b : blocks)
            for (const Device& d : b) {
                if (!d.sidechainTrack) continue;
                if (d.sidechainTrack == dst->id) return fail(d.name + " is keyed from that track.");
                if (keyReaches(dst->id, d.sidechainTrack)) return fail(d.name + "'s key would loop back through that track.");
            }
    DevicePlace place = at;
    if (!copy && from == to && place.beforeId > 0 && moving.contains(place.beforeId)) {
        // dropped next to itself: before the next device (at that level) that stays
        place.beforeId = 0;
        bool passed = false;
        for (const Device& d : src->devices) {
            if (d.id == at.beforeId) { passed = true; continue; }
            const bool level = at.rackId ? d.rack == at.rackId && d.chain == at.chain : d.rack == 0;
            if (passed && level && !moving.contains(d.id)) { place.beforeId = d.id; break; }
        }
    }
    const QString name = blocks.first().first().name;
    const QString label = blocks.size() == 1 ? QString(copy ? "Copy " : "Move ") + name : QString(copy ? "Copy Devices" : "Move Devices");
    if (copy) blocks = freshCopies(blocks);
    QVector<Device> srcDevs = src->devices;   // copies: nothing changes unless all of it fits
    if (!copy) srcDevs.erase(std::remove_if(srcDevs.begin(), srcDevs.end(), [&](const Device& d) { return moving.contains(d.id); }), srcDevs.end());
    QVector<Device> dstDevs = from == to ? srcDevs : dst->devices;
    QList<int> landed;
    if (!placeBlocks(dstDevs, blocks, place, !isReturn(to) && dst->type == TrackType::Midi, &landed, why)) return false;
    pruneMacroMaps(dstDevs);
    // to another track: their automation goes along
    QVector<Envelope> srcAuto = src->automation, dstAuto = dst->automation;
    if (!copy && from != to) {
        QVector<Envelope> stay;
        for (const Envelope& e : srcAuto) {
            const QStringList parts = e.target.split(':');
            if (parts.size() == 3 && parts[0] == "device" && moving.contains(parts[1].toInt())) dstAuto.push_back(e);
            else stay.push_back(e);
        }
        srcAuto = stay;
        pruneMacroMaps(srcDevs);
    }
    EditScope edit(this, label);
    Track* s = trackAt(from);   // after the snapshot
    Track* d = trackAt(to);
    if (from == to) {
        s->devices = dstDevs;
    } else {
        s->devices = srcDevs;
        d->devices = dstDevs;
        s->automation = srcAuto;
        d->automation = dstAuto;
    }
    emit structureChanged();
    if (placed) *placed = landed;
    return true;
}

bool Project::canConvertTrack(int index, TrackType to, QString* why) const {
    const Track* t = trackAt(index);
    auto no = [&](const QString& s) { if (why) *why = s; return false; };
    if (!t || isReturn(index) || isMaster(index) || t->isGroup() || (to != TrackType::Midi && to != TrackType::Audio)) return no("Only MIDI and audio tracks can change kind.");
    if (t->type == to) return true;
    bool clips = !t->arrangement.isEmpty();
    for (const ClipPtr& c : t->clipSlots) if (c) clips = true;
    if (clips) return no(t->name + " has clips: a track changes kind only while it holds none.");
    if (to == TrackType::Audio)
        for (const Device& d : t->devices) if (d.kind != DeviceKind::AudioEffect) return no(t->name + " has an instrument or MIDI effects: an audio track can't hold them.");
    return true;
}

bool Project::convertTrack(int index, TrackType to, QString* why) {
    if (!canConvertTrack(index, to, why)) return false;
    if (trackAt(index)->type == to) return true;
    EditScope edit(this, to == TrackType::Midi ? "Change to MIDI Track" : "Change to Audio Track");
    Track* t = trackAt(index);
    t->type = to;
    t->armed = false;
    t->monitor = Monitor::Auto;
    t->inputChannel = 0;
    emit structureChanged();
    emit mixerChanged();
    return true;
}

int Project::insertTrackFor(const DeviceBlocks& blocks, const QString& name, int after) {
    const bool midi = std::any_of(blocks.begin(), blocks.end(), [](const QVector<Device>& b) { return !b.isEmpty() && b.first().kind != DeviceKind::AudioEffect; });
    EditScope edit(this, midi ? "Insert MIDI Track" : "Insert Audio Track");
    const bool beside = after >= 0 && after < tracks.size();
    const int group = beside ? (tracks[after].isGroup() ? tracks[after].id : tracks[after].groupId) : 0;   // next to a track: in its group
    const int i = insertTrack(beside ? after + 1 : int(tracks.size()), midi ? TrackType::Midi : TrackType::Audio);
    tracks[i].groupId = group;   // after the last track: outside any group
    if (!name.trimmed().isEmpty()) tracks[i].name = name.trimmed();
    emit structureChanged();
    return i;
}

bool Project::deleteDevices(int trackIndex, const QList<int>& ids) {
    const DeviceBlocks blocks = deviceBlocks(trackIndex, ids);
    if (blocks.isEmpty()) return false;
    QSet<int> going;
    for (const QVector<Device>& b : blocks) for (const Device& d : b) going.insert(d.id);
    QVector<Device> devs = trackAt(trackIndex)->devices;
    devs.erase(std::remove_if(devs.begin(), devs.end(), [&](const Device& d) { return going.contains(d.id); }), devs.end());
    pruneMacroMaps(devs);
    EditScope edit(this, blocks.size() == 1 ? "Delete " + blocks.first().first().name : QString("Delete Devices"));
    trackAt(trackIndex)->devices = devs;
    emit structureChanged();
    return true;
}

void Project::setDeviceFolded(int trackIndex, int deviceId, bool folded) {
    const Track* t = trackAt(trackIndex);
    if (!t) return;
    for (int i = 0; i < t->devices.size(); ++i) {
        if (t->devices.at(i).id != deviceId || t->devices.at(i).folded == folded) continue;
        EditScope edit(this, folded ? "Fold Device" : "Unfold Device");
        trackAt(trackIndex)->devices[i].folded = folded;
        emit structureChanged();
        return;
    }
}

// ---- tracks ---------------------------------------------------------------

void Project::resizeSends() {
    for (Track& t : tracks) t.sends.resize(returns.size());
    for (Track& r : returns) r.sends.clear();
}

void Project::addTrack(const QString& name, const QColor& color) {
    Track t;
    t.id = m_nextTrackId++;
    t.name = name;
    t.color = color;
    t.clipSlots.resize(scenes.size());
    t.sends.resize(returns.size());
    tracks.push_back(t);
    emit structureChanged();
}

int Project::insertTrack(int at, TrackType type) {
    const bool audio = type == TrackType::Audio;
    EditScope edit(this, audio ? "Insert Audio Track" : "Insert MIDI Track");
    at = std::clamp(at, 0, int(tracks.size()));
    Track t;
    t.id = m_nextTrackId++;
    t.type = type;
    t.name = QString("%1 %2").arg(tracks.size() + 1).arg(audio ? "Audio" : "MIDI");
    t.color = palette()[(tracks.size() + m_colorIndex++) % palette().size()];
    t.clipSlots.resize(scenes.size());
    t.sends.resize(returns.size());
    if (at > 0 && type != TrackType::Group) {   // inside a group: the new track joins it
        const Track& before = tracks[at - 1];
        t.groupId = before.isGroup() ? before.id : before.groupId;
    }
    tracks.insert(at, t);
    selectedTrack = at;
    emit structureChanged();
    emit selectionChanged();
    return at;
}

int Project::insertGuitarTrack(int at) {
    EditScope edit(this, "Insert Guitar Track");
    const int i = insertTrack(at, TrackType::Audio);
    Track& t = tracks[i];   // (inside the scope: the snapshot was taken before the track existed)
    t.name = "Guitar";
    t.inputChannel = 1;          // Input 1: the interface's first input, where a guitar usually goes
    t.monitor = Monitor::In;     // hear it through the devices while you play
    t.devices = {makeDevice("Tuner", DeviceKind::AudioEffect), makeDevice("Guitar Amp", DeviceKind::AudioEffect)};
    emit structureChanged();
    emit mixerChanged();
    return i;
}

int Project::insertReturn() {
    if (returns.size() >= 8) return -1;   // the engine mixes up to 8 returns
    EditScope edit(this, "Insert Return Track");
    Track r;
    r.id = m_nextTrackId++;
    r.name = QString("%1 Return").arg(QChar(char16_t('A' + returns.size())));
    r.color = QColor("#9aa7b3");
    returns.push_back(r);
    resizeSends();
    selectedTrack = trackCount() - 1;
    emit structureChanged();
    emit mixerChanged();
    emit selectionChanged();
    return selectedTrack;
}

int Project::duplicateTrack(int index) {
    if (index < 0 || index >= tracks.size() || tracks[index].isGroup()) return -1;
    EditScope edit(this, "Duplicate Track");
    Track t = tracks[index];
    t.id = m_nextTrackId++;
    t.playingSlot = -1; t.playingClipBeat = -1; t.playingClipPass = 0;
    t.queuedSlot = -2; t.overridden = false; t.meter = 0; t.solo = false; t.armed = false;
    t.overriddenTargets.clear();
    QHash<const Clip*, ClipPtr> copies;   // a clip shared by a slot and the timeline stays shared in the copy
    auto copy = [&](const ClipPtr& c) -> ClipPtr {
        if (!c) return nullptr;
        if (!copies.contains(c.get())) copies.insert(c.get(), copyClip(c));
        return copies.value(c.get());
    };
    for (ClipPtr& c : t.clipSlots) c = copy(c);
    for (ArrangementClip& a : t.arrangement) a.clip = copy(a.clip);
    QHash<int, int> newIds;
    for (Device& d : t.devices) { int fresh = m_nextDeviceId++; newIds.insert(d.id, fresh); d.id = fresh; }
    for (Device& d : t.devices) if (d.rack) d.rack = newIds.value(d.rack, 0);   // chains stay in their (new) racks
    auto remap = [&newIds](QVector<Envelope>& envs) {
        for (Envelope& e : envs) {
            QStringList parts = e.target.split(':');
            if (parts.size() == 3 && parts[0] == "device" && newIds.contains(parts[1].toInt()))
                e.target = target::device(newIds.value(parts[1].toInt()), parts[2].toInt());
        }
    };
    remap(t.automation);
    for (const ClipPtr& c : copies) remap(c->envelopes);   // clip envelopes name the track's devices too
    for (Device& d : t.devices)                              // and Macros their targets
        for (MacroMap& m : d.maps) m.deviceId = newIds.value(m.deviceId, m.deviceId);
    tracks.insert(index + 1, t);
    selectedTrack = index + 1;
    emit structureChanged();
    emit selectionChanged();
    return index + 1;
}

void Project::deleteTrack(int index) {
    if (index < 0 || index >= trackCount()) return;
    if (!isReturn(index) && tracks.size() <= 1) return;   // keep at least one track
    EditScope edit(this, isReturn(index) ? "Delete Return Track" : "Delete Track");
    if (isReturn(index)) {
        const int k = index - int(tracks.size());
        returns.removeAt(k);
        for (Track& t : tracks) {
            if (k < t.sends.size()) t.sends.removeAt(k);
            // automation for this send goes; later sends move down one
            for (int e = t.automation.size() - 1; e >= 0; --e) {
                QStringList parts = t.automation[e].target.split(':');
                if (parts.size() != 3 || parts[1] != "send") continue;
                int s = parts[2].toInt();
                if (s == k) t.automation.removeAt(e);
                else if (s > k) t.automation[e].target = target::send(s - 1);
            }
        }
    } else {
        // A group goes with every track in it; keep at least one track.
        QList<int> doomed{index};
        if (tracks[index].isGroup()) doomed += membersOf(index);
        if (doomed.size() >= tracks.size()) return;
        std::sort(doomed.begin(), doomed.end(), std::greater<int>());
        for (int i : doomed) {
            if (selectedClip && trackOfClip(selectedClip) == i) selectedClip = nullptr;
            selectedTrackIds.remove(tracks[i].id);
            tracks.removeAt(i);
        }
        index = doomed.last();
    }
    selectedTrack = std::clamp(selectedTrack >= index ? selectedTrack - 1 : selectedTrack, 0, trackCount() - 1);
    emit structureChanged();
    emit mixerChanged();
    emit selectionChanged();
}

bool Project::moveTrack(int from, int to) {
    if (from < 0 || from >= tracks.size() || to < 0 || to >= tracks.size() || from == to) return false;
    EditScope edit(this, "Move Track");
    if (tracks[from].isGroup()) {
        // the group and its tracks move as one block, and never into another group
        const QList<int> members = membersOf(from);
        const int n = 1 + int(members.size());
        QVector<Track> block = tracks.mid(from, n);
        tracks.remove(from, n);
        int at = to > from ? std::max(0, to - n + 1) : to;
        at = std::clamp(at, 0, int(tracks.size()));
        while (at > 0 && at < tracks.size() && tracks[at].groupId) ++at;   // not between a group's tracks
        for (int k = 0; k < n; ++k) tracks.insert(at + k, block[k]);
        if (selectedTrack == from) selectedTrack = at;
    } else {
        tracks.move(from, to);
        // the moved track belongs to whatever group it lands in
        Track& t = tracks[to];
        const Track* before = to > 0 ? &tracks[to - 1] : nullptr;
        const Track* after = to + 1 < tracks.size() ? &tracks[to + 1] : nullptr;
        if (before && before->isGroup()) t.groupId = before->id;
        else if (before && before->groupId && after && after->groupId == before->groupId) t.groupId = before->groupId;
        else if (before && before->groupId && t.groupId == before->groupId) {}   // still the last one in its group
        else t.groupId = 0;
        if (selectedTrack == from) selectedTrack = to;
    }
    emit structureChanged();
    emit mixerChanged();
    emit selectionChanged();
    return true;
}

// ---- groups -------------------------------------------------------------------

QList<int> Project::selectedTrackIndexes() const {
    QList<int> out;
    for (int i = 0; i < tracks.size(); ++i) if (selectedTrackIds.contains(tracks[i].id)) out << i;
    if (out.isEmpty() && selectedTrack >= 0 && selectedTrack < tracks.size()) out << selectedTrack;
    return out;
}

int Project::groupOf(int index) const {
    if (index < 0 || index >= tracks.size() || !tracks[index].groupId) return -1;
    for (int i = 0; i < tracks.size(); ++i) if (tracks[i].id == tracks[index].groupId && tracks[i].isGroup()) return i;
    return -1;
}

QList<int> Project::membersOf(int groupIndex) const {
    QList<int> out;
    if (groupIndex < 0 || groupIndex >= tracks.size() || !tracks[groupIndex].isGroup()) return out;
    for (int i = 0; i < tracks.size(); ++i) if (tracks[i].groupId == tracks[groupIndex].id) out << i;
    return out;
}

ArrangementSpan Project::groupArrangementSpan(int groupIndex) const {
    ArrangementSpan span;
    if (groupIndex < 0 || groupIndex >= tracks.size() || !tracks[groupIndex].isGroup()) return span;
    const int id = tracks[groupIndex].id;
    bool found = false;
    for (const Track& t : tracks) {
        if (t.groupId != id) continue;
        for (const ArrangementClip& a : t.arrangement) {
            if (!a.clip || a.length <= 0) continue;
            span.start = found ? std::min(span.start, a.start) : a.start;
            span.end = found ? std::max(span.end, a.start + a.length) : a.start + a.length;
            found = true;
        }
    }
    return span;
}

bool Project::hiddenByFold(int index) const {
    const int g = groupOf(index);
    return g >= 0 && tracks[g].folded;
}

void Project::setFolded(int groupIndex, bool folded) {
    if (groupIndex < 0 || groupIndex >= tracks.size() || !tracks[groupIndex].isGroup() || tracks[groupIndex].folded == folded) return;
    tracks[groupIndex].folded = folded;
    emit structureChanged();
}

int Project::groupTracks(const QList<int>& indexes) {
    QList<int> picked;
    for (int i : indexes) if (i >= 0 && i < tracks.size() && !tracks[i].isGroup() && !picked.contains(i)) picked << i;
    if (picked.isEmpty()) return -1;
    std::sort(picked.begin(), picked.end());
    EditScope edit(this, "Group Tracks");
    Track g;
    g.id = m_nextTrackId++;
    g.type = TrackType::Group;
    int groups = 0;
    for (const Track& t : tracks) if (t.isGroup()) ++groups;
    g.name = groups ? QString("Group %1").arg(groups + 1) : QString("Group");
    g.color = tracks[picked.first()].color;
    g.clipSlots.resize(scenes.size());
    g.sends.resize(returns.size());
    // take the tracks out (keeping their order), then put the group and them where the first one
    // was; groups don't nest, so tracks picked out of a group make the new one right after it
    const int fromGroup = tracks[picked.first()].groupId;
    QVector<Track> members;
    for (int k = int(picked.size()) - 1; k >= 0; --k) { members.prepend(tracks[picked[k]]); tracks.removeAt(picked[k]); }
    int at = picked.first();
    if (fromGroup) {
        at = 0;
        while (at < tracks.size() && tracks[at].id != fromGroup) ++at;
        if (at < tracks.size()) ++at;
        while (at < tracks.size() && tracks[at].groupId == fromGroup) ++at;
    }
    tracks.insert(at, g);
    for (int k = 0; k < members.size(); ++k) {
        members[k].groupId = g.id;
        tracks.insert(at + 1 + k, members[k]);
    }
    // a group left with no tracks goes
    for (int i = int(tracks.size()) - 1; i >= 0; --i) {
        if (!tracks[i].isGroup() || tracks[i].id == g.id) continue;
        const int gid = tracks[i].id;
        if (std::none_of(tracks.begin(), tracks.end(), [gid](const Track& t) { return t.groupId == gid; })) tracks.removeAt(i);
    }
    at = 0;
    while (tracks[at].id != g.id) ++at;
    selectedTrack = at;
    selectedTrackIds = {g.id};
    emit structureChanged();
    emit mixerChanged();
    emit selectionChanged();
    return at;
}

void Project::ungroup(int groupIndex) {
    if (groupIndex < 0 || groupIndex >= tracks.size() || !tracks[groupIndex].isGroup()) return;
    EditScope edit(this, "Ungroup Tracks");
    const int id = tracks[groupIndex].id;
    for (Track& t : tracks) if (t.groupId == id) t.groupId = 0;
    tracks.removeAt(groupIndex);
    selectedTrack = std::clamp(groupIndex, 0, trackCount() - 1);
    selectedTrackIds.clear();
    emit structureChanged();
    emit mixerChanged();
    emit selectionChanged();
}

void Project::renameTrack(int index, const QString& name) {
    Track* t = trackAt(index);
    if (!t || name.trimmed().isEmpty() || t->name == name.trimmed()) return;
    EditScope edit(this, "Rename Track");
    t->name = name.trimmed();
    emit structureChanged();
}

void Project::setTrackColor(int index, const QColor& c) {
    Track* t = trackAt(index);
    if (!t || t->color == c) return;
    EditScope edit(this, "Track Color");
    t->color = c;
    emit structureChanged();
}

// ---- scenes ---------------------------------------------------------------

int Project::insertScene(int at) {
    EditScope edit(this, "Insert Scene");
    at = std::clamp(at, 0, int(scenes.size()));
    scenes.insert(at, QString());
    for (Track& t : tracks) t.clipSlots.insert(at, nullptr);
    emit structureChanged();
    return at;
}

bool Project::moveScene(int from, int to) {
    const int n = int(scenes.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    EditScope edit(this, "Move Scene");
    scenes.move(from, to);
    auto remap = [from, to](int s) {
        if (s == from) return to;
        if (from < to && s > from && s <= to) return s - 1;
        if (from > to && s >= to && s < from) return s + 1;
        return s;
    };
    for (Track& t : tracks) {
        if (from < t.clipSlots.size() && to < t.clipSlots.size()) t.clipSlots.move(from, to);
        if (t.playingSlot >= 0) t.playingSlot = remap(t.playingSlot);   // the engine finds them by clip anyway
        if (t.queuedSlot >= 0) t.queuedSlot = remap(t.queuedSlot);
    }
    emit structureChanged();
    return true;
}

int Project::duplicateScene(int index) {
    if (index < 0 || index >= scenes.size()) return -1;
    EditScope edit(this, "Duplicate Scene");
    scenes.insert(index + 1, scenes[index]);
    for (Track& t : tracks) t.clipSlots.insert(index + 1, copyClip(t.clipSlots.value(index)));
    emit structureChanged();
    return index + 1;
}

void Project::deleteScene(int index) {
    if (index < 0 || index >= scenes.size() || scenes.size() <= 1) return;
    EditScope edit(this, "Delete Scene");
    scenes.removeAt(index);
    for (Track& t : tracks) {
        if (index < t.clipSlots.size()) {
            if (selectedClip && t.clipSlots[index] == selectedClip && trackOfClip(selectedClip) >= 0) {
                bool elsewhere = false;
                for (const auto& a : t.arrangement) elsewhere |= a.clip == selectedClip;
                if (!elsewhere) selectedClip = nullptr;
            }
            t.clipSlots.removeAt(index);
        }
    }
    emit structureChanged();
    emit selectionChanged();
}

void Project::renameScene(int index, const QString& name) {
    if (index < 0 || index >= scenes.size() || scenes[index] == name.trimmed()) return;
    EditScope edit(this, "Rename Scene");
    scenes[index] = name.trimmed();
    emit structureChanged();
}

int Project::captureScene(int after) {
    EditScope edit(this, "Capture and Insert Scene");
    const int at = std::clamp(after + 1, 0, int(scenes.size()));
    scenes.insert(at, QString());
    for (Track& t : tracks) {
        ClipPtr playing = t.playingSlot >= 0 ? t.clipSlots.value(t.playingSlot) : nullptr;
        t.clipSlots.insert(at, copyClip(playing));
    }
    emit structureChanged();
    return at;
}

// ---- automation helpers ---------------------------------------------------

QStringList Project::automatableTargets(int index) const {
    QStringList out;
    const Track* t = trackAt(index);
    if (!t) return out;
    out << target::volume() << target::pan();
    if (!isReturn(index)) for (int k = 0; k < t->sends.size(); ++k) out << target::send(k);
    for (const Device& d : t->devices)
        for (int p = 0; p < d.params.size(); ++p) out << target::device(d.id, p);
    return out;
}

QString Project::targetName(int index, const QString& tg) const {
    const Track* t = trackAt(index);
    QStringList parts = tg.split(':');
    if (tg == target::volume()) return "Volume";
    if (tg == target::pan()) return "Pan";
    if (parts.size() == 3 && parts[1] == "send") {
        int k = parts[2].toInt();
        return k < returns.size() ? "Send " + QString(QChar('A' + k)) : "Send";
    }
    if (t && parts.size() == 3 && parts[0] == "device") {
        int id = parts[1].toInt(), p = parts[2].toInt();
        for (const Device& d : t->devices)
            if (d.id == id && p < d.params.size()) return d.name + " · " + d.params[p].name;
        return "(deleted device)";
    }
    return tg;
}

double Project::targetValue(int index, const QString& tg) const {
    const Track* t = trackAt(index);
    if (!t) return 0;
    if (tg == target::volume()) return t->volume;
    if (tg == target::pan()) return (t->pan + 1) / 2;
    QStringList parts = tg.split(':');
    if (parts.size() == 3 && parts[1] == "send") return t->sends.value(parts[2].toInt());
    if (parts.size() == 3 && parts[0] == "device") {
        int id = parts[1].toInt(), p = parts[2].toInt();
        for (const Device& d : t->devices) if (d.id == id && p < d.params.size()) return d.params[p].value;
    }
    return 0;
}

void Project::setTargetValue(int index, const QString& tg, double v) {
    Track* t = trackAt(index);
    if (!t) return;
    v = std::clamp(v, 0.0, 1.0);
    if (tg == target::volume()) { t->volume = v; return; }
    if (tg == target::pan()) { t->pan = v * 2 - 1; return; }
    QStringList parts = tg.split(':');
    if (parts.size() == 3 && parts[1] == "send") { int k = parts[2].toInt(); if (k < t->sends.size()) t->sends[k] = v; return; }
    if (parts.size() == 3 && parts[0] == "device") {
        int id = parts[1].toInt(), p = parts[2].toInt();
        for (Device& d : t->devices) if (d.id == id && p < d.params.size()) d.params[p].value = v;
    }
}

ClipPtr Project::playingClip(int index, double beat, double* local) const {
    const Track* t = trackAt(index);
    if (!t || isReturn(index)) return nullptr;
    ClipPtr c;
    double at = -1;
    if (t->playingSlot >= 0) {
        c = t->clipSlots.value(t->playingSlot);
        if (c) at = !c->warp && t->playingClipBeat >= 0 ? t->playingClipBeat : c->positionAt(beat - t->slotStart);
    } else if (!t->overridden) {
        for (const ArrangementClip& a : t->arrangement)
            if (a.clip && beat >= a.start && beat < a.start + a.length) { c = a.clip; at = c->positionFrom(a.offset, beat - a.start); }
    }
    if (!c || at < 0) return nullptr;
    if (local) *local = at;
    return c;
}

double Project::clipPosition(const ClipPtr& clip, double beat) const {
    const Track* t = clip ? trackAt(trackOfClip(clip)) : nullptr;
    if (!t) return -1;
    if (t->playingSlot >= 0 && t->clipSlots.value(t->playingSlot) == clip)
        return !clip->warp && t->playingClipBeat >= 0 ? t->playingClipBeat : clip->positionAt(beat - t->slotStart);
    if (!t->overridden)
        for (const ArrangementClip& a : t->arrangement)
            if (a.clip == clip && beat >= a.start && beat < a.start + a.length) return clip->positionFrom(a.offset, beat - a.start);
    return -1;
}

const MacroMap* Project::macroFor(int index, int deviceId, int param, int* macrosDeviceId) const {
    const Track* t = trackAt(index);
    if (!t) return nullptr;
    for (const Device& d : t->devices)
        for (const MacroMap& m : d.maps)
            if (m.deviceId == deviceId && m.param == param) {
                if (macrosDeviceId) *macrosDeviceId = d.id;
                return &m;
            }
    return nullptr;
}

double Project::drivenValue(int index, const QString& tg, double beat, bool playing) const {
    const Track* t = trackAt(index);
    if (!t) return -1;
    // the playing clip's envelope, else the track's automation
    auto enveloped = [&](const QString& target) {
        double local = 0;
        if (playing)
            if (const ClipPtr c = playingClip(index, beat, &local))
                if (const Envelope* e = std::as_const(*c).envelope(target); e && !e->points.isEmpty()) return e->valueAt(local);
        const Envelope* e = t->envelope(target);
        if (e && !e->points.isEmpty() && !t->overriddenTargets.contains(target)) return e->valueAt(beat);
        return -1.0;
    };
    const QStringList parts = tg.split(':');
    if (parts.size() == 3 && parts[0] == "device") {
        int macrosId = 0;
        if (const MacroMap* m = macroFor(index, parts[1].toInt(), parts[2].toInt(), &macrosId)) {
            const QString knob = target::device(macrosId, m->macro);
            double v = enveloped(knob);
            if (v < 0) v = targetValue(index, knob);
            return std::clamp(m->lo + (m->hi - m->lo) * v, 0.0, 1.0);
        }
    }
    return enveloped(tg);
}

void Project::touchTarget(int index, const QString& tg) {
    Track* t = trackAt(index);
    if (!t || !t->envelope(tg) || t->overriddenTargets.contains(tg)) return;
    t->overriddenTargets.insert(tg);
    emit automationChanged();
}

bool Project::anyAutomationOverridden() const {
    for (int i = 0; i < trackCount(); ++i) if (!trackAt(i)->overriddenTargets.isEmpty()) return true;
    return false;
}

void Project::reEnableAutomation() {
    for (int i = 0; i < trackCount(); ++i) trackAt(i)->overriddenTargets.clear();
    emit automationChanged();
}

bool Project::anySolo() const {
    for (const Track& t : tracks) if (t.solo) return true;
    return false;
}

void Project::setTempo(double bpm) {
    if (bpm == tempo) return;
    tempo = bpm;
    // A tempo move changes playback speed, never the source grid or musical placement.
    // In particular, crossing a half/double-time threshold must not jump a live clip.
    emit tempoChanged();
}

// ---- undo -------------------------------------------------------------------

void Project::beginEdit(const QString& label, quintptr mergeKey) {
    if (m_editDepth++ > 0) return;
    m_editBefore = captureEditState();
    m_editLabel = label;
    m_editKey = mergeKey;
}

void Project::endEdit() {
    if (m_editDepth == 0 || --m_editDepth > 0) return;
    EditState after = captureEditState();
    if (after == m_editBefore) return;
    m_undo->push(new EditCommand(this, m_editLabel, m_editKey, std::move(m_editBefore), std::move(after)));
    m_editBefore = {};
}

EditState Project::captureEditState() const {
    EditState s;
    s.scenes = scenes;
    s.tempo = tempo;
    s.mainVolume = mainVolume;
    s.locators = locators;
    auto keep = [&s](const ClipPtr& c) {
        if (c && !s.clips.contains(c->id)) s.clips.insert(c->id, *c);
        return c ? c->id : 0;
    };
    auto capture = [&](const Track& t) {
        EditState::TrackState ts{t.id, t.name, t.color, t.volume, t.pan, t.sends, t.active, t.preFader, {}, {}, t.devices, t.automation, t.type,
                                 t.monitor, t.inputChannel, t.groupId};
        ts.groupFadeIn = t.groupFadeIn; ts.groupFadeOut = t.groupFadeOut;
        ts.groupFadeInCurve = t.groupFadeInCurve; ts.groupFadeOutCurve = t.groupFadeOutCurve;
        for (const ClipPtr& c : t.clipSlots) ts.slotClips.push_back(keep(c));
        for (const ArrangementClip& a : t.arrangement) ts.arrangement.push_back({a.start, a.length, a.offset, keep(a.clip), a.fadeIn, a.fadeOut, a.fadeInCurve, a.fadeOutCurve});
        return ts;
    };
    for (const Track& t : tracks) s.tracks.push_back(capture(t));
    for (const Track& r : returns) s.returns.push_back(capture(r));
    s.master = capture(master);
    return s;
}

void Project::restoreEditState(const EditState& s) {
    // Reuse live Clip objects by id so editors holding a ClipPtr keep pointing at the right clip.
    QHash<int, ClipPtr> live;
    for (auto it = s.clips.begin(); it != s.clips.end(); ++it) {
        ClipPtr c = m_clipRegistry.value(it.key()).lock();
        if (!c) {
            c = std::make_shared<Clip>();
            m_clipRegistry.insert(it.key(), c);
        }
        *c = it.value();
        live.insert(c->id, c);
    }

    // Rebuild the track lists, keeping each surviving track's runtime state (playback, solo, arm, meters).
    auto rebuild = [&](QVector<Track>& list, const QVector<EditState::TrackState>& states) {
        QHash<int, Track> old;
        for (Track& t : list) old.insert(t.id, std::move(t));
        list.clear();
        for (const EditState::TrackState& ts : states) {
            Track t = old.contains(ts.id) ? old.take(ts.id) : Track{};
            t.id = ts.id; t.type = ts.type; t.name = ts.name; t.color = ts.color;
            t.volume = ts.volume; t.pan = ts.pan; t.sends = ts.sends; t.active = ts.active; t.preFader = ts.preFader;
            t.monitor = ts.monitor; t.inputChannel = ts.inputChannel; t.groupId = ts.groupId;
            t.groupFadeIn = ts.groupFadeIn; t.groupFadeOut = ts.groupFadeOut;
            t.groupFadeInCurve = ts.groupFadeInCurve; t.groupFadeOutCurve = ts.groupFadeOutCurve;
            t.clipSlots.clear();
            for (int id : ts.slotClips) t.clipSlots.push_back(live.value(id));
            t.arrangement.clear();
            for (const EditState::Placed& p : ts.arrangement) t.arrangement.push_back({p.start, p.length, p.offset, live.value(p.clip), p.fadeIn, p.fadeOut, p.fadeInCurve, p.fadeOutCurve});
            t.devices = ts.devices;
            t.automation = ts.automation;
            list.push_back(std::move(t));
        }
    };
    rebuild(tracks, s.tracks);
    rebuild(returns, s.returns);
    master.devices = s.master.devices;
    master.automation = s.master.automation;

    scenes = s.scenes;
    locators = s.locators;
    mainVolume = s.mainVolume;
    const bool tempoMoved = tempo != s.tempo;
    tempo = s.tempo;
    if (selectedClip && !live.contains(selectedClip->id)) selectedClip = nullptr;
    selectedTrack = std::clamp(selectedTrack, 0, std::max(0, trackCount()));   // trackCount(): the Master

    // Drop registry entries nobody holds any more.
    for (auto it = m_clipRegistry.begin(); it != m_clipRegistry.end();)
        it = it->expired() ? m_clipRegistry.erase(it) : std::next(it);

    emit restored();
    emit structureChanged();
    emit clipChanged();
    emit mixerChanged();
    emit automationChanged();
    emit locatorsChanged();
    if (tempoMoved) emit tempoChanged();
    emit selectionChanged();
}

// ---- demo content -------------------------------------------------------

static Note N(int p, double s, double d, double v = 0.8) { return Note{p, s, d, v, 1.0}; }

static QVector<Note> beatNotes() {
    QVector<Note> n;
    for (int i = 0; i < 8; ++i) n.push_back(N(42, i / 2.0, 0.25, i % 2 ? 0.5 : 0.8));   // hats
    for (int b = 0; b < 4; ++b) n.push_back(N(36, b, 0.25, 0.95));                       // kick
    n.push_back(N(38, 1, 0.25, 0.85));                                                    // snare
    n.push_back(N(38, 3, 0.25, 0.85));
    return n;
}

void Project::clearSet() {
    tracks.clear();
    returns.clear();
    master.devices.clear();
    master.automation.clear();
    scenes.clear();
    locators.clear();
    selectedTrack = 0;
    selectedClip = nullptr;
    m_clipRegistry.clear();
    mappings.clear();
    m_nextClipId = m_nextDeviceId = m_nextTrackId = 1;
    m_colorIndex = 0;
    m_editDepth = 0;   // a drag that was recording into the old set ends with it
    m_editBefore = {};
    m_undo->clear();
}

void Project::addDefaultReturns() {
    // Effects only, fully wet.
    Track ra; ra.id = m_nextTrackId++; ra.name = "A Reverb"; ra.color = QColor("#9db4c8");
    ra.devices = {makeDevice("Reverb", DeviceKind::AudioEffect)};
    ra.devices[0].params[0].value = 0.65;   // size
    ra.devices[0].params[1].value = 0.6;    // decay
    ra.devices[0].params[3].value = 1.0;    // dry/wet
    Track rb; rb.id = m_nextTrackId++; rb.name = "B Delay"; rb.color = QColor("#c8b39d");
    rb.devices = {makeDevice("Delay", DeviceKind::AudioEffect)};
    rb.devices[0].params[3].value = 1.0;
    returns = {ra, rb};
    resizeSends();
}

void Project::loadDemo() {
    clearSet();
    tempo = 120;
    mainVolume = 0.75;
    cueVolume = 0.6;
    scenes = {"Intro", "Verse", "Chorus", "Break", "Outro"};

    addTrack("Drums", QColor("#f2905e"));
    addTrack("Bass",  QColor("#f3c95c"));
    addTrack("Keys",  QColor("#62c9dc"));
    addTrack("Pad",   QColor("#a593f0"));

    ClipPtr d1 = newClip("Beat A", 4); d1->notes = beatNotes();
    ClipPtr d2 = newClip("Beat B", 4); d2->notes = beatNotes();
    d2->notes.push_back(N(36, 2.5, 0.25, 0.7));
    d2->notes.push_back(N(38, 3.75, 0.25, 0.45));

    ClipPtr b1 = newClip("Bass Line", 8);
    const int bassSeq[] = {33, 33, 45, 33, 40, 40, 38, 36};
    for (int i = 0; i < 8; ++i) {
        b1->notes.push_back(N(bassSeq[i], i, 0.4, 0.85));
        b1->notes.push_back(N(bassSeq[i] + 12, i + 0.5, 0.2, 0.55));
    }

    ClipPtr k1 = newClip("Chords", 8);
    const int chords[4][3] = {{57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 55, 59}};
    for (int i = 0; i < 4; ++i)
        for (int p : chords[i]) k1->notes.push_back(N(p, i * 2, 1.75, 0.7));

    ClipPtr p1 = newClip("Pad Swell", 16);
    const int pads[2][4] = {{45, 52, 57, 64}, {41, 48, 53, 60}};
    for (int i = 0; i < 2; ++i)
        for (int p : pads[i]) p1->notes.push_back(N(p, i * 8, 8, 0.6));

    tracks[0].clipSlots[0] = d1; tracks[0].clipSlots[1] = d2;
    tracks[1].clipSlots[1] = b1;
    tracks[2].clipSlots[2] = k1;
    tracks[3].clipSlots[0] = p1;

    tracks[0].arrangement = {{0, 16, 0, d1}, {16, 16, 0, d2}};
    tracks[1].arrangement = {{16, 16, 0, b1}};
    tracks[2].arrangement = {{24, 16, 0, k1}};
    tracks[3].arrangement = {{0, 16, 0, p1}};

    tracks[0].devices = {makeDevice("Drum Kit", DeviceKind::Instrument), makeDevice("Compressor", DeviceKind::AudioEffect)};
    tracks[1].devices = {makeDevice("Analog Synth", DeviceKind::Instrument), makeDevice("Saturator", DeviceKind::AudioEffect)};
    tracks[2].devices = {makeDevice("FM Synth", DeviceKind::Instrument), makeDevice("Reverb", DeviceKind::AudioEffect)};
    tracks[3].devices = {makeDevice("Wavetable Synth", DeviceKind::Instrument), makeDevice("Delay", DeviceKind::AudioEffect), makeDevice("Reverb", DeviceKind::AudioEffect)};

    addDefaultReturns();
    tracks[0].sends = {0.12, 0.0};
    tracks[2].sends = {0.35, 0.2};
    tracks[3].sends = {0.4, 0.0};

    // The pad fades in over its first two bars.
    tracks[3].automation = {{target::volume(), {{0, 0.35}, {8, 0.75}}}};

    selectedTrack = 0;
    selectedClip = d1;
    emitAllChanged();
}

void Project::loadEmpty() {
    clearSet();
    tempo = 120;
    mainVolume = 0.75;
    cueVolume = 0.6;
    for (int i = 0; i < 8; ++i) scenes.push_back(QString());
    addTrack("1 MIDI", palette()[0]);
    addTrack("2 MIDI", palette()[1]);
    addDefaultReturns();
    emitAllChanged();
}

void Project::emitAllChanged() {
    emit restored();
    emit structureChanged();
    emit clipChanged();
    emit mixerChanged();
    emit automationChanged();
    emit locatorsChanged();
    emit tempoChanged();
    emit selectionChanged();
}

void Project::loadEditState(const EditState& s) {
    clearSet();
    // New ids continue after the highest one in the set (envelopes of deleted devices count too).
    int maxTrack = 0, maxDevice = 0;
    auto scan = [&](const QVector<EditState::TrackState>& list) {
        for (const EditState::TrackState& t : list) {
            maxTrack = std::max(maxTrack, t.id);
            for (const Device& d : t.devices) maxDevice = std::max(maxDevice, d.id);
            for (const Envelope& e : t.automation) {
                const QStringList parts = e.target.split(':');
                if (parts.size() == 3 && parts[0] == "device") maxDevice = std::max(maxDevice, parts[1].toInt());
            }
        }
    };
    scan(s.tracks);
    scan(s.returns);
    scan(QVector<EditState::TrackState>{s.master});
    for (const Clip& c : s.clips)   // clip envelopes name devices too
        for (const Envelope& e : c.envelopes) {
            const QStringList parts = e.target.split(':');
            if (parts.size() == 3 && parts[0] == "device") maxDevice = std::max(maxDevice, parts[1].toInt());
        }
    m_nextTrackId = maxTrack + 1;
    m_nextDeviceId = maxDevice + 1;
    m_nextClipId = (s.clips.isEmpty() ? 0 : s.clips.lastKey()) + 1;
    restoreEditState(s);   // builds fresh tracks and clips (nothing survives clearSet) and emits every change signal
}

} // namespace sessions
