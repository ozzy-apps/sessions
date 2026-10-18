#pragma once
// Arrangement editing operations shared by the Arrangement View, the Edit menu
// and recording. None of these opens an undo step; callers wrap them in an
// EditScope and emit structureChanged afterwards.

#include "Model.h"

namespace sessions::arrangement {

// Splits every placement on the track that spans `beat`. The right half gets
// its own copy of the clip, so the halves can be edited independently.
void split(Project& p, Track& t, double beat);

// Removes the material in [start, end) on the track, splitting at the edges.
void clearRange(Project& p, Track& t, double start, double end);

// Time operations on every track, the locators and the automation.
void insertTime(Project& p, double start, double length);
void deleteTime(Project& p, double start, double end);
void duplicateTime(Project& p, double start, double end);   // copies [start, end) to just after `end`

// Joins everything in [start, end) on a MIDI track into one new clip; returns it (or null if
// empty, or on an audio track).
ClipPtr consolidate(Project& p, Track& t, double start, double end);

// The notes that sound in [start, end) of a placement, in timeline beats.
QVector<Note> notesIn(const ArrangementClip& a, double start, double end);

} // namespace sessions::arrangement
