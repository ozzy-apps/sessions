#pragma once
#include "Model.h"
#include <deque>

namespace sessions {
// UI-thread retrospective note history. Seconds bound retention; monotonic beats preserve
// tempo changes and loop passes. Track ids survive reordering. Preview notes never enter it.
class MidiCapture {
public:
    static constexpr int kMaxNotes = 4096;
    static constexpr double kHistorySeconds = 120, kPhraseGapSeconds = 2, kMaxBeats = 128;
    struct Phrase { QVector<Note> notes; double length = 4, timelineStart = 0; bool playedWhileRunning = false; };
    void noteOn(int trackId, int pitch, double velocity, double seconds, double beat,
                double timelineBeat, bool playing);
    void noteOff(int trackId, int pitch, double seconds, double beat);
    Phrase phrase(int trackId, double seconds, double beat) const;
    void consume(int trackId);
    void clear();
    void newSegment() { ++m_segment; }
    int size() const { return int(m_notes.size()); }
private:
    struct Played { int track, pitch; double velocity, seconds, endSeconds, beat, endBeat, timeline; bool playing; quint64 segment; };
    std::deque<Played> m_notes;
    quint64 m_segment = 0;
};
} // namespace sessions
