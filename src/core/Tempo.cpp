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
std::vector<float> onsetCurve(const SampleData& s, qint64 frames, int hop, double& averageLevel,
                              std::vector<float>* attacks = nullptr) {
    std::vector<float> out;
    out.reserve(size_t(frames / hop + 1));
    if (attacks) attacks->reserve(size_t(frames / hop + 1));
    const double rate = s.rate;
    const float aLow = float(std::exp(-2 * M_PI * 160.0 / rate)), aMid = float(std::exp(-2 * M_PI * 2000.0 / rate));
    float zLow[2] = {}, zMid[2] = {};
    double e[3] = {}, prev[3] = {}, energyFast[3] = {}, energySlow[3] = {}, previousAttack[3] = {};
    // A second, slower energy envelope checks that 5 ms RMS-window ripple in low notes
    // has not aliased into a plausible tempo. Two poles suppress the carrier before the
    // envelope is sampled at the onset hop rate; the fast cue retains sharp drum timing.
    const double energyDecay = std::exp(-1 / (rate * 0.012));
    int inHop = 0;
    bool first = true;   // the first hop sets the levels; the jump from nothing to them isn't an onset
    averageLevel = 0;
    for (qint64 i = 0; i < frames; ++i) {
        double instant[3] = {};
        // Sum channel energies instead of collapsing stereo first. Wide / phase-inverted
        // drums can cancel almost completely in a mono sum while remaining perfectly audible.
        for (int ch = 0; ch < 2; ++ch) {
            const float raw = ch == 0 ? s.left[size_t(i)] : s.right[size_t(i)];
            const float x = std::isfinite(raw) ? raw : 0;
            zLow[ch] = x + (zLow[ch] - x) * aLow;
            zMid[ch] = x + (zMid[ch] - x) * aMid;
            const float low = zLow[ch], mid = zMid[ch] - zLow[ch], high = x - zMid[ch];
            instant[0] += 0.5 * double(low) * low; instant[1] += 0.5 * double(mid) * mid; instant[2] += 0.5 * double(high) * high;
        }
        for (int b = 0; b < 3; ++b) {
            e[b] += instant[b];
            if (attacks) {
                energyFast[b] = instant[b] + (energyFast[b] - instant[b]) * energyDecay;
                energySlow[b] = energyFast[b] + (energySlow[b] - energyFast[b]) * energyDecay;
            }
        }
        if (++inHop == hop) {
            float o = 0, attack = 0;
            static const float weight[3] = {1.0f, 0.8f, 0.6f};   // the kick and the snare say more about the beat than the hats
            for (int b = 0; b < 3; ++b) {
                const double v = std::pow(e[b] / hop, 0.25);
                averageLevel += weight[b] * v;
                if (!first) o += weight[b] * float(std::max(0.0, v - prev[b]));
                prev[b] = v;
                if (attacks) {
                    const double level = std::pow(energySlow[b], 0.25);
                    if (!first) attack += weight[b] * float(std::max(0.0, level - previousAttack[b]));
                    previousAttack[b] = level;
                }
                e[b] = 0;
            }
            first = false;
            out.push_back(o);
            if (attacks) attacks->push_back(attack);
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
    if (const double tagged = filenameBpm(sourcePath.isEmpty() ? s.path : sourcePath, s.seconds()); tagged > 0) return {tagged, 1.0, true};
    const qint64 frames = std::min<qint64>(s.frames(), qint64(maxSeconds * s.rate));
    if (frames < qint64(s.rate * 1.0)) return none;   // under a second: no beat to find
    const int hop = std::max(32, int(std::lround(s.rate / 187.5)));   // ~5.3 ms, whatever the file's rate
    const double fps = s.rate / hop;
    double averageLevel = 0;
    std::vector<float> attacks;
    std::vector<float> o = onsetCurve(s, frames, hop, averageLevel, &attacks);
    const int n = int(o.size());
    if (n < 16) return none;
    auto prepare = [&](const std::vector<float>& raw) {
        // Smooth single-hop noise, then remove the slow level trend.
        std::vector<float> sm(size_t(n), 0.0f), result(size_t(n), 0.0f);
        for (int i = 0; i < n; ++i)
            sm[size_t(i)] = (raw[size_t(std::max(0, i - 1))] + raw[size_t(i)] + raw[size_t(std::min(n - 1, i + 1))]) / 3;
        const int win = std::max(1, int(fps));
        double run = 0;
        int count = 0;
        for (int i = 0; i < n; ++i) {
            run += sm[size_t(i)];
            ++count;
            if (i >= win) { run -= sm[size_t(i - win)]; --count; }
            result[size_t(i)] = std::max(0.0f, sm[size_t(i)] - float(run / count));
        }
        return result;
    };
    std::vector<float> on = prepare(o);
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
    const int lMin = std::max(2, int(std::ceil(fps * 60 / kMaxBpm)));
    int lMax = int(std::ceil(fps * 60 / kMinBpm));
    if (lMax > n - 8) lMax = n - 8;
    if (lMax <= lMin + 2) return none;
    const int lTop = std::min(n - 2, 8 * lMax + 2);
    std::vector<double> r(size_t(lTop + 1), 0.0);
    for (int L = 0; L <= lTop; ++L) {
        double acc = 0;
        for (int i = 0; i + L < n; ++i) acc += (double(on[size_t(i)]) - onMean) * (double(on[size_t(i + L)]) - onMean);
        r[size_t(L)] = acc / std::max(1, n - L);
    }
    if (r[0] <= 0) return none;
    auto at = [&](double lag) {
        if (lag < 0 || lag + 1 >= double(r.size())) return 0.0;
        const int a = int(std::floor(lag));
        return std::max(0.0, r[size_t(a)] + (r[size_t(a + 1)] - r[size_t(a)]) * (lag - a));
    };
    const double shortest = fps * 60 / kMaxBpm, longest = std::min(fps * 60 / kMinBpm, double(lMax));
    auto loopPrior = [&](double period) {
        const double beats = s.seconds() * fps / period, bars = std::round(beats / 4);
        return bars >= 1 && beats <= 64.025 && std::abs(beats - 4 * bars) < 0.04 ? 1.25 : 1.0;
    };
    auto comb = [&](double period) {
        // Fractional periods matter at fast tempos: rounding one beat to a hop misaligns
        // its later repeats, systematically favouring a three-eighth-note alias. Eighths
        // and sixteenths distinguish that alias from a coherent duple grid; long repeats
        // retain the bar structure when the rhythm does not hit every quarter note.
        return at(period) + 0.5 * at(2 * period) + 0.2 * at(3 * period)
            + 0.25 * at(4 * period) + 0.15 * at(8 * period)
            + 0.2 * at(period / 2) + 0.05 * at(period / 4);
    };
    auto score = [&](double period) {
        const double bpm = fps * 60 / period;
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 110.0) / 2.0, 2));
        return comb(period) * prior * loopPrior(period);
    };
    double period = shortest, bestScore = -1;
    // Search the metrical grid itself, rather than integer peaks of its first repeat.
    // The score includes subdivisions and up to eight beats, so every related candidate
    // (including 2/3 and 3/4 aliases) receives the same fractional timing accuracy.
    for (double lag = shortest; lag <= longest; lag += 0.125) {
        const double value = score(lag);
        if (value > bestScore) { bestScore = value; period = lag; }
    }
    auto fitPeriod = [&](double center, double radius = 1.0) {
        double chosen = center, value = -1;
        for (int step = -40; step <= 40; ++step) {
            const double lag = center + radius * step / 40;
            if (lag < shortest || lag > longest) continue;
            const double candidate = comb(lag) * loopPrior(lag);
            if (candidate > value) { value = candidate; chosen = lag; }
        }
        return chosen;
    };
    period = fitPeriod(period);
    // Select the faster member only when its quarter and dotted-quarter repeats carry
    // substantial evidence themselves. Weak hats cannot turn an ordinary beat into double
    // time. Meter remains genuinely ambiguous for isolated half-time percussion.
    const double base = period, baseStrength = at(base);
    for (int octave = 1; octave <= 2; ++octave) {
        if (base / std::exp2(octave) < shortest) break;
        const double faster = fitPeriod(base / std::exp2(octave));
        if (at(faster) >= 0.55 * baseStrength && at(3 * faster) >= 0.35 * baseStrength) period = faster;
    }
    // Fit widely separated repeat peaks. This reduces fast-beat quantization drift while
    // retaining the actual performance tempo instead of fitting an arbitrary file tail.
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
    if (period < shortest || period > longest) return none;
    const double strength = at(period) / r[0];
    // Sustained/repeating timbres often have a broad autocorrelation hill at a modulation
    // frequency, unrelated to the song's beat. Absolute correlation alone calls those a
    // confident tempo. A beat also needs local contrast and supported longer repeats.
    const double prominence = (at(period) - 0.5 * (at(0.92 * period) + at(1.08 * period))) / r[0];
    if (strength < 0.15 || prominence < std::max(0.06, 0.25 * strength)
        || std::max(at(2 * period), at(4 * period)) < 0.35 * at(period)
        || (4 * period + 8 < n && std::max(at(4 * period), at(8 * period)) < 0.35 * at(period))) return none;
    const auto slow = prepare(attacks);
    double slowMean = 0, slowEnergy = 0;
    for (float value : slow) slowMean += value;
    slowMean /= n;
    for (float value : slow) slowEnergy += (value - slowMean) * (value - slowMean);
    slowEnergy /= n;
    if (slowEnergy <= 1e-18) return none;
    auto slowAt = [&](double lag) {
        auto integral = [&](int offset) {
            if (offset < 0 || offset >= n - 8) return 0.0;
            double sum = 0;
            for (int i = 0; i + offset < n; ++i)
                sum += (slow[size_t(i)] - slowMean) * (slow[size_t(i + offset)] - slowMean);
            return sum / ((n - offset) * slowEnergy);
        };
        const int a = int(std::floor(lag));
        return std::max(0.0, integral(a) * (a + 1 - lag) + integral(a + 1) * (lag - a));
    };
    // Independently corroborate the candidate with a carrier-suppressed attack envelope.
    // A resonating bass note can make a very sharp peak in the fast RMS curve while its
    // actual attacks have no corresponding beat or phrase at that tempo.
    const double attackStrength = slowAt(period);
    if (attackStrength < 0.12 || (4 * period + 8 < n
        && std::max(slowAt(4 * period), slowAt(8 * period)) < std::max(0.12, 0.5 * attackStrength))) return none;
    // A long stem must repeat in separate musical passages. An intro, a single fill or a
    // short stretch of accidental periodicity must not define the grid for the whole song.
    double agreement = 1;
    if (n >= int(24 * fps)) {
        const int size = std::min(n, int(20 * fps));
        int active = 0, supported = 0;
        for (int window = 0; window < 5; ++window) {
            const int begin = (n - size) * window / 4;
            double mean = 0, energy = 0;
            for (int i = begin; i < begin + size; ++i) mean += on[size_t(i)];
            mean /= size;
            for (int i = begin; i < begin + size; ++i) energy += std::pow(on[size_t(i)] - mean, 2);
            energy /= size;
            if (energy < 0.05 * r[0]) continue;
            ++active;
            auto localAt = [&](double lag) {
                auto integral = [&](int offset) {
                    if (offset < 0 || offset >= size - 8) return 0.0;
                    double sum = 0;
                    for (int i = begin; i + offset < begin + size; ++i)
                        sum += (on[size_t(i)] - mean) * (on[size_t(i + offset)] - mean);
                    return sum / ((size - offset) * energy);
                };
                const int a = int(std::floor(lag));
                return std::max(0.0, integral(a) * (a + 1 - lag) + integral(a + 1) * (lag - a));
            };
            bool matches = false;
            // Permit small performance drift between passages, without accepting another
            // metrical family in the same way as a matching beat.
            for (int step = -5; step <= 5 && !matches; ++step) {
                const double p = period * (1 + step * 0.002);
                const double beat = localAt(p);
                const double contrast = beat - 0.5 * (localAt(0.92 * p) + localAt(1.08 * p));
                matches = beat >= 0.12 && contrast >= std::max(0.045, 0.2 * beat)
                    && std::max(localAt(2 * p), localAt(4 * p)) >= 0.3 * beat;
            }
            if (matches) ++supported;
        }
        if (active < 2 || supported < 2 || supported * 2 < active) return none;
        agreement = double(supported) / active;
    }
    const double bpm = fps * 60 / period;
    if (qEnvironmentVariableIsSet("SESSIONS_TEMPO_DEBUG"))
        std::fprintf(stderr, "tempo: %.6f BPM, correlation %.3f, prominence %.3f, passage agreement %.2f\n", bpm, strength, prominence, agreement);
    return {bpm, std::clamp(std::min(strength, attackStrength) * agreement, 0.0, 1.0), false};

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
    if (std::isfinite(detectedBpm) && detectedBpm > 0) {
        out.detected = detectedBpm;
        out.confidence = 1;
    } else {
        const auto estimate = detect(s, 90, sourcePath);
        out.detected = estimate.bpm;
        out.confidence = estimate.confidence;
        out.fromMetadata = estimate.fromMetadata;
    }
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
