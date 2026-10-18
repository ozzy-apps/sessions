#pragma once
// Tempo: finding the tempo of an audio file, and fitting a sample to the set's tempo.
//
// detect() listens for the beat: an onset strength curve (how much each of three bands jumps
// from one 5 ms hop to the next), its autocorrelation over beat periods from 40 to 240 BPM, a
// mild preference for tempos near 110, and the file's length (a loop is a whole number of
// bars). syncTo() then does what auto-warp in the classic DAWs does, with the user's rule on top: the set's
// tempo is the master, and the sample is warped to the nearest tempo that's in sync with it,
// halving or doubling until it's close (an 87 BPM sample at a 172 BPM set plays at 86: two set
// beats per sample beat).

#include "Sample.h"

namespace sessions::tempo {

struct Estimate {
    double bpm = 0;          // 0: no steady beat found (a hit, a word, silence, under a second of audio)
    double confidence = 0;   // 0..1, how clear the beat was
};
Estimate detect(const SampleData& s, double maxSeconds = 90);

// The power of two that brings the set's tempo nearest the sample's: clip beats = sample beats
// * 2^k, and the sample plays at setTempo / 2^k instead of its own tempo. -3 .. 3.
int octaveFor(double sampleBpm, double setTempo);

// A sample as a clip at the set's tempo: its length in set beats, whether it loops (a whole
// number of beats, within a few percent), and the tempo that was found (0: none; then the
// length is guessed from the file's duration as before: a power of two bars within 20 %, else
// the file at its own speed, played once).
struct Sync { double beats = 4; bool loop = false; double detected = 0; };
Sync syncTo(const SampleData& s, double setTempo, double detectedBpm = 0);

} // namespace sessions::tempo
