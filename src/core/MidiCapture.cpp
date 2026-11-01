#include "MidiCapture.h"

namespace sessions {
void MidiCapture::noteOn(int track, int pitch, double velocity, double seconds, double beat,
                         double timeline, bool playing) {
    if (pitch < 0 || pitch > 127 || !std::isfinite(velocity) || !std::isfinite(seconds) || !std::isfinite(beat)) return;
    if (velocity <= 0) { noteOff(track, pitch, seconds, beat); return; }
    noteOff(track, pitch, seconds, beat);
    while (!m_notes.empty() && (int(m_notes.size()) >= kMaxNotes || m_notes.front().seconds < seconds - kHistorySeconds))
        m_notes.pop_front();
    m_notes.push_back({track, pitch, std::clamp(velocity, 0.0, 1.0), seconds, -1, beat, -1,
                       std::max(0.0, timeline), playing, m_segment});
}
void MidiCapture::noteOff(int track, int pitch, double seconds, double beat) {
    for (auto i = m_notes.rbegin(); i != m_notes.rend(); ++i) if (i->track == track && i->pitch == pitch && i->endBeat < 0) {
        i->endSeconds = std::max(i->seconds, seconds);
        i->endBeat = std::max(i->beat + 1.0 / 64, beat);
        return;
    }
}
MidiCapture::Phrase MidiCapture::phrase(int track, double seconds, double beat) const {
    Phrase out;
    std::vector<const Played*> notes;
    for (const auto& n : m_notes) if (n.track == track && n.seconds >= seconds - kHistorySeconds) notes.push_back(&n);
    if (notes.empty()) return out;
    const auto segment = notes.back()->segment;
    // Keep the most recent phrase. A chord held across a pause does not split the phrase.
    size_t first = 0;
    double soundingUntil = notes.front()->endSeconds < 0 ? seconds : notes.front()->endSeconds;
    for (size_t i = 1; i < notes.size(); ++i) {
        const double end = notes[i]->endSeconds < 0 ? seconds : notes[i]->endSeconds;
        if (notes[i]->segment != notes[i - 1]->segment || notes[i]->seconds - soundingUntil > kPhraseGapSeconds) {
            first = i;
            soundingUntil = end;
        } else soundingUntil = std::max(soundingUntil, end);
    }
    const double lastBeat = notes.back()->endBeat < 0 ? beat : notes.back()->endBeat;
    while (first + 1 < notes.size() && (notes[first]->segment != segment || notes[first]->beat < lastBeat - kMaxBeats + 4)) ++first;
    const Played& origin = *notes[first];
    out.playedWhileRunning = origin.playing;
    out.timelineStart = origin.playing ? std::floor(origin.timeline / 4) * 4 : 0;
    const double offset = origin.playing ? origin.timeline - out.timelineStart : 0;
    double end = 0;
    for (size_t i = first; i < notes.size(); ++i) {
        const Played& n = *notes[i];
        if (n.segment != segment) continue;
        const double at = offset + n.beat - origin.beat;
        const double duration = std::max(1.0 / 64, (n.endBeat < 0 ? beat : n.endBeat) - n.beat);
        if (at >= kMaxBeats) continue;
        const double length = std::min(duration, kMaxBeats - at);
        out.notes.push_back({n.pitch, at, length, n.velocity, 1});
        end = std::max(end, at + length);
    }
    out.length = std::clamp(std::ceil(end / 4 - 1e-9) * 4, 4.0, kMaxBeats);
    return out;
}
void MidiCapture::consume(int track) {
    m_notes.erase(std::remove_if(m_notes.begin(), m_notes.end(), [track](const Played& n) { return n.track == track; }), m_notes.end());
}
void MidiCapture::clear() { m_notes.clear(); m_segment = 0; }
} // namespace sessions
