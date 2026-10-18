#include "DeviceDrag.h"
#include "Browser.h"
#include "audio/Nam.h"
#include "audio/Builtin.h"
#include "core/Import.h"
#include "core/Sample.h"
#include "core/DeviceSpecs.h"
#include "core/Presets.h"
#include <QCoreApplication>
#include <algorithm>
#include <QDateTime>
#include <QDropEvent>
#include <QFileInfo>
#include <QHash>
#include <QMimeData>
#include <QUrl>
#include <memory>

namespace sessions::devicedrag {

namespace {

// Presets read for drag feedback, until the file changes.
struct Cached { QDateTime modified; qint64 size = 0; Project::DeviceBlocks blocks; QString name; bool ok = false; };

const Cached& cachedPreset(const QString& path) {
    static QHash<QString, Cached> cache;
    const QFileInfo fi(path);
    Cached& c = cache[path];
    if (c.modified != fi.lastModified() || c.size != fi.size() || (!c.ok && c.blocks.isEmpty())) {
        c = {fi.lastModified(), fi.size(), {}, {}, false};
        c.ok = presets::read(path, c.blocks, &c.name);
    }
    return c;
}

QString trackName(const Project& p, int i) { const Track* t = p.trackAt(i); return t ? t->name : QString(); }

} // namespace

Payload parse(const QMimeData* mime) {
    Payload pl;
    if (!mime) return pl;
    if (mime->hasFormat(kMime)) {
        const QStringList parts = QString::fromUtf8(mime->data(kMime)).split('|');
        if (parts.size() == 3 && parts[0].toLongLong() == QCoreApplication::applicationPid()) {   // not another window's
            pl.trackId = parts[1].toInt();
            for (const QString& id : parts[2].split(',', Qt::SkipEmptyParts)) if (id.toInt() > 0) pl.ids << id.toInt();
            if (!pl.ids.isEmpty()) pl.source = Payload::Devices;
        }
        return pl;
    }
    if (mime->hasFormat(Browser::mimeType())) {
        QStringList parts = QString::fromUtf8(mime->data(Browser::mimeType())).split('|');
        if (parts.size() < 2) return pl;
        const QString kind = parts.takeLast();
        const QString name = parts.join('|');
        if (kind == "u") { pl.source = Payload::Preset; pl.path = name; pl.name = presets::nameOf(name); return pl; }
        if (kind == "n") { pl.source = Payload::Model; pl.path = name; pl.name = QFileInfo(name).completeBaseName(); return pl; }
        if (kind == "r") { pl.source = Payload::Ir; pl.path = name; pl.name = audio::isBuiltin(name) ? audio::builtinName(name) : QFileInfo(name).completeBaseName(); return pl; }
        if (kind != "i" && kind != "f" && kind != "m") return pl;   // samples drop as audio files
        pl.source = Payload::Item;
        pl.name = name;
        pl.kind = kind == "i" ? DeviceKind::Instrument : kind == "f" ? DeviceKind::AudioEffect : DeviceKind::MidiEffect;
        return pl;
    }
    for (const QUrl& u : mime->urls()) {
        if (!u.isLocalFile()) continue;
        if (presets::isPreset(u.toLocalFile())) {
            pl.source = Payload::Preset;
            pl.path = QFileInfo(u.toLocalFile()).absoluteFilePath();
            pl.name = presets::nameOf(pl.path);
            return pl;
        }
        if (nam::isModelFile(u.toLocalFile())) {
            pl.source = Payload::Model;
            pl.path = QFileInfo(u.toLocalFile()).absoluteFilePath();
            pl.name = QFileInfo(pl.path).completeBaseName();
            return pl;
        }
    }
    return pl;
}

QMimeData* mimeFor(int trackId, const QList<int>& ids) {
    QStringList list;
    for (int id : ids) list << QString::number(id);
    auto* mime = new QMimeData;
    mime->setData(kMime, QString("%1|%2|%3").arg(QCoreApplication::applicationPid()).arg(trackId).arg(list.join(',')).toUtf8());
    return mime;
}

bool copying(const QDropEvent* e) { return e && (e->modifiers() & Qt::ControlModifier); }

Project::DeviceBlocks shape(const Project& p, const Payload& pl) {
    switch (pl.source) {
    case Payload::Item: {
        Device d;   // enough to check kinds: its name says if it's a rack
        d.name = pl.name;
        d.kind = pl.kind;
        if (const DeviceSpec* s = deviceSpec(pl.name)) d.kind = s->kind;
        return {{d}};
    }
    case Payload::Preset: {
        const Cached& c = cachedPreset(pl.path);
        return c.ok ? c.blocks : Project::DeviceBlocks{};
    }
    case Payload::Devices: return p.deviceBlocks(p.indexOfTrack(pl.trackId), pl.ids);
    case Payload::Model: {   // a NAM device holding the capture
        Device d;
        d.name = "NAM";
        d.kind = DeviceKind::AudioEffect;
        return {{d}};
    }
    case Payload::Ir: {   // a Cabinet holding the IR (or the track's own amp takes it: see drop)
        Device d;
        d.name = "Cabinet";
        d.kind = DeviceKind::AudioEffect;
        return {{d}};
    }
    case Payload::None: break;
    }
    return {};
}

namespace {
bool needsMidi(const Project::DeviceBlocks& blocks) {
    return std::any_of(blocks.begin(), blocks.end(), [](const QVector<Device>& b) { return !b.isEmpty() && b.first().kind != DeviceKind::AudioEffect; });
}
// An instrument or MIDI effect on an audio track: the track can become a MIDI track for it (0.21).
bool wouldConvert(const Project& p, int track, const Project::DeviceBlocks& blocks, const Project::DevicePlace& at, QString* why) {
    const Track* t = p.trackAt(track);
    if (!t || t->type != TrackType::Audio || p.isReturn(track) || at.rackId || !needsMidi(blocks)) return false;
    return p.canConvertTrack(track, TrackType::Midi, why);
}
}

bool accepts(const Project& p, const Payload& pl, int track, const Project::DevicePlace& at, QString* why) {
    const Project::DeviceBlocks blocks = shape(p, pl);
    if (blocks.isEmpty()) { if (why) *why = pl.source == Payload::Preset ? "The preset can't be read." : "Those devices aren't there any more."; return false; }
    if (pl.source == Payload::Devices && at.rackId)
        for (const QVector<Device>& b : blocks) if (b.first().id == at.rackId) { if (why) *why = "A rack can't go inside itself."; return false; }
    QString w;
    if (p.canPlace(track, blocks, at, &w)) return true;
    const Track* t = p.trackAt(track);
    if (t && t->type == TrackType::Audio && !p.isReturn(track) && !at.rackId && needsMidi(blocks)) return wouldConvert(p, track, blocks, at, why);
    if (why) *why = w;
    return false;
}

Result drop(Project& p, const Payload& pl, int track, const Project::DevicePlace& at, bool copy) {
    Result r;
    const Project::DeviceBlocks blocks = shape(p, pl);
    const QString what = pl.source == Payload::Devices && blocks.size() > 1 ? QString("%1 devices").arg(blocks.size()) : pl.name.isEmpty() && !blocks.isEmpty() ? blocks.first().first().name : pl.name;
    auto failed = [&](const QString& why) {
        r.title = "Can't put " + what + " there";
        r.text = why;
        return r;
    };
    if (!pl.valid()) return failed("That isn't a device.");
    // an IR dropped on a track that already has an amp, a NAM or a Cabinet: it goes in there
    if (pl.source == Payload::Ir && track >= 0 && !at.rackId && at.beforeId < 0) {
        if (Device* d = import::irDevice(p, track)) {
            const int id = d->id;
            const QString into = d->name;
            QString why;
            bool ok = false;
            { EditScope edit(&p, "Load IR"); ok = import::loadIr(p, track, id, pl.path, &why); }
            if (!ok) return failed(why);
            r.ok = true; r.track = track; r.ids = {id};
            r.title = pl.name + " loaded";
            r.text = "Into the " + into + " on " + trackName(p, track) + ".";
            return r;
        }
    }
    if (pl.source == Payload::Model && !nam::load(pl.path)) { QString why; nam::load(pl.path, &why); return failed(QFileInfo(pl.path).fileName() + ": " + why); }
    if (blocks.isEmpty()) return failed(pl.source == Payload::Preset ? "The preset can't be read: " + QFileInfo(pl.path).fileName() : "Those devices aren't there any more.");
    const QString verb = pl.source == Payload::Devices ? (copy ? "Copy " : "Move ") : "Add ";
    // a new track: it and the devices are one undo step
    std::unique_ptr<EditScope> scope;
    Project::DevicePlace place = at;
    if (track < 0) {
        scope = std::make_unique<EditScope>(&p, verb + what);
        track = p.insertTrackFor(blocks, pl.source == Payload::Devices ? blocks.first().first().name : pl.name);
        place = {};
    }
    QString why;
    bool ok = false;
    bool converted = false;
    if (!scope && !p.canPlace(track, blocks, place) && wouldConvert(p, track, blocks, place, &why)) {   // an audio track takes a MIDI device: it becomes a MIDI track
        scope = std::make_unique<EditScope>(&p, verb + what);
        if (!p.convertTrack(track, TrackType::Midi, &why)) return failed(why);
        converted = true;
    }
    switch (pl.source) {
    case Payload::Item: ok = p.insertDevices(track, {p.makeDeviceBlock(pl.name, pl.kind)}, place, "Add " + pl.name, &r.ids, &why); break;
    case Payload::Preset: ok = p.insertDevices(track, p.freshCopies(blocks), place, "Add " + pl.name, &r.ids, &why); break;
    case Payload::Devices: ok = p.moveDevices(p.indexOfTrack(pl.trackId), pl.ids, track, place, copy, &r.ids, &why); break;
    case Payload::Ir: {
        Device d = p.makeDevice("Cabinet", DeviceKind::AudioEffect);
        QString why2;
        d.sample = samples::load(pl.path, &why2);
        if (!d.sample) return failed(pl.name + ": " + why2);
        d.samplePath = audio::isBuiltin(pl.path) ? pl.path : d.sample->path.isEmpty() ? pl.path : d.sample->path;
        ok = p.insertDevices(track, {{d}}, place, "Add Cabinet", &r.ids, &why);
        break;
    }
    case Payload::Model: {
        Device d = p.makeDevice("NAM", DeviceKind::AudioEffect);
        d.modelPath = pl.path;
        d.model = nam::load(pl.path);   // null (the device plays dry, the panel says so) when the file can't be read
        ok = p.insertDevices(track, {{d}}, place, "Add " + pl.name, &r.ids, &why);
        break;
    }
    case Payload::None: break;
    }
    if (!ok) return failed(why);
    r.ok = true;
    r.track = track;
    r.title = pl.source == Payload::Devices ? (copy ? "Copied " : "Moved ") + what : what + " added";
    r.text = place.rackId ? "Into the rack's chain on " + trackName(p, track) + "." : "On " + trackName(p, track) + (converted ? ", now a MIDI track." : ".");
    return r;
}

} // namespace sessions::devicedrag
