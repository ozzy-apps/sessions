#pragma once
// Editing an audio clip's audio (0.22): Delete in the sample editor cuts a stretch out, Crop
// keeps only a stretch. Samples are immutable and shared, so the result is a new WAV (32-bit
// float, the sample's own rate) in `dir`, and the clip is pointed at it: its warp markers, its
// length and its envelopes follow, so what's left plays on the beats it played on. The old
// file stays where it was (undo brings the clip back to it). A clip with takes can't be
// edited this way (its audio is spread over the takes). None of these opens an undo step.

#include "Model.h"

namespace sessions::sampleedit {

// Removes clip beats [b0, b1): the audio after them moves earlier. False (and why) on failure.
bool cut(Clip& c, double b0, double b1, const QString& dir, QString* error = nullptr);
// Keeps only clip beats [b0, b1): they become the clip, from 1.1.1.
bool crop(Clip& c, double b0, double b1, const QString& dir, QString* error = nullptr);
// Where an edited copy of `original` goes in `dir`: "<name> (edit).wav", numbered when taken.
QString editedPath(const QString& dir, const QString& original);

} // namespace sessions::sampleedit
