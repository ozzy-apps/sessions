#pragma once
// Bringing audio files into a set: the browser, drops on the Session and Arrangement
// Views and the Device View all go through here. None of these opens an undo step;
// callers wrap them in an EditScope (and they emit structureChanged themselves).

#include "Model.h"
#include "Tempo.h"
#include <QStringList>

namespace sessions::import {

// A shared recording needs both its source folder and its exact exported time span.
// Named matching is deliberately restricted to recognizable stem roles; a multi-file
// import can also establish the relationship for other long, equally cut exports.
QString stemRole(const QString& path);
bool matchingStemSource(const Clip& a, const Clip& b, bool requireRoles = true);
tempo::Estimate sharedSourceTempo(const QVector<ClipPtr>& clips, ClipPtr* guide = nullptr);
bool automaticSourceMap(const Clip& clip);
void rememberAutomaticSourceMap(Clip& clip);

// One audio clip per file that loads; the others are listed in `errors` ("name: why").
// The files are decoded side by side (samples::loadMany).
QVector<ClipPtr> audioClips(Project& p, const QStringList& paths, QStringList* errors = nullptr);
// The same from samples already loaded (samples::loadAsync): one clip per non-null sample.
QVector<ClipPtr> audioClips(Project& p, const QStringList& paths, const QVector<SamplePtr>& loaded);

// Puts clips into a track's slots from `scene` down, adding scenes as needed. When `track`
// isn't an audio track, a new one is inserted after it (or after the last track) and named
// after the first clip. Returns the track's index.
int toSession(Project& p, int track, int scene, const QVector<ClipPtr>& clips);

// Places clips one after another on a track's timeline from `beat`, over whatever was
// there. Same rule for non-audio tracks. Returns the track's index.
int toArrangement(Project& p, int track, double beat, const QVector<ClipPtr>& clips);

// Loads a sample into a MIDI track's Sampler (`deviceId`, or its first Sampler); without
// one, a Sampler replaces the track's instrument. False (and why) for other tracks or a bad file.
bool toSampler(Project& p, int track, int deviceId, const QString& path, QString* error = nullptr);

// A MIDI clip of a Sampler's slices (0.32): one note per slice from C1 up, at the beat the slice
// falls on at the sample's own tempo (the set's `tempo` when it has none), each as long as its
// slice; the clip is a whole number of beats. nullptr when the device isn't a Sampler with a sample.
ClipPtr slicesToClip(Project& p, const Device& d, double tempo);

// Loads a sample onto a Drum Rack's pad (0..15), named after the file. False (and why) if the
// device isn't a Drum Rack on that track or the file doesn't load. Emits structureChanged.
bool toDrumPad(Project& p, int track, int deviceId, int pad, const QString& path, QString* error = nullptr);
// Empties a pad.
void clearDrumPad(Project& p, int track, int deviceId, int pad);

// ---- NAM captures
// Every .nam capture the browser lists, sorted by path: the User Library folder and your sample
// folders (QSettings browser/folders), subfolders included.
QStringList captureLibrary();
// Loads the capture after (dir > 0) or before (dir < 0) the one a NAM device holds, wrapping
// around: through the library when the device's capture is in it (or it holds none), else
// through the files of the capture's own folder. Captures that can't be read are skipped. False
// (and why) when there's nothing to step to, or the device isn't a NAM on that track. Emits
// structureChanged and deviceChanged; wrap it in an EditScope.
bool stepCapture(Project& p, int track, int deviceId, int dir, QString* error = nullptr);
// Where a capture sits in `library` (1-based) and how many there are, for "3 / 42"; 0 when it isn't in it.
int captureIndex(const QString& path, const QStringList& library);


// ---- impulse responses (0.19)
// The IR library: the built-in cabinets (audio/Builtin.h) first, then every audio file in the
// Impulse Responses folder (irFolder(): ~/.local/share/Sessions/Sessions/Impulse Responses,
// SESSIONS_IRS overrides it) and in your IR folders (QSettings browser/irFolders), subfolders
// included, sorted by path.
QString     irFolder();
QStringList irFolders();
void        addIrFolder(const QString& dir);
void        removeIrFolder(const QString& dir);
QStringList irLibrary();
QStringList irsIn(const QString& dir);   // the audio files below one folder, absolute, sorted
int         irIndex(const QString& path, const QStringList& library);   // 1-based; 0 when it isn't in it
// Loads an IR (a file, or a built-in "builtin:ir/…") into a device that holds one (Guitar Amp,
// NAM, Cabinet) and switches it in: the amp's Cab to Loaded IR, NAM's / Cabinet's IR on. False
// (and why) for other devices or a bad file. Emits structureChanged / deviceChanged; wrap it in an EditScope.
bool loadIr(Project& p, int track, int deviceId, const QString& path, QString* error = nullptr);
// The IR after (dir > 0) or before (dir < 0) the device's in the library, wrapping; an IR
// outside the library steps through its own folder; none loaded starts at the first (or last).
bool stepIr(Project& p, int track, int deviceId, int dir, QString* error = nullptr);
// The last device on the track that holds an IR (Guitar Amp, NAM, Cabinet), or nullptr.
Device* irDevice(Project& p, int track);

} // namespace sessions::import
