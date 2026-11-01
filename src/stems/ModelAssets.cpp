#include "ModelAssets.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
namespace sessions::stems {
namespace {
QJsonObject manifest() {
    QFile file(":/stems-assets.json");
    file.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(file.readAll()).object();
}
}
const QVector<ModelAsset>& modelAssets() {
    static const QVector<ModelAsset> assets = [] {
        QVector<ModelAsset> result;
        for (const auto& value : manifest()["models"].toArray()) {
            const auto o = value.toObject();
            result.push_back({o["name"].toString(), o["file"].toString(), o["sha256"].toString(),
                              qint64(o["size"].toDouble()), QUrl(o["url"].toString())});
        }
        return result;
    }();
    return assets;
}
QString assetsRoot() {
    const QString app = QCoreApplication::applicationDirPath();
    QStringList roots{app + "/../share/sessions/stems", app + "/share/sessions/stems"};
#ifdef SESSIONS_STEMS_ASSET_DIR
    roots.push_back(QString::fromUtf8(SESSIONS_STEMS_ASSET_DIR));
#endif
    for (const auto& root : roots) if (QFileInfo::exists(root + "/assets.json")) return QDir(root).absolutePath();
    return {};
}
bool modelFilesPresent(const QString& root) {
    if (root.isEmpty() || modelAssets().size() != 5) return false;
    for (const auto& asset : modelAssets()) {
        QFileInfo file(QDir(root).filePath(asset.file));
        if (!file.isFile() || file.size() != asset.size) return false;
    }
    return true;
}
bool verifyModelFiles(const QString& root, QString* error) {
    if (!modelFilesPresent(root)) { if (error) *error = "The five Spleeter model files are missing or incomplete."; return false; }
    for (const auto& asset : modelAssets()) {
        QFile file(QDir(root).filePath(asset.file));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file) || hash.result().toHex() != asset.sha256.toLatin1()) {
            if (error) *error = "Model checksum mismatch: " + asset.file;
            return false;
        }
    }
    return true;
}
QString modelDirectory() {
    const QString custom = QSettings("Sessions", "Sessions").value("stems/modelDirectory").toString();
    if (modelFilesPresent(custom)) return custom;
    const QString cached = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Stems/models";
    if (modelFilesPresent(cached)) return cached;
    return assetsRoot().isEmpty() ? QString{} : assetsRoot() + "/models";
}
QString runtimeLibrary() {
#ifdef Q_OS_WIN
    const QString platform = "windows";
#else
    const QString platform = "linux";
#endif
    const QString library = manifest()["runtime"].toObject()[platform].toObject()["library"].toString();
    return assetsRoot().isEmpty() ? QString{} : assetsRoot() + "/runtime/" + platform + "/" + library;
}
QString workerProgram() {
    QString program = QCoreApplication::applicationDirPath() + "/sessions-stems-worker";
#ifdef Q_OS_WIN
    program += ".exe";
#endif
    return program;
}
} // namespace sessions::stems
