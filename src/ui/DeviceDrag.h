#pragma once
// Dragging devices. Four things drag as devices: browser items (instruments, effects, Sounds,
// racks: Browser::mimeType() "Name|kind"), your presets (User Library items "path|u", or
// .sessionspreset files from the file manager) and device panels in the Device View, by their
// title bar (kMime: "pid|trackId|id,id,…"), and .nam captures (browser items "path|n", or .nam files from
// the file manager), which land as a NAM device holding the capture, and impulse responses from the
// browser ("path|r"), which go into the track's amp / NAM / Cabinet when it has one (dropped on a
// track) or land as a new Cabinet holding them. They drop on the Device View (where the orange line
// shows), into a rack's chain, on a track (Session columns and mixer strips, Arrangement lanes
// and headers) or on the empty space next to / below the tracks (a new track named after
// them). Device panels move; with Ctrl held they're copied. An instrument or a MIDI effect
// dropped on an audio track that holds no clips turns it into a MIDI track first (0.21).

#include "core/Model.h"

class QMimeData;
class QDropEvent;

namespace sessions::devicedrag {

inline constexpr char kMime[] = "application/x-sessions-device";

struct Payload {
    enum Source { None, Item, Preset, Devices, Model, Ir } source = None;
    QString    name;                           // Item: the browser item; Preset: its name; Devices: the first one's; Model: the capture's; Ir: the IR's
    DeviceKind kind = DeviceKind::AudioEffect; // Item
    QString    path;                           // Preset: the file; Model: the .nam file (it lands as a NAM device holding it); Ir: the IR (a file or builtin:ir/…)
    int        trackId = 0;                    // Devices: the track they're on
    QList<int> ids;                            // Devices
    bool valid() const { return source != None; }
};

Payload    parse(const QMimeData* mime);                       // None for anything else (samples, sets)
QMimeData* mimeFor(int trackId, const QList<int>& ids);        // what a device panel's drag carries
bool       copying(const QDropEvent* e);                       // Ctrl held: copy device panels instead of moving them

// What it would put on a track, for checking where it may go (ids are meaningless).
Project::DeviceBlocks shape(const Project& p, const Payload& pl);
// Would it go on track `track` at `at`? `why` says why not.
bool accepts(const Project& p, const Payload& pl, int track, const Project::DevicePlace& at = {}, QString* why = nullptr);

struct Result {
    bool       ok = false;
    int        track = -1;   // where it went
    QList<int> ids;          // the devices that landed (racks: the rack)
    QString    title, text;  // for the Info View
};
// A track that can't take it, and can't become a MIDI track for it (an audio track with clips, a
// group, a return): an instrument or a MIDI effect goes on a new MIDI track right after it instead
// (0.26: as flexible as a sample, which makes a track of its kind wherever it lands).
bool newTrackAfter(const Project& p, const Payload& pl, int track);
// Drops it (one undo step) on track `track` at `at`; track -1: a new track, after track `after`
// (in its group) or after the last one.
Result drop(Project& p, const Payload& pl, int track, const Project::DevicePlace& at, bool copy, int after = -1);

} // namespace sessions::devicedrag
