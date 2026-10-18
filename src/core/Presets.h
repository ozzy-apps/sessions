#pragma once
// Your own presets: one device, a rack with its chains, or several devices in a row, saved as
// a .sessionspreset file in presets::folder(). JSON: {"format": "sessions-preset", "version": 1,
// "name": …, "devices": [ …each as in a set (setfile::deviceToJson)… ]}. The browser lists them
// under User Library; they drop and load like any device. Samples (a Sampler's, the amp's IR,
// Drum Rack pads) are kept by their absolute paths.

#include "Model.h"
#include <QObject>

namespace sessions::presets {

inline constexpr char kSuffix[] = "sessionspreset";   // file extension, without the dot

QString     folder();                        // ~/.local/share/Sessions/Sessions/Presets (SESSIONS_PRESETS overrides it)
QStringList list();                          // the presets in it, absolute paths, by name
QString     pathFor(const QString& name);    // where a preset called `name` goes (characters a file name can't hold become _)
bool        isPreset(const QString& path);   // by its extension
// Saves `blocks` (in chain order) as `name`, replacing the file atomically.
bool save(const QString& path, const QString& name, const Project::DeviceBlocks& blocks, QString* error = nullptr);
// Reads one: its name and blocks, with the ids they were saved with (give them new ones with
// Project::freshCopies); macro mappings to devices it doesn't hold are left out.
bool read(const QString& path, Project::DeviceBlocks& blocks, QString* name = nullptr, QString* error = nullptr);
QString nameOf(const QString& path);         // its name; the file's when it can't be read
bool rename(const QString& path, const QString& name, QString* newPath = nullptr, QString* error = nullptr);
bool remove(const QString& path);            // to the desktop's trash (deleted when there's none)

// ---- the clip library (0.22): clips saved from the Session / Arrangement / Clip View as
// .sessionsclip files in clipFolder() (folder()/Clips). JSON: {"format": "sessions-clip",
// "version": 1, "name": …, "clip": …as in a set (setfile::clipToJson)…}. An audio clip's files
// (its sample and its takes) are copied into clipFolder()/Samples unless they're already in the
// library, and written relative to the clip file too, so the library can move. The browser
// lists them under User Library; they drop onto slots and lanes, and double-click to load.
inline constexpr char kClipSuffix[] = "sessionsclip";
QString     clipFolder();
QStringList listClips();                        // absolute paths, by name
QString     clipPathFor(const QString& name);
bool        isClipFile(const QString& path);    // by its extension
bool        saveClip(const QString& path, const QString& name, const Clip& clip, QString* error = nullptr);
bool        readClip(const QString& path, Clip& clip, QString* name = nullptr, QString* error = nullptr);   // the clip with the id it was saved with
QString     clipNameOf(const QString& path);
bool        renameClip(const QString& path, const QString& name, QString* newPath = nullptr, QString* error = nullptr);
bool        removeClip(const QString& path);    // to the trash

// Tells the browser when presets come and go.
class Library : public QObject {
    Q_OBJECT
public:
    static Library* instance();
signals:
    void changed();
};

} // namespace sessions::presets
