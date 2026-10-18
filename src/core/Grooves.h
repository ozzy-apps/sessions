#pragma once
// Grooves: the Drum Studio's library of drum patterns, by style (Rock, Funk, Hip Hop, House,
// Drum & Bass, Metal, Jazz, Reggae, Latin, …), each a bar or two on the General MIDI drum
// notes, so they play on the Drum Studio, the drum kits and a Drum Rack alike.
//
// A groove is written as lanes of sixteenth steps: "K|x...x...x...x..." is a kick on every
// beat. Lanes: K kick (36), S snare (38), H hi-hat (42), R ride (51), C crash (49), 1 / 2 / 3
// the high / mid / floor tom (48 / 45 / 41), P clap (39). Steps: '.' nothing, 'x' a hit,
// 'X' an accent, 'o' a ghost note, 'f' a flam (a grace note just before); on the H lane 'O' is
// the open hat and 'p' the pedal; on the S lane 'r' the side stick and 'R' a rimshot; on the R
// lane 'b' the bell; on the C lane 'c' the china and 's' a splash. Swing delays the off-steps
// (sixteenths, or eighths for a shuffle) towards the triplet.
//
// notes() turns a groove into clip notes, humanized a little if asked (velocity and timing),
// the same every time for the same seed. Grooves drag from the panel as kMime; dropped on a
// Session slot or an Arrangement lane they become a MIDI clip (a MIDI track with a Drum
// Studio is made when the drop isn't on one).

#include "Model.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

class QMimeData;

namespace sessions::grooves {

struct Groove {
    const char* style;
    const char* name;
    int         bars;
    double      swing;    // 0.5 straight .. 0.67 triplet
    bool        swing8;   // the swing moves the off-eighths (a shuffle) instead of the off-sixteenths
    const char* lanes;    // "K|…\nS|…\nH|…", 16 steps per bar in each lane
    double      tempo;    // a tempo it's written for (shown; the set's tempo is the master)
};

const std::vector<Groove>& all();
QStringList styles();                                   // in library order
QVector<int> inStyle(const QString& style);             // indices into all()
int  find(const QString& style, const QString& name);   // -1 when there's none

// The groove as clip notes (length bars × 4). humanVel / humanTime 0 .. 1 add a little life
// (±15 % of the velocity, ±1/64 of a beat at most); the same seed gives the same notes.
QVector<Note> notes(const Groove& g, double humanVel = 0, double humanTime = 0, unsigned seed = 1);
QString       describe(const Groove& g);   // "2 bars · swing · 128 BPM" for the panel

// Drag and drop: the groove's index as text.
inline constexpr char kMime[] = "application/x-sessions-groove";
QMimeData* mimeFor(int index, double humanVel = 0, double humanTime = 0);
int        indexIn(const QMimeData* mime, double* humanVel = nullptr, double* humanTime = nullptr);   // -1 for anything else

// A clip of the groove, named "<style> <name>". The track: a MIDI track takes it; anything else
// gets a new MIDI track after it (or after the last track) with a Drum Studio on it. Both emit
// structureChanged and expect the caller's EditScope; they return the track's index.
ClipPtr makeClip(Project& p, const Groove& g, double humanVel = 0, double humanTime = 0);
int toSession(Project& p, int track, int scene, const ClipPtr& clip);
int toArrangement(Project& p, int track, double beat, const ClipPtr& clip);

} // namespace sessions::grooves
