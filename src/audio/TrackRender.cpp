#include "TrackRender.h"
#include "core/Model.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTemporaryFile>

namespace sessions::audio {
namespace {
bool fail(QString* error, const QString& why) { if (error) *error = why; return false; }
void snapshot(const Project& source, Project& target, bool ignoreSolo = true) {
    target.restoreEditState(source.captureEditState());
    // Exports use Arrangement playback, never a monitored input or a Session takeover.
    for (int i = 0; i < target.tracks.size(); ++i) {
        auto& t = target.tracks[i]; t.monitor = Monitor::Off; t.armed = false;
        t.solo = !ignoreSolo && source.tracks[i].solo;
    }
    for (auto& t : target.returns) t.solo = false;
}
bool publishFile(const QString& from, const QString& to, QString* error) {
    QFile input(from);
    QSaveFile output(to);
    if (!input.open(QIODevice::ReadOnly)) return fail(error, input.errorString());
    if (!output.open(QIODevice::WriteOnly)) return fail(error, output.errorString());
    while (!input.atEnd()) {
        const QByteArray data = input.read(1024 * 1024);
        if (data.isEmpty() && input.error() != QFileDevice::NoError) return fail(error, input.errorString());
        if (output.write(data) != data.size()) return fail(error, output.errorString());
    }
    if (!output.commit()) return fail(error, output.errorString());
    return true;
}
QString safeName(QString name) {
    name.replace(QRegularExpression("[^\\p{L}\\p{N} _.-]"), "_");
    name = name.trimmed().left(80);
    while (name.endsWith('.') || name.endsWith(' ')) name.chop(1);
    return name.isEmpty() ? "Track" : name;
}
} // namespace

bool renderSnapshot(const Project& project, const QString& path, const RenderOptions& options,
                    QString* error, const std::function<bool(double)>& progress) {
    Project copy;
    snapshot(project, copy, options.sourceTrackId != 0);
    QTemporaryFile staging(QFileInfo(path).absoluteDir().filePath(".sessions-render-XXXXXX.wav"));
    if (!staging.open()) return fail(error, staging.errorString());
    const QString temporary = staging.fileName();
    staging.close();
    Engine engine(&copy);
    if (!engine.renderOffline(temporary, options, progress, error)) return false;
    return publishFile(temporary, path, error);
}

bool exportStems(const Project& project, const QString& directory, const RenderOptions& options,
                 bool includeGroups, QString* error, const std::function<bool(double)>& progress) {
    const QFileInfo destination(directory);
    if (destination.exists()) return fail(error, "Choose a new folder for the stem export.");
    if (!destination.absoluteDir().exists()) return fail(error, "The destination's parent folder does not exist.");
    Project copy;
    snapshot(project, copy);
    QVector<int> ids;
    for (const Track& t : copy.tracks) if (includeGroups || !t.isGroup()) ids.push_back(t.id);
    for (const Track& t : copy.returns) ids.push_back(t.id);
    if (ids.isEmpty()) return fail(error, "There are no tracks to export.");
    QTemporaryDir staging(destination.absoluteDir().filePath(".sessions-stems-XXXXXX"));
    if (!staging.isValid()) return fail(error, "Couldn't create the stem staging folder.");
    QJsonArray files;
    for (int i = 0; i < ids.size(); ++i) {
        const Track& t = *copy.trackAt(copy.indexOfTrack(ids[i]));
        const QString name = QString("%1-%2.wav").arg(i + 1, 3, 10, QLatin1Char('0')).arg(safeName(t.name));
        RenderOptions o = options;
        o.sourceTrackId = t.id;
        o.preRoll = true;
        Engine engine(&copy); // full arrangement runs, including every sidechain and send source
        if (!engine.renderOffline(QDir(staging.path()).filePath(name), o,
            [&](double f) { return !progress || progress((i + f) / ids.size()); }, error)) return false;
        files.push_back(QJsonObject{{"file", name}, {"trackId", t.id}, {"track", t.name},
            {"kind", copy.isReturn(copy.indexOfTrack(t.id)) ? "return" : t.isGroup() ? "group" : "track"}});
    }
    const QJsonObject manifest{{"schema", 1}, {"tempo", copy.tempo}, {"sampleRate", 48000},
        {"startBeat", options.startBeat}, {"lengthBeats", options.lengthBeats},
        {"tailSeconds", options.tailSeconds}, {"normalized", options.normalize},
        {"tap", "post-mixer; before parent groups and Master; returns separate"}, {"files", files}};
    QFile manifestFile(QDir(staging.path()).filePath("stems.json"));
    const QByteArray json = QJsonDocument(manifest).toJson();
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(json) != json.size())
        return fail(error, "Couldn't write the stem manifest.");
    manifestFile.close();
    if (QFileInfo::exists(destination.absoluteFilePath()) || !QDir().rename(staging.path(), destination.absoluteFilePath()))
        return fail(error, "Couldn't publish the stem folder. The destination may already exist.");
    staging.setAutoRemove(false);
    return true;
}

int bounceTrack(Project& project, int trackId, const QString& path, const RenderOptions& options,
                bool muteSource, QString* error, const std::function<bool(double)>& progress) {
    const int index = project.indexOfTrack(trackId);
    if (index < 0 || index >= project.tracks.size()) { fail(error, "Select an Arrangement track or group to bounce."); return -1; }
    if (QFileInfo::exists(path)) { fail(error, "The bounce file already exists."); return -1; }
    const EditState before = project.captureEditState();
    const Track source = project.tracks[index];
    RenderOptions o = options;
    o.sourceTrackId = trackId;
    o.format = samples::WavFormat::Float32;
    o.normalize = false;
    o.preRoll = true;
    if (!renderSnapshot(project, path, o, error, progress)) return -1;
    auto discard = [&] { QFile::remove(path); return -1; };
    if (!(before == project.captureEditState())) {
        fail(error, "The set changed during the bounce. Try again."); return discard();
    }
    auto sample = samples::load(path, error);
    if (!sample) return discard();
    int at = index + 1;
    if (source.isGroup()) { // a new sibling follows the whole source group, not its first child
        while (at < project.tracks.size()) {
            int parent = project.tracks[at].groupId;
            QSet<int> seen;
            while (parent && parent != source.id && !seen.contains(parent)) {
                seen.insert(parent);
                const int p = project.indexOfTrack(parent);
                parent = p >= 0 ? project.tracks[p].groupId : 0;
            }
            if (parent != source.id) break;
            ++at;
        }
    }
    EditScope edit(&project, "Bounce Track to Audio");
    const int inserted = project.insertTrack(at, TrackType::Audio);
    Track& bounced = project.tracks[inserted];
    bounced.name = source.name + " (Bounce)";
    bounced.color = source.color;
    bounced.groupId = source.groupId;
    bounced.monitor = Monitor::Off;
    bounced.active = muteSource; // a parallel audition copy starts muted, avoiding double playback
    auto clip = project.newClip(bounced.name, sample->seconds() * project.tempo / 60.0);
    clip->samplePath = QFileInfo(path).absoluteFilePath();
    clip->sample = sample;
    clip->loop = false;
    clip->warp = false;
    clip->detectedBpm = project.tempo;
    clip->sourceTempoOrigin = SourceTempoOrigin::Manual;
    clip->warpMarkers = {{0, 0}, {clip->length, sample->seconds()}};
    bounced.arrangement.push_back({o.startBeat, clip->length, 0, clip});
    if (muteSource) project.tracks[project.indexOfTrack(trackId)].active = false;
    emit project.structureChanged();
    emit project.mixerChanged();
    return inserted;
}
} // namespace sessions::audio
