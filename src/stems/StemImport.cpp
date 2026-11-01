#include "StemImport.h"
#include "audio/Engine.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
namespace sessions::stems {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}
bool roomForStems(const Project& project, QString* error) {
    int groups = 0;
    for (const auto& track : project.tracks) groups += track.isGroup();
    if (project.tracks.size() + 6 > audio::kMaxTracks || groups >= audio::kMaxGroups)
        return fail(error, "There is no room for five tracks and a group. Sessions supports 64 tracks and 16 groups.");
    return true;
}
Result readResult(const QString& directory, qint64 expected, QString* error) {
    auto invalid = [&](const QString& message) { fail(error, message); return Result{}; };
    QFile file(QDir(directory).filePath("result.json"));
    if (file.size() > 65536 || !file.open(QIODevice::ReadOnly)) return invalid("The separation result is missing or too large.");
    const auto json = QJsonDocument::fromJson(file.readAll()).object();
    const double count = json["frames"].toDouble();
    if (json["schema"].toInt() != 1 || json["model"].toString() != "spleeter-5stems-onnx-v1"
        || json["rate"].toInt() != 44100 || !std::isfinite(count) || count < 1 || count > 44100. * 3600
        || count != std::floor(count) || qint64(count) != expected) return invalid("The separated audio has an invalid format or duration.");
    Result result; result.frames = qint64(count);
    const auto stems = json["stems"].toArray();
    const QSet<QString> expectedNames{"vocals", "drums", "bass", "piano", "other"};
    QSet<QString> names;
    for (const auto& value : stems) {
        const auto item = value.toObject(); const QString name = item["name"].toString(), relative = item["file"].toString();
        if (!expectedNames.contains(name) || names.contains(name) || relative != name + ".wav") return invalid("Invalid or duplicate stem name.");
        names.insert(name);
        const QFileInfo info(QDir(directory).filePath(relative));
        if (!info.isFile() || info.isSymLink() || info.canonicalPath() != QDir(directory).canonicalPath()
            || info.size() != 44 + result.frames * 8) return invalid("A separated file is missing, truncated or outside the output folder.");
        QFile wav(info.absoluteFilePath());
        if (!wav.open(QIODevice::ReadOnly)) return invalid("Couldn't read a separated file.");
        const QByteArray h = wav.read(44);
        if (h.size() != 44 || h.left(4) != "RIFF" || h.mid(8, 8) != "WAVEfmt " || h.mid(36, 4) != "data"
            || qFromLittleEndian<quint16>(h.constData() + 20) != 3 || qFromLittleEndian<quint16>(h.constData() + 22) != 2
            || qFromLittleEndian<quint32>(h.constData() + 24) != 44100 || qFromLittleEndian<quint16>(h.constData() + 34) != 32
            || qFromLittleEndian<quint32>(h.constData() + 40) != result.frames * 8) return invalid("A separated file has an invalid WAV header.");
        result.files.push_back({name, info.absoluteFilePath()});
    }
    if (names != expectedNames) return invalid("The separation did not produce all five stems.");
    return result;
}
int importTracks(Project& project, const Result& result, const QVector<SamplePtr>& decoded,
                 const QString& name, double start, QString* error) {
    if (!result.valid() || decoded.size() != 5 || !std::isfinite(start) || start < 0 || !roomForStems(project, error)) {
        if (error && error->isEmpty()) *error = "Invalid stem import.";
        return -1;
    }
    QSet<QString> names;
    for (const auto& file : result.files) names.insert(file.name);
    if (names != QSet<QString>{"vocals", "drums", "bass", "piano", "other"} || result.rate != 44100) {
        fail(error, "Invalid stem names or sample rate."); return -1;
    }
    for (const auto& sample : decoded) if (!sample || sample->rate != 44100 || sample->frames() != result.frames || sample->channels != 2) {
        fail(error, "A separated file couldn't be decoded with the expected duration."); return -1;
    }
    EditScope edit(&project, "Create Stems");
    const int group = project.insertTrack(project.tracks.size(), TrackType::Group);
    project.tracks[group].name = name.isEmpty() ? "Stems" : name;
    project.tracks[group].color = QColor("#557fb7");
    const int id = project.tracks[group].id;
    const QStringList order{"vocals", "drums", "bass", "piano", "other"};
    const QColor colors[] = {QColor("#8f77ba"), QColor("#b3855a"), QColor("#5578b9"), QColor("#738bac"), QColor("#969ba6")};
    for (int k = 0; k < order.size(); ++k) {
        int which = 0;
        while (which < result.files.size() && result.files[which].name != order[k]) ++which;
        const int track = project.insertTrack(project.tracks.size(), TrackType::Audio);
        auto& target = project.tracks[track];
        QString label = order[k]; label[0] = label[0].toUpper();
        target.name = label; target.groupId = id; target.color = colors[k]; target.monitor = Monitor::Off;
        auto clip = project.newClip(label, double(result.frames) / result.rate * project.tempo / 60.);
        clip->samplePath = result.files[which].path; clip->sample = decoded[which];
        clip->loop = false; clip->warp = false; clip->detectedBpm = project.tempo;
        clip->sourceTempoOrigin = SourceTempoOrigin::Manual;
        clip->warpMarkers = {{0, 0}, {clip->length, double(result.frames) / result.rate}};
        target.arrangement.push_back({start, clip->length, 0, clip});
    }
    project.selectedTrack = group;
    project.selectedClip.reset();
    emit project.structureChanged(); emit project.selectionChanged();
    return group;
}
} // namespace sessions::stems
