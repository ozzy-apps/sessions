#pragma once
#include "core/Model.h"
namespace sessions::stems {
struct StemFile { QString name, path; };
struct Result { QVector<StemFile> files; qint64 frames = 0; double rate = 44100; bool valid() const { return files.size() == 5 && frames > 0; } };
Result readResult(const QString& directory, qint64 expectedFrames, QString* error = nullptr);
// Call only with decoded files validated against Result; returns the new group index.
// The original tracks/devices remain intact. The entire group is one undo operation.
int importTracks(Project& project, const Result& result, const QVector<SamplePtr>& decoded,
                 const QString& groupName, double startBeat, QString* error = nullptr);
bool roomForStems(const Project& project, QString* error = nullptr);
} // namespace sessions::stems
