#pragma once
// Tempo: finding the tempo of an audio file, and fitting a sample to the set's tempo.
//
// detect() listens for the beat: an onset strength curve (how much each of three bands jumps
// from one 5 ms hop to the next), its autocorrelation over beat periods from 40 to 240 BPM, a
// mild preference for tempos near 110, and the file's length (a loop is a whole number of
// bars). A BPM token in the filename supplies the source tempo when available. The source
// beat grid never depends on the project's tempo: changing the project stretches the same
// beats, without silently changing the clip's metrical interpretation.

#include "Sample.h"

namespace sessions::tempo {

struct Estimate {
    double bpm = 0;          // 0: no steady beat / source BPM metadata found
    double confidence = 0;   // 0..1, how clear the beat was
};
Estimate detect(const SampleData& s, double maxSeconds = 90, const QString& sourcePath = {});

// The power of two that brings the set's tempo nearest the sample's: clip beats = sample beats
// * 2^k, and the sample plays at setTempo / 2^k instead of its own tempo. -3 .. 3.
int octaveFor(double sampleBpm, double setTempo);

// A sample as a clip: its length in source beats, whether it loops, and its source tempo.
// Unknown source tempo keeps the file at its recorded speed, played once, with an explicitly
// unverified grid at setTempo. It is never guessed to be a nearby number of project bars.
struct Sync { double beats = 4; bool loop = false; double detected = 0; };
Sync syncTo(const SampleData& s, double setTempo, double detectedBpm = 0, const QString& sourcePath = {});

} // namespace sessions::tempo
