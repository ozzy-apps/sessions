#include "core/Import.h"
#include "core/SetFile.h"
#include <QCoreApplication>
#include <QUndoStack>
#include <cmath>
#include <cstdio>

using namespace sessions;
namespace {
int checks = 0, failures = 0;
void check(bool pass, const char* name) { ++checks; if (!pass) { ++failures; std::fprintf(stderr, "FAIL: %s\n", name); } }
bool near(double a, double b, double tolerance = 1e-8) { return std::abs(a - b) < tolerance; }
SamplePtr tone(double seconds = 31) {
    const double rate = 8000;
    std::vector<float> signal(size_t(seconds * rate));
    for (size_t i = 0; i < signal.size(); ++i) signal[i] = float(0.2 * std::sin(2 * M_PI * 220 * i / rate));
    return samples::fromFrames(std::move(signal), {}, rate);
}
SamplePtr drums(double seconds = 31) {
    const double rate = 8000, period = 60.0 / 176;
    std::vector<float> signal(size_t(seconds * rate), 0);
    for (double at = 0; at < seconds; at += period) {
        const size_t start = size_t(std::lround(at * rate));
        for (size_t i = 0; i < 1000 && start + i < signal.size(); ++i)
            signal[start + i] += float(std::sin(2 * M_PI * 110 * i / rate) * std::exp(-double(i) / 100));
    }
    return samples::fromFrames(std::move(signal), {}, rate);
}
int attach(Project& p, const ClipPtr& c) {
    if (p.scenes.isEmpty()) p.scenes.push_back("1");
    const int t = p.insertTrack(p.tracks.size(), TrackType::Audio);
    p.tracks[t].clipSlots[0] = c;
    return t;
}
ClipPtr estimated(Project& p, const QString& path, const SamplePtr& sample, double bpm, double confidence = 0.5) {
    auto c = p.newAudioClip(path, sample, false);
    c->detectedBpm = bpm; c->sourceTempoOrigin = SourceTempoOrigin::Detected; c->sourceTempoConfidence = confidence;
    c->warp = true; c->loop = false; c->length = sample->seconds() * bpm / 60;
    c->warpMarkers = {{0, 0}, {c->length, sample->seconds()}};
    import::rememberAutomaticSourceMap(*c);
    return c;
}
bool sameMap(const ClipPtr& a, const ClipPtr& b) {
    if (a->warpMarkers.size() != b->warpMarkers.size()) return false;
    for (int i = 0; i < a->warpMarkers.size(); ++i)
        if (!near(a->warpMarkers[i].beat, b->warpMarkers[i].beat) || !near(a->warpMarkers[i].seconds, b->warpMarkers[i].seconds)) return false;
    return true;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto sustained = tone();
    {
        Project p; p.tempo = 120;
        const auto clips = import::audioClips(p, {"/exports/song/reference_176bpm.wav", "/exports/song/airy.wav"}, {sustained, sustained});
        check(clips.size() == 2 && sameMap(clips[0], clips[1]), "one equal-span multi-file import shares its authoritative source grid");
        check(clips[0]->sourceTempoOrigin == SourceTempoOrigin::Metadata && clips[1]->sourceTempoOrigin == SourceTempoOrigin::Shared,
              "metadata remains authoritative while its companion records shared provenance");
        check(near(clips[0]->sampleBpm(), 176) && clips[1]->warp && near(p.tempo, 120), "stem source176 is independent of project120");
        Project reverse; reverse.tempo = 120;
        const auto reordered = import::audioClips(reverse, {"/exports/song/airy.wav", "/exports/song/reference_176bpm.wav"}, {sustained, sustained});
        check(sameMap(clips[0], reordered[0]) && sameMap(clips[1], reordered[1]), "batch order does not change the shared source interpretation");
    }
    {
        Project p; p.tempo = 120;
        auto guide = estimated(p, "/exports/one/0 Drums.wav", sustained, 176);
        attach(p, guide);
        const auto keys = import::audioClips(p, {"/exports/one/2 Keyboard.wav"}, {sustained}).first(); attach(p, keys);
        const auto bass = import::audioClips(p, {"/exports/one/1 Bass.wav"}, {sustained}).first(); attach(p, bass);
        check(keys->warp && bass->warp && sameMap(guide, keys) && sameMap(keys, bass), "separately imported named stems inherit the existing drum reference");
        check(p.matchingStemClips(keys).size() == 3, "matching-stem repair discovers already placed siblings without scanning folders");
        const auto elsewhere = import::audioClips(p, {"/exports/two/3 Synth.wav"}, {sustained}).first();
        check(!elsewhere->warp && elsewhere->detectedBpm == 0, "same-length audio in another folder stays independent");
        const auto otherLength = import::audioClips(p, {"/exports/one/4 Other.wav"}, {tone(32)}).first();
        check(!otherLength->warp, "different source durations do not inherit a stem tempo");
        p.setTempo(90);
        check(near(guide->sampleBpm(), 176) && near(keys->sampleBpm(), 176) && near(bass->sampleBpm(), 176), "changing the project clock does not rewrite any shared source grid");
    }
    {
        Project p; p.tempo = 120;
        auto keys = import::audioClips(p, {"/exports/last/2 Keyboard.wav"}, {sustained}).first(); attach(p, keys);
        check(!keys->warp, "a sustained first stem stays at its recorded speed until tempo evidence exists");
        p.undoStack()->clear(); const auto before = p.captureEditState();
        ClipPtr rhythm;
        {
            EditScope edit(&p, "Import Drums");
            rhythm = import::audioClips(p, {"/exports/last/0 Drums.wav"}, {drums()}).first(); attach(p, rhythm);
        }
        check(rhythm->detectedBpm > 0 && keys->warp && sameMap(keys, rhythm), "a later drum import repairs the earlier automatically unknown sibling");
        check(p.undoStack()->count() == 1, "adding the guide and correcting earlier stem maps is one undo step");
        p.undoStack()->undo(); check(p.captureEditState() == before && !keys->warp, "undo restores both the earlier stem and the imported track set");
        p.undoStack()->redo(); check(keys->warp && sameMap(keys, rhythm), "redo restores the shared source map and its provenance");
    }
    {
        Project p;
        auto old = estimated(p, "/exports/protected/0 Drums.wav", sustained, 176); attach(p, old);
        p.setClipSourceBpm(old, 176);
        check(old->sourceTempoOrigin == SourceTempoOrigin::Manual, "entering the same BPM locks an automatic estimate as an explicit correction");
        const auto saved = p.captureEditState();
        auto keys = import::audioClips(p, {"/exports/protected/2 Keyboard.wav"}, {sustained}).first();
        check(old->sourceTempoOrigin == SourceTempoOrigin::Manual && near(old->sampleBpm(), 176) && sameMap(old, keys), "later imports respect an explicit source correction and inherit it");
        old->sourceTempoOrigin = SourceTempoOrigin::Legacy;
        auto unknown = import::audioClips(p, {"/exports/protected/1 Bass.wav"}, {sustained}).first();
        check(old->sourceTempoOrigin == SourceTempoOrigin::Legacy && near(old->sampleBpm(), 176) && !unknown->warp, "legacy maps are neither rewritten nor silently promoted to trusted references");
        (void)saved;
        old->sourceTempoOrigin = SourceTempoOrigin::Detected; import::rememberAutomaticSourceMap(*old);
        old->warpMarkers.insert(1, {10, 2});
        const auto changed = old->warpMarkers;
        unknown = import::audioClips(p, {"/exports/protected/1 Bass.wav"}, {sustained}).first();
        check(old->warpMarkers.size() == changed.size() && near(old->warpMarkers[1].seconds, 2) && !unknown->warp,
              "an edited automatic warp map is protected from later stem imports");
    }
    {
        Project p;
        auto quarter = estimated(p, "/exports/meter/0 Drums.wav", sustained, 176, 0.2);
        auto half = estimated(p, "/exports/meter/1 Percussion.wav", sustained, 88, 0.95);
        auto halfBass = estimated(p, "/exports/meter/2 Bass.wav", sustained, 88, 0.95);
        ClipPtr guide;
        const auto common = import::sharedSourceTempo({half, halfBass, quarter}, &guide);
        check(near(common.bpm, 176) && guide == quarter, "a corroborated half-time percussion alias does not outvote the drum reference");
        auto pitched = estimated(p, "/exports/meter/3 Synth.wav", sustained, 132, 0.3);
        check(import::sharedSourceTempo({pitched}).bpm == 0, "one weak pitched pattern is insufficient evidence for shared recording tempo");
        auto a = p.newAudioClip("/exports/conflict/one_176bpm.wav", sustained, false);
        auto b = p.newAudioClip("/exports/conflict/two_178bpm.wav", sustained, false);
        check(import::sharedSourceTempo({a, b}).bpm == 0, "incompatible explicit source metadata is not silently resolved by confidence");
    }
    {
        Project p;
        auto c = estimated(p, "/exports/nonlinear/0 Drums.wav", sustained, 120); const int t = attach(p, c);
        c->envelopes = {{"mixer:volume", {{8, 0.5}, {50, 0.8}}}};
        c->comp = {{8, 50, 0}}; c->takes = {{"", {}, "take", 8}};
        p.tracks[t].arrangement = {{16, 40, 8, c, 4, 6}};
        const auto before = p.captureEditState();
        const QVector<WarpMarker> map{{0, 0}, {30, 10}, {114, 31}};
        {
            EditScope edit(&p, "Adaptive Source");
            p.setClipWarpMap(c, map);
        }
        // Old grid120: offset8=4s -> new12; end48=24s -> new86.
        const auto& placed = p.tracks[t].arrangement[0];
        check(near(c->length, 114) && near(placed.start, 16) && near(placed.offset, 12) && near(placed.length, 74),
              "nonlinear source remapping preserves Arrangement source endpoints and song position");
        check(near(placed.fadeIn, 6) && near(placed.fadeOut, 12), "fade nodes preserve their source-audio edge times under a nonlinear grid");
        check(near(c->envelopes[0].points[0].beat, 12) && near(c->envelopes[0].points[1].beat, 90)
            && near(c->comp[0].start, 12) && near(c->comp[0].end, 90) && near(c->takes[0].startBeat, 12),
              "automation, comp regions and take offsets follow source seconds rather than an average BPM ratio");
        p.undoStack()->undo(); check(p.captureEditState() == before, "one undo restores every nonlinear source-coordinate change");
        p.undoStack()->redo();
        c->takes.clear(); import::rememberAutomaticSourceMap(*c);
        const auto second = import::audioClips(p, {"/exports/nonlinear/1 Bass.wav"}, {sustained}).first(); attach(p, second);
        check(sameMap(c, second), "later stems reuse the exact accepted adaptive grid");
        p.setClipSourceBpm(c, 180, true);
        check(sameMap(c, second) && c->sourceTempoOrigin == SourceTempoOrigin::Manual && second->sourceTempoOrigin == SourceTempoOrigin::Manual,
              "an explicit matching-stem correction shares the selected nonlinear shape exactly");
        const auto preserved = c->warpMarkers;
        p.syncClipToTempo(c);
        check(c->warpMarkers.size() == preserved.size() && near(c->warpMarkers[1].beat, preserved[1].beat), "normal Sync does not flatten an accepted variable timing map");
        const Clip loaded = setfile::clipFromJson(setfile::clipToJson(*second));
        check(loaded.sourceTempoOrigin == second->sourceTempoOrigin && near(loaded.sourceTempoConfidence, second->sourceTempoConfidence),
              "source provenance and confidence survive serialization");
        auto json = setfile::clipToJson(*second); auto audio = json["audio"].toObject();
        audio.remove("sourceTempoOrigin"); audio.remove("sourceTempoConfidence"); audio.remove("sourceTempoMap"); json["audio"] = audio;
        check(setfile::clipFromJson(json).sourceTempoOrigin == SourceTempoOrigin::Legacy, "older sets load protected without inventing provenance");
    }
    {
        Project p;
        const auto shortTone = tone(4);
        const auto clips = import::audioClips(p, {"/loops/known_176bpm.wav", "/loops/other.wav"}, {shortTone, shortTone});
        check(clips[0]->warp && !clips[1]->warp, "equal-length short loop-pack files are not assumed to be aligned stems");
        const auto unknown = import::audioClips(p, {"/exports/unknown/0 Drums.wav", "/exports/unknown/1 Bass.wav"}, {sustained, sustained});
        check(!unknown[0]->warp && !unknown[1]->warp && sameMap(unknown[0], unknown[1]), "a wholly uncertain stem cohort retains one common natural-speed grid");
    }
    {
        Project p;
        auto c = p.newAudioClip("/exports/trimmed_120bpm.wav", sustained, false);
        attach(p, c);
        c->loopStart = 8; c->length = 40; c->start = 8;
        const double originalEnd = c->secondsAt(c->loopEnd());
        p.syncClipToTempo(c);
        check(near(c->loopStart, 8) && near(c->length, 40) && near(c->secondsAt(c->loopEnd()), originalEnd)
            && c->secondsAt(c->loopEnd()) <= c->sample->seconds(),
              "Sync preserves a trimmed loop end instead of extending its length past the source");
    }
    std::printf("stem_import: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
