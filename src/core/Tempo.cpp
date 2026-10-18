#include "Tempo.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdio>
#include <QtGlobal>

namespace sessions::tempo {

namespace {

constexpr double kMinBpm = 40, kMaxBpm = 240;

// Onset strength per hop: three bands (under 160 Hz, 160 Hz .. 2 kHz, above), each's level in
// the hop (the square root of its RMS: loud hits count more than quiet ones, but not by their
// whole power), and how much it rose since the last hop (falls count nothing).
std::vector<float> onsetCurve(const SampleData& s, qint64 frames, int hop) {
    std::vector<float> out;
    out.reserve(size_t(frames / hop + 1));
    const double rate = s.rate;
    const float aLow = float(std::exp(-2 * M_PI * 160.0 / rate)), aMid = float(std::exp(-2 * M_PI * 2000.0 / rate));
    float zLow = 0, zMid = 0;
    double e[3] = {0, 0, 0}, prev[3] = {0, 0, 0};
    int inHop = 0;
    bool first = true;   // the first hop sets the levels; the jump from nothing to them isn't an onset
    for (qint64 i = 0; i < frames; ++i) {
        const float x = 0.5f * (s.left[size_t(i)] + s.right[size_t(i)]);
        zLow = x + (zLow - x) * aLow;
        zMid = x + (zMid - x) * aMid;
        const float low = zLow, mid = zMid - zLow, high = x - zMid;
        e[0] += double(low) * low; e[1] += double(mid) * mid; e[2] += double(high) * high;
        if (++inHop == hop) {
            float o = 0;
            static const float weight[3] = {1.0f, 0.8f, 0.6f};   // the kick and the snare say more about the beat than the hats
            for (int b = 0; b < 3; ++b) {
                const double v = std::pow(e[b] / hop, 0.25);
                if (!first) o += weight[b] * float(std::max(0.0, v - prev[b]));
                prev[b] = v;
                e[b] = 0;
            }
            first = false;
            out.push_back(o);
            inHop = 0;
        }
    }
    return out;
}

} // namespace

Estimate detect(const SampleData& s, double maxSeconds) {
    Estimate none;
    if (s.rate <= 0 || s.left.size() != s.right.size()) return none;
    const qint64 frames = std::min<qint64>(s.frames(), qint64(maxSeconds * s.rate));
    if (frames < qint64(s.rate * 1.0)) return none;   // under a second: no beat to find
    const int hop = std::max(32, int(std::lround(s.rate / 187.5)));   // ~5.3 ms, whatever the file's rate
    const double fps = s.rate / hop;
    std::vector<float> o = onsetCurve(s, frames, hop);
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
    if (hits < 3 || sd <= 0) return none;
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
    int best = -1;
    double bestScore = 0;
    for (int L = lMin; L <= lMax; ++L) {
        if (!(r[size_t(L)] > r[size_t(L - 1)] && r[size_t(L)] >= r[size_t(L + 1)])) continue;   // a peak of its own, not the slope down from lag 0
        const double bpm = fps * 60 / L;
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 110.0) / 2.0, 2));
        const double comb = at(L) + 0.5 * at(2 * L) + 0.2 * at(3 * L) + 0.25 * at(4 * L);
        const double score = comb * prior;
        if (score > bestScore) { bestScore = score; best = L; }
    }
    if (best < 0) return none;
    if (qEnvironmentVariableIsSet("SESSIONS_TEMPO_DEBUG")) {   // the top lags, for tuning the detector
        std::vector<std::pair<double, int>> top;
        for (int L = lMin; L <= lMax; ++L) {
            const double bpm = fps * 60 / L;
            const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 110.0) / 2.0, 2));
            top.push_back({(at(L) + 0.5 * at(2 * L) + 0.2 * at(3 * L) + 0.25 * at(4 * L)) * prior, L});
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
    // A beat repeats: the period's double correlates too. Noise's best lag is just the luckiest of many.
    const double strength = r[size_t(best)] / r[0];
    if (strength < 0.15 || r[size_t(best)] < lagMean + 3.0 * lagSd || at(2 * best) < 0.35 * at(best)) return none;
    // refine the period between hops (a parabola through the three points around the peak)
    double period = best;
    if (best > 0 && best + 1 < int(r.size())) {
        const double a = r[size_t(best - 1)], b = r[size_t(best)], c = r[size_t(best + 1)];
        const double den = a - 2 * b + c;
        if (std::fabs(den) > 1e-12) period += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    double bpm = fps * 60 / period;
    // the octave: a loop is a whole number of bars; otherwise stay near 100 BPM
    const double seconds = double(frames) / s.rate;
    double chosen = bpm, chosenCost = 1e9;
    for (int k = -1; k <= 1; ++k) {
        const double cand = bpm * std::exp2(k);
        if (cand < kMinBpm || cand > kMaxBpm) continue;
        const double beats = seconds * cand / 60;
        const double bars = beats / 4;
        const bool round = std::round(bars) >= 1 && std::fabs(bars - std::round(bars)) < 0.03 * std::round(bars);
        const double cost = (round ? 0.0 : 1.0) + std::fabs(std::log2(cand / 100.0));
        if (cost < chosenCost) { chosenCost = cost; chosen = cand; }
    }
    return {chosen, std::min(1.0, strength)};
}

int octaveFor(double sampleBpm, double setTempo) {
    if (sampleBpm <= 0 || setTempo <= 0) return 0;
    return std::clamp(int(std::lround(std::log2(setTempo / sampleBpm))), -3, 3);
}

Sync syncTo(const SampleData& s, double setTempo, double detectedBpm) {
    Sync out;
    const double secs = s.seconds() > 0 ? s.seconds() : 1.0;
    const double tempoNow = setTempo > 0 ? setTempo : 120.0;
    out.detected = detectedBpm > 0 ? detectedBpm : detect(s).bpm;
    if (out.detected > 0) {
        const int k = octaveFor(out.detected, tempoNow);
        const double own = secs * out.detected / 60;            // the sample's beats at its own tempo
        double beats = own * std::exp2(k);                        // set beats
        const double bars = beats / 4, wholeBeats = std::round(beats);
        // a loop that's a little short or long of its bars (a tail cut off, a sloppy edit) is stretched to them
        if (std::round(bars) >= 1 && std::fabs(bars - std::round(bars)) < 0.08 * std::round(bars)) { beats = std::round(bars) * 4; out.loop = true; }
        else if (wholeBeats >= 1 && std::fabs(beats - wholeBeats) < 0.04) { beats = wholeBeats; out.loop = true; }
        out.beats = std::max(0.25, beats);
        return out;
    }
    // no beat found: the old guess (a power of two bars at a plausible tempo), else the file as it is
    const double atTempo = secs * tempoNow / 60.0;
    double beats = atTempo, best = 0.2;
    for (int bars : {1, 2, 4, 8, 16, 32}) {
        const double b = bars * 4.0, bpm = b * 60.0 / secs;
        const double err = std::abs(b - atTempo) / atTempo;
        if (bpm >= 60 && bpm <= 200 && err < best) { best = err; beats = b; out.loop = true; }
    }
    out.beats = std::max(0.25, beats);
    return out;
}

} // namespace sessions::tempo
