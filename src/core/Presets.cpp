#include "Presets.h"
#include "SetFile.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

namespace sessions::presets {

namespace {
constexpr char kFormat[] = "sessions-preset";
constexpr int kVersion = 1;
constexpr qint64 kMaxBytes = 8 << 20;   // a preset is a few KB; refuse anything absurd
}

QString folder() {
    const QByteArray env = qgetenv("SESSIONS_PRESETS");
    if (!env.isEmpty()) return QDir(QString::fromLocal8Bit(env)).absolutePath();
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Presets";
}

QStringList list() {
    QDir dir(folder());
    QStringList out;
    for (const QFileInfo& fi : dir.entryInfoList({QString("*.") + kSuffix}, QDir::Files | QDir::Readable)) out << fi.absoluteFilePath();
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) {
        return QFileInfo(a).completeBaseName().compare(QFileInfo(b).completeBaseName(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString pathFor(const QString& name) {
    QString file = name.trimmed();
    for (QChar& c : file) if (c < QChar(32) || QString("/\\:*?\"<>|").contains(c)) c = '_';
    if (file.isEmpty() || file == "." || file == "..") file = "Preset";
    return QDir(folder()).filePath(file + "." + kSuffix);
}

bool isPreset(const QString& path) { return QFileInfo(path).suffix().compare(kSuffix, Qt::CaseInsensitive) == 0; }

bool save(const QString& path, const QString& name, const Project::DeviceBlocks& blocks, QString* error) {
    QJsonArray devices;
    for (const QVector<Device>& b : blocks) for (const Device& d : b) devices.append(setfile::deviceToJson(d));
    const QJsonObject root{{"format", kFormat}, {"version", kVersion}, {"name", name.trimmed()}, {"devices", devices}};
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    emit Library::instance()->changed();
    return true;
}

bool read(const QString& path, Project::DeviceBlocks& blocks, QString* name, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());
    if (f.size() > kMaxBytes) return fail("The file is too big to be a preset.");
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) return fail("Not a preset: " + pe.errorString());
    const QJsonObject root = doc.object();
    if (root.value("format").toString() != kFormat) return fail("Not a Sessions preset.");
    QVector<Device> devs;
    QSet<int> ids;
    bool idsOk = true;
    for (const QJsonValue& v : root.value("devices").toArray()) {
        if (!v.isObject()) continue;
        Device d = setfile::deviceFromJson(v.toObject());
        idsOk &= d.id > 0 && !ids.contains(d.id);
        ids.insert(d.id);
        devs.push_back(d);
    }
    if (devs.isEmpty()) return fail("The preset holds no devices.");
    if (!idsOk) {   // ids a set never has: start them over, and nothing is in a rack any more
        int next = 1;
        for (Device& d : devs) { d.id = next++; d.rack = 0; d.chain = 0; d.maps.clear(); }
        ids.clear();
        for (const Device& d : devs) ids.insert(d.id);
    }
    for (Device& d : devs) {   // a mapping to a device the preset doesn't hold would turn some other device
        d.maps.erase(std::remove_if(d.maps.begin(), d.maps.end(), [&](const MacroMap& m) { return !ids.contains(m.deviceId); }), d.maps.end());
        d.sidechainTrack = 0;   // its key source was a track of another set: pick one again
    }
    normalizeRacks(devs);
    blocks.clear();
    for (int i = 0; i < devs.size(); i = deviceBlockEnd(devs, i)) blocks.push_back(devs.mid(i, deviceBlockEnd(devs, i) - i));
    if (name) {
        *name = root.value("name").toString().trimmed();
        if (name->isEmpty()) *name = QFileInfo(path).completeBaseName();
    }
    return true;
}

QString nameOf(const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.size() <= kMaxBytes) {
        const QString n = QJsonDocument::fromJson(f.readAll()).object().value("name").toString().trimmed();
        if (!n.isEmpty()) return n;
    }
    return QFileInfo(path).completeBaseName();
}

bool rename(const QString& path, const QString& name, QString* newPath, QString* error) {
    Project::DeviceBlocks blocks;
    if (!read(path, blocks, nullptr, error)) return false;
    const QString to = pathFor(name);
    if (QFileInfo(to) != QFileInfo(path) && QFileInfo::exists(to)) { if (error) *error = "There's a preset called that already."; return false; }
    if (!save(to, name, blocks, error)) return false;
    if (QFileInfo(to) != QFileInfo(path)) QFile::remove(path);
    if (newPath) *newPath = to;
    emit Library::instance()->changed();
    return true;
}

bool remove(const QString& path) {
    const bool ok = QFile::moveToTrash(path) || QFile::remove(path);
    if (ok) emit Library::instance()->changed();
    return ok;
}

// ---- the clip library ---------------------------------------------------------------------

namespace {
constexpr char kClipFormat[] = "sessions-clip";
QString safeName(const QString& name, const QString& fallback) {
    QString file = name.trimmed();
    for (QChar& c : file) if (c < QChar(32) || QString("/\\:*?\"<>|").contains(c)) c = '_';
    if (file.isEmpty() || file == "." || file == "..") file = fallback;
    return file;
}
// A sample the clip refers to: copied into the library's Samples folder (once) unless it's
// already somewhere under the library; the path it should be saved with.
QString libraryCopy(const QString& abs, QString* error) {
    if (abs.isEmpty() || !QFileInfo::exists(abs)) return abs;
    const QString lib = QDir(folder()).absolutePath();
    if (QFileInfo(abs).absoluteFilePath().startsWith(lib + "/")) return abs;
    const QString dir = QDir(clipFolder()).filePath("Samples");
    QDir().mkpath(dir);
    const QFileInfo fi(abs);
    QString to = QDir(dir).filePath(fi.fileName());
    for (int n = 2; QFileInfo::exists(to); ++n) {
        if (QFileInfo(to).size() == fi.size()) return to;   // the same file again (the same clip saved twice)
        to = QDir(dir).filePath(QString("%1 %2.%3").arg(fi.completeBaseName()).arg(n).arg(fi.suffix()));
    }
    if (!QFile::copy(abs, to)) { if (error) *error = "Couldn't copy " + fi.fileName() + " into the library."; return abs; }
    return to;
}
} // namespace

QString clipFolder() { return QDir(folder()).filePath("Clips"); }

QStringList listClips() {
    QDir dir(clipFolder());
    QStringList out;
    for (const QFileInfo& fi : dir.entryInfoList({QString("*.") + kClipSuffix}, QDir::Files | QDir::Readable)) out << fi.absoluteFilePath();
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) {
        return QFileInfo(a).completeBaseName().compare(QFileInfo(b).completeBaseName(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

QString clipPathFor(const QString& name) { return QDir(clipFolder()).filePath(safeName(name, "Clip") + "." + kClipSuffix); }
bool isClipFile(const QString& path) { return QFileInfo(path).suffix().compare(kClipSuffix, Qt::CaseInsensitive) == 0; }

bool saveClip(const QString& path, const QString& name, const Clip& clip, QString* error) {
    Clip c = clip;
    QString copyError;
    if (c.isAudio()) {
        c.samplePath = libraryCopy(c.samplePath, &copyError);
        for (Take& t : c.takes) t.samplePath = libraryCopy(t.samplePath, &copyError);
    }
    c.name = name.trimmed().isEmpty() ? c.name : name.trimmed();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QJsonObject root{{"format", kClipFormat}, {"version", 1}, {"name", c.name}, {"clip", setfile::clipToJson(c, QFileInfo(path).absolutePath())}};
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson()) < 0 || !f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    if (!copyError.isEmpty() && error) *error = copyError;   // saved, but with the sample where it was
    emit Library::instance()->changed();
    return true;
}

bool readClip(const QString& path, Clip& clip, QString* name, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return fail(f.errorString());
    if (f.size() > kMaxBytes) return fail("The file is too big to be a clip.");
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) return fail("Not a clip: " + pe.errorString());
    const QJsonObject root = doc.object();
    if (root.value("format").toString() != kClipFormat) return fail("Not a Sessions clip.");
    clip = setfile::clipFromJson(root.value("clip").toObject(), QFileInfo(path).absolutePath());
    if (name) {
        *name = root.value("name").toString().trimmed();
        if (name->isEmpty()) *name = QFileInfo(path).completeBaseName();
    }
    if (clip.name.isEmpty()) clip.name = QFileInfo(path).completeBaseName();
    return true;
}

QString clipNameOf(const QString& path) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.size() <= kMaxBytes) {
        const QString n = QJsonDocument::fromJson(f.readAll()).object().value("name").toString().trimmed();
        if (!n.isEmpty()) return n;
    }
    return QFileInfo(path).completeBaseName();
}

bool renameClip(const QString& path, const QString& name, QString* newPath, QString* error) {
    Clip c;
    if (!readClip(path, c, nullptr, error)) return false;
    const QString to = clipPathFor(name);
    if (QFileInfo(to) != QFileInfo(path) && QFileInfo::exists(to)) { if (error) *error = "There's a clip called that already."; return false; }
    if (!saveClip(to, name, c, error)) return false;
    if (QFileInfo(to) != QFileInfo(path)) QFile::remove(path);
    if (newPath) *newPath = to;
    emit Library::instance()->changed();
    return true;
}

bool removeClip(const QString& path) {
    const bool ok = QFile::moveToTrash(path) || QFile::remove(path);
    if (ok) emit Library::instance()->changed();
    return ok;
}

Library* Library::instance() {
    static Library* library = new Library;   // never deleted: it outlives every window
    return library;
}

} // namespace sessions::presets
