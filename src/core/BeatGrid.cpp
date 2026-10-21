#include "BeatGrid.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>
#include <vector>

namespace sessions::tempo {
namespace {
constexpr double pi = 3.14159265358979323846;
struct Observation { double seconds = 0, bpm = 0, score = 0; bool valid = false; };
struct Feature { double rhythm = 0, high = 0; };

std::vector<Feature> onsets(const SampleData& sample, int hop) {
    const double lowCoefficient = std::exp(-2 * pi * 160 / sample.rate);
    const double midCoefficient = std::exp(-2 * pi * 2000 / sample.rate);
    double low[2] = {}, mid[2] = {}, energy[3] = {}, previous[3] = {};
    std::vector<Feature> result;
    result.reserve(sample.left.size() / size_t(hop));
    int inHop = 0;
    for (size_t i = 0; i < sample.left.size(); ++i) {
        for (int channel = 0; channel < 2; ++channel) {
            const double raw = channel ? sample.right[i] : sample.left[i];
            const double value = std::isfinite(raw) ? raw : 0;
            low[channel] = value + (low[channel] - value) * lowCoefficient;
            mid[channel] = value + (mid[channel] - value) * midCoefficient;
            const double bands[] = {low[channel], mid[channel] - low[channel], value - mid[channel]};
            for (int band = 0; band < 3; ++band) energy[band] += .5 * bands[band] * bands[band];
        }
        if (++inHop != hop) continue;
        Feature feature;
        for (int band = 0; band < 3; ++band) {
            const double level = std::pow(energy[band] / hop, .25);
            const double rise = result.empty() ? 0 : std::max(0.0, level - previous[band]);
            feature.rhythm += rise * (band == 0 ? 1 : band == 1 ? .8 : .6);
            if (band == 2) feature.high = rise;
            previous[band] = level;
            energy[band] = 0;
        }
        result.push_back(feature);
        inHop = 0;
    }
    return result;
}

double at(const std::vector<double>& values, double position) {
    if (values.empty()) return 0;
    position = std::clamp(position, 0.0, double(values.size() - 1));
    const size_t index = size_t(position);
    return values[index] + (values[std::min(index + 1, values.size() - 1)] - values[index]) * (position - index);
}

Observation measure(const std::vector<double>& onset, double center, double fps, double acceptedBpm) {
    Observation out; out.seconds = center;
    const int begin = std::max(0, int((center - 8) * fps));
    const int end = std::min(int(onset.size()), int((center + 8) * fps));
    const int count = end - begin;
    if (count < int(12 * fps)) return out;
    const double mean = std::accumulate(onset.begin() + begin, onset.begin() + end, 0.0) / count;
    std::vector<double> r(size_t(std::min(count - 2, int(8 * fps))) + 1, 0);
    for (size_t lag = 0; lag < r.size(); ++lag) {
        double sum = 0;
        for (int i = begin; i + int(lag) < end; ++i) sum += (onset[size_t(i)] - mean) * (onset[size_t(i) + lag] - mean);
        r[lag] = sum / (count - int(lag));
    }
    if (r[0] <= 1e-12) return out;
    const double zero = r[0];
    for (double& value : r) value /= zero;
    const double minimumBpm = std::max(30.0, acceptedBpm * .86);
    const double maximumBpm = std::min(320.0, acceptedBpm * 1.14);
    double period = 60 * fps / acceptedBpm, best = -1;
    for (double bpm = minimumBpm; bpm <= maximumBpm; bpm += .025) {
        const double lag = 60 * fps / bpm;
        double score = 0;
        for (int multiple = 1; multiple <= 16 && multiple * lag < double(r.size() - 1); ++multiple)
            score += std::max(0.0, at(r, multiple * lag)) / std::pow(double(multiple), .3);
        if (score > best) { best = score; period = lag; }
    }
    // Fit well-supported distant peaks, rather than letting an integer first lag turn a
    // 174 BPM recording into 173.1. No onset is moved or individually quantized.
    double numerator = 0, denominator = 0;
    int supported = 0;
    for (int multiple = 1; multiple <= 24; ++multiple) {
        const int centerLag = int(std::lround(multiple * period));
        if (centerLag + 4 >= int(r.size())) break;
        int peak = centerLag;
        for (int lag = std::max(1, centerLag - 2); lag <= centerLag + 2; ++lag)
            if (r[size_t(lag)] > r[size_t(peak)]) peak = lag;
        if (peak <= 0 || peak + 1 >= int(r.size()) || r[size_t(peak)] < .15
            || r[size_t(peak)] < r[size_t(peak - 1)] || r[size_t(peak)] < r[size_t(peak + 1)]) continue;
        double refined = peak;
        const double a = r[size_t(peak - 1)], b = r[size_t(peak)], c = r[size_t(peak + 1)], den = a - 2 * b + c;
        if (std::abs(den) > 1e-12) refined += std::clamp(.5 * (a - c) / den, -.5, .5);
        const double weight = b * b;
        numerator += multiple * refined * weight;
        denominator += multiple * multiple * weight;
        ++supported;
    }
    if (denominator == 0) return out;
    period = numerator / denominator;
    out.bpm = 60 * fps / period;
    out.score = best;
    out.valid = supported >= 4 && best >= 1.8 && out.bpm > minimumBpm + .15 && out.bpm < maximumBpm - .15;
    return out;
}

double interpolate(const std::vector<double>& times, const std::vector<double>& values, double time) {
    if (time <= times.front()) return values.front();
    if (time >= times.back()) return values.back();
    const size_t next = size_t(std::upper_bound(times.begin(), times.end(), time) - times.begin());
    const double ratio = (time - times[next - 1]) / (times[next] - times[next - 1]);
    return values[next - 1] + ratio * (values[next] - values[next - 1]);
}

void simplify(const std::vector<double>& beats, double fps, int begin, int end, QVector<GridPoint>& out) {
    const double first = beats[size_t(begin)], last = beats[size_t(end)];
    double error = 0; int split = -1;
    for (int i = begin + 1; i < end; ++i) {
        const double expected = first + (last - first) * (i - begin) / (end - begin);
        const double deviation = std::abs(beats[size_t(i)] - expected);
        if (deviation > error) { error = deviation; split = i; }
    }
    // Broad tempo curves only; ~2 ms at DnB speed, with no marker at every drum hit.
    if (error <= .006 || split < 0) { out.push_back({last, (end + .5) / fps}); return; }
    simplify(beats, fps, begin, split, out);
    simplify(beats, fps, split, end, out);
}
} // namespace

TimingGrid trackGrid(const SampleData& sample, double acceptedBpm) {
    TimingGrid fallback;
    if (!std::isfinite(acceptedBpm) || acceptedBpm < 20 || acceptedBpm > 999
        || !std::isfinite(sample.rate) || sample.rate <= 0 || sample.left.size() != sample.right.size() || sample.left.empty()) return fallback;
    const double duration = sample.seconds();
    fallback.points = {{0, 0}, {duration * acceptedBpm / 60, duration}};
    // A short loop does not contain enough independent evidence for a changing tempo.
    // Keep very long recordings bounded while leaving their accepted constant grid intact.
    if (duration < 48 || duration > 1800) return fallback;
    const int hop = std::max(32, int(std::lround(sample.rate / 187.5)));
    const double fps = sample.rate / hop;
    const auto features = onsets(sample, hop);
    if (features.size() < size_t(32 * fps)) return fallback;
    std::vector<double> onset(features.size()), prefix(features.size() + 1);
    for (size_t i = 0; i < features.size(); ++i) prefix[i + 1] = prefix[i] + features[i].rhythm;
    const int radius = int(fps * .5);
    for (int i = 0; i < int(features.size()); ++i) {
        const int low = std::max(0, i - radius), high = std::min(int(features.size()), i + radius + 1);
        onset[size_t(i)] = std::max(0.0, features[size_t(i)].rhythm - (prefix[size_t(high)] - prefix[size_t(low)]) / (high - low));
    }
    std::vector<Observation> observations;
    std::vector<double> times, tempos;
    for (double seconds = 8; seconds + 8 < duration; seconds += 4) {
        observations.push_back(measure(onset, seconds, fps, acceptedBpm));
        const auto& o = observations.back();
        if (o.valid) { times.push_back(o.seconds); tempos.push_back(o.bpm); }
    }
    const double coverage = double(times.size()) / std::max<size_t>(1, observations.size());
    if (times.size() < 7 || coverage < .55 || times.front() > 32 || times.back() < duration * .65) return fallback;
    // Reject isolated fits. A measured gradual change has support on both sides, while a
    // random periodic fluctuation or a fill must not bend an otherwise steady recording.
    auto sorted = tempos;
    std::sort(sorted.begin(), sorted.end());
    const double lowTempo = sorted[sorted.size() / 10], highTempo = sorted[sorted.size() * 9 / 10];
    if (highTempo - lowTempo < std::max(.7, acceptedBpm * .004)) return fallback;
    for (size_t i = 1; i < times.size(); ++i)
        if (std::abs(tempos[i] - tempos[i - 1]) > std::max(.8, (times[i] - times[i - 1]) * .35)) return fallback;
    std::vector<double> smoothTimes, smoothTempo;
    for (const auto& observation : observations) {
        smoothTimes.push_back(observation.seconds);
        smoothTempo.push_back(interpolate(times, tempos, observation.seconds));
    }
    const auto unsmoothed = smoothTempo;
    for (size_t i = 0; i < smoothTempo.size(); ++i)
        smoothTempo[i] = .25 * unsmoothed[i ? i - 1 : i] + .5 * unsmoothed[i] + .25 * unsmoothed[std::min(i + 1, unsmoothed.size() - 1)];
    std::vector<double> beats(features.size()), bpm(features.size());
    for (size_t i = 0; i < features.size(); ++i) {
        bpm[i] = interpolate(smoothTimes, smoothTempo, (i + .5) / fps);
        beats[i] = i ? beats[i - 1] + (bpm[i - 1] + bpm[i]) / (120 * fps) : bpm[i] / (120 * fps);
    }
    double coherent = 0, fixedCoherent = 0; int phaseCount = 0;
    std::vector<double> phaseTimes, phases, slowPhases, quarterPhases;
    for (const auto& observation : observations) {
        if (!observation.valid) continue;
        const int begin = std::max(0, int((observation.seconds - 8) * fps));
        const int end = std::min(int(features.size()), int((observation.seconds + 8) * fps));
        std::complex<double> adaptive(0, 0), slow(0, 0), quarter(0, 0), fixed(0, 0); double weight = 0, quarterWeight = 0;
        for (int i = begin; i < end; ++i) {
            const double w = features[size_t(i)].high;
            adaptive += std::polar(w, 8 * pi * beats[size_t(i)]);
            slow += std::polar(w, 4 * pi * beats[size_t(i)]);
            quarter += std::polar(features[size_t(i)].rhythm, 2 * pi * beats[size_t(i)]);
            quarterWeight += features[size_t(i)].rhythm;
            fixed += std::polar(w, 8 * pi * (i + .5) * acceptedBpm / (60 * fps));
            weight += w;
        }
        if (weight <= 1e-9) continue;
        const double coherence = std::abs(adaptive) / weight;
        coherent += coherence; fixedCoherent += std::abs(fixed) / weight; ++phaseCount;
        if (coherence >= .28) {
            phaseTimes.push_back(observation.seconds);
            phases.push_back(std::arg(adaptive) / (8 * pi));
            slowPhases.push_back(std::abs(slow) / weight >= .20 ? std::arg(slow) / (4 * pi) : std::nan(""));
            quarterPhases.push_back(quarterWeight > 1e-9 && std::abs(quarter) / quarterWeight >= .10 ? std::arg(quarter) / (2 * pi) : std::nan(""));
        }
    }
    if (phaseCount < 5 || coherent / phaseCount < .20 || coherent < 1.12 * fixedCoherent + .015 * phaseCount) return fallback;
    for (size_t i = 1; i < phaseTimes.size(); ++i)
        if (phaseTimes[i] - phaseTimes[i - 1] > 12 && (!std::isfinite(slowPhases[i - 1]) || !std::isfinite(slowPhases[i])))
            return fallback; // A sixteenth-note-only pulse cannot establish the beat count across a silent bridge.
    if (phases.size() >= 5) {
        const double reference = phases.front();
        std::vector<double> correctionTimes{0}, corrections{0};
        double previous = reference;
        double slowReference = 0, previousSlow = 0;
        bool haveSlow = false;
        double quarterReference = 0, previousQuarter = 0;
        bool haveQuarter = false;
        for (size_t i = 0; i < phases.size(); ++i) {
            const double phase = phases[i] + .25 * std::round((previous - phases[i]) / .25);
            if (std::abs(phase - reference) > .10) continue;
            double correction = reference - phase;
            if (std::isfinite(slowPhases[i])) {
                if (!haveSlow) { slowReference = previousSlow = slowPhases[i]; haveSlow = true; }
                const double slow = slowPhases[i] + .5 * std::round((previousSlow - slowPhases[i]) / .5);
                // Sixteenth-note coherence alone conceals a quarter-beat count error after
                // a drumless passage. The eighth-note harmonic chooses the cycle; the
                // stronger sixteenth harmonic retains the fine timing within that cycle.
                correction += .25 * std::round((slowReference - slow - correction) / .25);
                previousSlow = slow;
            }
            if (std::isfinite(quarterPhases[i])) {
                if (!haveQuarter) { quarterReference = previousQuarter = quarterPhases[i]; haveQuarter = true; }
                const double quarter = quarterPhases[i] + std::round(previousQuarter - quarterPhases[i]);
                correction += .5 * std::round((quarterReference - quarter - correction) / .5);
                previousQuarter = quarter;
            } else if (std::abs(correction) > .15) return fallback;
            if (std::abs(correction) > .35) return fallback;
            correctionTimes.push_back(phaseTimes[i]); corrections.push_back(correction); previous = phase;
        }
        const auto original = corrections;
        for (size_t i = 1; i + 1 < corrections.size(); ++i)
            if (correctionTimes[i] - correctionTimes[i - 1] <= 8 && correctionTimes[i + 1] - correctionTimes[i] <= 8)
                corrections[i] = .25 * original[i - 1] + .5 * original[i] + .25 * original[i + 1];
        for (size_t i = 0; i < beats.size(); ++i) beats[i] += interpolate(correctionTimes, corrections, (i + .5) / fps);
    }
    TimingGrid result;
    result.points.push_back({0, 0});
    simplify(beats, fps, 0, int(beats.size() - 1), result.points);
    const double finalBeat = beats.back() + (duration - (beats.size() - .5) / fps) * bpm.back() / 60;
    if (duration > result.points.back().seconds + 1e-9) result.points.push_back({finalBeat, duration});
    else result.points.back() = {finalBeat, duration};
    if (result.points.size() < 3 || result.points.size() > 512) return fallback;
    for (int i = 1; i < result.points.size(); ++i) {
        const auto a = result.points[i - 1], b = result.points[i];
        const double localBpm = 60 * (b.beat - a.beat) / (b.seconds - a.seconds);
        if (!std::isfinite(localBpm) || b.seconds <= a.seconds || b.beat <= a.beat || localBpm < acceptedBpm * .82 || localBpm > acceptedBpm * 1.18) return fallback;
    }
    result.variable = true;
    result.confidence = std::clamp(coverage * coherent / phaseCount, 0.0, 1.0);
    return result;
}
} // namespace sessions::tempo
