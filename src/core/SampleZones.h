#pragma once
#include "Model.h"
namespace sessions::samplezones {
bool readSfz(const QString& path, QVector<SampleZone>& zones, QString* error = nullptr);
QVector<SampleZone> mapAudioFiles(const QStringList& paths);
} // namespace sessions::samplezones
