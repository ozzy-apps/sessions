#include "SampleEdit.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace sessions::sampleedit {

QString editedPath(const QString& dir, const QString& original) {
    QDir().mkpath(dir);
    QString base = QFileInfo(original).completeBaseName();
    base.remove(QRegularExpression(" \\(edit( \\d+)?\\)$"));
    if (base.isEmpty()) base = "Audio";
    QString path = QDir(dir).filePath(base + " (edit).wav");
    for (int n = 2; QFileInfo::exists(path); ++n) path = QDir(dir).filePath(QString("%1 (edit %2).wav").arg(base).arg(n));
    return path;
}

namespace {

// Writes the oriented sample's frames [f0, f1) and [f2, f3) (either may be empty) to a new file.
bool writePieces(const SampleData& s, qint64 f0, qint64 f1, qint64 f2, qint64 f3, const QString& path, QString* error) {
    samples::WavWriter w;
    if (!w.open(path, int(std::lround(s.rate)), samples::WavFormat::Float32, error)) return false;
    std::vector<float> buf;
    auto put = [&](qint64 a, qint64 b) {
        a = std::clamp<qint64>(a, 0, s.frames()); b = std::clamp<qint64>(b, a, s.frames());
        buf.resize(size_t(b - a) * 2);
        for (qint64 f = a; f < b; ++f) { buf[size_t(f - a) * 2] = s.left[size_t(f)]; buf[size_t(f - a) * 2 + 1] = s.right[size_t(f)]; }
        if (b > a) w.write(buf.data(), b - a);
    };
    put(f0, f1);
    put(f2, f3);
    return w.close(error);
}

// The audio's times in seconds, in the oriented sample, at two clip beats (sorted).
void secondsOf(const Clip& c, double b0, double b1, double& s0, double& s1) {
    s0 = c.secondsAt(std::min(b0, b1));
    s1 = c.secondsAt(std::max(b0, b1));
}

void shiftEnvelopes(Clip& c, double from, double by, double drop0, double drop1) {
    for (Envelope& e : c.envelopes) {
        e.points.erase(std::remove_if(e.points.begin(), e.points.end(), [&](const AutoPoint& p) { return p.beat >= drop0 && p.beat < drop1; }), e.points.end());
        for (AutoPoint& p : e.points) if (p.beat >= from) p.beat = std::max(0.0, p.beat + by);
    }
}

} // namespace

bool cut(Clip& c, double b0, double b1, const QString& dir, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    if (!c.isAudio() || !c.sample) return fail("The clip has no audio to edit.");
    if (!c.takes.isEmpty()) return fail("A clip with takes can't be cut: its audio is spread over the takes.");
    if (b1 < b0) std::swap(b0, b1);
    if (b1 - b0 < 1e-6) return fail("Select a stretch of the waveform first.");
    const SamplePtr src = c.reverse ? samples::reversed(c.sample) : c.sample;
    double s0, s1;
    secondsOf(c, b0, b1, s0, s1);
    const double dur = src->seconds();
    s0 = std::clamp(s0, 0.0, dur); s1 = std::clamp(s1, s0, dur);
    const qint64 f0 = qint64(std::llround(s0 * src->rate)), f1 = qint64(std::llround(s1 * src->rate));
    if (f1 - f0 < 1 && s1 <= 0) return fail("There's no audio in the selection.");
    const QString path = editedPath(dir, c.samplePath);
    if (!writePieces(*src, 0, f0, f1, src->frames(), path, error)) return false;
    SamplePtr out = samples::load(path, error);
    if (!out) return false;
    // the warp map: what's before stays; a marker pins the cut; what's after moves earlier in beats and seconds
    const double dBeat = b1 - b0, dSec = s1 - s0;
    QVector<WarpMarker> kept;
    for (const WarpMarker& m : c.warpMarkers) if (m.beat < b0 - 1e-9) kept.push_back(m);
    kept.push_back({b0, s0});
    for (const WarpMarker& m : c.warpMarkers)
        if (m.beat > b1 + 1e-9) {
            const WarpMarker moved{m.beat - dBeat, m.seconds - dSec};
            if (moved.beat > kept.last().beat + 1e-9 && moved.seconds > kept.last().seconds + 1e-9) kept.push_back(moved);
        }
    if (kept.size() < 2) {   // nothing after the cut: carry the tempo on from the last marker before it
        const double bpm = c.sampleBpm() > 0 ? c.sampleBpm() : 120;
        const double rest = std::max(0.0, out->seconds() - s0);
        kept.push_back({b0 + std::max(0.25, rest * bpm / 60.0), s0 + std::max(0.25 * 60.0 / bpm, rest)});
    }
    c.warpMarkers = kept;
    c.samplePath = path;
    c.sample = out;
    c.reverse = false;   // the new file is written the way it was shown
    c.detectedBpm = 0;
    // the start marker and the brace follow the audio: what was after the cut moves earlier, what was inside it lands on its start
    const double end = c.loopEnd();
    auto moved = [&](double x) { return x >= b1 ? x - dBeat : x > b0 ? b0 : x; };
    c.start = moved(c.start);
    c.loopStart = moved(c.loopStart);
    c.length = std::max(0.25, moved(end) - c.loopStart);
    shiftEnvelopes(c, b1, -dBeat, b0, b1);
    return true;
}

bool crop(Clip& c, double b0, double b1, const QString& dir, QString* error) {
    auto fail = [error](const QString& e) { if (error) *error = e; return false; };
    if (!c.isAudio() || !c.sample) return fail("The clip has no audio to edit.");
    if (!c.takes.isEmpty()) return fail("A clip with takes can't be cropped: its audio is spread over the takes.");
    if (b1 < b0) std::swap(b0, b1);
    if (b1 - b0 < 1e-6) return fail("Select the stretch to keep first.");
    const SamplePtr src = c.reverse ? samples::reversed(c.sample) : c.sample;
    double s0, s1;
    secondsOf(c, b0, b1, s0, s1);
    const double dur = src->seconds();
    s0 = std::clamp(s0, 0.0, dur); s1 = std::clamp(s1, s0, dur);
    const qint64 f0 = qint64(std::llround(s0 * src->rate)), f1 = qint64(std::llround(s1 * src->rate));
    if (f1 - f0 < 1) return fail("There's no audio in the selection.");
    const QString path = editedPath(dir, c.samplePath);
    if (!writePieces(*src, f0, f1, 0, 0, path, error)) return false;
    SamplePtr out = samples::load(path, error);
    if (!out) return false;
    QVector<WarpMarker> kept{{0, 0}};
    for (const WarpMarker& m : c.warpMarkers)
        if (m.beat > b0 + 1e-9 && m.beat < b1 - 1e-9) {
            const WarpMarker moved{m.beat - b0, m.seconds - s0};
            if (moved.beat > kept.last().beat + 1e-9 && moved.seconds > kept.last().seconds + 1e-9) kept.push_back(moved);
        }
    if (b1 - b0 > kept.last().beat + 1e-9 && s1 - s0 > kept.last().seconds + 1e-9) kept.push_back({b1 - b0, s1 - s0});
    if (kept.size() < 2) kept.push_back({std::max(0.25, b1 - b0), std::max(1e-3, s1 - s0)});
    c.warpMarkers = kept;
    c.samplePath = path;
    c.sample = out;
    c.reverse = false;
    c.detectedBpm = 0;
    c.length = std::max(0.25, b1 - b0);
    c.start = c.loopStart = 0;   // the kept stretch is the clip, from 1.1.1
    for (Envelope& e : c.envelopes) {
        e.points.erase(std::remove_if(e.points.begin(), e.points.end(), [&](const AutoPoint& p) { return p.beat < b0 || p.beat >= b1; }), e.points.end());
        for (AutoPoint& p : e.points) p.beat -= b0;
    }
    return true;
}

} // namespace sessions::sampleedit
