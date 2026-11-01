#pragma once
#include <QString>
#include <QVector>
#include <QUrl>
namespace sessions::stems {
struct ModelAsset { QString name, file, sha256; qint64 size = 0; QUrl url; };
const QVector<ModelAsset>& modelAssets();
QString assetsRoot();
QString modelDirectory();
QString runtimeLibrary();
bool modelFilesPresent(const QString& directory);
bool verifyModelFiles(const QString& directory, QString* error = nullptr);
QString workerProgram();
} // namespace sessions::stems
