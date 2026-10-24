#include "Import.h"
#include "ArrangementOps.h"
#include "DeviceSpecs.h"
#include "audio/Builtin.h"
#include "audio/Nam.h"
#include "Presets.h"
#include "Tempo.h"
#include "BeatGrid.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sessions::import {

QString stemRole(const QString& path) {
    static const QRegularExpression role(
        "^(?:[0-9]+[ _.-]+)?(drums?|percussion|bass|keyboards?|keys?|synths?|other|vocals?|voices?|guitars?|pianos?|strings?|pads?|mix|master|instrumental)(?:[ _.-]+[0-9]+)?$",
        QRegularExpression::CaseInsensitiveOption);
    const auto m = role.match(QFileInfo(path).completeBaseName().trimmed());
    QString name = m.hasMatch() ? m.captured(1).toLower() : QString();
    if (name == "drum") name = "drums";
    if (name == "key" || name == "keys" || name == "keyboards") name = "keyboard";
    if (name == "synths") name = "synth";
    return name;
}

bool matchingStemSource(const Clip& a, const Clip& b, bool requireRoles) {
    if (!a.isAudio() || !b.isAudio() || !a.sample || !b.sample || a.sample->seconds() < 30 || b.sample->seconds() < 30) return false;
    const QFileInfo ai(a.samplePath), bi(b.samplePath);
    auto folder = [](const QFileInfo& f) { const QString canonical = f.dir().canonicalPath(); return canonical.isEmpty() ? f.absolutePath() : canonical; };
    if (folder(ai) != folder(bi)) return false;
    const double tolerance = 1.0 / std::max({1.0, a.sample->rate, b.sample->rate});
    if (std::abs(a.sample->seconds() - b.sample->seconds()) > tolerance + 1e-12) return false;
    if (requireRoles && (stemRole(a.samplePath).isEmpty() || stemRole(b.samplePath).isEmpty())) return false;
    return true;
}

namespace {
quint64 sourceMapFingerprint(const Clip& c) {
    quint64 hash = 1469598103934665603ULL;
    auto add = [&hash](double value) { quint64 bits = 0; std::memcpy(&bits, &value, sizeof(bits)); hash ^= bits; hash *= 1099511628211ULL; };
    for (const auto& point : c.warpMarkers) { add(point.beat); add(point.seconds); }
    add(c.length); add(c.start); add(c.loopStart); add(c.loop); add(c.warp); add(c.reverse);
    return hash;
}
}

void rememberAutomaticSourceMap(Clip& c) { c.sourceTempoMapFingerprint = sourceMapFingerprint(c); }

bool automaticSourceMap(const Clip& c) {
    if (c.sourceTempoOrigin != SourceTempoOrigin::Detected && c.sourceTempoOrigin != SourceTempoOrigin::Shared
        && c.sourceTempoOrigin != SourceTempoOrigin::Unknown) return false;
    if (!c.sample || c.reverse || !c.takes.isEmpty() || c.warpMarkers.size() < 2 || c.start != 0 || c.loopStart != 0
        || c.sourceTempoMapFingerprint == 0 || c.sourceTempoMapFingerprint != sourceMapFingerprint(c)) return false;
    const auto& first = c.warpMarkers.first(); const auto& last = c.warpMarkers.last();
    return std::abs(first.beat) < 1e-9 && std::abs(first.seconds) < 1e-9
        && std::abs(last.seconds - c.sample->seconds()) < 1e-9 && std::abs(last.beat - c.length) < 1e-9;
}

tempo::Estimate sharedSourceTempo(const QVector<ClipPtr>& clips, ClipPtr* guide) {
    if (guide) *guide = nullptr;
    ClipPtr prescribed;
    for (const auto& c : clips) {
        if (!c || c->detectedBpm <= 0 || (c->sourceTempoOrigin != SourceTempoOrigin::Manual && c->sourceTempoOrigin != SourceTempoOrigin::Metadata)) continue;
        if (prescribed && std::abs(prescribed->detectedBpm - c->detectedBpm) > 0.01) return {}; // incompatible explicit source grids
        prescribed = c;
    }
    if (prescribed) { if (guide) *guide = prescribed; return {prescribed->detectedBpm, 1.0, prescribed->sourceTempoOrigin == SourceTempoOrigin::Metadata}; }
    ClipPtr best;
    double bestScore = -1;
    for (const auto& c : clips) {
        if (!c || c->detectedBpm <= 0 || c->sourceTempoOrigin == SourceTempoOrigin::Legacy || !automaticSourceMap(*c)) continue;
        const QString role = stemRole(c->samplePath);
        const int rhythmRank = role == "drums" || role == "mix" || role == "master" || role == "instrumental" ? 4 : role == "percussion" ? 3 : 0;
        QSet<QString> supports;
        for (const auto& other : clips) if (other && other->detectedBpm > 0 && other->sourceTempoOrigin != SourceTempoOrigin::Legacy
            && std::abs(other->detectedBpm / c->detectedBpm - 1) < 0.015) supports.insert(QFileInfo(other->samplePath).absoluteFilePath());
        // A pitched stem's periodic notes alone do not establish the recording's meter.
        if (rhythmRank == 0 && supports.size() < 2 && c->sourceTempoConfidence < 0.6) continue;
        const double score = rhythmRank * 10.0 + std::min(3, int(supports.size())) + c->sourceTempoConfidence;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    if (guide) *guide = best;
    return best ? tempo::Estimate{best->detectedBpm, best->sourceTempoConfidence, false} : tempo::Estimate{};
}

QVector<ClipPtr> audioClips(Project& p, const QStringList& paths, QStringList* errors) {
    return audioClips(p, paths, samples::loadMany(paths, errors));
}

QVector<ClipPtr> audioClips(Project& p, const QStringList& paths, const QVector<SamplePtr>& loaded) {
    QVector<ClipPtr> out;
    for (int i = 0; i < paths.size() && i < loaded.size(); ++i)
        if (loaded[i]) out.push_back(p.newAudioClip(paths[i], loaded[i], false));
    const QVector<ClipPtr> existing = p.audioClips();
    QSet<int> grouped;
    bool coherentChanges = false;
    for (const ClipPtr& seed : out) {
        if (grouped.contains(seed->id)) continue;
        QVector<ClipPtr> group{seed};
        QSet<QString> distinct{QFileInfo(seed->samplePath).absoluteFilePath()};
        for (const ClipPtr& c : out) if (c != seed && matchingStemSource(*seed, *c, false)) {
            group.push_back(c); distinct.insert(QFileInfo(c->samplePath).absoluteFilePath());
        }
        for (const ClipPtr& c : existing) if (matchingStemSource(*seed, *c)) {
            group.push_back(c); distinct.insert(QFileInfo(c->samplePath).absoluteFilePath());
        }
        if (distinct.size() < 2) continue;
        ClipPtr guide;
        const auto shared = sharedSourceTempo(group, &guide);
        QVector<WarpMarker> sharedMap;
        if (guide && shared.bpm > 0) {
            // Track one rhythmic reference; independent stem grids would destroy their phase relationship.
            const bool fullOrigin = guide->warpMarkers.size() >= 2 && std::abs(guide->warpMarkers.first().beat) < 1e-9
                && std::abs(guide->warpMarkers.first().seconds) < 1e-9
                && std::abs(guide->warpMarkers.last().seconds - guide->sample->seconds()) < 1e-9;
            if ((guide->sourceTempoOrigin == SourceTempoOrigin::Manual || guide->sourceTempoOrigin == SourceTempoOrigin::Metadata) && !fullOrigin)
                sharedMap = {{0, 0}, {guide->sample->seconds() * shared.bpm / 60, guide->sample->seconds()}};
            else if (guide->sourceTempoOrigin == SourceTempoOrigin::Manual || guide->sourceTempoOrigin == SourceTempoOrigin::Metadata
                || (automaticSourceMap(*guide) && guide->warpMarkers.size() > 2)) sharedMap = guide->warpMarkers;
            else for (const auto& point : tempo::trackGrid(*guide->sample, shared.bpm).points) sharedMap.push_back({point.beat, point.seconds});
        }
        EditScope edit(&p, "Import Stem Tempo");
        for (const ClipPtr& c : group) {
            grouped.insert(c->id);
            if (!automaticSourceMap(*c)) continue; // manual, legacy and edited maps retain their source grid
            coherentChanges = true;
            if (shared.bpm > 0) {
                if (sharedMap.size() >= 2) p.setClipWarpMap(c, sharedMap);
                c->detectedBpm = shared.bpm; c->warp = true;
                c->sourceTempoOrigin = SourceTempoOrigin::Shared;
                c->sourceTempoConfidence = shared.confidence;
            } else {
                // Uncertain groups play together at recorded speed instead of at conflicting guesses.
                const double beats = c->sample->seconds() * std::max(1.0, p.tempo) / 60.0;
                p.setClipWarpMap(c, {{0, 0}, {beats, c->sample->seconds()}});
                c->loop = false; c->warp = false; c->detectedBpm = 0;
                c->sourceTempoOrigin = SourceTempoOrigin::Unknown; c->sourceTempoConfidence = 0;
            }
            rememberAutomaticSourceMap(*c);
        }
    }
    for (const auto& c : out) if (!grouped.contains(c->id) && c->sample && c->sample->seconds() >= 30
        && c->detectedBpm > 0 && c->sourceTempoOrigin == SourceTempoOrigin::Detected) {
        const auto grid = tempo::trackGrid(*c->sample, c->detectedBpm);
        if (grid.variable && grid.points.size() >= 2) {
            c->warpMarkers.clear();
            for (const auto& point : grid.points) c->warpMarkers.push_back({point.beat, point.seconds});
            c->length = c->warpMarkers.last().beat;
            rememberAutomaticSourceMap(*c);
        }
    }
    if (coherentChanges) { emit p.clipChanged(); emit p.structureChanged(); }
    return out;
}

namespace {

// The audio track to put clips on: `track` itself, or a new one after it.
int audioTrackFor(Project& p, int track, const QVector<ClipPtr>& clips) {
    if (track >= 0 && track < p.tracks.size() && p.tracks[track].type == TrackType::Audio) return track;
    // an empty MIDI track (no clips, no instrument) becomes an audio track for the clips (0.21)
    if (track >= 0 && track < p.tracks.size() && p.tracks[track].type == TrackType::Midi && p.canConvertTrack(track, TrackType::Audio) && p.convertTrack(track, TrackType::Audio)) return track;
    const int at = track >= 0 && track < p.tracks.size() ? track + 1 : int(p.tracks.size());
    const int t = p.insertTrack(at, TrackType::Audio);
    if (!clips.isEmpty()) p.tracks[t].name = clips.first()->name;
    return t;
}

} // namespace

int toSession(Project& p, int track, int scene, const QVector<ClipPtr>& clips) {
    if (clips.isEmpty()) return -1;
    const int t = audioTrackFor(p, track, clips);
    scene = std::clamp(scene, 0, int(p.scenes.size()));
    for (int i = 0; i < clips.size(); ++i) {
        while (scene + i >= p.scenes.size()) p.insertScene(int(p.scenes.size()));
        p.tracks[t].clipSlots[scene + i] = clips[i];
    }
    emit p.structureChanged();
    return t;
}

int toArrangement(Project& p, int track, double beat, const QVector<ClipPtr>& clips) {
    if (clips.isEmpty()) return -1;
    const int t = audioTrackFor(p, track, clips);
    double at = std::max(0.0, beat);
    for (const ClipPtr& c : clips) {
        arrangement::clearRange(p, p.tracks[t], at, at + c->length);
        p.tracks[t].arrangement.push_back({at, c->length, 0, c});
        at += c->length;
    }
    auto& arr = p.tracks[t].arrangement;
    std::sort(arr.begin(), arr.end(), [](const ArrangementClip& a, const ArrangementClip& b) { return a.start < b.start; });
    emit p.structureChanged();
    return t;
}

bool toSampler(Project& p, int track, int deviceId, const QString& path, QString* error) {
    Track* t = p.trackAt(track);
    if (!t || p.isReturn(track) || t->type != TrackType::Midi) {
        if (error) *error = "Samples go into a Sampler on a MIDI track, or onto an audio track as clips.";
        return false;
    }
    QString why;
    SamplePtr s = samples::load(path, &why);
    if (!s) { if (error) *error = QFileInfo(path).fileName() + ": " + why; return false; }
    auto find = [&]() -> Device* {
        for (Device& d : t->devices) if ((deviceId < 0 || d.id == deviceId) && d.name == "Sampler") return &d;
        return nullptr;
    };
    Device* d = find();
    if (!d && deviceId < 0) {
        const int keep = p.selectedTrack;
        p.selectedTrack = track;
        p.addDeviceToSelected("Sampler", DeviceKind::Instrument);   // replaces the instrument
        p.selectedTrack = keep;
        t = p.trackAt(track);
        d = find();
    }
    if (!d) { if (error) *error = "That device isn't a Sampler."; return false; }
    d->samplePath = s->path;
    d->sample = s;
    d->sampleBpm = tempo::detect(*s, 90, path).bpm;   // source tempo (0: unknown; the panel's field sets one)
    d->slices.clear();                      // manual slices belonged to the old sample
    emit p.structureChanged();
    emit p.deviceChanged();
    return true;
}

ClipPtr slicesToClip(Project& p, const Device& d, double tempo) {
    using namespace sampler;
    const DeviceType type = deviceType(d.name);
    if (type != DeviceType::Sampler || !d.sample || d.params.size() < kParams) return nullptr;
    auto pos = [&](int i) { return d.params[i].value; };
    double at[kMaxSlices];
    const int n = slices(d.sample.get(), choiceIndex(type, SliceBy, pos(SliceBy)), pos(Sensitivity), paramValue(type, Division, pos(Division)),
                         int(paramValue(type, Regions, pos(Regions))), d.sampleBpm, tempo, d.slices.constData(), int(d.slices.size()),
                         pos(Start), pos(End), at, kMaxSlices);
    if (n <= 0) return nullptr;
    const double dur = d.sample->seconds();
    const double end = std::clamp(pos(End), 0.0, 1.0) * dur;
    const double perBeat = 60.0 / (d.sampleBpm > 0 ? d.sampleBpm : std::max(1.0, tempo));   // the slices' beats at the sample's own tempo
    const double total = std::max(0.25, std::ceil((end - at[0]) / perBeat - 1e-6));
    ClipPtr c = p.newClip(QFileInfo(d.samplePath).completeBaseName() + " Slices", total);
    for (int k = 0; k < n; ++k) {
        const double b0 = (at[k] - at[0]) / perBeat, b1 = ((k + 1 < n ? at[k + 1] : end) - at[0]) / perBeat;
        c->notes.push_back(Note{kFirstSliceNote + k, b0, std::max(1.0 / 32, b1 - b0), 0.8, 1.0});
    }
    return c;
}

namespace {
Device* drumRack(Project& p, int track, int deviceId) {
    Track* t = p.trackAt(track);
    if (!t) return nullptr;
    for (Device& d : t->devices)
        if (d.id == deviceId && deviceType(d.name) == DeviceType::DrumRack) {
            if (d.pads.size() < drumrack::kPads) d.pads.resize(drumrack::kPads);
            return &d;
        }
    return nullptr;
}
} // namespace

bool toDrumPad(Project& p, int track, int deviceId, int pad, const QString& path, QString* error) {
    if (pad < 0 || pad >= drumrack::kPads || !drumRack(p, track, deviceId)) { if (error) *error = "That isn't a Drum Rack pad."; return false; }
    QString why;
    SamplePtr s = samples::load(path, &why);
    if (!s) { if (error) *error = QFileInfo(path).fileName() + ": " + why; return false; }
    Device* d = drumRack(p, track, deviceId);   // after loading: look it up once, then write
    d->pads[pad] = {audio::isBuiltin(path) ? audio::builtinName(path) : QFileInfo(path).completeBaseName(), s->path, s};
    emit p.structureChanged();
    emit p.deviceChanged();
    return true;
}

void clearDrumPad(Project& p, int track, int deviceId, int pad) {
    Device* d = drumRack(p, track, deviceId);
    if (!d || pad < 0 || pad >= d->pads.size()) return;
    d->pads[pad] = {};
    emit p.structureChanged();
    emit p.deviceChanged();
}

// ---- NAM captures ---------------------------------------------------------------

namespace {
QStringList capturesIn(const QString& dir, bool recursive) {
    QStringList out;
    if (dir.isEmpty() || !QDir(dir).exists()) return out;
    QDirIterator it(dir, nam::nameFilters(), QDir::Files | QDir::Readable, recursive ? QDirIterator::Subdirectories | QDirIterator::FollowSymlinks : QDirIterator::NoIteratorFlags);
    int n = 0;
    while (it.hasNext() && n++ < 5000) out << QFileInfo(it.next()).absoluteFilePath();
    return out;
}
}

QStringList captureLibrary() {
    QStringList out = capturesIn(presets::folder(), true);
    for (const QString& dir : QSettings("Sessions", "Sessions").value("browser/folders").toStringList()) out += capturesIn(dir, true);
    out.removeDuplicates();
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return out;
}

int captureIndex(const QString& path, const QStringList& library) {
    if (path.isEmpty()) return 0;
    const int i = library.indexOf(QFileInfo(path).absoluteFilePath());
    return i < 0 ? 0 : i + 1;
}

bool stepCapture(Project& p, int track, int deviceId, int dir, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    Track* t = p.trackAt(track);
    if (!t) return fail("There's no track there.");
    Device* d = nullptr;
    for (Device& o : t->devices) if (o.id == deviceId) d = &o;
    if (!d || deviceType(d->name) != DeviceType::Nam) return fail("That isn't a NAM device.");
    const QString current = d->modelPath.isEmpty() ? QString() : QFileInfo(d->modelPath).absoluteFilePath();
    QStringList files = captureLibrary();
    if (!current.isEmpty() && !files.contains(current)) {   // not in the library: its own folder's captures
        files = capturesIn(QFileInfo(current).absolutePath(), false);
        std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    }
    if (files.isEmpty()) return fail("No captures to step through: put .nam files in the User Library folder or in a sample folder of the browser.");
    const int n = int(files.size());
    int at = files.indexOf(current);   // -1: start from the first (or the last, stepping back)
    const int step = dir < 0 ? -1 : 1;
    if (at < 0) at = step > 0 ? -1 : 0;
    QString why;
    for (int tries = 0; tries < n; ++tries) {
        at = ((at + step) % n + n) % n;
        const QString path = files.at(at);
        if (path == current) continue;
        const nam::ModelPtr m = nam::load(path, &why);
        if (!m) continue;   // unreadable: the next one
        d->modelPath = path;
        d->model = m;
        emit p.structureChanged();
        emit p.deviceChanged();
        return true;
    }
    return fail(n == 1 && !current.isEmpty() ? "That's the only capture there is." : "None of the other captures could be read: " + why);
}


// ---- impulse responses ---------------------------------------------------------------

QString irFolder() {
    const QByteArray env = qgetenv("SESSIONS_IRS");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Impulse Responses";
}

QStringList irFolders() {
    QStringList out;
    for (const QString& dir : QSettings("Sessions", "Sessions").value("browser/irFolders").toStringList())
        if (!dir.isEmpty()) out << dir;
    return out;
}

void addIrFolder(const QString& dir) {
    const QString abs = QDir(dir).absolutePath();
    QStringList dirs = irFolders();
    if (!dirs.contains(abs)) dirs << abs;
    QSettings("Sessions", "Sessions").setValue("browser/irFolders", dirs);
}

void removeIrFolder(const QString& dir) {
    QStringList dirs = irFolders();
    dirs.removeAll(dir);
    dirs.removeAll(QDir(dir).absolutePath());
    QSettings("Sessions", "Sessions").setValue("browser/irFolders", dirs);
}

QStringList irsIn(const QString& dir) {
    QStringList out;
    if (dir.isEmpty() || !QDir(dir).exists()) return out;
    QDirIterator it(dir, samples::nameFilters(), QDir::Files | QDir::Readable, QDirIterator::Subdirectories | QDirIterator::FollowSymlinks);
    int n = 0;
    while (it.hasNext() && n++ < 5000) out << QFileInfo(it.next()).absoluteFilePath();
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return out;
}

QStringList irLibrary() {
    QStringList files = irsIn(irFolder());
    for (const QString& dir : irFolders()) files += irsIn(dir);
    files.removeDuplicates();
    std::sort(files.begin(), files.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return audio::builtinIrPaths() + files;
}

int irIndex(const QString& path, const QStringList& library) {
    if (path.isEmpty()) return 0;
    const int i = library.indexOf(audio::isBuiltin(path) ? path : QFileInfo(path).absoluteFilePath());
    return i < 0 ? 0 : i + 1;
}

Device* irDevice(Project& p, int track) {
    Track* t = p.trackAt(track);
    if (!t) return nullptr;
    Device* found = nullptr;
    for (Device& d : t->devices) if (holdsIr(deviceType(d.name))) found = &d;
    return found;
}

namespace {
Device* irDeviceById(Project& p, int track, int deviceId) {
    Track* t = p.trackAt(track);
    if (!t) return nullptr;
    for (Device& d : t->devices) if (d.id == deviceId && holdsIr(deviceType(d.name))) return &d;
    return nullptr;
}
void switchIrIn(Device& d) {
    const DeviceType type = deviceType(d.name);
    if (type == DeviceType::GuitarAmp && d.params.size() > guitaramp::Cab) d.params[guitaramp::Cab].value = choicePosition(guitaramp::kLoadedIr, guitaramp::kCabs);
    if (type == DeviceType::Nam && d.params.size() > namdev::Ir) d.params[namdev::Ir].value = 1;
    if (type == DeviceType::Cabinet && d.params.size() > cabinet::Ir) d.params[cabinet::Ir].value = 1;
    if (type == DeviceType::HybridReverb && d.params.size() > hybrid::Routing) d.params[hybrid::Routing].value = choicePosition(2, 4);
}
}

bool loadIr(Project& p, int track, int deviceId, const QString& path, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    if (!irDeviceById(p, track, deviceId)) return fail("That device doesn't take an impulse response (the Guitar Amp, NAM and Cabinet do).");
    QString why;
    const SamplePtr s = samples::load(path, &why);
    if (!s) return fail((audio::isBuiltin(path) ? audio::builtinName(path) : QFileInfo(path).fileName()) + ": " + why);
    Device* d = irDeviceById(p, track, deviceId);   // after loading: look it up once, then write
    d->samplePath = audio::isBuiltin(path) ? path : (s->path.isEmpty() ? path : s->path);
    d->sample = s;
    switchIrIn(*d);
    emit p.structureChanged();
    emit p.deviceChanged();
    return true;
}

bool stepIr(Project& p, int track, int deviceId, int dir, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    Device* d = irDeviceById(p, track, deviceId);
    if (!d) return fail("That device doesn't take an impulse response (the Guitar Amp, NAM and Cabinet do).");
    const QString current = d->samplePath.isEmpty() ? QString() : audio::isBuiltin(d->samplePath) ? d->samplePath : QFileInfo(d->samplePath).absoluteFilePath();
    QStringList files = irLibrary();
    if (!current.isEmpty() && !files.contains(current)) files = irsIn(QFileInfo(current).absolutePath());   // not in the library: its own folder
    if (files.isEmpty()) return fail("No impulse responses to step through.");
    const int n = int(files.size());
    int at = files.indexOf(current);
    const int step = dir < 0 ? -1 : 1;
    if (at < 0) at = step > 0 ? -1 : 0;
    QString why;
    for (int tries = 0; tries < n; ++tries) {
        at = ((at + step) % n + n) % n;
        const QString path = files.at(at);
        if (path == current) continue;
        const SamplePtr s = samples::load(path, &why);
        if (!s) continue;
        d = irDeviceById(p, track, deviceId);
        d->samplePath = audio::isBuiltin(path) ? path : s->path.isEmpty() ? path : s->path;
        d->sample = s;
        switchIrIn(*d);
        emit p.structureChanged();
        emit p.deviceChanged();
        return true;
    }
    return fail(n == 1 && !current.isEmpty() ? "That's the only impulse response there is." : "None of the other files could be read: " + why);
}

} // namespace sessions::import
