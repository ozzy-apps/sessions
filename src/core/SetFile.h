#pragma once
// Sessions — .sessions set files.
//
// A set is one UTF-8 JSON document: everything in EditState (tracks and
// returns with their ids, mixer settings, devices, automation, slots and
// arrangement; clips; scenes; tempo; main volume; locators) plus the settings
// that live outside the undo snapshot (cue volume, the loop region, global
// launch quantization, metronome). Ids are written as they are, so a loaded
// set refers to its tracks, devices and clips exactly as before. The layout
// is described in handoff.md ("Set file format").

#include "Model.h"
#include <QSet>
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace sessions {

namespace setfile {

constexpr int kFormatVersion = 2;   // 2: audio tracks, audio clips, Sampler samples
inline constexpr char kSuffix[]   = "sessions";                     // file extension, without the dot
inline constexpr char kMimeType[] = "application/x-sessions-set";   // assets/sessions-mime.xml

// What a set holds besides EditState.
struct Extras {
    double cueVolume = 0.6;
    bool   loopOn = false;
    double loopStart = 0.0, loopLength = 16.0;
    double launchQuantization = 4.0;   // beats; 0 = none
    bool   metronome = false;
    bool   punchIn = false, punchOut = false;
    QMap<int, int> trackWidths;        // Session View column widths by track id (Track::width; 0 = default, left out)
    QSet<int>      foldedGroups;       // group tracks shown folded (Track::folded), by id
    QVector<Mapping> mappings;         // MIDI Map / Key Map (Project::mappings)
    bool operator==(const Extras& o) const {
        return cueVolume == o.cueVolume && loopOn == o.loopOn && loopStart == o.loopStart && loopLength == o.loopLength
            && launchQuantization == o.launchQuantization && metronome == o.metronome && punchIn == o.punchIn && punchOut == o.punchOut
            && trackWidths == o.trackWidths
            && foldedGroups == o.foldedGroups && mappings == o.mappings;
    }
    bool operator!=(const Extras& o) const { return !(*this == o); }
};

// The parts of Extras that live in the Project rather than the Transport.
void fillFromProject(Extras& extras, const Project& project);   // cue volume, column widths, mappings
void applyToProject(const Extras& extras, Project& project);    // after Project::loadEditState

// Sample paths are written absolute and, when `baseDir` (the set's folder) is given,
// relative to it as well; reading prefers the relative one when that file exists, so a
// set moved together with its samples still finds them.
QByteArray toJson(const EditState& state, const Extras& extras, const QString& baseDir = {});
// Parses and sanitizes a set (slot counts match the scenes, sends match the
// returns, ids are unique and positive, points and locators sorted) and loads
// its samples (a missing file leaves the clip's path with no audio). On failure
// returns false and leaves `state` / `extras` untouched.
bool fromJson(const QByteArray& json, EditState& state, Extras& extras, QString* error = nullptr, const QString& baseDir = {});

// One device as a set holds it (presets are lists of these). Reading loads its samples
// (a missing file leaves the path with no audio) and gives knobs added since their defaults.
QJsonObject deviceToJson(const Device& d, const QString& baseDir = {});
Device      deviceFromJson(const QJsonObject& o, const QString& baseDir = {});
// One clip as a set holds it (the clip library saves these, 0.22). Reading loads its sample
// (a missing file leaves the path with no audio); the id comes back as written (give it a new one).
QJsonObject clipToJson(const Clip& c, const QString& baseDir = {});
Clip        clipFromJson(const QJsonObject& o, const QString& baseDir = {});

// File helpers. save() writes atomically (QSaveFile) and takes the cue volume and
// column widths from the project. read() only parses; load() = read() +
// Project::loadEditState + applyToProject, so the project is replaced only when
// the whole file was read successfully.
bool save(const QString& path, const Project& project, const Extras& extras, QString* error = nullptr);
bool read(const QString& path, EditState& state, Extras& extras, QString* error = nullptr);
bool load(const QString& path, Project& project, Extras& extras, QString* error = nullptr);

} // namespace setfile
} // namespace sessions
