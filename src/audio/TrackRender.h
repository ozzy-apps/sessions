#pragma once
#include "Engine.h"

namespace sessions::audio {
// Value snapshot: editing, Session launches, monitoring and the live engine cannot change an export.
bool renderSnapshot(const Project& project, const QString& path, const RenderOptions& options,
                    QString* error = nullptr, const std::function<bool(double)>& progress = {});
// Writes into a staging directory and publishes it only when every aligned stem and manifest succeeds.
// The destination must not exist. Solos are ignored; activators, faders and automation are retained.
bool exportStems(const Project& project, const QString& directory, const RenderOptions& options,
                 bool includeGroups, QString* error = nullptr,
                 const std::function<bool(double)>& progress = {});
// Prints a post-mixer track/group into a new track beside it. The original remains editable.
// Returns the inserted index, or -1 without changing the project. One undo restores both tracks.
int bounceTrack(Project& project, int trackId, const QString& path, const RenderOptions& options,
                bool muteSource, QString* error = nullptr,
                const std::function<bool(double)>& progress = {});
} // namespace sessions::audio
