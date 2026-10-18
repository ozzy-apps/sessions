#include "Tempo.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtGlobal>

namespace sessions::tempo {

namespace {

constexpr double kMinBpm = 40, kMaxBpm = 240;

// Sample packs commonly carry the actual metrical tempo in their names. Explicit BPM tags
// are authoritative; a bare number also needs a tightly cut musical duration, so kit numbers,
// MIDI notes and catalogue IDs do not accidentally become source tempos. Ignore directories.
double filenameBpm(const QString& path, double seconds) {
    const QString name = QFileInfo(path).completeBaseName();
    static const QRegularExpression tagged(
        "(?:^|[^a-z0-9])(?:(\\d{2,3}(?:\\.\\d{1,2})?)[ _-]*bpm|bpm[ _-]*(\\d{2,3}(?:\\.\\d{1,2})?))(?=$|[^a-z0-9])",
        QRegularExpression::CaseInsensitiveOption);
    double found = 0;
    auto matches = tagged.globalMatch(name);
    while (matches.hasNext()) {
        const auto m = matches.next();
        const double value = (!m.captured(1).isEmpty() ? m.captured(1) : m.captured(2)).toDouble();
        if (value < 20 || value > 999) continue;
        if (found > 0 && std::abs(found - value) > 0.01) return 0; // ambiguous conversion name
        found = value;
    }
    if (found > 0) return found;
    static const QRegularExpression number("(?:^|[ _\\-\\(\\[])([1-9]\\d{1,2}(?:\\.\\d{1,2})?)(?=$|[ _\\-\\)\\]])");
    matches = number.globalMatch(name);
    while (matches.hasNext()) {
        const double value = matches.next().captured(1).toDouble();
        if (value < kMinBpm || value > kMaxBpm || seconds <= 0) continue;
        const double beats = seconds * value / 60.0;
        // Full bars, at most 32 bars: a long song's arbitrary duration is not loop evidence.
        const double bars = std::round(beats / 4);
        if (bars < 1 || bars > 32 || std::abs(beats - 4 * bars) > 0.025) continue;
        if (found > 0 && std::abs(found - value) > 0.01) return 0;
        found = value;
    }
    return found;
}

// Onset strength per hop: three bands (under 160 Hz, 160 Hz .. 2 kHz, above), each's level in
// the hop (the square root of its RMS: loud hits count more than quiet ones, but not by their
// whole power), and how much it rose since the last hop (falls count nothing).
std::vector<float> onsetCurve(const SampleData& s, qint64 frames, int hop, double& averageLevel) {
    std::vector<float> out;
    out.reserve(size_t(frames / hop + 1));
    const double rate = s.rate;
    const float aLow = float(std::exp(-2 * M_PI * 160.0 / rate)), aMid = float(std::exp(-2 * M_PI * 2000.0 / rate));
    float zLow[2] = {}, zMid[2] = {};
    double e[3] = {0, 0, 0}, prev[3] = {0, 0, 0};
    int inHop = 0;
    bool first = true;   // the first hop sets the levels; the jump from nothing to them isn't an onset
    averageLevel = 0;
    for (qint64 i = 0; i < frames; ++i) {
        // Sum channel energies instead of collapsing stereo first. Wide / phase-inverted
        // drums can cancel almost completely in a mono sum while remaining perfectly audible.
        for (int ch = 0; ch < 2; ++ch) {
            const float raw = ch == 0 ? s.left[size_t(i)] : s.right[size_t(i)];
            const float x = std::isfinite(raw) ? raw : 0;
            zLow[ch] = x + (zLow[ch] - x) * aLow;
            zMid[ch] = x + (zMid[ch] - x) * aMid;
            const float low = zLow[ch], mid = zMid[ch] - zLow[ch], high = x - zMid[ch];
            e[0] += 0.5 * double(low) * low; e[1] += 0.5 * double(mid) * mid; e[2] += 0.5 * double(high) * high;
        }
        if (++inHop == hop) {
            float o = 0;
            static const float weight[3] = {1.0f, 0.8f, 0.6f};   // the kick and the snare say more about the beat than the hats
            for (int b = 0; b < 3; ++b) {
                const double v = std::pow(e[b] / hop, 0.25);
                averageLevel += weight[b] * v;
                if (!first) o += weight[b] * float(std::max(0.0, v - prev[b]));
                prev[b] = v;
                e[b] = 0;
            }
            first = false;
            out.push_back(o);
            inHop = 0;
        }
    }
    if (!out.empty()) averageLevel /= out.size();
    return out;
}

} // namespace

Estimate detect(const SampleData& s, double maxSeconds, const QString& sourcePath) {
    Estimate none;
    if (!std::isfinite(s.rate) || s.rate <= 0 || s.left.size() != s.right.size() || !std::isfinite(maxSeconds) || maxSeconds <= 0) return none;
    if (const double tagged = filenameBpm(sourcePath.isEmpty() ? s.path : sourcePath, s.seconds()); tagged > 0) return {tagged, 1.0};
    const qint64 frames = std::min<qint64>(s.frames(), qint64(maxSeconds * s.rate));
    if (frames < qint64(s.rate * 1.0)) return none;   // under a second: no beat to find
    const int hop = std::max(32, int(std::lround(s.rate / 187.5)));   // ~5.3 ms, whatever the file's rate
    const double fps = s.rate / hop;
    double averageLevel = 0;
    std::vector<float> o = onsetCurve(s, frames, hop, averageLevel);
    const int n = int(o.size());
    if (n < 16) return none;
    // a little smoothing, then the slow trend taken out so sustained notes don't count
    std::vector<float> sm(size_t(n), 0.0f);
    for (int i = 0; i < n; ++i) sm[size_t(i)] = (o[size_t(std::max(0, i - 1))] + o[size_t(i)] + o[size_t(std::min(n - 1, i + 1))]) / 3;
    const int win = std::max(1, int(fps));   // one second
    std::vector<float> on(size_t(n), 0.0f);
    double run = 0;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        run += sm[size_t(i)];
        ++count;
        if (i >= win) { run -= sm[size_t(i - win)]; --count; }
        on[size_t(i)] = std::max(0.0f, sm[size_t(i)] - float(run / count));
    }
    // enough hits to call it a beat?
    double mean = 0, sq = 0;
    for (float v : on) { mean += v; sq += double(v) * v; }
    mean /= n;
    const double sd = std::sqrt(std::max(0.0, sq / n - mean * mean));
    int hits = 0;
    for (int i = 1; i + 1 < n; ++i) if (on[size_t(i)] > mean + 2 * sd && on[size_t(i)] >= on[size_t(i - 1)] && on[size_t(i)] >= on[size_t(i + 1)]) ++hits;
    // RMS-window ripple in a steady tone is periodic too, but is not a musical onset.
    // This level-relative gate remains unchanged when quiet audio is turned down.
    if (hits < 3 || sd <= averageLevel * 0.008) return none;
    // the autocovariance over beat periods (the mean taken out, so noise correlates with nothing)
    double onMean = 0;
    for (float v : on) onMean += v;
    onMean /= n;
    const int lMin = std::max(2, int(fps * 60 / kMaxBpm));
    int lMax = int(fps * 60 / kMinBpm);
    if (lMax > n - 8) lMax = n - 8;
    if (lMax <= lMin + 2) return none;
    const int lTop = std::min(n - 2, 4 * lMax + 1);   // the comb below looks up to four periods out
    std::vector<double> r(size_t(lTop + 1), 0.0);
    for (int L = 0; L <= lTop; ++L) {
        double acc = 0;
        for (int i = 0; i + L < n; ++i) acc += (double(on[size_t(i)]) - onMean) * (double(on[size_t(i + L)]) - onMean);
        r[size_t(L)] = acc / std::max(1, n - L);
    }
    if (r[0] <= 0) return none;
    // The strongest period: a lag scores with its multiples too (a real beat repeats at two, three
    // and four beats; a lag of three eighths doesn't), with a gentle preference for tempos near
    // 110 BPM (a prior two octaves wide).
    auto at = [&](int L) { return L >= 0 && L < int(r.size()) ? std::max(0.0, r[size_t(L)]) : 0.0; };
    auto loopPrior = [&](int lag) {
        double period = lag;
        const double a = r[size_t(lag - 1)], b = r[size_t(lag)], c = r[size_t(lag + 1)], den = a - 2 * b + c;
        if (std::abs(den) > 1e-12) period += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        const double beats = s.seconds() * fps / period, bars = std::round(beats / 4);
        // A complete short loop is useful evidence against a three-eighth-note alias.
        // This is independent of project tempo and never fits an arbitrary long song tail.
        return bars >= 1 && beats <= 64.025 && std::abs(beats - 4 * bars) < 0.04 ? 1.25 : 1.0;
    };
    int best = -1;
    double bestScore = 0;
    for (int L = lMin; L <= lMax; ++L) {
        if (!(r[size_t(L)] > r[size_t(L - 1)] && r[size_t(L)] >= r[size_t(L + 1)])) continue;   // a peak of its own, not the slope down from lag 0
        const double bpm = fps * 60 / L;
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 110.0) / 2.0, 2));
        const double comb = at(L) + 0.5 * at(2 * L) + 0.2 * at(3 * L) + 0.25 * at(4 * L);
        const double score = comb * prior * loopPrior(L);
        if (score > bestScore) { bestScore = score; best = L; }
    }
    if (best < 0) return none;
    if (qEnvironmentVariableIsSet("SESSIONS_TEMPO_DEBUG")) {   // the top lags, for tuning the detector
        std::vector<std::pair<double, int>> top;
        for (int L = lMin; L <= lMax; ++L) {
            const double bpm = fps * 60 / L;
            const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 110.0) / 2.0, 2));
            top.push_back({(at(L) + 0.5 * at(2 * L) + 0.2 * at(3 * L) + 0.25 * at(4 * L)) * prior * loopPrior(L), L});
        }
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
        std::fprintf(stderr, "tempo: n %d fps %.1f r0 %.4f hits %d |", n, fps, r[0], hits);
        for (int k = 0; k < 6 && k < int(top.size()); ++k) {
            const int L = top[size_t(k)].second;
            std::fprintf(stderr, " %.1f bpm (L %d r %.3f 2L %.3f 3L %.3f 4L %.3f score %.4f)", fps * 60 / L, L, at(L) / r[0], at(2 * L) / r[0], at(3 * L) / r[0], at(4 * L) / r[0], top[size_t(k)].first / r[0]);
        }
        std::fprintf(stderr, "\n");
    }
    // a beat's peak towers over the other lags; noise's best lag is just the luckiest of many
    double lagMean = 0, lagSq = 0;
    for (int L = lMin; L <= lMax; ++L) { lagMean += r[size_t(L)]; lagSq += r[size_t(L)] * r[size_t(L)]; }
    lagMean /= (lMax - lMin + 1);
    const double lagSd = std::sqrt(std::max(1e-18, lagSq / (lMax - lMin + 1) - lagMean * lagMean));
    // A beat repeats: its multiples must agree. Dense breakbeats can have many good lags,
    // so demanding three standard deviations above *all* lags rejects strongly periodic
    // music. A supported peak still needs meaningful correlation and local prominence.
    const double strength = r[size_t(best)] / r[0];
    if (strength < 0.15 || r[size_t(best)] < lagMean + 1.5 * lagSd || at(2 * best) < 0.35 * at(best)) return none;
    // refine the period between hops (a parabola through the three points around the peak)
    double period = best;
    if (best > 0 && best + 1 < int(r.size())) {
        const double a = r[size_t(best - 1)], b = r[size_t(best)], c = r[size_t(best + 1)];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) period += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    auto fractional = [&](double lag) {
        const int a = int(std::floor(lag));
        return at(a) + (at(a + 1) - at(a)) * (lag - a);
    };
    // Integer lag rounding makes the second/fourth multiples of a fast beat miss their
    // peaks. Fit a single fractional period to all four repeats; this matters at DnB tempos
    // where a beat is only about 65 analysis hops long.
    auto fitPeriod = [&](double center) {
        double chosen = center, score = -1;
        for (int step = -40; step <= 40; ++step) {
            const double lag = center + step * 0.025;
            if (lag < lMin || lag > lMax) continue;
            const double value = fractional(lag) + 0.5 * fractional(2 * lag) + 0.2 * fractional(3 * lag) + 0.25 * fractional(4 * lag);
            if (value > score) { score = value; chosen = lag; }
        }
        return chosen;
    };
    period = fitPeriod(period);
    // A similarly strong double-time fundamental is better evidence than a bias towards
    // 100 BPM. This retains clear fast breakbeats instead of always folding them to 88 BPM.
    // Weak eighth-note hats do not qualify. The user's :2/x2 controls resolve ambiguous meter.
    const double base = period, baseStrength = fractional(base);
    for (int octave = 1; octave <= 2; ++octave) {
        if (base / std::exp2(octave) < lMin) break;
        const double faster = fitPeriod(base / std::exp2(octave));
        if (fractional(faster) >= 0.55 * baseStrength && fractional(3 * faster) >= 0.35 * baseStrength) period = faster;
    }
    // Refine from several widely separated repeats. A one-hop error in a 65-hop period is
    // audible drift over a long stem; fitting the peaks out to sixteen beats greatly reduces
    // that quantization error without imposing the file's (possibly tailed) duration.
    double numerator = 0, denominator = 0;
    for (int multiple = 1; multiple <= 16 && multiple * period + 3 < int(r.size()); ++multiple) {
        const int center = int(std::lround(multiple * period));
        int peak = center;
        for (int lag = std::max(1, center - 2); lag <= center + 2; ++lag)
            if (r[size_t(lag)] > r[size_t(peak)]) peak = lag;
        if (peak <= 0 || peak + 1 >= int(r.size()) || r[size_t(peak)] < 0.2 * r[0]
            || r[size_t(peak)] < r[size_t(peak - 1)] || r[size_t(peak)] < r[size_t(peak + 1)]) continue;
        double refined = peak;
        const double a = r[size_t(peak - 1)], b = r[size_t(peak)], c = r[size_t(peak + 1)], den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) refined += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        const double weight = std::pow(r[size_t(peak)] / r[0], 2);
        numerator += weight * multiple * refined;
        denominator += weight * multiple * multiple;
    }
    if (denominator > 0) period = numerator / denominator;
    const double bpm = fps * 60 / period;
    return {bpm, std::min(1.0, strength)};
}

int octaveFor(double sampleBpm, double setTempo) {
    if (sampleBpm <= 0 || setTempo <= 0) return 0;
    return std::clamp(int(std::lround(std::log2(setTempo / sampleBpm))), -3, 3);
}

Sync syncTo(const SampleData& s, double setTempo, double detectedBpm, const QString& sourcePath) {
    Sync out;
    const double secs = s.seconds() > 0 ? s.seconds() : 1.0;
    const double tempoNow = setTempo > 0 ? setTempo : 120.0;
    const bool prescribed = (std::isfinite(detectedBpm) && detectedBpm > 0)
        || filenameBpm(sourcePath.isEmpty() ? s.path : sourcePath, secs) > 0;
    out.detected = std::isfinite(detectedBpm) && detectedBpm > 0 ? detectedBpm : detect(s, 90, sourcePath).bpm;
    if (out.detected > 0) {
        const double own = secs * out.detected / 60;            // the sample's beats at its own tempo
        double beats = own;
        const double bars = beats / 4, wholeBeats = std::round(beats);
        // An acoustically estimated short loop can use its tightly cut beat length to refine
        // the estimate. A supplied source BPM only permits sample-frame rounding: truncating
        // even a small tail must not silently change the beat grid. Long stems never fit bars.
        const double tolerance = prescribed ? std::max(1e-9, s.rate > 0 ? 2 * out.detected / (60 * s.rate) : 0) : 0.025;
        if (std::round(bars) >= 1 && beats <= 64.025 && std::fabs(beats - std::round(bars) * 4) <= tolerance) { beats = std::round(bars) * 4; out.loop = true; }
        else if (wholeBeats >= 1 && beats <= 64 && std::fabs(beats - wholeBeats) <= tolerance) { beats = wholeBeats; out.loop = true; }
        if (out.loop && !prescribed) out.detected = beats * 60 / secs;
        out.beats = beats; // even a short one-shot must retain its actual source-tempo span
        return out;
    }
    // No tempo evidence: retain natural speed and a one-shot. A nearby project bar length
    // is not evidence of the source BPM.
    const double atTempo = secs * tempoNow / 60.0;
    out.beats = std::max(0.25, atTempo);
    return out;
}

} // namespace sessions::tempo
