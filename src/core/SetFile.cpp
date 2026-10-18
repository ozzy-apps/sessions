#include "SetFile.h"
#include "audio/Nam.h"
#include "DeviceSpecs.h"
#include "Scales.h"
#include "audio/Builtin.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace sessions::setfile {

namespace {

const char* kFormatName = "Sessions Set";

// ---- enums as words, so the file reads well and survives reordering ----------

const char* const kKinds[]   = {"instrument", "audioEffect", "midiEffect"};
const char* const kTypes[]   = {"midi", "audio", "group"};
const char* const kMonitors[] = {"in", "auto", "off"};
const char* const kWarps[]   = {"beats", "tones", "texture", "repitch", "complex", "complexpro"};   // WarpMode's order
const char* const kSources[] = {"cc", "note", "key"};
const char* const kModes[]   = {"trigger", "gate", "toggle", "repeat"};
const char* const kFollows[] = {"none", "stop", "again", "previous", "next", "first", "last", "any", "other"};

template <size_t N>
int wordIndex(const char* const (&words)[N], const QJsonValue& v, int fallback) {
    const QString s = v.toString();
    for (size_t i = 0; i < N; ++i) if (s == QLatin1String(words[i])) return int(i);
    return fallback;
}

// Numbers come back as doubles; anything missing, null or not finite takes the default.
double num(const QJsonObject& o, const char* key, double def) {
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return def;
    const double d = v.toDouble();
    return std::isfinite(d) ? d : def;
}
int integer(const QJsonObject& o, const char* key, int def) {
    const double d = num(o, key, def);
    return d >= -2147483648.0 && d <= 2147483647.0 ? int(std::lround(d)) : def;
}
bool flag(const QJsonObject& o, const char* key, bool def) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : def;
}
QString text(const QJsonObject& o, const char* key, const QString& def = {}) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : def;
}

// ---- sample paths ------------------------------------------------------------

QJsonObject writePath(const QString& abs, const QString& baseDir) {
    if (audio::isBuiltin(abs)) return QJsonObject{{"builtin", abs}};   // a built-in sound: no file
    QJsonObject o{{"path", abs}};
    if (!baseDir.isEmpty()) o.insert("relativePath", QDir(baseDir).relativeFilePath(abs));
    return o;
}

// The relative path wins when that file exists (the set moved along with its samples).
QString readPath(const QJsonObject& o, const QString& baseDir) {
    if (const QString b = text(o, "builtin"); audio::isBuiltin(b)) return b;
    const QString rel = text(o, "relativePath"), abs = text(o, "path");
    if (!baseDir.isEmpty() && !rel.isEmpty()) {
        const QString candidate = QDir::cleanPath(QDir(baseDir).absoluteFilePath(rel));
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return abs.isEmpty() || QFileInfo(abs).isAbsolute() ? abs : QDir::cleanPath(QDir(baseDir).absoluteFilePath(abs));
}

// ---- writing ---------------------------------------------------------------

QJsonArray writeEnvelopes(const QVector<Envelope>& envelopes) {
    QJsonArray out;
    for (const Envelope& e : envelopes) {
        QJsonArray points;
        for (const AutoPoint& p : e.points) points.append(QJsonArray{p.beat, p.value});
        out.append(QJsonObject{{"target", e.target}, {"points", points}});
    }
    return out;
}

// Envelopes back: points sorted, values clamped, one envelope per target.
QVector<Envelope> readEnvelopes(const QJsonArray& arr) {
    QVector<Envelope> out;
    for (const QJsonValue& v : arr) {
        const QJsonObject eo = v.toObject();
        Envelope e;
        e.target = eo.value("target").toString();
        for (const QJsonValue& pv : eo.value("points").toArray()) {
            const QJsonArray p = pv.toArray();
            if (p.size() < 2 || !p[0].isDouble() || !p[1].isDouble() || !std::isfinite(p[0].toDouble()) || !std::isfinite(p[1].toDouble())) continue;
            e.points.push_back({std::max(0.0, p[0].toDouble()), std::clamp(p[1].toDouble(), 0.0, 1.0)});
        }
        std::stable_sort(e.points.begin(), e.points.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.beat < b.beat; });
        const bool taken = std::any_of(out.begin(), out.end(), [&](const Envelope& o) { return o.target == e.target; });
        if (!e.target.isEmpty() && !taken) out.push_back(e);
    }
    return out;
}

QJsonObject writeClip(const Clip& c, const QString& baseDir) {
    QJsonArray notes;
    for (const Note& n : c.notes)
        notes.append(QJsonObject{{"pitch", n.pitch}, {"start", n.start}, {"length", n.length},
                                 {"velocity", n.velocity}, {"chance", n.chance}});
    const QJsonObject follow{{"enabled", c.followEnabled}, {"actionA", kFollows[int(c.followA)]},
                             {"actionB", kFollows[int(c.followB)]}, {"chanceA", c.followChanceA}, {"time", c.followTime}};
    const QJsonObject launch{{"mode", kModes[int(c.launchMode)]}, {"legato", c.legato},
                             {"quantization", c.quantization}, {"follow", follow}};
    QJsonObject scale{{"on", c.scaleOn}, {"root", c.scaleRoot}};
    if (c.scale >= 0 && c.scale < kScaleCount) scale.insert("name", kScales[c.scale].name);
    else scale.insert("index", c.scale);
    QJsonObject o{{"id", c.id}, {"name", c.name}, {"length", c.length}, {"loop", c.loop},
                  {"start", c.start}, {"loopStart", c.loopStart},
                  {"color", c.color.isValid() ? c.color.name() : QString()},
                  {"launch", launch}, {"scale", scale}, {"notes", notes}};
    if (!c.envelopes.isEmpty()) o.insert("envelopes", writeEnvelopes(c.envelopes));
    if (c.isAudio()) {
        QJsonObject audio = writePath(c.samplePath, baseDir);
        QJsonArray markers;
        for (const WarpMarker& m : c.warpMarkers) markers.append(QJsonArray{m.beat, m.seconds});
        audio.insert("warpMode", kWarps[int(c.warpMode)]);
        audio.insert("markers", markers);
        audio.insert("gain", c.gain);
        audio.insert("transpose", c.transpose);
        audio.insert("reverse", c.reverse);
        audio.insert("warp", c.warp);
        audio.insert("grain", c.grain);
        audio.insert("grainMs", c.grainMs);
        audio.insert("flux", c.flux);
        audio.insert("formants", c.formants);
        if (c.detectedBpm > 0) audio.insert("detectedBpm", c.detectedBpm);
        o.insert("audio", audio);
        if (!c.takes.isEmpty()) {   // loop-recorded passes, and which plays where (none: the newest)
            QJsonArray takes, comp;
            for (const Take& t : c.takes) {
                QJsonObject to = writePath(t.samplePath, baseDir);
                to.insert("name", t.name);
                to.insert("start", t.startBeat);
                takes.append(to);
            }
            for (const CompRegion& r : c.comp) comp.append(QJsonArray{r.start, r.end, r.take});
            o.insert("takes", takes);
            o.insert("comp", comp);
        }
    }
    return o;
}

QJsonObject writeDevice(const Device& d, const QString& baseDir) {
    QJsonArray params;
    for (const DeviceParam& p : d.params) params.append(QJsonObject{{"name", p.name}, {"value", p.value}});
    QJsonObject o{{"id", d.id}, {"name", d.name}, {"kind", kKinds[int(d.kind)]}, {"enabled", d.enabled}, {"params", params}};
    if (!d.samplePath.isEmpty()) o.insert("sample", writePath(d.samplePath, baseDir));
    if (!d.modelPath.isEmpty()) o.insert("model", writePath(d.modelPath, baseDir));   // NAM: its capture
    if (!d.pads.isEmpty()) {   // Drum Rack
        QJsonArray pads;
        for (const DrumPad& p : d.pads) {
            QJsonObject po{{"name", p.name}};
            if (!p.samplePath.isEmpty()) po.insert("sample", writePath(p.samplePath, baseDir));
            pads.append(po);
        }
        o.insert("pads", pads);
    }
    if (!d.maps.isEmpty()) {   // Macros
        QJsonArray maps;
        for (const MacroMap& m : d.maps)
            maps.append(QJsonObject{{"macro", m.macro}, {"device", m.deviceId}, {"param", m.param}, {"min", m.lo}, {"max", m.hi}});
        o.insert("maps", maps);
    }
    if (!d.mods.isEmpty()) {   // Monolith: modulation routes
        QJsonArray mods;
        for (const ModRoute& r : d.mods) mods.append(QJsonObject{{"source", r.source}, {"param", r.param}, {"amount", r.amount}});
        o.insert("mods", mods);
    }
    if (std::any_of(d.curves.begin(), d.curves.end(), [](const QVector<CurvePoint>& c) { return !c.isEmpty(); })) {   // Monolith: drawn curves (0.21): [[x, y], …] per curve
        QJsonArray curves;
        for (const QVector<CurvePoint>& c : d.curves) {
            QJsonArray pts;
            for (const CurvePoint& pt : c) {
                QJsonArray point{pt.x, pt.y};
                if (pt.bend != 0 || pt.shape != 0) { point.append(pt.bend); point.append(pt.shape); }
                pts.append(point);
            }
            curves.append(pts);
        }
        o.insert("curves", curves);
    }
    if (!d.chains.isEmpty()) o.insert("chains", QJsonArray::fromStringList(d.chains));   // racks
    if (std::any_of(d.zones.begin(), d.zones.end(), [](const ChainZones& z) { return !z.isDefault(); })) {   // racks: [lo, hi, fadeLo, fadeHi] per zone
        QJsonArray zones;
        auto zone = [](const Zone& z) { return QJsonArray{z.lo, z.hi, z.fadeLo, z.fadeHi}; };
        for (const ChainZones& z : d.zones) zones.append(QJsonObject{{"key", zone(z.key)}, {"velocity", zone(z.vel)}, {"select", zone(z.select)}});
        o.insert("zones", zones);
    }
    if (d.rack) { o.insert("rack", d.rack); o.insert("chain", d.chain); }               // a device in a rack's chain
    if (d.folded) o.insert("folded", true);                                              // shown as just its title
    if (d.sidechainTrack) o.insert("sidechain", d.sidechainTrack);                        // the track that keys it
    return o;
}

QJsonObject writeTrack(const EditState::TrackState& t, bool isReturn, int width, bool folded, const QString& baseDir) {
    QJsonArray sends, devices;
    for (double s : t.sends) sends.append(s);
    for (const Device& d : t.devices) devices.append(writeDevice(d, baseDir));
    const QJsonArray automation = writeEnvelopes(t.automation);
    QJsonObject o{{"id", t.id}, {"type", kTypes[int(t.type)]}, {"name", t.name},
                  {"color", t.color.name(t.color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb)},
                  {"volume", t.volume}, {"pan", t.pan}, {"sends", sends}, {"active", t.active},
                  {"preFader", t.preFader}, {"devices", devices}, {"automation", automation}};
    if (width > 0) o.insert("width", width);
    if (t.type == TrackType::Audio) {
        o.insert("monitor", kMonitors[int(t.monitor)]);
        o.insert("input", t.inputChannel);
    }
    if (t.groupId) o.insert("group", t.groupId);
    if (folded) o.insert("folded", true);
    if (!isReturn) {
        QJsonArray slotIds, arrangement;
        for (int id : t.slotClips) slotIds.append(id ? QJsonValue(id) : QJsonValue());
        for (const EditState::Placed& p : t.arrangement)
            arrangement.append(QJsonObject{{"start", p.start}, {"length", p.length}, {"offset", p.offset}, {"clip", p.clip}});
        o.insert("slots", slotIds);
        o.insert("arrangement", arrangement);
    }
    return o;
}

// ---- reading ---------------------------------------------------------------

Clip readClip(const QJsonObject& o, const QString& baseDir) {
    Clip c;
    c.id = integer(o, "id", 0);
    c.name = text(o, "name");
    c.length = num(o, "length", c.length);
    if (c.length <= 0) c.length = 4.0;
    c.loop = flag(o, "loop", c.loop);
    c.start = std::max(0.0, num(o, "start", c.start));           // 0.23; older files: from 1.1.1
    c.loopStart = std::max(0.0, num(o, "loopStart", c.loopStart));
    const QJsonObject launch = o.value("launch").toObject();
    c.launchMode = LaunchMode(wordIndex(kModes, launch.value("mode"), 0));
    c.legato = flag(launch, "legato", c.legato);
    c.quantization = num(launch, "quantization", c.quantization);
    const QJsonObject follow = launch.value("follow").toObject();
    c.followEnabled = flag(follow, "enabled", c.followEnabled);
    c.followA = FollowAction(wordIndex(kFollows, follow.value("actionA"), int(c.followA)));
    c.followB = FollowAction(wordIndex(kFollows, follow.value("actionB"), int(c.followB)));
    c.followChanceA = std::clamp(num(follow, "chanceA", c.followChanceA), 0.0, 1.0);
    c.followTime = std::max(0.0, num(follow, "time", c.followTime));
    const QJsonObject scale = o.value("scale").toObject();
    c.scaleOn = flag(scale, "on", c.scaleOn);
    c.scaleRoot = std::clamp(integer(scale, "root", c.scaleRoot), 0, 11);
    c.scale = integer(scale, "index", 0);
    const QString scaleName = text(scale, "name");
    for (int i = 0; i < kScaleCount; ++i) if (scaleName == QLatin1String(kScales[i].name)) c.scale = i;
    for (const QJsonValue& v : o.value("notes").toArray()) {
        const QJsonObject n = v.toObject();
        Note note;
        note.pitch = std::clamp(integer(n, "pitch", note.pitch), 0, 127);
        note.start = std::max(0.0, num(n, "start", note.start));
        note.length = num(n, "length", note.length);
        if (note.length <= 0) note.length = 0.25;
        note.velocity = std::clamp(num(n, "velocity", note.velocity), 0.0, 1.0);
        note.chance = std::clamp(num(n, "chance", note.chance), 0.0, 1.0);
        c.notes.push_back(note);
    }
    c.envelopes = readEnvelopes(o.value("envelopes").toArray());
    if (const QString col = text(o, "color"); !col.isEmpty() && QColor::isValidColorName(col)) c.color = QColor(col);
    const QJsonObject audio = o.value("audio").toObject();
    c.samplePath = readPath(audio, baseDir);
    if (c.isAudio()) {
        c.sample = samples::load(c.samplePath);   // null while the file is missing; the path stays
        c.warpMode = WarpMode(wordIndex(kWarps, audio.value("warpMode"), 0));
        c.gain = std::clamp(num(audio, "gain", 0), -70.0, 24.0);
        c.transpose = std::clamp(integer(audio, "transpose", 0), -48, 48);
        c.reverse = flag(audio, "reverse", false);
        c.warp = flag(audio, "warp", true);
        c.grain = num(audio, "grain", 0.25);   // sets from before transient slicing used a 1/16 grid
        c.grainMs = std::clamp(num(audio, "grainMs", 50), 12.0, 256.0);
        c.flux = std::clamp(num(audio, "flux", 0.25), 0.0, 1.0);
        c.formants = std::clamp(num(audio, "formants", 0), 0.0, 1.0);
        c.detectedBpm = std::clamp(num(audio, "detectedBpm", 0), 0.0, 999.0);
        if (c.grain != 0) c.grain = std::clamp(c.grain, 1.0 / 32, 4.0);
        for (const QJsonValue& v : audio.value("markers").toArray()) {
            const QJsonArray m = v.toArray();
            if (m.size() < 2 || !m[0].isDouble() || !m[1].isDouble()) continue;
            c.warpMarkers.push_back({m[0].toDouble(), m[1].toDouble()});
        }
        std::stable_sort(c.warpMarkers.begin(), c.warpMarkers.end(), [](const WarpMarker& a, const WarpMarker& b) { return a.beat < b.beat; });
        QVector<WarpMarker> kept;   // both beats and seconds must rise from marker to marker
        for (const WarpMarker& m : c.warpMarkers)
            if (kept.isEmpty() || (m.beat > kept.last().beat + 1e-9 && m.seconds > kept.last().seconds + 1e-9)) kept.push_back(m);
        c.warpMarkers = kept;
        if (c.warpMarkers.size() < 2) {
            const double secs = c.sample ? c.sample->seconds() : c.length * 0.5;
            c.warpMarkers = {{0, 0}, {c.length, secs > 0 ? secs : c.length * 0.5}};
        }
        for (const QJsonValue& v : o.value("takes").toArray()) {
            const QJsonObject to = v.toObject();
            Take t;
            t.samplePath = readPath(to, baseDir);
            if (t.samplePath.isEmpty()) continue;
            t.sample = samples::load(t.samplePath);   // null while missing; the path stays
            t.name = text(to, "name", QString("Take %1").arg(c.takes.size() + 1));
            t.startBeat = std::max(0.0, num(to, "start", 0));
            c.takes.push_back(t);
        }
        for (const QJsonValue& v : o.value("comp").toArray()) {
            const QJsonArray r = v.toArray();
            if (r.size() < 3 || !r[0].isDouble() || !r[1].isDouble() || !r[2].isDouble()) continue;
            CompRegion cr{std::clamp(r[0].toDouble(), 0.0, c.length), std::clamp(r[1].toDouble(), 0.0, c.length), r[2].toInt()};
            if (cr.take < 0 || cr.take >= c.takes.size() || cr.end - cr.start < 1e-9) continue;
            c.comp.push_back(cr);
        }
        std::stable_sort(c.comp.begin(), c.comp.end(), [](const CompRegion& a, const CompRegion& b) { return a.start < b.start; });
        for (int i = 1; i < c.comp.size(); ++i)   // regions never overlap
            if (c.comp[i].start < c.comp[i - 1].end) c.comp[i].start = std::min(c.comp[i - 1].end, c.comp[i].end);
        c.comp.removeIf([](const CompRegion& r) { return r.end - r.start < 1e-9; });
    }
    return c;
}

Device readDevice(const QJsonObject& o, const QString& baseDir) {
    Device d;
    d.id = integer(o, "id", 0);
    d.name = text(o, "name", "Device");
    d.kind = DeviceKind(wordIndex(kKinds, o.value("kind"), int(DeviceKind::AudioEffect)));
    d.enabled = flag(o, "enabled", true);
    for (const QJsonValue& v : o.value("params").toArray()) {
        const QJsonObject p = v.toObject();
        d.params.push_back({text(p, "name"), std::clamp(num(p, "value", 0.5), 0.0, 1.0)});
    }
    // A device this version knows: the kind and the knobs' names come from its spec, and knobs
    // added since the set was saved get their defaults.
    if (const DeviceSpec* s = deviceSpec(d.name)) {
        d.kind = s->kind;
        const int n = int(s->params.size());
        if (d.params.size() > n) d.params.resize(n);
        for (int i = 0; i < n; ++i) {
            if (i < d.params.size()) d.params[i].name = s->params[size_t(i)].name;
            else d.params.push_back({s->params[size_t(i)].name, s->params[size_t(i)].def});
        }
    }
    d.samplePath = readPath(o.value("sample").toObject(), baseDir);
    if (!d.samplePath.isEmpty()) d.sample = samples::load(d.samplePath);
    d.modelPath = readPath(o.value("model").toObject(), baseDir);
    if (!d.modelPath.isEmpty()) d.model = nam::load(d.modelPath);   // null while missing; the path stays
    for (const QJsonValue& v : o.value("pads").toArray()) {
        if (d.pads.size() >= drumrack::kPads) break;
        const QJsonObject po = v.toObject();
        DrumPad p;
        p.name = text(po, "name");
        p.samplePath = readPath(po.value("sample").toObject(), baseDir);
        if (!p.samplePath.isEmpty()) p.sample = samples::load(p.samplePath);   // null while missing; the path stays
        d.pads.push_back(p);
    }
    if (deviceType(d.name) == DeviceType::DrumRack) d.pads.resize(drumrack::kPads);   // always 16 pads
    for (const QJsonValue& v : o.value("maps").toArray()) {
        const QJsonObject mo = v.toObject();
        MacroMap m;
        m.macro = std::clamp(integer(mo, "macro", 0), 0, macros::kKnobs - 1);
        m.deviceId = integer(mo, "device", 0);
        m.param = std::max(0, integer(mo, "param", 0));
        m.lo = std::clamp(num(mo, "min", 0), 0.0, 1.0);
        m.hi = std::clamp(num(mo, "max", 1), 0.0, 1.0);
        if (m.deviceId > 0) d.maps.push_back(m);
    }
    for (const QJsonValue& v : o.value("mods").toArray()) {
        const QJsonObject mo = v.toObject();
        ModRoute r;
        r.source = integer(mo, "source", 0);
        r.param = integer(mo, "param", 0);
        r.amount = std::clamp(num(mo, "amount", 0), -1.0, 1.0);
        if (r.source >= 0 && r.source < monolith::kSources && r.param >= 0 && r.param < d.params.size() && d.mods.size() < monolith::kMaxRoutes) d.mods.push_back(r);
    }
    for (const QJsonValue& v : o.value("curves").toArray()) {
        if (d.curves.size() >= monolith::kCurves) break;
        QVector<CurvePoint> pts;
        for (const QJsonValue& pv : v.toArray()) {
            const QJsonArray pa = pv.toArray();
            if (pa.size() < 2 || pts.size() >= monolith::kCurvePoints) continue;
            pts.push_back({std::clamp(pa[0].toDouble(), 0.0, 1.0), std::clamp(pa[1].toDouble(), 0.0, 1.0),
                           pa.size() > 2 ? std::clamp(pa[2].toDouble(), -1.0, 1.0) : 0,
                           pa.size() > 3 ? std::clamp(pa[3].toInt(), 0, 2) : 0});
        }
        std::sort(pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
        d.curves.push_back(pts);
    }
    for (const QJsonValue& v : o.value("chains").toArray()) d.chains.push_back(v.toString());
    if (isRackType(deviceType(d.name))) for (const QJsonValue& v : o.value("zones").toArray()) {   // normalizeRacks fits them to the chains
        const QJsonObject zo = v.toObject();
        auto zone = [&zo](const char* key, Zone z) {
            const QJsonArray a = zo.value(key).toArray();
            if (a.size() == 4) z = Zone{a[0].toInt(z.lo), a[1].toInt(z.hi), a[2].toInt(0), a[3].toInt(0)};
            return z.sane();
        };
        ChainZones z;
        z.key = zone("key", z.key);
        z.vel = zone("velocity", z.vel);
        z.select = zone("select", z.select);
        d.zones.push_back(z);
    }
    d.rack = std::max(0, integer(o, "rack", 0));
    d.chain = std::max(0, integer(o, "chain", 0));
    d.folded = flag(o, "folded", false);
    d.sidechainTrack = std::max(0, integer(o, "sidechain", 0));
    return d;
}

EditState::TrackState readTrack(const QJsonObject& o, bool isReturn, const QString& baseDir) {
    EditState::TrackState t{};
    t.id = integer(o, "id", 0);
    t.type = isReturn ? TrackType::Midi : TrackType(wordIndex(kTypes, o.value("type"), 0));
    t.name = text(o, "name", isReturn ? "Return" : "MIDI");
    t.color = QColor(text(o, "color"));
    if (!t.color.isValid()) t.color = isReturn ? QColor("#9aa7b3") : Project::palette().first();
    t.volume = std::clamp(num(o, "volume", 0.75), 0.0, 1.0);
    t.pan = std::clamp(num(o, "pan", 0.0), -1.0, 1.0);
    for (const QJsonValue& v : o.value("sends").toArray()) t.sends.push_back(std::clamp(v.isDouble() ? v.toDouble() : 0.0, 0.0, 1.0));
    t.active = flag(o, "active", true);
    t.preFader = flag(o, "preFader", false);
    for (const QJsonValue& v : o.value("devices").toArray()) t.devices.push_back(readDevice(v.toObject(), baseDir));
    t.automation = readEnvelopes(o.value("automation").toArray());
    if (t.type == TrackType::Audio) {
        t.monitor = Monitor(wordIndex(kMonitors, o.value("monitor"), int(Monitor::Auto)));
        t.inputChannel = std::clamp(integer(o, "input", 0), 0, 2);
    }
    t.groupId = isReturn ? 0 : std::max(0, integer(o, "group", 0));
    if (!isReturn) {
        for (const QJsonValue& v : o.value("slots").toArray()) t.slotClips.push_back(v.isDouble() ? std::max(0, v.toInt()) : 0);
        for (const QJsonValue& v : o.value("arrangement").toArray()) {
            const QJsonObject p = v.toObject();
            EditState::Placed placed{std::max(0.0, num(p, "start", 0)), num(p, "length", 4), std::max(0.0, num(p, "offset", 0)), integer(p, "clip", 0)};
            if (placed.length > 0) t.arrangement.push_back(placed);
        }
    }
    return t;
}

} // namespace

QJsonObject deviceToJson(const Device& d, const QString& baseDir) { return writeDevice(d, baseDir); }
Device deviceFromJson(const QJsonObject& o, const QString& baseDir) { return readDevice(o, baseDir); }
QJsonObject clipToJson(const Clip& c, const QString& baseDir) { return writeClip(c, baseDir); }
Clip clipFromJson(const QJsonObject& o, const QString& baseDir) { return readClip(o, baseDir); }

QByteArray toJson(const EditState& s, const Extras& x, const QString& baseDir) {
    QJsonObject root;
    root.insert("format", kFormatName);
    root.insert("version", kFormatVersion);
    const QString version = QCoreApplication::applicationVersion();
    root.insert("createdBy", version.isEmpty() ? QStringLiteral("Sessions") : "Sessions " + version);
    root.insert("tempo", s.tempo);
    root.insert("mainVolume", s.mainVolume);
    root.insert("cueVolume", x.cueVolume);
    root.insert("transport", QJsonObject{{"loop", x.loopOn}, {"loopStart", x.loopStart}, {"loopLength", x.loopLength},
                                         {"launchQuantization", x.launchQuantization}, {"metronome", x.metronome},
                                         {"punchIn", x.punchIn}, {"punchOut", x.punchOut}});
    root.insert("scenes", QJsonArray::fromStringList(s.scenes));
    QJsonArray locators;
    for (const Locator& l : s.locators) locators.append(QJsonObject{{"beat", l.beat}, {"name", l.name}});
    root.insert("locators", locators);
    QJsonArray tracks, returns, clips;
    for (const EditState::TrackState& t : s.tracks) tracks.append(writeTrack(t, false, x.trackWidths.value(t.id), x.foldedGroups.contains(t.id), baseDir));
    for (const EditState::TrackState& r : s.returns) returns.append(writeTrack(r, true, x.trackWidths.value(r.id), false, baseDir));
    for (const Clip& c : s.clips) clips.append(writeClip(c, baseDir));
    root.insert("tracks", tracks);
    root.insert("returns", returns);
    if (!s.master.devices.isEmpty() || !s.master.automation.isEmpty()) root.insert("master", writeTrack(s.master, true, 0, false, baseDir));   // 0.21
    root.insert("clips", clips);
    if (!x.mappings.isEmpty()) {
        QJsonArray maps;
        for (const Mapping& m : x.mappings)
            maps.append(QJsonObject{{"source", kSources[int(m.source)]}, {"channel", m.channel}, {"number", m.number}, {"target", m.target}});
        root.insert("mappings", maps);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

namespace {
// Every sample file a set refers to (clips' "audio", devices' "sample"), wherever it sits.
void samplePaths(const QJsonValue& v, const QString& baseDir, QStringList& out) {
    if (v.isArray()) { for (const QJsonValue& x : v.toArray()) samplePaths(x, baseDir, out); return; }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        if ((it.key() == "audio" || it.key() == "sample") && it->isObject()) {
            const QString p = readPath(it->toObject(), baseDir);
            if (!p.isEmpty() && !out.contains(p)) out << p;
        } else if (it.key() == "takes" && it->isArray()) {   // a clip's recorded passes
            for (const QJsonValue& t : it->toArray()) {
                const QString p = t.isObject() ? readPath(t.toObject(), baseDir) : QString();
                if (!p.isEmpty() && !out.contains(p)) out << p;
            }
        } else if (it->isArray() || it->isObject()) {
            samplePaths(*it, baseDir, out);
        }
    }
}
} // namespace

bool fromJson(const QByteArray& json, EditState& stateOut, Extras& extrasOut, QString* error, const QString& baseDir) {
    auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull()) return fail(QString("It isn't a readable set file (%1 at byte %2).").arg(pe.errorString()).arg(pe.offset));
    const QJsonObject root = doc.object();
    if (!doc.isObject() || root.value("format").toString() != QLatin1String(kFormatName))
        return fail("It isn't a Sessions set.");
    const int version = integer(root, "version", 0);
    if (version < 1) return fail("The file has no valid format version.");
    if (version > kFormatVersion)
        return fail(QString("It was saved by a newer version of Sessions (set format %1; this one reads up to %2).")
                        .arg(version).arg(kFormatVersion));

    // decode the set's samples side by side first; reading the clips then finds them in memory
    QStringList paths;
    samplePaths(root, baseDir, paths);
    const QVector<SamplePtr> warm = samples::loadMany(paths);

    EditState s;
    Extras x;
    s.tempo = std::clamp(num(root, "tempo", 120.0), 20.0, 999.0);
    s.mainVolume = std::clamp(num(root, "mainVolume", 0.75), 0.0, 1.0);
    x.cueVolume = std::clamp(num(root, "cueVolume", x.cueVolume), 0.0, 1.0);
    const QJsonObject tr = root.value("transport").toObject();
    x.loopOn = flag(tr, "loop", x.loopOn);
    x.loopStart = std::max(0.0, num(tr, "loopStart", x.loopStart));
    x.loopLength = std::max(0.25, num(tr, "loopLength", x.loopLength));
    x.launchQuantization = std::max(0.0, num(tr, "launchQuantization", x.launchQuantization));
    x.metronome = flag(tr, "metronome", x.metronome);
    x.punchIn = flag(tr, "punchIn", false);
    x.punchOut = flag(tr, "punchOut", false);

    for (const QJsonValue& v : root.value("mappings").toArray()) {
        const QJsonObject m = v.toObject();
        Mapping map{Mapping::Source(wordIndex(kSources, m.value("source"), 0)), std::clamp(integer(m, "channel", 0), 0, 15),
                    integer(m, "number", 0), text(m, "target")};
        if (!map.target.isEmpty()) x.mappings.push_back(map);
    }
    for (const QJsonValue& v : root.value("scenes").toArray()) s.scenes.push_back(v.toString());
    for (const QJsonValue& v : root.value("locators").toArray()) {
        const QJsonObject l = v.toObject();
        s.locators.push_back({std::max(0.0, num(l, "beat", 0)), text(l, "name")});
    }
    std::stable_sort(s.locators.begin(), s.locators.end(), [](const Locator& a, const Locator& b) { return a.beat < b.beat; });

    QMap<int, Clip> clips;
    for (const QJsonValue& v : root.value("clips").toArray()) {
        Clip c = readClip(v.toObject(), baseDir);
        if (c.id > 0 && !clips.contains(c.id)) clips.insert(c.id, c);   // ids are how slots refer to clips
    }
    QVector<int> widths;   // per track, then per return; keyed by id once the ids are settled
    QVector<bool> folded;
    for (const QJsonValue& v : root.value("tracks").toArray()) {
        s.tracks.push_back(readTrack(v.toObject(), false, baseDir));
        widths.push_back(integer(v.toObject(), "width", 0));
        folded.push_back(flag(v.toObject(), "folded", false));
    }
    for (const QJsonValue& v : root.value("returns").toArray()) {
        s.returns.push_back(readTrack(v.toObject(), true, baseDir));
        widths.push_back(integer(v.toObject(), "width", 0));
    }
    if (root.contains("master")) s.master = readTrack(root.value("master").toObject(), true, baseDir);
    s.master.id = Project::kMasterId;
    s.master.name = "Master";
    s.master.type = TrackType::Audio;
    s.master.slotClips.clear(); s.master.arrangement.clear(); s.master.sends.clear();

    // Ids must be positive and unique; a broken one gets a fresh number.
    auto fixIds = [](auto& items, auto idOf) {
        QSet<int> seen;
        int top = 0;
        for (auto& it : items) top = std::max(top, idOf(it));
        for (auto& it : items) {
            int& id = idOf(it);
            if (id <= 0 || seen.contains(id)) id = ++top;
            seen.insert(id);
        }
    };
    QVector<EditState::TrackState*> allTracks;
    for (auto& t : s.tracks) allTracks.push_back(&t);
    for (auto& r : s.returns) allTracks.push_back(&r);
    fixIds(allTracks, [](EditState::TrackState* t) -> int& { return t->id; });
    for (int i = 0; i < allTracks.size(); ++i)
        if (widths.value(i) > 0) x.trackWidths.insert(allTracks[i]->id, std::clamp(widths[i], 40, 1000));
    // A track's group must be a group track in this set (groups don't nest).
    QSet<int> groupIds;
    for (const auto& t : s.tracks) if (t.type == TrackType::Group) groupIds.insert(t.id);
    for (auto& t : s.tracks) if (t.type == TrackType::Group || !groupIds.contains(t.groupId)) t.groupId = 0;
    for (int i = 0; i < s.tracks.size(); ++i)
        if (folded.value(i) && s.tracks[i].type == TrackType::Group) x.foldedGroups.insert(s.tracks[i].id);
    QVector<Device*> allDevices;
    for (auto* t : allTracks) for (Device& d : t->devices) allDevices.push_back(&d);
    for (Device& d : s.master.devices) allDevices.push_back(&d);
    fixIds(allDevices, [](Device* d) -> int& { return d->id; });
    for (auto* t : allTracks) normalizeRacks(t->devices);   // racks keep their chains' devices right after them
    normalizeRacks(s.master.devices);

    // At least one scene and one track, like the editor keeps it.
    if (s.scenes.isEmpty()) {
        int most = 1;
        for (const auto& t : s.tracks) most = std::max(most, int(t.slotClips.size()));
        for (int i = 0; i < most; ++i) s.scenes.push_back(QString());
    }
    if (s.tracks.isEmpty()) {
        int top = 0;
        for (const auto& r : s.returns) top = std::max(top, r.id);
        EditState::TrackState t{};
        t.id = top + 1; t.name = "1 MIDI"; t.color = Project::palette().first();
        t.volume = 0.75; t.pan = 0; t.active = true; t.preFader = false;
        s.tracks.push_back(t);
    }

    // One slot per scene and one send per return; references to missing clips are dropped.
    QSet<int> used;
    for (auto& t : s.tracks) {
        t.slotClips.resize(s.scenes.size());
        for (int& id : t.slotClips) if (!clips.contains(id)) id = 0; else used.insert(id);
        t.arrangement.erase(std::remove_if(t.arrangement.begin(), t.arrangement.end(),
                                           [&](const EditState::Placed& p) { return !clips.contains(p.clip); }),
                            t.arrangement.end());
        for (const auto& p : t.arrangement) used.insert(p.clip);
        t.sends.resize(s.returns.size());
    }
    for (auto& r : s.returns) { r.sends.clear(); r.slotClips.clear(); r.arrangement.clear(); }
    for (auto it = clips.begin(); it != clips.end();) it = used.contains(it.key()) ? std::next(it) : clips.erase(it);
    s.clips = clips;

    stateOut = s;
    extrasOut = x;
    return true;
}

void fillFromProject(Extras& x, const Project& project) {
    x.cueVolume = project.cueVolume;
    x.trackWidths.clear();
    for (int i = 0; i < project.trackCount(); ++i)
        if (project.trackAt(i)->width > 0) x.trackWidths.insert(project.trackAt(i)->id, project.trackAt(i)->width);
    x.foldedGroups.clear();
    for (const Track& t : project.tracks) if (t.isGroup() && t.folded) x.foldedGroups.insert(t.id);
    x.mappings = project.mappings;
}

void applyToProject(const Extras& x, Project& project) {
    project.cueVolume = x.cueVolume;
    project.mappings = x.mappings;
    for (int i = 0; i < project.trackCount(); ++i) project.trackAt(i)->width = x.trackWidths.value(project.trackAt(i)->id);
    for (Track& t : project.tracks) t.folded = t.isGroup() && x.foldedGroups.contains(t.id);
    emit project.mixerChanged();
    emit project.structureChanged();
}

bool save(const QString& path, const Project& project, const Extras& extras, QString* error) {
    Extras x = extras;
    fillFromProject(x, project);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) { if (error) *error = f.errorString(); return false; }
    f.write(toJson(project.captureEditState(), x, QFileInfo(path).absolutePath()));
    if (!f.commit()) { if (error) *error = f.errorString(); return false; }
    return true;
}

bool read(const QString& path, EditState& state, Extras& extras, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return false; }
    return fromJson(f.readAll(), state, extras, error, QFileInfo(path).absolutePath());
}

bool load(const QString& path, Project& project, Extras& extras, QString* error) {
    EditState s;
    Extras x;
    if (!read(path, s, x, error)) return false;
    project.cueVolume = x.cueVolume;
    project.loadEditState(s);
    applyToProject(x, project);
    extras = x;
    return true;
}

} // namespace sessions::setfile
