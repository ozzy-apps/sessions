#include "Recovery.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace sessions {
struct RecoveryLease {
    QString dir;
    std::unique_ptr<QLockFile> lock;
    QByteArray digest; // worker only
};
namespace {
constexpr qint64 maxSnapshotBytes = 256 * 1024 * 1024;
QString token() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool sessionName(const QString& name) {
    static const QRegularExpression pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
    return pattern.match(name).hasMatch();
}
bool atomicWrite(const QString& path, const QByteArray& bytes, QString& error) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) { error = "Couldn't create " + QFileInfo(path).absolutePath(); return false; }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString(); return false;
    }
    return true;
}
bool envelope(const QString& path, RecoveryEntry& entry, QJsonObject& payload, QString& error) {
    entry.file = path;
    entry.time = QFileInfo(path).lastModified().toUTC();
    entry.name = "Recovery";
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    if (file.size() > maxSnapshotBytes) { error = "Recovery snapshot exceeds 256 MB"; return false; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) { error = "Invalid recovery snapshot"; return false; }
    payload = document.object();
    const auto meta = payload.take("_sessionsRecovery").toObject();
    if (meta["version"].toInt() != 1) { error = "Unsupported recovery snapshot"; return false; }
    entry.name = meta["name"].toString("Untitled");
    entry.originalPath = meta["originalPath"].toString();
    entry.time = QDateTime::fromString(meta["time"].toString(), Qt::ISODateWithMs);
    const auto digest = QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    if (meta["sha256"].toString().toLatin1() != digest) { error = "Recovery snapshot checksum failed"; return false; }
    return true;
}
void rotate(const QString& dir, int keep) {
    const auto files = QDir(dir).entryInfoList({"*.sessions"}, QDir::Files, QDir::Name | QDir::Reversed);
    for (qsizetype i = keep; i < files.size(); ++i) QFile::remove(files[i].absoluteFilePath());
}
bool retire(const std::shared_ptr<RecoveryLease>& lease) {
    QDir dir(lease->dir);
    for (const auto& file : dir.entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
        if (file.fileName() != "owner.lock" && !QFile::remove(file.absoluteFilePath())) return false;
    // Keep ownership until all snapshots are gone. Unlock removes our own lock file;
    // deleting it underneath QLockFile would produce warnings and obscure failures.
    lease->lock->unlock();
    return QDir().rmdir(lease->dir) || !QFileInfo::exists(lease->dir);
}
}

QString RecoveryManager::defaultRoot() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("Recovery");
}
int RecoveryManager::intervalSeconds() {
    bool ok = false; const int seconds = QSettings("Sessions", "Sessions").value("recovery/intervalSeconds", 120).toInt(&ok);
    return ok && (seconds == 0 || (seconds >= 30 && seconds <= 3600)) ? seconds : 120;
}
int RecoveryManager::backupCount() {
    bool ok = false; const int count = QSettings("Sessions", "Sessions").value("recovery/backupCount", 10).toInt(&ok);
    return ok && count >= 0 && count <= 50 ? count : 10;
}
QString RecoveryManager::backupFolder(const QString& path) {
    return QDir(QFileInfo(path).absolutePath()).filePath("Backups/" + QFileInfo(path).completeBaseName());
}
RecoveryManager::RecoveryManager(const QString& root, QObject* parent)
    : QObject(parent), m_root(QDir::cleanPath(QFileInfo(root.isEmpty() ? defaultRoot() : root).absoluteFilePath())) {
    m_worker = new QObject;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName("Recovery writer");
    m_thread.start();
}
RecoveryManager::~RecoveryManager() {
    // Flush coalesced last versions on ordinary application teardown. No Project is
    // accessed here; clean close has already cancelled/retired its recovery session.
    auto pending = std::move(m_pending); auto backups = std::move(m_backups);
    QMetaObject::invokeMethod(m_worker, [pending = std::move(pending), backups = std::move(backups)] {
        QString error;
        if (pending) write(*pending, false, error);
        for (const auto& backup : backups) write(backup, true, error);
    }, Qt::BlockingQueuedConnection);
    m_thread.quit(); m_thread.wait();
}
std::shared_ptr<RecoveryLease> RecoveryManager::leaseFor(const QString& dir, QString* error) const {
    if (!QDir().mkpath(dir)) { if (error) *error = "Couldn't create recovery folder"; return {}; }
    auto lease = std::make_shared<RecoveryLease>(); lease->dir = dir;
    lease->lock = std::make_unique<QLockFile>(QDir(dir).filePath("owner.lock"));
    lease->lock->setStaleLockTime(0);
    if (!lease->lock->tryLock(0)) { if (error) *error = "Recovery is in use or its folder isn't writable"; return {}; }
    return lease;
}
bool RecoveryManager::ownFile(const QString& file) const {
    const QFileInfo info(file); const auto parent = info.absoluteDir();
    return !info.isSymLink() && !QFileInfo(parent.absolutePath()).isSymLink()
        && sessionName(parent.dirName()) && parent.absolutePath() == QDir(m_root).filePath(parent.dirName())
        && parent.canonicalPath() == QDir(QDir(m_root).canonicalPath()).filePath(parent.dirName())
        && info.canonicalFilePath() == QDir(parent.canonicalPath()).filePath(info.fileName())
        && info.suffix() == "sessions";
}
QVector<RecoveryEntry> RecoveryManager::candidates() const {
    QVector<RecoveryEntry> entries;
    for (const auto& name : QDir(m_root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!sessionName(name)) continue;
        const auto dir = QDir(m_root).filePath(name);
        if (QFileInfo(dir).isSymLink()) continue;
        auto lease = leaseFor(dir, nullptr); if (!lease) continue; // live instances are never offered
        for (const auto& file : QDir(dir).entryInfoList({"*.sessions"}, QDir::Files)) {
            if (file.isSymLink()) continue;
            RecoveryEntry entry; QJsonObject payload;
            envelope(file.absoluteFilePath(), entry, payload, entry.error);
            entries.push_back(std::move(entry));
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.time > b.time; });
    return entries;
}
bool RecoveryManager::restore(const QString& file, EditState& state, setfile::Extras& extras, RecoveryEntry& entry, QString* error) {
    QString why;
    if (!ownFile(file) || !QFile::exists(file)) { if (error) *error = "Recovery snapshot isn't available"; return false; }
    auto lease = leaseFor(QFileInfo(file).absolutePath(), &why);
    if (!lease) { if (error) *error = why; return false; }
    QJsonObject payload;
    if (!envelope(file, entry, payload, why)
        || !setfile::fromJson(QJsonDocument(payload).toJson(QJsonDocument::Compact), state, extras, &why,
                             entry.originalPath.isEmpty() ? QString() : QFileInfo(entry.originalPath).absolutePath())) {
        if (error) *error = why; return false;
    }
    clear(); m_session = std::move(lease); // old snapshots survive until save/discard or a replacement checkpoint
    return true;
}
bool RecoveryManager::discard(const QString& file, QString* error) {
    if (!ownFile(file)) { if (error) *error = "Invalid recovery path"; return false; }
    auto lease = leaseFor(QFileInfo(file).absolutePath(), error); if (!lease) return false;
    if (!retire(lease)) { if (error) *error = "Couldn't delete recovery snapshots"; return false; }
    return true;
}
void RecoveryManager::clear() {
    ++m_epoch; m_pending.reset();
    if (auto lease = std::move(m_session)) QMetaObject::invokeMethod(m_worker, [this, lease] {
        if (!retire(lease)) QMetaObject::invokeMethod(this, [this] { emit writeFailed("Recovery cleanup", "Couldn't remove an old recovery snapshot"); }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
void RecoveryManager::checkpoint(EditState state, setfile::Extras extras, const QString& name, const QString& path) {
    if (!m_session) {
        QString error; m_session = leaseFor(QDir(m_root).filePath(token()), &error);
        if (!m_session) { emit writeFailed("Autosave", error); return; }
    }
    m_pending = Snapshot{std::move(state), std::move(extras), name, path, m_session, 3, m_epoch};
    next();
}
void RecoveryManager::backup(EditState state, setfile::Extras extras, const QString& path) {
    const int keep = backupCount(); if (path.isEmpty() || !keep) return;
    if (!m_backups.contains(path) && m_backups.size() >= 8) { emit writeFailed("Backup", "Eight projects are waiting for backup; retry Save when writing finishes"); return; }
    // Rapid repeated saves of the same document retain its newest waiting version.
    m_backups.insert(path, Snapshot{std::move(state), std::move(extras), {}, path, {}, keep, m_epoch});
    next();
}
quint64 RecoveryManager::saveManual(EditState state, setfile::Extras extras, const QString& path) {
    const auto request = ++m_nextManual; ++m_manualWrites;
    QMetaObject::invokeMethod(m_worker, [this, request, state = std::move(state), extras = std::move(extras), path] {
        QString error; const bool ok = setfile::saveState(path, state, extras, &error);
        QMetaObject::invokeMethod(this, [this, request, ok, error] {
            --m_manualWrites; emit manualSaveFinished(request, ok, error);
            if (!busy()) emit idle();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
    return request;
}
QString RecoveryManager::write(const Snapshot& snapshot, bool backup, QString& error) {
    const auto dir = backup ? backupFolder(snapshot.originalPath) : snapshot.lease->dir;
    if (!QDir().mkpath(dir)) { error = "Couldn't create " + dir; return {}; }
    std::unique_ptr<QLockFile> backupLock;
    if (backup) {
        backupLock = std::make_unique<QLockFile>(QDir(dir).filePath("backup.lock")); backupLock->setStaleLockTime(0);
        if (!backupLock->tryLock(0)) { error = "Another window is writing this project's backups"; return {}; }
    }
    const auto bytes = setfile::toJson(snapshot.state, snapshot.extras, backup ? dir : QString());
    QByteArray output = bytes;
    QByteArray digest;
    if (!backup) {
        auto payload = QJsonDocument::fromJson(bytes).object();
        digest = QCryptographicHash::hash(QJsonDocument(payload).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
        if (digest == snapshot.lease->digest) return {};
        payload.insert("_sessionsRecovery", QJsonObject{{"version", 1}, {"name", snapshot.name}, {"originalPath", snapshot.originalPath},
            {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}, {"sha256", QString::fromLatin1(digest)}});
        output = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        if (output.size() > maxSnapshotBytes) { error = "Recovery snapshot exceeds 256 MB"; return {}; }
    }
    // A sequence gives deterministic retention even for writes in the same millisecond
    // or when the system clock moves backwards. The UUID avoids name collisions.
    quint64 sequence = 1;
    for (const auto& name : QDir(dir).entryList({"snapshot-*.sessions"}, QDir::Files))
        sequence = std::max(sequence, name.section('-', 1, 1).toULongLong() + 1);
    const auto file = QDir(dir).filePath(QString("snapshot-%1-%2.sessions").arg(sequence, 20, 10, QChar('0')).arg(token()));
    if (!atomicWrite(file, output, error)) return {};
    if (!backup) snapshot.lease->digest = digest;
    rotate(dir, snapshot.keep); // delete older copies only after the new version committed
    return file;
}
void RecoveryManager::next() {
    if (m_busy) return;
    const bool backup = !m_pending.has_value();
    if (backup && m_backups.isEmpty()) { emit idle(); return; }
    Snapshot snapshot;
    if (backup) { auto it = m_backups.begin(); snapshot = std::move(it.value()); m_backups.erase(it); }
    else { snapshot = std::move(*m_pending); m_pending.reset(); }
    m_busy = true;
    QMetaObject::invokeMethod(m_worker, [this, snapshot = std::move(snapshot), backup] {
        QString error; const QString file = write(snapshot, backup, error);
        QMetaObject::invokeMethod(this, [this, file, error, backup, epoch = snapshot.epoch] {
            m_busy = false;
            if (!error.isEmpty()) emit writeFailed(backup ? "Backup" : "Autosave", error);
            else if (!file.isEmpty()) { if (backup) emit backupWritten(file); else if (epoch == m_epoch) emit snapshotWritten(file); }
            next();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
}
