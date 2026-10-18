#include "ArrangementOps.h"
#include <algorithm>
#include <cmath>

namespace sessions::arrangement {

namespace {
constexpr double kEps = 1e-9;

void sortPlacements(Track& t) {
    std::sort(t.arrangement.begin(), t.arrangement.end(), [](const ArrangementClip& a, const ArrangementClip& b) { return a.start < b.start; });
}

void shiftAutomation(Track& t, double from, double by) {
    for (Envelope& e : t.automation)
        for (AutoPoint& pt : e.points) if (pt.beat >= from - kEps) pt.beat += by;
}
} // namespace

void split(Project& p, Track& t, double beat) {
    QVector<ArrangementClip> extra;
    for (ArrangementClip& a : t.arrangement) {
        if (beat <= a.start + kEps || beat >= a.start + a.length - kEps) continue;
        ArrangementClip right = a;
        right.start = beat;
        right.length = a.start + a.length - beat;
        right.offset = a.offset + (beat - a.start);   // unwrapped: play time takes it round the brace (clipPositionAt)
        right.clip = p.copyClip(a.clip);
        right.fadeIn = 0; right.fadeInCurve = 0.5;   // the fade-in stays with the left part, the fade-out goes with the right
        a.length = beat - a.start;
        a.fadeOut = 0; a.fadeOutCurve = 0.5;
        a.fadeIn = std::min(a.fadeIn, a.length);
        right.fadeOut = std::min(right.fadeOut, right.length);
        extra.push_back(right);
    }
    t.arrangement += extra;
    sortPlacements(t);
}

void clearRange(Project& p, Track& t, double start, double end) {
    if (end <= start) return;
    split(p, t, start);
    split(p, t, end);
    t.arrangement.erase(std::remove_if(t.arrangement.begin(), t.arrangement.end(), [&](const ArrangementClip& a) {
        return a.start >= start - kEps && a.start + a.length <= end + kEps;
    }), t.arrangement.end());
}

void insertTime(Project& p, double start, double length) {
    if (length <= 0) return;
    for (Track& t : p.tracks) {
        split(p, t, start);
        for (ArrangementClip& a : t.arrangement) if (a.start >= start - kEps) a.start += length;
        shiftAutomation(t, start, length);
    }
    for (Track& r : p.returns) shiftAutomation(r, start, length);
    for (Locator& l : p.locators) if (l.beat >= start - kEps) l.beat += length;
}

void deleteTime(Project& p, double start, double end) {
    if (end <= start) return;
    const double len = end - start;
    auto cutAutomation = [&](Track& t) {
        for (Envelope& e : t.automation) {
            // keep the value at the cut so what follows doesn't jump
            const double at = e.valueAt(end);
            QVector<AutoPoint> kept;
            for (const AutoPoint& pt : e.points) {
                if (pt.beat < start - kEps) kept.push_back(pt);
                else if (pt.beat >= end - kEps) kept.push_back({pt.beat - len, pt.value});
            }
            if (!e.points.isEmpty() && e.points.last().beat > end) kept.push_back({start, at});
            std::sort(kept.begin(), kept.end(), [](const AutoPoint& a, const AutoPoint& b) { return a.beat < b.beat; });
            e.points = kept;
        }
    };
    for (Track& t : p.tracks) {
        clearRange(p, t, start, end);
        for (ArrangementClip& a : t.arrangement) if (a.start >= end - kEps) a.start -= len;
        cutAutomation(t);
    }
    for (Track& r : p.returns) cutAutomation(r);
    p.locators.erase(std::remove_if(p.locators.begin(), p.locators.end(), [&](const Locator& l) {
        return l.beat >= start - kEps && l.beat < end - kEps;
    }), p.locators.end());
    for (Locator& l : p.locators) if (l.beat >= end - kEps) l.beat -= len;
}

void duplicateTime(Project& p, double start, double end) {
    if (end <= start) return;
    const double len = end - start;
    // Copy the material first, then open the gap and drop the copies in.
    QVector<QVector<ArrangementClip>> copies(p.tracks.size());
    for (int i = 0; i < p.tracks.size(); ++i) {
        for (const ArrangementClip& a : p.tracks[i].arrangement) {
            const double s = std::max(a.start, start), e = std::min(a.start + a.length, end);
            if (e <= s + kEps) continue;
            ArrangementClip c = a;
            c.offset = a.offset + (s - a.start);
            c.start = s + len;
            c.length = e - s;
            c.clip = p.copyClip(a.clip);
            if (s > a.start + kEps) { c.fadeIn = 0; c.fadeInCurve = 0.5; }   // a cut edge has no fade
            if (e < a.start + a.length - kEps) { c.fadeOut = 0; c.fadeOutCurve = 0.5; }
            copies[i].push_back(c);
        }
    }
    insertTime(p, end, len);
    for (int i = 0; i < p.tracks.size(); ++i) {
        p.tracks[i].arrangement += copies[i];
        sortPlacements(p.tracks[i]);
    }
}

QVector<Note> notesIn(const ArrangementClip& a, double start, double end) {
    QVector<Note> out;
    if (!a.clip || a.clip->length <= 0) return out;
    const double lo = std::max(start, a.start), hi = std::min(end, a.start + a.length);
    if (hi <= lo) return out;
    forClipPasses(a.offset, a.clip->loopStart, a.clip->length, a.clip->loop, lo - a.start, hi - a.start, [&](double at, double from, double to) {
        for (const Note& n : a.clip->notes) {
            if (n.start < from || n.start >= to) continue;
            const double t = a.start + at + (n.start - from);
            if (t < lo - kEps || t >= hi - kEps) continue;
            Note c = n;
            c.start = t;
            c.length = std::min({n.length, to - n.start, hi - t});
            out.push_back(c);
        }
    });
    return out;
}

ClipPtr consolidate(Project& p, Track& t, double start, double end) {
    if (end <= start || t.type == TrackType::Audio) return nullptr;   // joining audio would need rendering it
    QVector<Note> notes;
    QString name;
    for (const ArrangementClip& a : t.arrangement) {
        if (a.start + a.length <= start + kEps || a.start >= end - kEps) continue;
        if (name.isEmpty() && a.clip) name = a.clip->name;
        for (Note n : notesIn(a, start, end)) { n.start -= start; notes.push_back(n); }
    }
    clearRange(p, t, start, end);
    ClipPtr c = p.newClip(name.isEmpty() ? QString("MIDI Clip") : name, end - start);
    c->notes = notes;
    t.arrangement.push_back({start, end - start, 0, c});
    sortPlacements(t);
    return c;
}

} // namespace sessions::arrangement
