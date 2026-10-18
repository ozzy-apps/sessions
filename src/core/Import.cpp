#include "Import.h"
#include "ArrangementOps.h"
#include "DeviceSpecs.h"
#include "audio/Builtin.h"
#include "audio/Nam.h"
#include "Presets.h"
#include "Tempo.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <algorithm>

namespace sessions::import {

QVector<ClipPtr> audioClips(Project& p, const QStringList& paths, QStringList* errors) {
    return audioClips(p, paths, samples::loadMany(paths, errors));
}

QVector<ClipPtr> audioClips(Project& p, const QStringList& paths, const QVector<SamplePtr>& loaded) {
    QVector<ClipPtr> out;
    for (int i = 0; i < paths.size() && i < loaded.size(); ++i)
        if (loaded[i]) out.push_back(p.newAudioClip(paths[i], loaded[i]));
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
